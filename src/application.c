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
#define GDK_DISABLE_DEPRECATION_WARNINGS

#include <string.h>
#include <gtk/gtk.h>
#include <gst/gst.h>
#include <glib/gi18n.h>
#include "application.h"

#include "tonegen.h"
#include "rtpencrypt.h"
#include "rtpdecrypt.h"

#define RECV_TIMEOUT 3000000000L // 3 seconds

struct _application
{
    GtkWidget *main_window, *entry, *button_start, *button_volume, *button_stop, *call_dialog;
    GtkWidget *config_window, *label_id, *label_received, *label_sent, *label_peers;
    GtkWidget *switch_ipv6, *spin_local_port, *entry_bootstrap_host, *spin_bootstrap_port;
    GtkWidget *switch_echo, *spin_bitrate, *spin_latency;
    GtkWidget *menu, *editor_window;
    GtkStatusIcon *status_icon;
    GtkListStore *completions;
    GtkTextBuffer *aliases;
    DhtClient *client;

    GstElement *rx_pipeline, *tx_pipeline;
    guint rx_watch, tx_watch;

    GKeyFile *config;
    gchar *config_file;
    gchar *aliases_file;
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
    if((strlen(text) == DHT_ID_LENGTH) && (text[DHT_ID_LENGTH - 1] == '='))
    {
        lookup_start(app, text);
        return;
    }

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
            lookup_start(app, id);
            return;
        }

        valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(app->completions), &iter);
    }
}

static void call_stop(GtkWidget *widget, Application *app)
{
    if(app->call_dialog)
    {
        gtk_dialog_response(GTK_DIALOG(app->call_dialog), GTK_RESPONSE_CANCEL);
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
        call_start(NULL, app);
    else if(gtk_widget_is_sensitive(app->button_stop))
        call_stop(NULL, app);
}

static void volume_changed(GtkWidget *widget, gdouble value, Application *app)
{
    if(app->rx_pipeline)
    {
        GstElement *volume = gst_bin_get_by_name(GST_BIN(app->rx_pipeline), "volume");
        g_object_set(volume, "volume", value, NULL);
        gst_object_unref(volume);
    }
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
            call_stop(NULL, app);
            break;
        }

        case GST_MESSAGE_WARNING:
        {
            g_autoptr(GError) error = NULL;
            g_autofree gchar *debug = NULL;
            gst_message_parse_warning(message, &error, &debug);
            g_warning("%s %s", error->message, debug);
            break;
        }

        case GST_MESSAGE_ELEMENT:
        {
            // Stop pipeline on socket timeout
            if(strcmp(gst_structure_get_name(gst_message_get_structure(message)), "GstRtpTimeout") == 0)
                call_stop(NULL, app);

            break;
        }

        default: break;
    }

    return TRUE;
}

