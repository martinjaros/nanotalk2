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
#include "gstrtpencrypt.h"
#include "gstrtpdecrypt.h"
#include "dhtclient.h"

#define DEFAULT_PORT 5004
#define RECV_TIMEOUT 3000000000L // 3 seconds

typedef struct _application Application;

struct _application
{
    GtkWidget *window, *entry, *button_start, *button_stop, *menu;
    GtkStatusIcon *status_icon;
    GtkListStore *completions;
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
        g_info("%s %s", id, error->message);
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
    gtk_widget_set_sensitive(app->button_start, TRUE);
    gtk_widget_set_sensitive(app->button_stop, FALSE);

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

        case GST_MESSAGE_WARNING:
        {
            // Stop pipeline on socket error
            if(strcmp(GST_OBJECT_NAME(message->src), "rtp_sink") == 0)
                call_stop(app->button_stop, app);

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

static gboolean accept_connection(DhtClient *client, const gchar *id, Application *app)
{
    if(gtk_widget_is_sensitive(app->button_start))
    {
        gtk_widget_set_sensitive(app->button_start, FALSE);
        return TRUE;
    }

    return FALSE;
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

    // Connect sink socket to receive ICMP errors
    g_autoptr(GSocket) sink_socket = g_socket_new(g_socket_get_family(socket), G_SOCKET_TYPE_DATAGRAM, G_SOCKET_PROTOCOL_UDP, NULL);
    g_socket_connect(sink_socket, sockaddr, NULL, NULL);

    g_autofree gchar *host = g_inet_address_to_string(g_inet_socket_address_get_address(G_INET_SOCKET_ADDRESS(sockaddr)));
    gint port = g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(sockaddr));

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

    GstElement *src_volume = gst_element_factory_make("volume", "src_volume");
    g_object_set(src_volume, "mute", remote, NULL);

    GstElement *audio_src = gst_element_factory_make("autoaudiosrc", "audio_src");
    GstElement *audio_enc = gst_element_factory_make("opusenc", "audio_enc");
    GstElement *audio_pay = gst_element_factory_make("rtpopuspay", "audio_pay");
    g_object_set(audio_enc, "cbr", FALSE, "dtx", TRUE, NULL);

    GstElement *rtp_enc = gst_element_factory_make("rtpencrypt", "rtp_enc");
    g_object_set(rtp_enc, "key", enc_key, NULL);

    GstElement *rtp_sink = gst_element_factory_make("udpsink", "rtp_sink");
    g_object_set(rtp_sink, "socket", sink_socket, "host", host, "port", port, NULL);

    gst_bin_add_many(GST_BIN(app->tx_pipeline), audio_src, src_volume, audio_enc, audio_pay, rtp_enc, rtp_sink, NULL);
    gst_element_link_many(audio_src, src_volume, audio_enc, audio_pay, rtp_enc, rtp_sink, NULL);
    gst_debug_bin_to_dot_file_with_ts(GST_BIN(app->tx_pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "start-tx-pipeline");
    gst_element_set_state(app->tx_pipeline, GST_STATE_PLAYING);

    if(remote)
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
        GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(app->window),
            GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
            "Incoming call from %s\nAnswer?", gtk_entry_get_text(GTK_ENTRY(app->entry)));

        gint response = gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        if(playbin)
        {
            g_source_remove(playbin_watch);
            gst_element_set_state(playbin, GST_STATE_NULL);
            gst_object_unref(playbin);
        }

        if(response == GTK_RESPONSE_YES)
        {
            g_object_set(src_volume, "mute", FALSE, NULL);
            g_object_set(sink_volume, "mute", FALSE, NULL);
        }
        else
        {
            // Terminate
            call_stop(app->button_stop, app);
            return;
        }
    }

    // Show main window
    gtk_widget_set_sensitive(app->button_stop, TRUE);
    if(!gtk_widget_get_visible(app->window))
        gtk_widget_show(app->window);
}

static void on_error(DhtClient *client, const gchar *id, GError *error, Application *app)
{
    gtk_widget_set_sensitive(app->button_start, TRUE);
    g_info("%s %s", id, error->message);
}

static void window_toggle(GtkWidget *widget, Application *app)
{
    if(!gtk_widget_get_visible(app->window))
        gtk_widget_show(app->window);
    else
        gtk_widget_hide(app->window);
}

static void menu_popup(GtkWidget *widget, guint button, guint activate_time, Application *app)
{
    gtk_menu_popup(GTK_MENU(app->menu), NULL, NULL, gtk_status_icon_position_menu, widget, button, activate_time);
}

static gboolean plugin_init(GstPlugin *plugin)
{
    return gst_element_register(plugin, "rtpencrypt", GST_RANK_NONE, GST_TYPE_RTP_ENCRYPT) &&
           gst_element_register(plugin, "rtpdecrypt", GST_RANK_NONE, GST_TYPE_RTP_DECRYPT);
}

