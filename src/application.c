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

#include <gtk/gtk.h>
#include <gst/gst.h>
#include <glib/gi18n.h>
#include "application.h"
#include "rtp-session.h"

#define DEFAULT_BITRATE 64000
#define DEFAULT_ENABLE_VBR FALSE

typedef struct _Application Application;

struct _Application
{
    GtkWidget *main_window, *main_entry, *button_start, *button_volume, *button_stop, *call_dialog;
    GtkWidget *config_window, *label_peers, *spin_local_port, *entry_bootstrap_host, *spin_bootstrap_port, *spin_bitrate, *switch_vbr;
    GtkWidget *editor_window, *status_menu;
    GtkStatusIcon *status_icon;

    GtkTextBuffer *aliases_buffer;
    GtkListStore *aliases_list;
    GHashTable *alias2id_table; // <string, DhtId>
    GHashTable *id2alias_table; // <DhtId, string>

    DhtClient *client;
    GKeyFile *config;

    RtpSession *session;
};

static void call_stop(Application *app);

static void lookup_finished_cb(DhtClient *client, GAsyncResult *result, Application *app)
{
    GError *error = NULL;
    DhtKey enc_key, dec_key;
    g_autoptr(GSocket) socket = NULL;
    if(dht_client_lookup_finish(client, result, &socket, &enc_key, &dec_key, &error))
    {
        app->session = rtp_session_new(socket, &enc_key, &dec_key);
        g_signal_connect_swapped(app->session, "hangup", (GCallback)call_stop, app);

        guint bitrate = g_key_file_get_integer(app->config, "audio", "bitrate", NULL);
        gboolean enable_vbr = g_key_file_get_boolean(app->config, "audio", "enable_vbr", NULL);
        rtp_session_set_bitrate(app->session, bitrate, enable_vbr);

        rtp_session_bind_volume(app->session, app->button_volume, "value");
        rtp_session_play(app->session);

        gtk_widget_set_sensitive(app->button_stop, TRUE);
    }

    if(error)
    {
        g_message("%s", error->message);
        g_clear_error(&error);

        gtk_widget_set_sensitive(app->button_start, TRUE);
        g_object_set(app->client, "listen", TRUE, NULL);
        return;
    }
}

static void call_start(Application *app)
{
    const gchar *text = gtk_entry_get_text(GTK_ENTRY(app->main_entry));
    DhtId tmp, *id = g_hash_table_lookup(app->alias2id_table, text);
    if(!id && dht_id_from_string(&tmp, text)) id = &tmp;

    if(id)
    {
        dht_client_lookup_async(app->client, id, (GAsyncReadyCallback)lookup_finished_cb, app);
        gtk_widget_set_sensitive(app->button_start, FALSE);
        g_object_set(app->client, "listen", FALSE, NULL);
    }
}

static void call_stop(Application *app)
{
    if(app->call_dialog)
    {
        gtk_dialog_response(GTK_DIALOG(app->call_dialog), GTK_RESPONSE_CANCEL);
        return;
    }

    if(app->session)
    {
        rtp_session_destroy(app->session);
        app->session = NULL;
    }

    gtk_widget_set_sensitive(app->button_start, TRUE);
    gtk_widget_set_sensitive(app->button_stop, FALSE);
    g_object_set(app->client, "listen", TRUE, NULL);
}

static void call_toggle(Application *app)
{
    if(gtk_widget_is_sensitive(app->button_start))
        call_start(app);
    else if(gtk_widget_is_sensitive(app->button_stop))
        call_stop(app);
}

