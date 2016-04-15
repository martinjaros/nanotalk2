/*
 * Copyright (C) 2016 - Martin Jaros <xjaros32@stud.feec.vutbr.cz>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif /* HAVE_CONFIG_H */

#define G_LOG_DOMAIN "Nanotalk"

#include <string.h>
#include <gtk/gtk.h>
#include <gst/gst.h>
#include <glib/gi18n.h>
#include "application.h"

#include "ringback.h"
#include "rtpencrypt.h"
#include "rtpdecrypt.h"

#define RECV_TIMEOUT 3000000000L // 3 seconds

struct _application
{
    GtkWidget *window, *entry, *button_start, *button_stop, *menu, *dialog;
    GtkStatusIcon *status_icon;
    GtkListStore *completions;

    GSocketFamily family;
    DhtClient *client;

    GstElement *rx_pipeline, *tx_pipeline;
    guint rx_watch, tx_watch;

    gchar *sound_file;
};

static void lookup_start(Application *app, const gchar *id)
{
    g_autoptr(GError) error = NULL;
    if(!dht_client_lookup(app->client, id, &error))
    {
        g_message("%s %s", id, error->message);
        return;
    }

    gtk_widget_set_sensitive(app->button_start, FALSE);
}

static void call_start(GtkWidget *widget, Application *app)
{
    const gchar *text = gtk_entry_get_text(GTK_ENTRY(app->entry));

    // Translate alias to ID
    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(app->completions), &iter);
    while(valid)
    {
        g_autofree gchar *id = NULL;
        g_autofree gchar *alias = NULL;
        gtk_tree_model_get(GTK_TREE_MODEL(app->completions), &iter, 0, &id, 1, &alias, -1);
        if(strcmp(text, alias) == 0)
            return lookup_start(app, id);

        valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(app->completions), &iter);
    }

    if(strlen(text) == DHT_ID_LENGTH)
        lookup_start(app, text);
}