static gboolean dialog_run(Application *app)
{
    app->call_dialog = gtk_message_dialog_new_with_markup(GTK_WINDOW(app->main_window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
        _("Answer an incoming call from <b>%s</b> ?"), gtk_entry_get_text(GTK_ENTRY(app->entry)));

    // Show dialog
    gtk_window_set_urgency_hint(GTK_WINDOW(app->main_window), TRUE);
    gint response = gtk_dialog_run(GTK_DIALOG(app->call_dialog));
    gtk_window_set_urgency_hint(GTK_WINDOW(app->main_window), FALSE);
    gtk_widget_destroy(app->call_dialog);
    app->call_dialog = NULL;

    if(response == GTK_RESPONSE_YES)
    {
        GstElement *volume = gst_bin_get_by_name(GST_BIN(app->rx_pipeline), "volume");
        g_object_set(volume, "mute", FALSE, NULL);
        gst_object_unref(volume);

        GstElement *tonegen = gst_bin_get_by_name(GST_BIN(app->tx_pipeline), "tonegen");
        g_object_set(tonegen, "enabled", FALSE, NULL);
        gst_object_unref(tonegen);
    }
    else call_stop(NULL, app);

    return G_SOURCE_REMOVE;
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
    if(remote)
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
    }

    GstElement *audio_src = NULL;
    GstElement *audio_sink = NULL;
    if(g_key_file_get_boolean(app->config, "audio", "echo-cancel", NULL))
    {
        // PulseAudio setup
        audio_src = gst_element_factory_make("pulsesrc", "audio_src");
        audio_sink = gst_element_factory_make("pulsesink", "audio_sink");
        if(audio_src && audio_sink)
        {
            GstStructure *props = gst_structure_new("props",
                "media.role", G_TYPE_STRING, "phone",
                "filter.want", G_TYPE_STRING, "echo-cancel",
                NULL);

            g_object_set(audio_src, "stream-properties", props, NULL);
            g_object_set(audio_sink, "stream-properties", props, NULL);
            gst_structure_free(props);
        }
        else g_warning("Echo cancellation is supported only for PulseAudio");
    }

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
    g_object_set(rtp_src, "caps", caps, "socket", socket, NULL);
    gst_caps_unref(caps);

    GstElement *rtp_dec = gst_element_factory_make("rtpdecrypt", "rtp_dec");
    g_object_set(rtp_dec, "key", dec_key, "timeout", RECV_TIMEOUT, NULL);

    GstElement *rtp_buf = gst_element_factory_make("rtpjitterbuffer", "rtp_buf");

    g_autofree gchar *latency = g_key_file_get_value(app->config, "audio", "latency", NULL);
    if(latency) gst_util_set_object_arg(G_OBJECT(rtp_buf), "latency", latency);

    GstElement *audio_depay = gst_element_factory_make("rtpopusdepay", "audio_depay");
    GstElement *audio_dec = gst_element_factory_make("opusdec", "audio_dec");
    GstElement *volume = gst_element_factory_make("volume", "volume");
    g_object_set(volume, "mute", remote, "volume", gtk_scale_button_get_value(GTK_SCALE_BUTTON(app->button_volume)), NULL);

    if(!audio_sink) audio_sink = gst_element_factory_make("autoaudiosink", "audio_sink");

    gst_bin_add_many(GST_BIN(app->rx_pipeline), rtp_src, rtp_dec, rtp_buf, audio_depay, audio_dec, volume, audio_sink, NULL);
    gst_element_link_many(rtp_src, rtp_dec, rtp_buf, audio_depay, audio_dec, volume, audio_sink, NULL);
    gst_debug_bin_to_dot_file_with_ts(GST_BIN(app->rx_pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "start-rx-pipeline");
    gst_element_set_state(app->rx_pipeline, GST_STATE_PLAYING);

    // Start transmitter
    app->tx_pipeline = gst_pipeline_new("tx_pipeline");
    bus = gst_pipeline_get_bus(GST_PIPELINE(app->tx_pipeline));
    app->tx_watch = gst_bus_add_watch(bus, (GstBusFunc)bus_watch, app);
    gst_object_unref(bus);

    if(!audio_src) audio_src = gst_element_factory_make("autoaudiosrc", "audio_src");

    GstElement *tonegen = gst_element_factory_make("tonegen", "tonegen");
    g_object_set(tonegen, "enabled", remote, NULL);

    GstElement *audio_enc = gst_element_factory_make("opusenc", "audio_enc");
    gst_util_set_object_arg(G_OBJECT(audio_enc), "audio-type", "voice");
    gst_util_set_object_arg(G_OBJECT(audio_enc), "bitrate-type", "constrained-vbr");

    g_autofree gchar *bitrate = g_key_file_get_value(app->config, "audio", "bitrate", NULL);
    if(bitrate) gst_util_set_object_arg(G_OBJECT(audio_enc), "bitrate", bitrate);

    GstElement *audio_pay = gst_element_factory_make("rtpopuspay", "audio_pay");
    GstElement *rtp_enc = gst_element_factory_make("rtpencrypt", "rtp_enc");
    g_object_set(rtp_enc, "key", enc_key, NULL);

    g_autofree gchar *host = g_inet_address_to_string(g_inet_socket_address_get_address(G_INET_SOCKET_ADDRESS(sockaddr)));
    gint port = g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(sockaddr));

    GstElement *rtp_sink = gst_element_factory_make("udpsink", "rtp_sink");
    g_object_set(rtp_sink, "socket", socket, "close-socket", FALSE, "host", host, "port", port, NULL);

    gst_bin_add_many(GST_BIN(app->tx_pipeline), audio_src, tonegen, audio_enc, audio_pay, rtp_enc, rtp_sink, NULL);
    gst_element_link_many(audio_src, tonegen, audio_enc, audio_pay, rtp_enc, rtp_sink, NULL);
    gst_debug_bin_to_dot_file_with_ts(GST_BIN(app->tx_pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "start-tx-pipeline");
    gst_element_set_state(app->tx_pipeline, GST_STATE_PLAYING);

    // Show windows
    if(remote) g_idle_add((GSourceFunc)dialog_run, app);
    gtk_widget_set_sensitive(app->button_stop, TRUE);
    if(!gtk_widget_get_visible(app->main_window))
        gtk_widget_show(app->main_window);
}

static void on_error(DhtClient *client, const gchar *id, GError *error, Application *app)
{
    g_message("%s %s", id, error->message);
    call_stop(NULL, app);
}

static void window_toggle(GtkWidget *widget, Application *app)
{
    if(!gtk_widget_get_visible(app->main_window))
        gtk_widget_show(app->main_window);
    else
        gtk_widget_hide(app->main_window);
}

static void completion_update(Application *app)
{
    GtkTextIter start;
    gtk_text_buffer_get_start_iter(app->aliases, &start);

    GtkTextIter end = start;
    while(!gtk_text_iter_is_end(&start))
    {
        gtk_text_iter_forward_line(&end);

        GtkTextIter mark = start;
        gtk_text_iter_forward_chars(&mark, DHT_ID_LENGTH);
        if(gtk_text_iter_compare(&mark, &end) < 0)
        {
            g_autofree gchar *id = gtk_text_iter_get_text(&start, &mark);
            g_autofree gchar *alias = gtk_text_iter_get_text(&mark, &end);
            g_strstrip(alias);

            if((id[DHT_ID_LENGTH - 1] == '=') && (strlen(alias) > 0))
            {
                GtkTreeIter iter;
                gtk_list_store_append(app->completions, &iter);
                gtk_list_store_set(app->completions, &iter, 0, id, 1, alias, -1);
            }
        }

        start = end;
    }

    gtk_text_iter_backward_char(&start);
    if(gtk_text_iter_get_char(&start) != '\n') // ensure trailing newline
        gtk_text_buffer_insert(app->aliases, &end, "\n", 1);
}

static void editor_save(GtkWidget *widget, Application *app)
{
    if(gtk_text_buffer_get_modified(app->aliases))
    {
        gtk_list_store_clear(app->completions);
        completion_update(app);

        GtkTextIter start, end;
        gtk_text_buffer_get_start_iter(app->aliases, &start);
        gtk_text_buffer_get_end_iter(app->aliases, &end);

        g_autoptr(GError) error = NULL;
        g_autofree gchar *aliases_data = gtk_text_iter_get_slice(&start, &end);
        if(g_file_set_contents(app->aliases_file, aliases_data, -1, &error))
            gtk_text_buffer_set_modified(app->aliases, FALSE);
        else
            g_warning("%s", error->message);
    }

    gtk_widget_hide(app->editor_window);
}

static void editor_show(GtkWidget *widget, GtkEntryIconPosition icon_pos, GdkEvent *event, Application *app)
{
    if(app->editor_window)
    {
        if(!gtk_widget_is_visible(app->editor_window))
            gtk_widget_show(app->editor_window);

        return;
    }

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    GtkWidget *textview = gtk_text_view_new_with_buffer(app->aliases);
    PangoFontDescription *font = pango_font_description_from_string("monospace");
    gtk_widget_override_font(textview, font);
    pango_font_description_free(font);

    GtkWidget *scrolledwindow = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(scrolledwindow), textview);
    gtk_box_pack_start(GTK_BOX(vbox), scrolledwindow, TRUE, TRUE, 0);

    GtkWidget *button = gtk_button_new_with_label(_("Save changes"));
    g_signal_connect(button, "clicked", (GCallback)editor_save, app);

    GtkWidget *hbox = gtk_button_box_new(GTK_ORIENTATION_HORIZONTAL);
    g_object_set(hbox, "layout-style", GTK_BUTTONBOX_END, "margin", 5, NULL);
    gtk_container_add(GTK_CONTAINER(hbox), button);
    gtk_box_pack_end(GTK_BOX(vbox), hbox, FALSE, TRUE, 0);

    app->editor_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    g_signal_connect(app->editor_window, "delete-event", (GCallback)gtk_widget_hide_on_delete, NULL);
    g_object_set(app->editor_window, "icon-name", "accessories-text-editor", "title", _("Nanotalk aliases"), NULL);
    gtk_window_set_transient_for(GTK_WINDOW(app->editor_window), GTK_WINDOW(app->main_window));
    gtk_window_set_destroy_with_parent(GTK_WINDOW(app->editor_window), TRUE);
    gtk_window_set_type_hint(GTK_WINDOW(app->editor_window), GDK_WINDOW_TYPE_HINT_DIALOG);
    gtk_window_set_default_size(GTK_WINDOW(app->editor_window), 500, 300);
    gtk_container_add(GTK_CONTAINER(app->editor_window), vbox);
    gtk_widget_show_all(app->editor_window);
}