static gboolean dialog_run(Application *app)
{
    app->call_dialog = gtk_message_dialog_new_with_markup(GTK_WINDOW(app->main_window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
        _("Answer an incoming call from <b>%s</b> ?"), gtk_entry_get_text(GTK_ENTRY(app->main_entry)));

    gtk_window_set_urgency_hint(GTK_WINDOW(app->main_window), TRUE);
    gint response = gtk_dialog_run(GTK_DIALOG(app->call_dialog));
    gtk_window_set_urgency_hint(GTK_WINDOW(app->main_window), FALSE);
    gtk_widget_destroy(app->call_dialog);
    app->call_dialog = NULL;

    if(response == GTK_RESPONSE_YES)
        rtp_session_set_tone(app->session, FALSE);
    else
        call_stop(app);

    return G_SOURCE_REMOVE;
}

static void new_connection(Application *app, DhtId *id, GSocket *socket, DhtKey *enc_key, DhtKey *dec_key)
{
    const gchar *alias = g_hash_table_lookup(app->id2alias_table, id);
    if(alias) gtk_entry_set_text(GTK_ENTRY(app->main_entry), alias);
    else
    {
        g_autofree gchar *tmp = dht_id_to_string(id);
        gtk_entry_set_text(GTK_ENTRY(app->main_entry), tmp);
    }

    app->session = rtp_session_new(socket, enc_key, dec_key);
    g_signal_connect_swapped(app->session, "hangup", (GCallback)call_stop, app);

    guint bitrate = g_key_file_get_integer(app->config, "audio", "bitrate", NULL);
    gboolean enable_vbr = g_key_file_get_boolean(app->config, "audio", "enable_vbr", NULL);
    rtp_session_set_bitrate(app->session, bitrate, enable_vbr);

    rtp_session_bind_volume(app->session, app->button_volume, "value");
    rtp_session_set_tone(app->session, TRUE);
    rtp_session_play(app->session);

    g_idle_add((GSourceFunc)dialog_run, app);
    gtk_widget_set_sensitive(app->button_start, FALSE);
    gtk_widget_set_sensitive(app->button_stop, TRUE);
    if(!gtk_widget_get_visible(app->main_window))
        gtk_widget_show(app->main_window);
}

static void completion_update(Application *app)
{
    GtkTextIter start;
    gtk_text_buffer_get_start_iter(app->aliases_buffer, &start);

    GtkTextIter end = start;
    while(!gtk_text_iter_is_end(&start))
    {
        gtk_text_iter_forward_line(&end);

        GtkTextIter mark = start;
        gtk_text_iter_forward_chars(&mark, ((4 * DHT_ID_SIZE / 3) + 3) & ~3);
        if(gtk_text_iter_compare(&mark, &end) < 0)
        {
            DhtId id;
            g_autofree gchar *id_string = gtk_text_iter_get_text(&start, &mark);
            if(dht_id_from_string(&id, id_string))
            {
                g_autofree gchar *alias = gtk_text_iter_get_text(&mark, &end);
                g_strstrip(alias);

                if(alias[0] != 0)
                {
                    GtkTreeIter iter;
                    gtk_list_store_append(app->aliases_list, &iter);
                    gtk_list_store_set(app->aliases_list, &iter, 0, alias, -1);
                    g_hash_table_insert(app->alias2id_table, g_strdup(alias), dht_key_copy(&id));
                    g_hash_table_insert(app->id2alias_table, dht_key_copy(&id), g_strdup(alias));
                }
            }
        }

        start = end;
    }

    // Ensure trailing newline
    gtk_text_iter_backward_char(&start);
    if(gtk_text_iter_get_char(&start) != '\n')
        gtk_text_buffer_insert(app->aliases_buffer, &end, "\n", 1);
}

static void editor_save(Application *app)
{
    if(gtk_text_buffer_get_modified(app->aliases_buffer))
    {
        gtk_text_buffer_set_modified(app->aliases_buffer, FALSE);
        gtk_list_store_clear(app->aliases_list);
        g_hash_table_remove_all(app->alias2id_table);
        g_hash_table_remove_all(app->id2alias_table);
        completion_update(app);

        GtkTextIter start, end;
        gtk_text_buffer_get_start_iter(app->aliases_buffer, &start);
        gtk_text_buffer_get_end_iter(app->aliases_buffer, &end);
        g_autofree gchar *aliases_data = gtk_text_iter_get_slice(&start, &end);

        GError *error = NULL;
        g_autofree gchar *base_path = g_build_filename(g_get_home_dir(), ".nanotalk", NULL);
        g_autofree gchar *aliases_path = g_build_filename(base_path, "aliases.txt", NULL);
        g_file_set_contents(aliases_path, aliases_data, -1, &error);
        if(error)
        {
            g_message("%s", error->message);
            g_clear_error(&error);
        }
    }

    gtk_widget_hide(app->editor_window);
}

static void editor_show(Application *app, GtkEntryIconPosition icon_pos, GdkEvent *event)
{
    if(app->editor_window)
    {
        gtk_widget_show(app->editor_window);
        return;
    }

    app->editor_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    g_signal_connect(app->editor_window, "delete-event", (GCallback)gtk_widget_hide_on_delete, NULL);
    g_object_set(app->editor_window, "icon-name", "accessories-text-editor", "title", _("Nanotalk aliases"), NULL);
    gtk_window_set_transient_for(GTK_WINDOW(app->editor_window), GTK_WINDOW(app->main_window));
    gtk_window_set_destroy_with_parent(GTK_WINDOW(app->editor_window), TRUE);
    gtk_window_set_type_hint(GTK_WINDOW(app->editor_window), GDK_WINDOW_TYPE_HINT_DIALOG);
    gtk_window_set_default_size(GTK_WINDOW(app->editor_window), 500, 300);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(app->editor_window), vbox);

    GtkWidget *scrolledwindow = gtk_scrolled_window_new(NULL, NULL);
    gtk_box_pack_start(GTK_BOX(vbox), scrolledwindow, TRUE, TRUE, 0);

    GtkWidget *textview = gtk_text_view_new_with_buffer(app->aliases_buffer);
    gtk_container_add(GTK_CONTAINER(scrolledwindow), textview);

    PangoFontDescription *font = pango_font_description_from_string("monospace");
    gtk_widget_override_font(textview, font);
    pango_font_description_free(font);

    GtkWidget *hbox = gtk_button_box_new(GTK_ORIENTATION_HORIZONTAL);
    g_object_set(hbox, "layout-style", GTK_BUTTONBOX_END, "margin", 5, NULL);
    gtk_box_pack_end(GTK_BOX(vbox), hbox, FALSE, TRUE, 0);

    GtkWidget *button = gtk_button_new_with_label(_("Save changes"));
    g_signal_connect_swapped(button, "clicked", (GCallback)editor_save, app);
    gtk_container_add(GTK_CONTAINER(hbox), button);

    gtk_widget_show_all(app->editor_window);
}

