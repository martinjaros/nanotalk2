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

#include "dhtclient.h"
#include "server.h"

#define DEFAULT_PORT 5004
#define REPORT_PERIOD 10000 // 10 seconds

#define FORMAT_TIME_DHMS(x) x/86400000000,x/3600000000%24,x/60000000%60,x/1000000%60

static gboolean print_report(DhtClient *client)
{
    g_autoptr(GSocketAddress) sockaddr = NULL;
    g_autofree gchar *id = NULL;
    guint peers = 0;
    gint64 uptime = 0, reception_time = 0;
    guint64 packets_received = 0, packets_sent = 0;
    guint64 bytes_received = 0, bytes_sent = 0;
    g_object_get(client,
            "id", &id,
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
    g_print("\x1b[2J\x1b[0;0H"
            "--- Nanotalk server report ---\n"
            "ID:                %s\n"
            "Public address:    [%s]:%hu\n"
            "Alive peers:       %u\n"
            "Total uptime:      %lu days, %lu hours, %lu minutes, %lu seconds\n"
            "Reception time:    %lu days, %lu hours, %lu minutes, %lu seconds\n"
            "Received:          %s (%lu packets)\n"
            "Sent:              %s (%lu packets)\n",
            id, address, port, peers, FORMAT_TIME_DHMS(uptime), FORMAT_TIME_DHMS(reception_time),
            bytes_received_str, packets_received, bytes_sent_str, packets_sent);

    return G_SOURCE_CONTINUE;
}

int main(int argc, char **argv)
{
    GError *error = NULL;
    GBytes *key = g_bytes_new_static(server_key, server_key_len);
    DhtClient *client = dht_client_new(G_SOCKET_FAMILY_IPV4, DEFAULT_PORT, key, &error);
    if(!client)
    {
        g_printerr("%s\n", error->message);
        return 1;
    }

    g_timeout_add(REPORT_PERIOD, (GSourceFunc)print_report, client);
    g_main_loop_run(g_main_loop_new(NULL, FALSE));
    return 0;
}