static void call_stop(GtkWidget *widget, Application *app)
{
    if(app->dialog)
    {
        gtk_dialog_response(GTK_DIALOG(app->dialog), GTK_RESPONSE_CANCEL);
        return;
    }

    if(app->tx_pipeline)
    {
        gst_debug_bin_to_dot_file_with_ts(GST_BIN(app->tx_pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "stop-tx-pipeline");
        gst_element_set_state(app->tx_pipeline, GST_STATE_NULL);
        g_source_remove(app->tx_watch);
        gst_object_unref(app->tx_pipeline);
        app->tx_pipeline = NULL;
    }

    if(app->rx_pipeline)
    {
        gst_debug_bin_to_dot_file_with_ts(GST_BIN(app->rx_pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "stop-rx-pipeline");
        gst_element_set_state(app->rx_pipeline, GST_STATE_NULL);
        g_source_remove(app->rx_watch);
        gst_object_unref(app->rx_pipeline);
        app->rx_pipeline = NULL;
    }

    gtk_widget_set_sensitive(app->button_start, TRUE);
    gtk_widget_set_sensitive(app->button_stop, FALSE);
}

static void call_toggle(GtkWidget *widget, Application *app)
{
    if(gtk_widget_is_sensitive(app->button_start))
        call_start(app->button_start, app);
    else if(gtk_widget_is_sensitive(app->button_stop))
        call_stop(app->button_stop, app);
}

static gboolean bus_watch(GstBus *bus, GstMessage *message, Application *app)
{
    switch(message->type)
    {
        case GST_MESSAGE_ERROR:
        {
            g_autoptr(GError) error = NULL;
            g_autofree gchar *debug = NULL;
            gst_message_parse_error(message, &error, &debug);
            g_warning("%s %s", error->message, debug);
            break;
        }

        case GST_MESSAGE_ELEMENT:
        {
            // Stop pipeline on socket timeout
            if(strcmp(gst_structure_get_name(gst_message_get_structure(message)), "GstUDPSrcTimeout") == 0)
                call_stop(app->button_stop, app);

            break;
        }

        case GST_MESSAGE_EOS:
        {
            if(strcmp(GST_OBJECT_NAME(message->src), "playbin_loop") == 0)
                gst_element_seek_simple(GST_ELEMENT(message->src), GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH, 0);

            break;
        }

        default: break;
    }

    return TRUE;
}

static gboolean dialog_run(Application *app)
{
    GstElement *playbin = NULL;
    guint playbin_watch;

    if(app->sound_file)
    {
        // Play sound file
        playbin = gst_element_factory_make("playbin", "playbin_loop");
        GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(playbin));
        playbin_watch = gst_bus_add_watch(bus, (GstBusFunc)bus_watch, app);
        gst_object_unref(bus);

        g_autofree gchar *uri = gst_filename_to_uri(app->sound_file, NULL);
        g_object_set(playbin, "uri", uri, NULL);
        gst_element_set_state(playbin, GST_STATE_PLAYING);
    }

    // Show dialog
    app->dialog = gtk_message_dialog_new_with_markup(GTK_WINDOW(app->window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
        _("Answer an incoming call from <b>%s</b> ?"), gtk_entry_get_text(GTK_ENTRY(app->entry)));

    gint response = gtk_dialog_run(GTK_DIALOG(app->dialog));
    gtk_widget_destroy(app->dialog);
    app->dialog = NULL;

    if(playbin)
    {
        // Stop call sound
        g_source_remove(playbin_watch);
        gst_element_set_state(playbin, GST_STATE_NULL);
        gst_object_unref(playbin);
    }

    if(response == GTK_RESPONSE_YES)
    {
        GstElement *sink_volume = gst_bin_get_by_name(GST_BIN(app->rx_pipeline), "sink_volume");
        g_object_set(sink_volume, "mute", FALSE, NULL);

        GstElement *ringback = gst_bin_get_by_name(GST_BIN(app->tx_pipeline), "ringback");
        g_object_set(ringback, "enabled", FALSE, NULL);
    }
    else call_stop(app->button_stop, app);

    return G_SOURCE_REMOVE;
}

static GSocket* accept_connection(DhtClient *client, const gchar *id, GSocketAddress *sockaddr, gboolean remote, Application *app)
{
    gboolean do_accept = !remote;
    if(remote && gtk_widget_is_sensitive(app->button_start))
    {
        gtk_widget_set_sensitive(app->button_start, FALSE);
        do_accept = TRUE;
    }

    return do_accept ? g_socket_new(app->family, G_SOCKET_TYPE_DATAGRAM, G_SOCKET_PROTOCOL_UDP, NULL) : NULL;
}

static void new_connection(DhtClient *client, const gchar *peer_id,
        GSocket *socket, GSocketAddress *sockaddr, GBytes *enc_key, GBytes *dec_key, gboolean remote, Application *app)
{
    // Translate ID to alias
    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(app->completions), &iter);
    while(valid)
    {
        g_autofree gchar *id = NULL;
        g_autofree gchar *alias = NULL;
        gtk_tree_model_get(GTK_TREE_MODEL(app->completions), &iter, 0, &id, 1, &alias, -1);
        if(strcmp(peer_id, id) == 0)
        {
            gtk_entry_set_text(GTK_ENTRY(app->entry), alias);
            break;
        }

        valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(app->completions), &iter);
    }

    // Display direct ID if no alias was found
    if(!valid) gtk_entry_set_text(GTK_ENTRY(app->entry), peer_id);

    // Start receiver
    app->rx_pipeline = gst_pipeline_new("rx_pipeline");
    GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(app->rx_pipeline));
    app->rx_watch = gst_bus_add_watch(bus, (GstBusFunc)bus_watch, app);
    gst_object_unref(bus);

    GstCaps *caps = gst_caps_new_simple("application/x-rtp",
        "media", G_TYPE_STRING, "audio",
        "clock-rate", G_TYPE_INT, 48000,
        "encoding-name", G_TYPE_STRING, "X-GST-OPUS-DRAFT-SPITTKA-00",
        NULL);

    GstElement *rtp_src = gst_element_factory_make("udpsrc", "rtp_src");
    g_object_set(rtp_src, "caps", caps, "socket", socket, "timeout", RECV_TIMEOUT, NULL);
    gst_caps_unref(caps);

    GstElement *rtp_dec = gst_element_factory_make("rtpdecrypt", "rtp_dec");
    g_object_set(rtp_dec, "key", dec_key, NULL);

    GstElement *rtp_buf = gst_element_factory_make("rtpjitterbuffer", "rtp_buf");
    GstElement *audio_depay = gst_element_factory_make("rtpopusdepay", "audio_depay");
    GstElement *audio_dec = gst_element_factory_make("opusdec", "audio_dec");
    GstElement *audio_sink = gst_element_factory_make("autoaudiosink", "audio_sink");

    GstElement *sink_volume = gst_element_factory_make("volume", "sink_volume");
    g_object_set(sink_volume, "mute", remote, NULL);

    gst_bin_add_many(GST_BIN(app->rx_pipeline), rtp_src, rtp_dec, rtp_buf, audio_depay, audio_dec, sink_volume, audio_sink, NULL);
    gst_element_link_many(rtp_src, rtp_dec, rtp_buf, audio_depay, audio_dec, sink_volume, audio_sink, NULL);
    gst_debug_bin_to_dot_file_with_ts(GST_BIN(app->rx_pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "start-rx-pipeline");
    gst_element_set_state(app->rx_pipeline, GST_STATE_PLAYING);

    // Start transmitter
    app->tx_pipeline = gst_pipeline_new("tx_pipeline");
    bus = gst_pipeline_get_bus(GST_PIPELINE(app->tx_pipeline));
    app->tx_watch = gst_bus_add_watch(bus, (GstBusFunc)bus_watch, app);
    gst_object_unref(bus);

    GstElement *ringback = gst_element_factory_make("ringback", "ringback");
    g_object_set(ringback, "enabled", remote, NULL);

    GstElement *audio_src = gst_element_factory_make("autoaudiosrc", "audio_src");
    GstElement *audio_enc = gst_element_factory_make("opusenc", "audio_enc");
    GstElement *audio_pay = gst_element_factory_make("rtpopuspay", "audio_pay");
    g_object_set(audio_enc, "cbr", FALSE, "dtx", TRUE, NULL);

    GstElement *rtp_enc = gst_element_factory_make("rtpencrypt", "rtp_enc");
    g_object_set(rtp_enc, "key", enc_key, NULL);

    g_autofree gchar *host = g_inet_address_to_string(g_inet_socket_address_get_address(G_INET_SOCKET_ADDRESS(sockaddr)));
    gint port = g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(sockaddr));

    GstElement *rtp_sink = gst_element_factory_make("udpsink", "rtp_sink");
    g_object_set(rtp_sink, "socket", socket, "close-socket", FALSE, "host", host, "port", port, NULL);

    gst_bin_add_many(GST_BIN(app->tx_pipeline), audio_src, ringback, audio_enc, audio_pay, rtp_enc, rtp_sink, NULL);
    gst_element_link_many(audio_src, ringback, audio_enc, audio_pay, rtp_enc, rtp_sink, NULL);
    gst_debug_bin_to_dot_file_with_ts(GST_BIN(app->tx_pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "start-tx-pipeline");
    gst_element_set_state(app->tx_pipeline, GST_STATE_PLAYING);

    // Show windows
    if(remote) g_idle_add((GSourceFunc)dialog_run, app);
    gtk_widget_set_sensitive(app->button_stop, TRUE);
    if(!gtk_widget_get_visible(app->window))
        gtk_widget_show(app->window);
}