static void config_apply(Application *app)
{
    GError *error = NULL;
    gboolean need_bootstrap = FALSE;
    guint16 local_port = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(app->spin_local_port));
    if(local_port != g_key_file_get_integer(app->config, "network", "local-port", NULL))
    {
        gboolean listen = TRUE;
        g_autoptr(DhtKey) key = NULL;
        g_object_get(app->client, "key", &key, "listen", &listen, NULL);
        DhtClient *client = dht_client_new(key);

        g_autoptr(GInetAddress) inaddr_any = g_inet_address_new_any(DHT_ADDRESS_FAMILY);
        g_autoptr(GSocketAddress) address = g_inet_socket_address_new(inaddr_any, local_port);
        if(dht_client_bind(client, address, FALSE, &error))
        {
            g_object_set(client, "listen", listen, NULL);
            g_signal_connect_swapped(client, "new-connection", (GCallback)new_connection, app);
            g_object_bind_property(client, "peers", app->label_peers, "label", G_BINDING_SYNC_CREATE);

            g_object_unref(app->client);
            app->client = client;

            need_bootstrap = TRUE;
            g_key_file_set_integer(app->config, "network", "local-port", local_port);
        }

        if(error)
        {
            g_message("%s", error->message);
            g_clear_error(&error);

            g_clear_object(&client);
        }
    }

    const gchar* bootstrap_host = gtk_entry_get_text(GTK_ENTRY(app->entry_bootstrap_host));
    guint16 bootstrap_port = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(app->spin_bootstrap_port));
    g_autofree gchar *prev_bootstrap_host = g_key_file_get_string(app->config, "network", "bootstrap-host", NULL);
    guint16 prev_bootstrap_port = g_key_file_get_integer(app->config, "network", "bootstrap-port", NULL);
    if(need_bootstrap || (g_strcmp0(bootstrap_host, prev_bootstrap_host) != 0) || (bootstrap_port != prev_bootstrap_port))
    {
        if(bootstrap_host[0] && bootstrap_port)
        {
            g_autoptr(GResolver) resolver = g_resolver_get_default();
            GList *list = g_resolver_lookup_by_name(resolver, bootstrap_host, NULL, &error);
            if(list)
            {
                g_autoptr(GSocketAddress) address = g_inet_socket_address_new(list->data, bootstrap_port);
                dht_client_bootstrap(app->client, address);
                g_resolver_free_addresses(list);
            }

            if(error)
            {
                g_message("%s", error->message);
                g_clear_error(&error);
            }
        }

        g_key_file_set_string(app->config, "network", "bootstrap-host", bootstrap_host);
        g_key_file_set_integer(app->config, "network", "bootstrap-port", bootstrap_port);
    }

    guint bitrate = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(app->spin_bitrate));
    gboolean enable_vbr = gtk_switch_get_active(GTK_SWITCH(app->switch_vbr));
    g_key_file_set_integer(app->config, "audio", "bitrate", bitrate);
    g_key_file_set_boolean(app->config, "audio", "enable-vbr", enable_vbr);
    if(app->session) rtp_session_set_bitrate(app->session, bitrate, enable_vbr);

    g_autofree gchar *base_path = g_build_filename(g_get_home_dir(), ".nanotalk", NULL);
    g_autofree gchar *config_path = g_build_filename(base_path, "user.cfg", NULL);
    g_key_file_save_to_file(app->config, config_path, &error);
    if(error)
    {
        g_message("%s", error->message);
        g_clear_error(&error);
    }
}

