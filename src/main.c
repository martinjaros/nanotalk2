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
#define DEFAULT_IPV6 FALSE

typedef struct _application Application;

struct _application
{
    GtkWidget *window, *entry, *button_start, *button_stop, *menu;
    GtkStatusIcon *status_icon;
    GtkListStore *completions;
    DhtClient *client;

    GstElement *rx_pipeline, *tx_pipeline;
    guint rx_watch, tx_watch;
};

static void call_start(GtkWidget *widget, gpointer arg)
{
    Application *app = (Application*)arg;
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
        {
            // Initiate lookup by alias
            g_autoptr(GError) error = NULL;
            if(!dht_client_lookup(app->client, id, &error))
            {
                g_info("%s %s", id, error->message);
                return;
            }

            gtk_widget_set_sensitive(app->button_start, FALSE);
            return;
        }

        valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(app->completions), &iter);
    }

    if(strlen(text) == DHT_ID_LENGTH)
    {
        // Initiate lookup by direct ID
        g_autoptr(GError) error = NULL;
        if(!dht_client_lookup(app->client, text, &error))
        {
            g_info("%s %s", text, error->message);
            return;
        }

        gtk_widget_set_sensitive(app->button_start, FALSE);
    }
}

static void call_stop(GtkWidget *widget, gpointer arg)
{
    Application *app = (Application*)arg;
    gtk_widget_set_sensitive(app->button_start, TRUE);
    gtk_widget_set_sensitive(app->button_stop, FALSE);

    // Cleanup pipelines
    gst_element_set_state(app->tx_pipeline, GST_STATE_NULL);
    gst_element_set_state(app->rx_pipeline, GST_STATE_NULL);
    g_object_unref(app->tx_pipeline);
    g_object_unref(app->rx_pipeline);
    g_source_remove(app->tx_watch);
    g_source_remove(app->rx_watch);
}

static gboolean bus_watch(GstBus *bus, GstMessage *message, gpointer arg)
{
    Application *app = (Application*)arg;
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
            // Handle socket errors
            if(strcmp(GST_OBJECT_NAME(message->src), "rtp_sink") == 0)
                call_stop(app->button_stop, app);

            break;
        }

        case GST_MESSAGE_ELEMENT:
        {
            // Handle timeout
            if(strcmp(gst_structure_get_name(gst_message_get_structure(message)), "GstUDPSrcTimeout") == 0)
                call_stop(app->button_stop, app);

            break;
        }

        default: break;
    }

    return TRUE;
}

static gboolean accept_connection(DhtClient *client, const gchar *id, gpointer arg)
{
    Application *app = (Application*)arg;
    if(gtk_widget_is_sensitive(app->button_start))
    {
        gtk_widget_set_sensitive(app->button_start, FALSE);
        return TRUE;
    }

    return FALSE;
}

static void new_connection(DhtClient *client, const gchar *peer_id,
        GSocket *socket, GSocketAddress *sockaddr, GBytes *enc_key, GBytes *dec_key, gboolean remote, gpointer arg)
{
    Application *app = (Application*)arg;
    gtk_widget_set_sensitive(app->button_stop, TRUE);
    if(remote && !gtk_widget_get_visible(app->window))
    {
        gtk_window_set_urgency_hint(GTK_WINDOW(app->window), TRUE);
        gtk_widget_show_all(app->window);
    }

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
    app->rx_watch = gst_bus_add_watch(bus, bus_watch, app);
    gst_object_unref(bus);

    GstCaps *caps = gst_caps_new_simple("application/x-rtp",
        "media", G_TYPE_STRING, "audio",
        "clock-rate", G_TYPE_INT, 48000,
        "encoding-name", G_TYPE_STRING, "X-GST-OPUS-DRAFT-SPITTKA-00",
        NULL);

    GstElement *rtp_src = gst_element_factory_make("udpsrc", "rtp_src"); g_assert(rtp_src);
    g_object_set(rtp_src, "caps", caps, "socket", socket, "timeout", 1000000000L, NULL);
    gst_caps_unref(caps);

    GstElement *rtp_dec = gst_element_factory_make("rtpdecrypt", "rtp_dec"); g_assert(rtp_dec);
    g_object_set(rtp_dec, "key", dec_key, NULL);

    GstElement *rtp_buf = gst_element_factory_make("rtpjitterbuffer", "rtp_buf"); g_assert(rtp_buf);
    GstElement *audio_depay = gst_element_factory_make("rtpopusdepay", "audio_depay"); g_assert(audio_depay);
    GstElement *audio_dec = gst_element_factory_make("opusdec", "audio_dec"); g_assert(audio_dec);
    GstElement *audio_sink = gst_element_factory_make("autoaudiosink", "audio_sink"); g_assert(audio_sink);

    gst_bin_add_many(GST_BIN(app->rx_pipeline), rtp_src, rtp_dec, rtp_buf, audio_depay, audio_dec, audio_sink, NULL);
    gst_element_link_many(rtp_src, rtp_dec, rtp_buf, audio_depay, audio_dec, audio_sink, NULL);
    gst_element_set_state(app->rx_pipeline, GST_STATE_PLAYING);

    // Start transmitter
    app->tx_pipeline = gst_pipeline_new("tx_pipeline");
    bus = gst_pipeline_get_bus(GST_PIPELINE(app->tx_pipeline));
    app->tx_watch = gst_bus_add_watch(bus, bus_watch, app);
    gst_object_unref(bus);

    GstElement *audio_src = gst_element_factory_make("autoaudiosrc", "audio_src"); g_assert(audio_src);
    GstElement *audio_enc = gst_element_factory_make("opusenc", "audio_enc"); g_assert(audio_enc);
    GstElement *audio_pay = gst_element_factory_make("rtpopuspay", "audio_pay"); g_assert(audio_pay);

    GstElement *rtp_enc = gst_element_factory_make("rtpencrypt", "rtp_enc"); g_assert(rtp_enc);
    g_object_set(rtp_enc, "key", enc_key, NULL);

    GstElement *rtp_sink = gst_element_factory_make("udpsink", "rtp_sink"); g_assert(rtp_sink);
    g_object_set(rtp_sink, "socket", sink_socket, "host", host, "port", port, NULL);

    gst_bin_add_many(GST_BIN(app->tx_pipeline), audio_src, audio_enc, audio_pay, rtp_enc, rtp_sink, NULL);
    gst_element_link_many(audio_src, audio_enc, audio_pay, rtp_enc, rtp_sink, NULL);
    gst_element_set_state(app->tx_pipeline, GST_STATE_PLAYING);
}