static void on_error(DhtClient *client, const gchar *id, GError *error, Application *app)
{
    gtk_widget_set_sensitive(app->button_start, TRUE);
    g_message("%s %s", id, error->message);
}

static void window_toggle(GtkWidget *widget, Application *app)
{
    if(!gtk_widget_get_visible(app->window))
        gtk_widget_show(app->window);
    else
        gtk_widget_hide(app->window);
}

static void show_about(GtkWidget *widget, Application *app)
{
    const gchar *authors[] = { "Martin Jaro\xC5\xA1 <xjaros32@stud.feec.vutbr.cz>", NULL };
    gtk_show_about_dialog(NULL,
        "logo-icon-name", "call-start-symbolic",
        "version", PACKAGE_VERSION,
        "comments", PACKAGE_NAME,
        "authors", authors,
        "license-type", GTK_LICENSE_GPL_2_0,
        "website", PACKAGE_URL,
        NULL);
}

static void menu_popup(GtkWidget *widget, guint button, guint activate_time, Application *app)
{
    gtk_menu_popup(GTK_MENU(app->menu), NULL, NULL, gtk_status_icon_position_menu, widget, button, activate_time);
}

static gboolean plugin_init(GstPlugin *plugin)
{
    return gst_element_register(plugin, "ringback", GST_RANK_NONE, GST_TYPE_RINGBACK) &&
           gst_element_register(plugin, "rtpencrypt", GST_RANK_NONE, GST_TYPE_RTP_ENCRYPT) &&
           gst_element_register(plugin, "rtpdecrypt", GST_RANK_NONE, GST_TYPE_RTP_DECRYPT);
}