static void config_show(Application *app)
{
    if(app->config_window)
    {
        gtk_widget_show(app->config_window);
        return;
    }

    app->config_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    g_signal_connect(app->config_window, "delete-event", (GCallback)gtk_widget_hide_on_delete, NULL);
    g_object_set(app->config_window, "icon-name", "preferences-desktop", "title", _("Nanotalk configuration"), "resizable", FALSE, NULL);
    gtk_window_set_transient_for(GTK_WINDOW(app->config_window), GTK_WINDOW(app->main_window));
    gtk_window_set_destroy_with_parent(GTK_WINDOW(app->config_window), TRUE);
    gtk_window_set_type_hint(GTK_WINDOW(app->config_window), GDK_WINDOW_TYPE_HINT_DIALOG);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_add(GTK_CONTAINER(app->config_window), vbox);

    GtkWidget *label, *seperator;
    GtkWidget *grid = gtk_grid_new();
    g_object_set(grid, "row-homogeneous", TRUE, "column-spacing", 15, "row-spacing", 5, "margin", 10, NULL);
    gtk_container_add(GTK_CONTAINER(vbox), grid);

    g_autoptr(DhtId) id = NULL;
    g_object_get(app->client, "id", &id, NULL);
    g_autofree gchar *id_string = dht_id_to_string(id);
    g_autofree gchar *id_markup = g_strconcat("<b>", id_string, "</b>", NULL);

    label = gtk_label_new(_("Client ID"));
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), label, 0, 0, 1, 1);
    label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(label), id_markup);
    gtk_label_set_selectable(GTK_LABEL(label), TRUE);
    gtk_widget_set_can_focus(label, FALSE);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_grid_attach(GTK_GRID(grid), label, 1, 0, 1, 1);

    PangoFontDescription *font = pango_font_description_from_string("monospace");
    gtk_widget_override_font(label, font);
    pango_font_description_free(font);

    label = gtk_label_new(_("Number of peers"));
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), label, 0, 1, 1, 1);
    app->label_peers = gtk_label_new(NULL);
    g_object_bind_property(app->client, "peers", app->label_peers, "label", G_BINDING_SYNC_CREATE);
    gtk_grid_attach(GTK_GRID(grid), app->label_peers, 1, 1, 1, 1);

    seperator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_grid_attach(GTK_GRID(grid), seperator, 0, 2, 2, 1);

    guint16 local_port = g_key_file_get_integer(app->config, "network", "local-port", NULL);
    g_autofree gchar *bootstrap_host = g_key_file_get_string(app->config, "network", "bootstrap-host", NULL);
    guint16 bootstrap_port = g_key_file_get_integer(app->config, "network", "bootstrap-port", NULL);

    label = gtk_label_new(_("Local port"));
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), label, 0, 3, 1, 1);
    app->spin_local_port = gtk_spin_button_new_with_range(0, G_MAXUINT16, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(app->spin_local_port), local_port);
    gtk_grid_attach(GTK_GRID(grid), app->spin_local_port, 1, 3, 1, 1);
    gtk_widget_set_tooltip_text(app->spin_local_port,
    		_("Local port to which the client is bound, this port needs to be forwarded on your router"));

    label = gtk_label_new(_("Bootstrap host"));
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), label, 0, 4, 1, 1);
    app->entry_bootstrap_host = gtk_entry_new();
    gtk_entry_set_width_chars(GTK_ENTRY(app->entry_bootstrap_host), 30);
    gtk_entry_set_text(GTK_ENTRY(app->entry_bootstrap_host), bootstrap_host ?: "");
    gtk_grid_attach(GTK_GRID(grid), app->entry_bootstrap_host, 1, 4, 1, 1);
    gtk_widget_set_tooltip_text(app->entry_bootstrap_host,
    		_("Hostname or IP address used to join the network"));

    label = gtk_label_new(_("Bootstrap port"));
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), label, 0, 5, 1, 1);
    app->spin_bootstrap_port = gtk_spin_button_new_with_range(0, G_MAXUINT16, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(app->spin_bootstrap_port), bootstrap_port);
    gtk_grid_attach(GTK_GRID(grid), app->spin_bootstrap_port, 1, 5, 1, 1);
    gtk_widget_set_tooltip_text(app->spin_bootstrap_port,
    		_("Port number of the boostrap host"));

    seperator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_grid_attach(GTK_GRID(grid), seperator, 0, 6, 2, 1);

    gboolean enable_vbr = g_key_file_get_boolean(app->config, "audio", "enable-vbr", NULL);
    guint bitrate = g_key_file_get_integer(app->config, "audio", "bitrate", NULL);

    label = gtk_label_new(_("Audio bitrate"));
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), label, 0, 7, 1, 1);
    app->spin_bitrate = gtk_spin_button_new_with_range(4000, 650000, 1000);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(app->spin_bitrate), bitrate);
    gtk_grid_attach(GTK_GRID(grid), app->spin_bitrate, 1, 7, 1, 1);
    gtk_widget_set_tooltip_text(app->spin_bitrate,
    		_("Target bitrate of the audio encoder"));

    label = gtk_label_new(_("Enable VBR"));
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), label, 0, 8, 1, 1);
    app->switch_vbr = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(app->switch_vbr), enable_vbr);
    gtk_widget_set_hexpand(app->switch_vbr, TRUE);
    gtk_widget_set_halign(app->switch_vbr, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), app->switch_vbr, 1, 8, 1, 1);
    gtk_widget_set_tooltip_text(app->switch_vbr,
    		_("Enables variable bitrate encoding"));

    GtkWidget *button;
    GtkWidget *hbox = gtk_button_box_new(GTK_ORIENTATION_HORIZONTAL);
    g_object_set(hbox, "layout-style", GTK_BUTTONBOX_END, "spacing", 5, "margin", 5, NULL);
    gtk_container_add(GTK_CONTAINER(vbox), hbox);

    button = gtk_button_new_with_label(_("Apply"));
    g_signal_connect_swapped(button, "clicked", (GCallback)config_apply, app);
    gtk_container_add(GTK_CONTAINER(hbox), button);

    button = gtk_button_new_with_label(_("Close"));
    g_signal_connect_swapped(button, "clicked", (GCallback)gtk_widget_hide, app->config_window);
    gtk_container_add(GTK_CONTAINER(hbox), button);

    gtk_widget_show_all(app->config_window);
}

