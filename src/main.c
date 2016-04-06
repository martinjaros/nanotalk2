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

#include <stdlib.h>
#include "application.h"

#define DEFAULT_PORT 5004

#define REPORT_PERIOD 30 // seconds
#define FORMAT_TIME_DHMS(x) x/86400000000,x/3600000000%24,x/60000000%60,x/1000000%60

// Periodical status reporting
static gboolean print_report(DhtClient *client)
{
    g_autoptr(GSocketAddress) sockaddr = NULL;
    guint peers = 0;
    gint64 uptime = 0, reception_time = 0;
    guint64 packets_received = 0, packets_sent = 0;
    guint64 bytes_received = 0, bytes_sent = 0;
    g_object_get(client,
            "public-address", &sockaddr,
            "peers", &peers,
            "uptime", &uptime,
            "reception-time", &reception_time,
            "packets-received", &packets_received,
            "packets-sent", &packets_sent,
            "bytes-received", &bytes_received,
            "bytes-sent", &bytes_sent,
            NULL);

    g_autofree gchar *bytes_received_str = g_format_size(bytes_received);
    g_autofree gchar *bytes_sent_str = g_format_size(bytes_sent);
    g_autofree gchar *address = g_inet_address_to_string(g_inet_socket_address_get_address(G_INET_SOCKET_ADDRESS(sockaddr)));
    guint16 port = g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(sockaddr));
    g_info("Status report\n"
           "Public address: [%s]:%hu\n"
           "Alive peers:    %u\n"
           "Total uptime:   %lu days, %lu hours, %lu minutes, %lu seconds\n"
           "Reception time: %lu days, %lu hours, %lu minutes, %lu seconds\n"
           "Received:       %s (%lu packets)\n"
           "Sent:           %s (%lu packets)\n",
           address, port, peers, FORMAT_TIME_DHMS(uptime), FORMAT_TIME_DHMS(reception_time),
           bytes_received_str, packets_received, bytes_sent_str, packets_sent);

    return G_SOURCE_CONTINUE;
}

static gboolean startup(int *argc, char ***argv, Application **app, GError **error)
{
    g_autoptr(GBytes) key = NULL;
    g_autofree gchar *key_path = NULL;
    g_autofree gchar *aliases_path = NULL;
    g_autofree gchar *bootstrap_host = NULL;
    g_autofree gchar *sound_file = NULL;
    gint bootstrap_port = DEFAULT_PORT;
    gint local_port = DEFAULT_PORT;
    gboolean ipv6 = FALSE;
    gboolean version = FALSE;
    GOptionEntry options[] =
    {
        { "key", 'k', 0, G_OPTION_ARG_FILENAME, &key_path, "Private key", "FILE" },
        { "aliases", 'a', 0, G_OPTION_ARG_FILENAME, &aliases_path, "List of aliases", "FILE" },
        { "local-port", 'l', 0, G_OPTION_ARG_INT, &local_port, "Source port (default " G_STRINGIFY(DEFAULT_PORT) ")", "NUM" },
        { "ipv6", '6', 0, G_OPTION_ARG_NONE, &ipv6, "Enable IPv6", NULL },
        { "bootstrap-host", 'h', 0, G_OPTION_ARG_STRING, &bootstrap_host, "Bootstrap address", "ADDR" },
        { "bootstrap-port", 'p', 0, G_OPTION_ARG_INT, &bootstrap_port, "Bootstrap port", "NUM" },
        { "call-sound", 's', 0, G_OPTION_ARG_STRING, &sound_file, "Incoming call sound", "FILE" },
        { "version", 'V', 0, G_OPTION_ARG_NONE, &version, "Print program version", NULL },
        { NULL }
    };

    g_autoptr(GOptionContext) context = g_option_context_new(NULL);
    g_option_context_add_main_entries(context, options, NULL);
    g_option_context_set_summary(context, PACKAGE_STRING);
    g_option_context_set_description(context,
        "To see debug messages run the applications as:\nG_MESSAGES_DEBUG=Nanotalk nanotalk\n\n"
        PACKAGE_BUGREPORT "\n" PACKAGE_URL);

    application_add_option_group(context);
    if(!g_option_context_parse(context, argc, argv, error))
        return FALSE;

    if(version)
    {
        // Print program version
        g_print(PACKAGE_TARNAME " " PACKAGE_VERSION "\n");
        exit(EXIT_SUCCESS);
    }

    if(!application_init(error))
        return FALSE;

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
    g_autoptr(DhtClient) client = dht_client_new(ipv6 ? G_SOCKET_FAMILY_IPV6 : G_SOCKET_FAMILY_IPV4, local_port, key, error);
    if(!client) return FALSE;

    if(bootstrap_host && bootstrap_port && !dht_client_bootstrap(client, bootstrap_host, bootstrap_port, error))
        return FALSE;

    g_autofree gchar *id = NULL;
    g_object_get(client, "id", &id, NULL);
    g_message("Client ID %s", id);

    g_timeout_add_seconds(REPORT_PERIOD, (GSourceFunc)print_report, client);
    *app = application_new(client, aliases_path, sound_file);

    return TRUE;
}

int main(int argc, char **argv)
{
    GError *error = NULL;
    Application *app = NULL;
    if(!startup(&argc, &argv, &app, &error))
    {
        g_printerr("%s\n", error->message);
        return EXIT_FAILURE;
    }

    application_run();
    application_free(app);
    return EXIT_SUCCESS;
}