gboolean application_init(GError **error)
{
    static gboolean initialized = FALSE;
    if(initialized) return TRUE;

    GstRegistry *registry = gst_registry_get();
    if(!gst_registry_check_feature_version(registry, "playbin", GST_VERSION_MAJOR, GST_VERSION_MINOR, GST_VERSION_MICRO) ||
       !gst_registry_check_feature_version(registry, "volume", GST_VERSION_MAJOR, GST_VERSION_MINOR, GST_VERSION_MICRO))
    {
        g_set_error_literal(error, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_INIT, _("Missing GStreamer Base Plugins"));
        return FALSE;
    }

    if(!gst_registry_check_feature_version(registry, "udpsrc", GST_VERSION_MAJOR, GST_VERSION_MINOR, GST_VERSION_MICRO) ||
       !gst_registry_check_feature_version(registry, "udpsink", GST_VERSION_MAJOR, GST_VERSION_MINOR, GST_VERSION_MICRO) ||
       !gst_registry_check_feature_version(registry, "autoaudiosrc", GST_VERSION_MAJOR, GST_VERSION_MINOR, GST_VERSION_MICRO) ||
       !gst_registry_check_feature_version(registry, "autoaudiosink", GST_VERSION_MAJOR, GST_VERSION_MINOR, GST_VERSION_MICRO) ||
       !gst_registry_check_feature_version(registry, "rtpjitterbuffer", GST_VERSION_MAJOR, GST_VERSION_MINOR, GST_VERSION_MICRO))
    {
        g_set_error_literal(error, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_INIT, _("Missing GStreamer Good Plugins"));
        return FALSE;
    }

    if(!gst_registry_check_feature_version(registry, "rtpopusdepay", GST_VERSION_MAJOR, GST_VERSION_MINOR, GST_VERSION_MICRO) ||
       !gst_registry_check_feature_version(registry, "opusdec", GST_VERSION_MAJOR, GST_VERSION_MINOR, GST_VERSION_MICRO) ||
       !gst_registry_check_feature_version(registry, "opusenc", GST_VERSION_MAJOR, GST_VERSION_MINOR, GST_VERSION_MICRO) ||
       !gst_registry_check_feature_version(registry, "rtpopuspay", GST_VERSION_MAJOR, GST_VERSION_MINOR, GST_VERSION_MICRO))
    {
        g_set_error_literal(error, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_INIT, _("Missing Opus plugin for GStreamer"));
        return FALSE;
    }

    if(!gst_plugin_register_static(GST_VERSION_MAJOR, GST_VERSION_MINOR, "nanotalk-gst", "Nanotalk GStreamer plugin",
            plugin_init, PACKAGE_VERSION, "GPL", PACKAGE_TARNAME, PACKAGE_NAME, PACKAGE_URL))
    {
        g_set_error_literal(error, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_INIT, _("Cannot register GStreamer plugin"));
        return FALSE;
    }

    initialized = TRUE;
    return TRUE;
}

void application_add_option_group(GOptionContext *context)
{
    g_option_context_add_group(context, gtk_get_option_group(TRUE));
    g_option_context_add_group(context, gst_init_get_option_group());
}