static void about_show(Application *app)
{
    const gchar *authors[] = { "Martin Jaro\xC5\xA1 <xjaros32@stud.feec.vutbr.cz>", NULL };
    gtk_show_about_dialog(GTK_WINDOW(app->main_window),
        "logo-icon-name", "call-start-symbolic",
        "program-name", "nanotalk",
        "version", VERSION,
        "comments", _("Nanotalk distributed voice client"),
        "authors", authors,
        "license-type", GTK_LICENSE_GPL_2_0,
        "website", "https://github.com/martinjaros/nanotalk2",
        NULL);
}

static void menu_popup(Application *app, guint button, guint activate_time)
{
    if(!app->status_menu)
    {
        GtkWidget *menu_item, *image;
        app->status_menu = gtk_menu_new();

        image = gtk_image_new_from_icon_name("preferences-desktop", GTK_ICON_SIZE_MENU);
        menu_item = gtk_image_menu_item_new_with_label(_("Preferences"));
        g_signal_connect_swapped(menu_item, "activate", (GCallback)config_show, app);
        gtk_image_menu_item_set_image(GTK_IMAGE_MENU_ITEM(menu_item), image);
        gtk_menu_shell_append(GTK_MENU_SHELL(app->status_menu), menu_item);

        image = gtk_image_new_from_icon_name("help-about", GTK_ICON_SIZE_MENU);
        menu_item = gtk_image_menu_item_new_with_label(_("About"));
        g_signal_connect_swapped(menu_item, "activate", (GCallback)about_show, app);
        gtk_image_menu_item_set_image(GTK_IMAGE_MENU_ITEM(menu_item), image);
        gtk_menu_shell_append(GTK_MENU_SHELL(app->status_menu), menu_item);

        image = gtk_image_new_from_icon_name("application-exit", GTK_ICON_SIZE_MENU);
        menu_item = gtk_image_menu_item_new_with_label(_("Quit"));
        g_signal_connect(menu_item, "activate", (GCallback)gtk_main_quit, NULL);
        gtk_image_menu_item_set_image(GTK_IMAGE_MENU_ITEM(menu_item), image);
        gtk_menu_shell_append(GTK_MENU_SHELL(app->status_menu), menu_item);

        gtk_widget_show_all(app->status_menu);
    }

    gtk_menu_popup(GTK_MENU(app->status_menu), NULL, NULL, gtk_status_icon_position_menu, app->status_icon, button, activate_time);
}