static void on_error(DhtClient *client, const gchar *id, GError *error, gpointer arg)
{
    Application *app = (Application*)arg;
    gtk_widget_set_sensitive(app->button_start, TRUE);
    g_info("%s %s", id, error->message);
}

static void activate_entry(GtkWidget *widget, gpointer arg)
{
    Application *app = (Application*)arg;
    if(gtk_widget_is_sensitive(app->button_start))
        call_start(widget, arg);
    else if(gtk_widget_is_sensitive(app->button_stop))
        call_stop(widget, arg);
}

static void activate_icon(GtkWidget *widget, gpointer arg)
{
    Application *app = (Application*)arg;
    if(!gtk_widget_get_visible(app->window))
        gtk_widget_show_all(app->window);
    else
        gtk_widget_hide(app->window);
}

static void popup_menu(GtkWidget *widget, guint button, guint activate_time, gpointer arg)
{
    Application *app = (Application*)arg;
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
    gboolean ipv6 = DEFAULT_IPV6;

    GOptionEntry options[] =
    {
        { "key", 'k', 0, G_OPTION_ARG_FILENAME, &key_path, "Private key", "FILE" },
        { "aliases", 'a', 0, G_OPTION_ARG_FILENAME, &aliases_path, "List of aliases", "FILE" },
        { "local-port", 'l', 0, G_OPTION_ARG_INT, &local_port, "Source port (default " G_STRINGIFY(DEFAULT_PORT) ")", "NUM" },
        { "ipv6", '6', 0, G_OPTION_ARG_NONE, &ipv6, "Enable IPv6", NULL },
        { "bootstrap-host", 'h', 0, G_OPTION_ARG_STRING, &bootstrap_host, "Bootstrap address", "ADDR" },
        { "bootstrap-port", 'p', 0, G_OPTION_ARG_INT, &bootstrap_port, "Bootstrap port", "NUM" },
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
    g_signal_connect(app->entry, "activate", (GCallback)activate_entry, app);
    app->button_start = gtk_button_new_from_icon_name("call-start", GTK_ICON_SIZE_BUTTON);
    g_signal_connect(app->button_start, "clicked", (GCallback)call_start, app);
    app->button_stop = gtk_button_new_from_icon_name("call-stop", GTK_ICON_SIZE_BUTTON);
    g_signal_connect(app->button_stop, "clicked", (GCallback)call_stop, app);
    gtk_widget_set_sensitive(app->button_stop, FALSE);

    // Completion
    GtkEntryCompletion *completion = gtk_entry_completion_new();
    gtk_entry_completion_set_model(completion, GTK_TREE_MODEL(app->completions));
    gtk_entry_completion_set_inline_completion(completion, TRUE);
    gtk_entry_completion_set_inline_selection(completion, TRUE);
    gtk_entry_completion_set_text_column(completion, 1);
    gtk_entry_set_completion(GTK_ENTRY(app->entry), completion);
    g_object_unref(completion);

    // Geometry
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 5);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 5);
    gtk_grid_attach(GTK_GRID(grid), app->entry, 0, 0, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), app->button_start, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), app->button_stop, 1, 1, 1, 1);
    g_object_set(grid, "margin", 10, NULL);

    // Main window
    app->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    g_signal_connect(app->window, "delete-event", (GCallback)gtk_widget_hide_on_delete, NULL);
    gtk_window_set_icon_name(GTK_WINDOW(app->window), "call-start-symbolic");
    gtk_window_set_title(GTK_WINDOW(app->window), "Nanotalk");
    gtk_window_set_resizable(GTK_WINDOW(app->window), FALSE);
    gtk_container_add(GTK_CONTAINER(app->window), GTK_WIDGET(grid));

    // Status icon menu
    GtkWidget *menu_quit = gtk_menu_item_new_with_label("Quit");
    g_signal_connect(menu_quit, "activate", (GCallback)gtk_main_quit, NULL);

    app->menu = gtk_menu_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(app->menu), menu_quit);
    gtk_widget_show_all(app->menu);

    app->status_icon = gtk_status_icon_new_from_icon_name("call-start-symbolic");
    g_signal_connect(app->status_icon, "popup-menu", (GCallback)popup_menu, app);
    g_signal_connect(app->status_icon, "activate", (GCallback)activate_icon, app);
    gtk_status_icon_set_tooltip_text(app->status_icon, "Nanotalk");
    gtk_status_icon_set_title(app->status_icon, "Nanotalk");

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