static gboolean application_init(Application *app, int *argc, char ***argv, GError **error)
{
    g_autofree gchar *key_path = NULL;
    g_autofree gchar *aliases_path = NULL;
    g_autofree gchar *bootstrap_host = NULL;
    gint bootstrap_port = DEFAULT_PORT;
    gint local_port = DEFAULT_PORT;
    gboolean ipv6 = FALSE;

    GOptionEntry options[] =
    {
        { "key", 'k', 0, G_OPTION_ARG_FILENAME, &key_path, "Private key", "FILE" },
        { "aliases", 'a', 0, G_OPTION_ARG_FILENAME, &aliases_path, "List of aliases", "FILE" },
        { "local-port", 'l', 0, G_OPTION_ARG_INT, &local_port, "Source port (default " G_STRINGIFY(DEFAULT_PORT) ")", "NUM" },
        { "ipv6", '6', 0, G_OPTION_ARG_NONE, &ipv6, "Enable IPv6", NULL },
        { "bootstrap-host", 'h', 0, G_OPTION_ARG_STRING, &bootstrap_host, "Bootstrap address", "ADDR" },
        { "bootstrap-port", 'p', 0, G_OPTION_ARG_INT, &bootstrap_port, "Bootstrap port", "NUM" },
        { "call-sound", 's', 0, G_OPTION_ARG_STRING, &app->sound_file, "Incoming call sound", "FILE" },
        { NULL }
    };

    // Parse command line
    g_autoptr(GOptionContext) context = g_option_context_new(NULL);
    g_option_context_set_summary(context, PACKAGE_STRING);
    g_option_context_set_description(context, PACKAGE_BUGREPORT "\n" PACKAGE_URL);
    g_option_context_add_main_entries(context, options, NULL);
    g_option_context_add_group(context, gtk_get_option_group(TRUE));
    g_option_context_add_group(context, gst_init_get_option_group());
    if(!g_option_context_parse(context, argc, argv, error))
        return FALSE;

    // Check GStreamer plugins
    GstRegistry *registry = gst_registry_get();
    if(!gst_registry_check_feature_version(registry, "playbin", GST_VERSION_MAJOR, GST_VERSION_MINOR, GST_VERSION_MICRO) ||
       !gst_registry_check_feature_version(registry, "volume", GST_VERSION_MAJOR, GST_VERSION_MINOR, GST_VERSION_MICRO))
    {
        g_warning("Missing GStreamer Base Plugins");
        if(error) *error = g_error_new_literal(GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_INIT, "Cannot load plugin");
        return FALSE;
    }

    if(!gst_registry_check_feature_version(registry, "udpsrc", GST_VERSION_MAJOR, GST_VERSION_MINOR, GST_VERSION_MICRO) ||
       !gst_registry_check_feature_version(registry, "udpsink", GST_VERSION_MAJOR, GST_VERSION_MINOR, GST_VERSION_MICRO) ||
       !gst_registry_check_feature_version(registry, "autoaudiosrc", GST_VERSION_MAJOR, GST_VERSION_MINOR, GST_VERSION_MICRO) ||
       !gst_registry_check_feature_version(registry, "autoaudiosink", GST_VERSION_MAJOR, GST_VERSION_MINOR, GST_VERSION_MICRO) ||
       !gst_registry_check_feature_version(registry, "rtpjitterbuffer", GST_VERSION_MAJOR, GST_VERSION_MINOR, GST_VERSION_MICRO))
    {
        g_warning("Missing GStreamer Good Plugins");
        if(error) *error = g_error_new_literal(GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_INIT, "Cannot load plugin");
        return FALSE;
    }

    if(!gst_registry_check_feature_version(registry, "rtpopusdepay", GST_VERSION_MAJOR, GST_VERSION_MINOR, GST_VERSION_MICRO) ||
       !gst_registry_check_feature_version(registry, "opusdec", GST_VERSION_MAJOR, GST_VERSION_MINOR, GST_VERSION_MICRO) ||
       !gst_registry_check_feature_version(registry, "opusenc", GST_VERSION_MAJOR, GST_VERSION_MINOR, GST_VERSION_MICRO) ||
       !gst_registry_check_feature_version(registry, "rtpopuspay", GST_VERSION_MAJOR, GST_VERSION_MINOR, GST_VERSION_MICRO))
    {
        g_warning("Missing Opus plugin for GStreamer");
        if(error) *error = g_error_new_literal(GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_INIT, "Cannot load plugin");
        return FALSE;
    }

    if(!gst_plugin_register_static(GST_VERSION_MAJOR, GST_VERSION_MINOR, "rtpcrypto", "RTP encryption/decryption",
            plugin_init, PACKAGE_VERSION, "GPL", PACKAGE_TARNAME, PACKAGE_NAME, PACKAGE_URL))
    {
        if(error) *error = g_error_new_literal(GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_INIT, "Cannot register plugin");
        return FALSE;
    }

    app->completions = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_STRING);
    if(aliases_path)
    {
        // Load aliases from file
        g_autoptr(GIOChannel) channel = g_io_channel_new_file(aliases_path, "r", error);
        if(!channel)
        {
            g_warning("Failed to read aliases from %s", aliases_path);
            return FALSE;
        }

        while(1)
        {
            gsize len = 0;
            g_autofree gchar *str = NULL;
            GIOStatus status = g_io_channel_read_line(channel, &str, &len, NULL, error);
            if((status == G_IO_STATUS_EOF) || (status == G_IO_STATUS_ERROR)) break;
            if((status == G_IO_STATUS_NORMAL) && (len > DHT_ID_LENGTH))
            {
                // Strip whitespace
                gchar *start = str + DHT_ID_LENGTH, *end = str + len - 1;
                while(g_ascii_isspace(*start)) start++;
                while(g_ascii_isspace(*end)) end--;
                if(start <= end)
                {
                    g_autofree gchar *alias = g_strndup(start, end - start + 1);
                    str[DHT_ID_LENGTH] = 0;

                    GtkTreeIter iter;
                    gtk_list_store_append(app->completions, &iter);
                    gtk_list_store_set(app->completions, &iter, 0, str, 1, alias, -1);
                }
            }
        }
    }

    g_autoptr(GBytes) key = NULL;
    if(key_path)
    {
        // Load key from file
        g_autoptr(GFile) file = g_file_new_for_path(key_path);

        gsize len = DHT_KEY_SIZE;
        guint8 buffer[len];
        g_autoptr(GInputStream) stream = G_INPUT_STREAM(g_file_read(file, NULL, error));
        if(!stream || !g_input_stream_read_all(stream, buffer, sizeof(buffer), &len, NULL, error))
        {
            g_warning("Failed to read key from %s", key_path);
            return FALSE;
        }

        key = g_bytes_new(buffer, len);
    }

    // Create client
    app->client = dht_client_new(ipv6 ? G_SOCKET_FAMILY_IPV6 : G_SOCKET_FAMILY_IPV4, local_port, key, error);
    if(!app->client) return FALSE;

    g_autofree gchar *id = NULL;
    g_object_get(app->client, "id", &id, NULL);
    g_info("%s Initialized", id);

    // Connect signals and bootstrap
    g_signal_connect(app->client, "accept-connection", (GCallback)accept_connection, app);
    g_signal_connect(app->client, "new-connection", (GCallback)new_connection, app);
    g_signal_connect(app->client, "on-error", (GCallback)on_error, app);
    if(bootstrap_host && bootstrap_port && !dht_client_bootstrap(app->client, bootstrap_host, bootstrap_port, error))
    {
        g_warning("Failed to bootstrap %s. %s", bootstrap_host, (*error)->message);
        g_clear_error(error);
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

    // Status icon
    GtkWidget *menu_quit = gtk_menu_item_new_with_label("Quit");
    g_signal_connect(menu_quit, "activate", (GCallback)gtk_main_quit, NULL);

    app->menu = gtk_menu_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(app->menu), menu_quit);
    gtk_widget_show_all(app->menu);

    app->status_icon = gtk_status_icon_new_from_icon_name("call-start-symbolic");
    g_signal_connect(app->status_icon, "popup-menu", (GCallback)menu_popup, app);
    g_signal_connect(app->status_icon, "activate", (GCallback)window_toggle, app);
    g_object_set(app->status_icon, "tooltip-text", "Nanotalk", "title", "Nanotalk", NULL);

    return TRUE;
}

static void application_cleanup(Application *app)
{
    if(app->button_stop && gtk_widget_is_sensitive(app->button_stop))
        call_stop(app->button_stop, app);

    if(app->window) gtk_widget_destroy(app->window);
    if(app->menu) gtk_widget_destroy(app->menu);
    if(app->status_icon) g_object_unref(app->status_icon);
    if(app->completions) g_object_unref(app->completions);
    if(app->client) g_object_unref(app->client);

    g_free(app->sound_file);
    memset(app, 0, sizeof(Application));
}

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(Application, application_cleanup);

int main(int argc, char **argv)
{
    g_autoptr(GError) error = NULL;
    g_auto(Application) application = { };
    if(!application_init(&application, &argc, &argv, &error))
    {
        g_critical("%s", error->message);
        return 1;
    }

    gtk_main();
    return 0;
}