static void window_toggle(Application *app)
{
    if(!gtk_widget_get_visible(app->main_window))
        gtk_widget_show(app->main_window);
    else
        gtk_widget_hide(app->main_window);
}

static void application_startup(Application *app)
{
    g_object_set(app->client, "listen", TRUE, NULL);
    g_signal_connect_swapped(app->client, "new-connection", (GCallback)new_connection, app);

    app->aliases_buffer = gtk_text_buffer_new(NULL);
    app->aliases_list = gtk_list_store_new(1, G_TYPE_STRING);
    app->alias2id_table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, dht_id_free);
    app->id2alias_table = g_hash_table_new_full(dht_id_hash, dht_id_equal, dht_id_free, g_free);

    // Load aliases
    gsize aliases_len = 0;
    g_autofree gchar *aliases_data = NULL;
    g_autofree gchar *base_path = g_build_filename(g_get_home_dir(), ".nanotalk", NULL);
    g_autofree gchar *aliases_path = g_build_filename(base_path, "aliases.txt", NULL);
    if(g_file_get_contents(aliases_path, &aliases_data, &aliases_len, NULL))
    {
        gtk_text_buffer_set_text(app->aliases_buffer, aliases_data, aliases_len);
        gtk_text_buffer_set_modified(app->aliases_buffer, FALSE);
        completion_update(app);
    }

    // Create widgets
    app->main_entry = gtk_entry_new();
    g_signal_connect_swapped(app->main_entry, "activate", (GCallback)call_toggle, app);
    g_signal_connect_swapped(app->main_entry, "icon-press", (GCallback)editor_show, app);
    g_object_set(app->main_entry, "secondary-icon-name", "address-book-new", "secondary-icon-tooltip-text", _("Edit aliases"), NULL);
    gtk_entry_set_width_chars(GTK_ENTRY(app->main_entry), 30);

    app->button_start = gtk_button_new_from_icon_name("call-start", GTK_ICON_SIZE_BUTTON);
    g_signal_connect_swapped(app->button_start, "clicked", (GCallback)call_start, app);
    gtk_widget_set_can_focus(app->button_start, FALSE);
    gtk_widget_set_hexpand(app->button_start, TRUE);

    app->button_volume = gtk_volume_button_new();
    gtk_scale_button_set_value(GTK_SCALE_BUTTON(app->button_volume), 1.0);
    gtk_widget_set_can_focus(app->button_volume, FALSE);

    app->button_stop = gtk_button_new_from_icon_name("call-stop", GTK_ICON_SIZE_BUTTON);
    g_signal_connect_swapped(app->button_stop, "clicked", (GCallback)call_stop, app);
    gtk_widget_set_can_focus(app->button_stop, FALSE);
    gtk_widget_set_hexpand(app->button_stop, TRUE);
    gtk_widget_set_sensitive(app->button_stop, FALSE);

    // Completion
    GtkEntryCompletion *completion = gtk_entry_completion_new();
    g_object_set(completion, "model", app->aliases_list, "inline-completion", TRUE, "inline-selection", TRUE, NULL);
    gtk_entry_completion_set_text_column(completion, 0);
    gtk_entry_set_completion(GTK_ENTRY(app->main_entry), completion);
    g_object_unref(completion);

    // Geometry
    GtkWidget *grid = gtk_grid_new();
    g_object_set(grid, "column-spacing", 5, "row-spacing", 5, "margin", 10, NULL);
    gtk_grid_attach(GTK_GRID(grid), app->main_entry, 0, 0, 3, 1);
    gtk_grid_attach(GTK_GRID(grid), app->button_start, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), app->button_volume, 1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), app->button_stop, 2, 1, 1, 1);

    // Main window
    app->main_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    g_signal_connect(app->main_window, "delete-event", (GCallback)gtk_widget_hide_on_delete, NULL);
    g_object_set(app->main_window, "icon-name", "call-start-symbolic", "title", "Nanotalk", "resizable", FALSE, NULL);
    gtk_container_add(GTK_CONTAINER(app->main_window), grid);
    gtk_widget_show_all(GTK_WIDGET(app->main_window));

    app->status_icon = gtk_status_icon_new_from_icon_name("call-start-symbolic");
    g_signal_connect_swapped(app->status_icon, "popup-menu", (GCallback)menu_popup, app);
    g_signal_connect_swapped(app->status_icon, "activate", (GCallback)window_toggle, app);
    g_object_set(app->status_icon, "tooltip-text", "Nanotalk", "title", "Nanotalk", NULL);
    gtk_status_icon_set_visible(app->status_icon, FALSE);
    gtk_status_icon_set_visible(app->status_icon, TRUE);
}

void application_run(DhtClient *client, GKeyFile *config)
{
    if(!g_key_file_has_group(config, "audio"))
    {
        g_key_file_set_integer(config, "audio", "bitrate", DEFAULT_BITRATE);
        g_key_file_set_boolean(config, "audio", "enable-vbr", DEFAULT_ENABLE_VBR);
    }

    Application *app = &(Application){0};
    app->config = config;
    app->client = client;

    application_startup(app);
    gtk_main();

    // Cleanup
    if(app->session) rtp_session_destroy(app->session);
    if(app->config_window) gtk_widget_destroy(app->config_window);
    if(app->editor_window) gtk_widget_destroy(app->editor_window);
    if(app->status_menu) gtk_widget_destroy(app->status_menu);

    gtk_widget_destroy(app->main_window);
    g_object_unref(app->status_icon);

    g_object_unref(app->aliases_buffer);
    g_object_unref(app->aliases_list);
    g_hash_table_destroy(app->alias2id_table);
    g_hash_table_destroy(app->id2alias_table);

    g_key_file_unref(config);
    g_object_unref(client);
}

void application_init(gint argc, gchar *argv[])
{
    gtk_init(&argc, &argv);
    gst_init(&argc, &argv);
}