static gboolean status_update(Application *app)
{
    if(gtk_widget_is_visible(app->config_window))
    {
        guint64 bytes_received = 0, packets_received = 0;
        guint64 bytes_sent = 0, packets_sent = 0;
        guint peers = 0;

        g_object_get(app->client,
            "bytes-received", &bytes_received,
            "packets-received", &packets_received,
            "bytes-sent", &bytes_sent,
            "packets-sent", &packets_sent,
            "peers", &peers,
            NULL);

        g_autofree gchar *received_str = g_strdup_printf("%s (%lu %s)",
                g_format_size(bytes_received), packets_received, ngettext("packet", "packets", packets_received));

        g_autofree gchar *sent_str = g_strdup_printf("%s (%lu %s)",
                g_format_size(bytes_sent), packets_sent, ngettext("packet", "packets", packets_sent));

        g_autofree gchar *peers_str = g_strdup_printf("%u", peers);

        gtk_label_set_text(GTK_LABEL(app->label_received), received_str);
        gtk_label_set_text(GTK_LABEL(app->label_sent), sent_str);
        gtk_label_set_text(GTK_LABEL(app->label_peers), peers_str);
        return G_SOURCE_CONTINUE;
    }

    return G_SOURCE_REMOVE;
}

static void config_apply(GtkWidget *widget, Application *app)
{
    g_autoptr(GError) error = NULL;

    // Network
    gboolean enable_ipv6 = gtk_switch_get_active(GTK_SWITCH(app->switch_ipv6));
    guint16 local_port = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(app->spin_local_port));
    const gchar* bootstrap_host = gtk_entry_get_text(GTK_ENTRY(app->entry_bootstrap_host));
    guint16 bootstrap_port = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(app->spin_bootstrap_port));

    if((enable_ipv6 != g_key_file_get_boolean(app->config, "network", "enable-ipv6", NULL)) ||
       (local_port != g_key_file_get_integer(app->config, "network", "local-port", NULL)))
    {
        g_autoptr(GBytes) key = NULL;
        g_object_get(app->client, "key", &key, NULL);
        g_object_unref(app->client);

        // Create new client
        app->client = dht_client_new(enable_ipv6 ? G_SOCKET_FAMILY_IPV6 : G_SOCKET_FAMILY_IPV4, local_port, key, &error);
        if(!app->client) g_error("%s", error->message);

        g_signal_connect(app->client, "accept-connection", (GCallback)accept_connection, app);
        g_signal_connect(app->client, "new-connection", (GCallback)new_connection, app);
        g_signal_connect(app->client, "on-error", (GCallback)on_error, app);
    }

    g_autofree gchar *prev_bootstrap_host = g_key_file_get_string(app->config, "network", "bootstrap-host", NULL);
    guint16 prev_bootstrap_port = g_key_file_get_integer(app->config, "network", "bootstrap-port", NULL);
    if((g_strcmp0(bootstrap_host, prev_bootstrap_host) != 0) || (bootstrap_port != prev_bootstrap_port))
        dht_client_bootstrap(app->client, bootstrap_host, bootstrap_port);

    // Audio
    gboolean echo_cancel = gtk_switch_get_active(GTK_SWITCH(app->switch_echo));
    guint bitrate = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(app->spin_bitrate));
    guint latency = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(app->spin_latency));

    if(app->tx_pipeline)
    {
        GstElement *audio_enc = gst_bin_get_by_name(GST_BIN(app->tx_pipeline), "audio_enc");
        g_object_set(audio_enc, "bitrate", bitrate, NULL);
        gst_object_unref(audio_enc);
    }

    // Save configuration
    g_key_file_set_boolean(app->config, "network", "enable-ipv6", enable_ipv6);
    g_key_file_set_integer(app->config, "network", "local-port", local_port);
    g_key_file_set_string(app->config, "network", "bootstrap-host", bootstrap_host);
    g_key_file_set_integer(app->config, "network", "bootstrap-port", bootstrap_port);

    g_key_file_set_boolean(app->config, "audio", "echo-cancel", echo_cancel);
    g_key_file_set_integer(app->config, "audio", "bitrate", bitrate);
    g_key_file_set_integer(app->config, "audio", "latency", latency);

    if(!g_key_file_save_to_file(app->config, app->config_file, &error))
        g_warning("%s", error->message);
}