Application* application_new(DhtClient *client, gboolean ipv6, const gchar *aliases_path, const gchar *sound_file)
{
    Application *app = g_new0(Application, 1);
    app->sound_file = g_strdup(sound_file);

    app->client = g_object_ref(client);
    g_signal_connect(app->client, "accept-connection", (GCallback)accept_connection, app);
    g_signal_connect(app->client, "new-connection", (GCallback)new_connection, app);
    g_signal_connect(app->client, "on-error", (GCallback)on_error, app);
    app->family = ipv6 ? G_SOCKET_FAMILY_IPV6 : G_SOCKET_FAMILY_IPV4;

    app->completions = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_STRING);
    if(aliases_path)
    {
        // Load aliases from file
        g_autoptr(GError) error = NULL;
        g_autoptr(GIOChannel) channel = g_io_channel_new_file(aliases_path, "r", &error);
        if(!channel) g_warning("Failed to read aliases from %s. %s", aliases_path, error->message);
        else while(1)
        {
            gsize len = 0;
            g_autofree gchar *str = NULL;
            GIOStatus status = g_io_channel_read_line(channel, &str, &len, NULL, NULL);
            if((status == G_IO_STATUS_EOF) || (status == G_IO_STATUS_ERROR)) break;
            if((status == G_IO_STATUS_NORMAL) && (len > DHT_ID_LENGTH))
            {
                // Split into ID and alias, strip trailing whitespace
                str[DHT_ID_LENGTH] = 0;
                gchar *end = str + len - 1;
                while(g_ascii_isspace(*end)) *end-- = 0;
                if(end > str + DHT_ID_LENGTH)
                {
                    GtkTreeIter iter;
                    gtk_list_store_append(app->completions, &iter);
                    gtk_list_store_set(app->completions, &iter, 0, str, 1, str + DHT_ID_LENGTH + 1, -1);
                }
            }
        }
    }

    // Create widgets
    app->entry = gtk_entry_new();
    g_signal_connect(app->entry, "activate", (GCallback)call_toggle, app);
    app->button_start = gtk_button_new_from_icon_name("call-start", GTK_ICON_SIZE_BUTTON);
    g_signal_connect(app->button_start, "clicked", (GCallback)call_start, app);
    app->button_stop = gtk_button_new_from_icon_name("call-stop", GTK_ICON_SIZE_BUTTON);
    g_signal_connect(app->button_stop, "clicked", (GCallback)call_stop, app);
    gtk_widget_set_sensitive(app->button_stop, FALSE);

    // Completion
    GtkEntryCompletion *completion = gtk_entry_completion_new();
    g_object_set(completion, "model", app->completions, "inline-completion", TRUE, "inline-selection", TRUE, NULL);
    gtk_entry_completion_set_text_column(completion, 1);
    gtk_entry_set_completion(GTK_ENTRY(app->entry), completion);
    g_object_unref(completion);

    // Geometry
    GtkWidget *grid = gtk_grid_new();
    g_object_set(grid, "column-homogeneous", TRUE, "column-spacing", 5, "row-spacing", 5, "margin", 10, NULL);
    gtk_grid_attach(GTK_GRID(grid), app->entry, 0, 0, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), app->button_start, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), app->button_stop, 1, 1, 1, 1);
    gtk_widget_show_all(GTK_WIDGET(grid));

    // Main window
    app->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    g_signal_connect(app->window, "delete-event", (GCallback)gtk_widget_hide_on_delete, NULL);
    g_object_set(app->window, "icon-name", "call-start-symbolic", "title", "Nanotalk", "resizable", FALSE, NULL);
    gtk_container_add(GTK_CONTAINER(app->window), GTK_WIDGET(grid));

    // Status icon menu
    app->menu = gtk_menu_new();

    GtkWidget *menu_item = gtk_menu_item_new();
    g_signal_connect(menu_item, "activate", (GCallback)show_about, app);
    GtkWidget *menu_icon = gtk_image_new_from_icon_name("help-about", GTK_ICON_SIZE_MENU);
    GtkWidget *menu_label = gtk_label_new(_("About"));
    GtkWidget *menu_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_container_add(GTK_CONTAINER(menu_box), menu_icon);
    gtk_container_add(GTK_CONTAINER(menu_box), menu_label);
    gtk_container_add(GTK_CONTAINER(menu_item), menu_box);
    gtk_menu_shell_append(GTK_MENU_SHELL(app->menu), menu_item);

    menu_item = gtk_menu_item_new();
    g_signal_connect(menu_item, "activate", (GCallback)gtk_main_quit, NULL);
    menu_icon = gtk_image_new_from_icon_name("application-exit", GTK_ICON_SIZE_MENU);
    menu_label = gtk_label_new(_("Quit"));
    menu_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_container_add(GTK_CONTAINER(menu_box), menu_icon);
    gtk_container_add(GTK_CONTAINER(menu_box), menu_label);
    gtk_container_add(GTK_CONTAINER(menu_item), menu_box);
    gtk_menu_shell_append(GTK_MENU_SHELL(app->menu), menu_item);

    gtk_widget_show_all(app->menu);
    app->status_icon = gtk_status_icon_new_from_icon_name("call-start-symbolic");
    g_signal_connect(app->status_icon, "popup-menu", (GCallback)menu_popup, app);
    g_signal_connect(app->status_icon, "activate", (GCallback)window_toggle, app);
    g_object_set(app->status_icon, "tooltip-text", "Nanotalk", "title", "Nanotalk", NULL);

    return app;
}

void application_free(Application *app)
{
    if(gtk_widget_is_sensitive(app->button_stop))
        call_stop(app->button_stop, app);

    gtk_widget_destroy(app->window);
    gtk_widget_destroy(app->menu);
    g_object_unref(app->status_icon);
    g_object_unref(app->completions);
    g_object_unref(app->client);

    g_free(app->sound_file);
    g_free(app);
}

void application_run()
{
    gtk_main();
}
