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

#if ENABLE_GUI
#include "application.h"
static Application *application = NULL;
#endif /* ENABLE_GUI */

#include "dhtclient.h"

#define DEFAULT_PORT 5004

#ifdef G_OS_UNIX
#include <glib-unix.h>

// Called for SIGUSR1
static gboolean print_stats(DhtClient *client)
{
    guint peers = 0;
    g_autoptr(GDateTime) last_seen = NULL;
    guint64 bytes_received = 0, packets_received = 0;
    guint64 bytes_sent = 0, packets_sent = 0;
    g_object_get(client,
            "peers", &peers,
            "last-seen", &last_seen,
            "bytes-received", &bytes_received,
            "packets-received", &packets_received,
            "bytes-sent", &bytes_sent,
            "packets-sent", &packets_sent,
            NULL);

    // Print statistics
    g_autofree gchar *last_seen_str = g_date_time_format(last_seen, "%c");
    g_autofree gchar *bytes_received_str = g_format_size(bytes_received);
    g_autofree gchar *bytes_sent_str = g_format_size(bytes_sent);
    g_print("\n"
            "Peers:     %u\n"
            "Last seen: %s\n"
            "Received:  %s (%lu packets)\n"
            "Sent:      %s (%lu packets)\n",
            peers, last_seen_str, bytes_received_str, packets_received, bytes_sent_str, packets_sent);

    return G_SOURCE_CONTINUE;
}

#endif /* G_OS_UNIX */

static gboolean startup(int *argc, char ***argv, GError **error)
{
    g_autoptr(GBytes) key = NULL;
    g_autofree gchar *key_path = NULL;
    g_autofree gchar *bootstrap_host = NULL;
    gint bootstrap_port = DEFAULT_PORT;
    gint local_port = DEFAULT_PORT;
    gboolean ipv6 = FALSE;
    gboolean version = FALSE;

#if ENABLE_GUI
    g_autofree gchar *aliases_path = NULL;
    g_autofree gchar *sound_file = NULL;
#endif /* ENABLE_GUI */

    GOptionEntry options[] =
    {
        { "ipv6", '6', 0, G_OPTION_ARG_NONE, &ipv6, "Enable IPv6", NULL },
        { "key", 'k', 0, G_OPTION_ARG_FILENAME, &key_path, "Private key", "FILE" },
        { "local-port", 'l', 0, G_OPTION_ARG_INT, &local_port, "Source port (default " G_STRINGIFY(DEFAULT_PORT) ")", "NUM" },
        { "bootstrap-host", 'h', 0, G_OPTION_ARG_STRING, &bootstrap_host, "Bootstrap address", "ADDR" },
        { "bootstrap-port", 'p', 0, G_OPTION_ARG_INT, &bootstrap_port, "Bootstrap port", "NUM" },

#if ENABLE_GUI
        { "aliases", 'a', 0, G_OPTION_ARG_FILENAME, &aliases_path, "List of aliases", "FILE" },
        { "call-sound", 's', 0, G_OPTION_ARG_STRING, &sound_file, "Incoming call sound", "FILE" },
#endif /* ENABLE_GUI */

        { "version", 'V', 0, G_OPTION_ARG_NONE, &version, "Print program version", NULL },
        { NULL }
    };

    g_autoptr(GOptionContext) context = g_option_context_new(NULL);
    g_option_context_add_main_entries(context, options, NULL);
    g_option_context_set_summary(context, PACKAGE_STRING);
    g_option_context_set_description(context, PACKAGE_BUGREPORT "\n" PACKAGE_URL);

#if ENABLE_GUI
    application_add_option_group(context);
#endif /* ENABLE_GUI */

    if(!g_option_context_parse(context, argc, argv, error))
        return FALSE;

    if(version)
    {
        // Print program version
        g_print(PACKAGE_TARNAME " " PACKAGE_VERSION "\n");
        exit(EXIT_SUCCESS);
    }

#if ENABLE_GUI
    if(!application_init(error))
        return FALSE;
#endif /* ENABLE_GUI */

    if(key_path)
    {
        // Load key from file
        g_autoptr(GFile) file = g_file_new_for_path(key_path);
        gsize len = DHT_KEY_SIZE;
        guint8 buffer[len];

        g_autoptr(GInputStream) stream = G_INPUT_STREAM(g_file_read(file, NULL, error));
        if(!stream || !g_input_stream_read_all(stream, buffer, sizeof(buffer), &len, NULL, error))
            return FALSE;

        key = g_bytes_new(buffer, len);
    }

    // Create client and bootstrap
    DhtClient *client = dht_client_new(ipv6 ? G_SOCKET_FAMILY_IPV6 : G_SOCKET_FAMILY_IPV4, local_port, key, error);
    if(!client) return FALSE;

    if(bootstrap_host && bootstrap_port && !dht_client_bootstrap(client, bootstrap_host, bootstrap_port, error))
        return FALSE;

    g_autofree gchar *id = NULL;
    g_object_get(client, "id", &id, NULL);
    g_print("Client ID %s\n", id);

#ifdef G_OS_UNIX
    g_unix_signal_add(SIGUSR1, (GSourceFunc)print_stats, client);
#endif /* G_OS_UNIX */

#if ENABLE_GUI
    application = application_new(client, aliases_path, sound_file);
    g_object_unref(client);
#endif /* ENABLE_GUI */

    return TRUE;
}

int main(int argc, char **argv)
{
    GError *error = NULL;
    if(!startup(&argc, &argv, &error))
    {
        g_printerr("%s\n", error->message);
        return EXIT_FAILURE;
    }

#if ENABLE_GUI
    application_run();
    application_free(application);
#else
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);
#endif /* ENABLE_GUI */

    return EXIT_SUCCESS;
}