static void config_show(GtkWidget *widget, Application *app)
{
    if(app->config_window)
    {
        if(!gtk_widget_is_visible(app->config_window))
        {
            gtk_widget_show(app->config_window);
            g_timeout_add_seconds(1, (GSourceFunc)status_update, app);
            status_update(app);
        }

        return;
    }

    app->config_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    g_signal_connect(app->config_window, "delete-event", (GCallback)gtk_widget_hide_on_delete, NULL);
    g_object_set(app->config_window, "icon-name", "preferences-desktop", "title", _("Nanotalk configuration"), "resizable", FALSE, NULL);
    gtk_window_set_transient_for(GTK_WINDOW(app->config_window), GTK_WINDOW(app->main_window));
    gtk_window_set_destroy_with_parent(GTK_WINDOW(app->config_window), TRUE);
    gtk_window_set_type_hint(GTK_WINDOW(app->config_window), GDK_WINDOW_TYPE_HINT_DIALOG);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(app->config_window), vbox);
    GtkWidget *notebook = gtk_notebook_new();
    gtk_container_add(GTK_CONTAINER(vbox), notebook);

    // Status page
    GtkWidget *grid = gtk_grid_new();
    g_object_set(grid, "column-spacing", 10, "row-spacing", 5, "margin", 10, NULL);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), grid, gtk_label_new(_("Status")));

    g_autofree gchar *id = NULL;
    g_object_get(app->client, "id", &id, NULL);
    g_autofree gchar *id_markup = g_strconcat("<b>", id, "</b>", NULL);

    GtkWidget *label = gtk_label_new(_("Client ID"));
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), label, 0, 0, 1, 1);
    app->label_id = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(app->label_id), id_markup);
    gtk_label_set_selectable(GTK_LABEL(app->label_id), TRUE);
    gtk_widget_set_can_focus(app->label_id, FALSE);
    gtk_widget_set_hexpand(app->label_id, TRUE);
    gtk_grid_attach(GTK_GRID(grid), app->label_id, 1, 0, 1, 1);

    label = gtk_label_new(_("Received"));
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), label, 0, 1, 1, 1);
    app->label_received = gtk_label_new(NULL);
    gtk_grid_attach(GTK_GRID(grid), app->label_received, 1, 1, 1, 1);

    label = gtk_label_new(_("Sent"));
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), label, 0, 2, 1, 1);
    app->label_sent = gtk_label_new(NULL);
    gtk_grid_attach(GTK_GRID(grid), app->label_sent, 1, 2, 1, 1);

    label = gtk_label_new(_("Peers"));
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), label, 0, 3, 1, 1);
    app->label_peers = gtk_label_new(NULL);
    gtk_grid_attach(GTK_GRID(grid), app->label_peers, 1, 3, 1, 1);

    // Network page
    grid = gtk_grid_new();
    g_object_set(grid, "column-spacing", 10, "row-spacing", 5, "margin", 10, NULL);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), grid, gtk_label_new(_("Network")));

    gboolean enable_ipv6 = g_key_file_get_boolean(app->config, "network", "enable-ipv6", NULL);
    guint16 local_port = g_key_file_get_integer(app->config, "network", "local-port", NULL);
    g_autofree gchar *bootstrap_host = g_key_file_get_string(app->config, "network", "bootstrap-host", NULL);
    guint16 bootstrap_port = g_key_file_get_integer(app->config, "network", "bootstrap-port", NULL);

    label = gtk_label_new(_("Enable IPv6"));
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), label, 0, 0, 1, 1);
    app->switch_ipv6 = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(app->switch_ipv6), enable_ipv6);
    gtk_widget_set_hexpand(app->switch_ipv6, TRUE);
    gtk_widget_set_halign(app->switch_ipv6, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), app->switch_ipv6, 0, 0, 2, 1);

    label = gtk_label_new(_("Local port"));
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), label, 0, 1, 1, 1);
    app->spin_local_port = gtk_spin_button_new_with_range(0, G_MAXUINT16, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(app->spin_local_port), local_port);
    gtk_grid_attach(GTK_GRID(grid), app->spin_local_port, 1, 1, 1, 1);

    label = gtk_label_new(_("Bootstrap host"));
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), label, 0, 2, 1, 1);
    app->entry_bootstrap_host = gtk_entry_new();
    gtk_entry_set_width_chars(GTK_ENTRY(app->entry_bootstrap_host), 30);
    gtk_entry_set_text(GTK_ENTRY(app->entry_bootstrap_host), bootstrap_host ?: "");
    gtk_grid_attach(GTK_GRID(grid), app->entry_bootstrap_host, 1, 2, 1, 1);

    label = gtk_label_new(_("Bootstrap port"));
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), label, 0, 3, 1, 1);
    app->spin_bootstrap_port = gtk_spin_button_new_with_range(0, G_MAXUINT16, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(app->spin_bootstrap_port), bootstrap_port);
    gtk_grid_attach(GTK_GRID(grid), app->spin_bootstrap_port, 1, 3, 1, 1);

    // Audio page
    grid = gtk_grid_new();
    g_object_set(grid, "column-spacing", 10, "row-spacing", 5, "margin", 10, NULL);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), grid, gtk_label_new(_("Audio")));

    gboolean echo_cancel = g_key_file_get_boolean(app->config, "audio", "echo-cancel", NULL);
    guint bitrate = g_key_file_get_integer(app->config, "audio", "bitrate", NULL);
    guint latency = g_key_file_get_integer(app->config, "audio", "latency", NULL);

    label = gtk_label_new(_("Echo cancellation"));
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), label, 0, 0, 1, 1);
    app->switch_echo = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(app->switch_echo), echo_cancel);
    gtk_widget_set_hexpand(app->switch_echo, TRUE);
    gtk_widget_set_halign(app->switch_echo, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), app->switch_echo, 1, 0, 2, 1);

    label = gtk_label_new(_("Bitrate"));
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), label, 0, 1, 1, 1);
    app->spin_bitrate = gtk_spin_button_new_with_range(4000, 650000, 1000);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(app->spin_bitrate), bitrate);
    gtk_grid_attach(GTK_GRID(grid), app->spin_bitrate, 1, 1, 2, 1);

    label = gtk_label_new(_("Latency"));
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), label, 0, 2, 1, 1);
    app->spin_latency = gtk_spin_button_new_with_range(0, 1000, 10);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(app->spin_latency), latency);
    gtk_grid_attach(GTK_GRID(grid), app->spin_latency, 1, 2, 2, 1);

    // Button box
    GtkWidget *hbox = gtk_button_box_new(GTK_ORIENTATION_HORIZONTAL);
    g_object_set(hbox, "layout-style", GTK_BUTTONBOX_END, "spacing", 5, "margin", 5, NULL);
    gtk_container_add(GTK_CONTAINER(vbox), hbox);
    GtkWidget *button = gtk_button_new_with_label(_("Apply"));
    g_signal_connect(button, "clicked", (GCallback)config_apply, app);
    gtk_container_add(GTK_CONTAINER(hbox), button);

    button = gtk_button_new_with_label(_("Close"));
    g_signal_connect_swapped(button, "clicked", (GCallback)gtk_widget_hide, app->config_window);
    gtk_container_add(GTK_CONTAINER(hbox), button);

    gtk_widget_show_all(app->config_window);
    g_timeout_add_seconds(1, (GSourceFunc)status_update, app);
    status_update(app);
}

static void about_show(GtkWidget *widget, Application *app)
{
    const gchar *authors[] = { "Martin Jaro\xC5\xA1 <xjaros32@stud.feec.vutbr.cz>", NULL };
    gtk_show_about_dialog(GTK_WINDOW(app->main_window),
        "logo-icon-name", "call-start-symbolic",
        "program-name", "nanotalk",
        "version", PACKAGE_VERSION,
        "comments", _("Nanotalk distributed voice client"),
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
    return gst_element_register(plugin, "tonegen", GST_RANK_NONE, GST_TYPE_TONEGEN) &&
           gst_element_register(plugin, "rtpencrypt", GST_RANK_NONE, GST_TYPE_RTP_ENCRYPT) &&
           gst_element_register(plugin, "rtpdecrypt", GST_RANK_NONE, GST_TYPE_RTP_DECRYPT);
}

static gboolean plugin_register(GError **error)
{
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

    return TRUE;
}

void application_add_option_group(GOptionContext *context)
{
    g_option_context_add_group(context, gtk_get_option_group(TRUE));
    g_option_context_add_group(context, gst_init_get_option_group());
}

Application* application_new(DhtClient *client, GKeyFile *config, const gchar *config_file, const gchar *aliases_file, GError **error)
{
    static gboolean initialized = FALSE;
    if(!initialized)
    {
        if(!plugin_register(error)) return NULL;
        initialized = TRUE;
    }

    Application *app = g_new0(Application, 1);
    app->config = g_key_file_ref(config);
    app->config_file = g_strdup(config_file);
    app->aliases_file = g_strdup(aliases_file);

    app->client = g_object_ref(client);
    g_signal_connect(app->client, "accept-connection", (GCallback)accept_connection, app);
    g_signal_connect(app->client, "new-connection", (GCallback)new_connection, app);
    g_signal_connect(app->client, "on-error", (GCallback)on_error, app);

    // Load aliases from file
    gsize aliases_len = 0;
    g_autofree gchar *aliases_data = NULL;
    app->aliases = gtk_text_buffer_new(NULL);
    app->completions = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_STRING);
    if(g_file_get_contents(aliases_file, &aliases_data, &aliases_len, NULL))
    {
        gtk_text_buffer_set_text(app->aliases, aliases_data, aliases_len);
        gtk_text_buffer_set_modified(app->aliases, FALSE);
        completion_update(app);
    }

    // Create widgets
    app->entry = gtk_entry_new();
    g_signal_connect(app->entry, "activate", (GCallback)call_toggle, app);
    g_signal_connect(app->entry, "icon-press", (GCallback)editor_show, app);
    gtk_entry_set_width_chars(GTK_ENTRY(app->entry), 30);
    g_object_set(app->entry, "secondary-icon-name", "address-book-new", "secondary-icon-tooltip-text", "Edit aliases", NULL);

    app->button_start = gtk_button_new_from_icon_name("call-start", GTK_ICON_SIZE_BUTTON);
    g_signal_connect(app->button_start, "clicked", (GCallback)call_start, app);
    gtk_widget_set_can_focus(app->button_start, FALSE);
    gtk_widget_set_hexpand(app->button_start, TRUE);

    app->button_volume = gtk_volume_button_new();
    g_signal_connect(app->button_volume, "value-changed", (GCallback)volume_changed, app);
    gtk_scale_button_set_value(GTK_SCALE_BUTTON(app->button_volume), 1.0);
    gtk_widget_set_can_focus(app->button_volume, FALSE);

    app->button_stop = gtk_button_new_from_icon_name("call-stop", GTK_ICON_SIZE_BUTTON);
    g_signal_connect(app->button_stop, "clicked", (GCallback)call_stop, app);
    gtk_widget_set_can_focus(app->button_stop, FALSE);
    gtk_widget_set_hexpand(app->button_stop, TRUE);
    gtk_widget_set_sensitive(app->button_stop, FALSE);

    // Completion
    GtkEntryCompletion *completion = gtk_entry_completion_new();
    g_object_set(completion, "model", app->completions, "inline-completion", TRUE, "inline-selection", TRUE, NULL);
    gtk_entry_completion_set_text_column(completion, 1);
    gtk_entry_set_completion(GTK_ENTRY(app->entry), completion);
    g_object_unref(completion);

    // Geometry
    GtkWidget *grid = gtk_grid_new();
    g_object_set(grid, "column-spacing", 5, "row-spacing", 5, "margin", 10, NULL);
    gtk_grid_attach(GTK_GRID(grid), app->entry, 0, 0, 3, 1);
    gtk_grid_attach(GTK_GRID(grid), app->button_start, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), app->button_volume, 1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), app->button_stop, 2, 1, 1, 1);

    // Main window
    app->main_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    g_signal_connect(app->main_window, "delete-event", (GCallback)gtk_widget_hide_on_delete, NULL);
    g_object_set(app->main_window, "icon-name", "call-start-symbolic", "title", "Nanotalk", "resizable", FALSE, NULL);
    gtk_container_add(GTK_CONTAINER(app->main_window), grid);
    gtk_widget_show_all(GTK_WIDGET(app->main_window));

    // Status icon menu
    GtkWidget *menu_item, *image;
    app->menu = gtk_menu_new();

    image = gtk_image_new_from_icon_name("preferences-desktop", GTK_ICON_SIZE_MENU);
    menu_item = gtk_image_menu_item_new_with_label(_("Preferences"));
    g_signal_connect(menu_item, "activate", (GCallback)config_show, app);
    gtk_image_menu_item_set_image(GTK_IMAGE_MENU_ITEM(menu_item), image);
    gtk_menu_shell_append(GTK_MENU_SHELL(app->menu), menu_item);

    image = gtk_image_new_from_icon_name("help-about", GTK_ICON_SIZE_MENU);
    menu_item = gtk_image_menu_item_new_with_label(_("About"));
    g_signal_connect(menu_item, "activate", (GCallback)about_show, app);
    gtk_image_menu_item_set_image(GTK_IMAGE_MENU_ITEM(menu_item), image);
    gtk_menu_shell_append(GTK_MENU_SHELL(app->menu), menu_item);

    image = gtk_image_new_from_icon_name("application-exit", GTK_ICON_SIZE_MENU);
    menu_item = gtk_image_menu_item_new_with_label(_("Quit"));
    g_signal_connect(menu_item, "activate", (GCallback)gtk_main_quit, app);
    gtk_image_menu_item_set_image(GTK_IMAGE_MENU_ITEM(menu_item), image);
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
        call_stop(NULL, app);

    gtk_widget_destroy(app->main_window);
    gtk_widget_destroy(app->menu);
    g_object_unref(app->status_icon);
    g_object_unref(app->completions);
    g_object_unref(app->aliases);
    g_object_unref(app->client);
    g_key_file_unref(app->config);
    g_free(app->config_file);
    g_free(app->aliases_file);
    g_free(app);
}

void application_run()
{
    gtk_main();
}
