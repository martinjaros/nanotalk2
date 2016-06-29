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

#include <locale.h>
#include <glib/gi18n.h>
#include "dht-client.h"

#ifdef ENABLE_GUI
#include "application.h"
#endif /* ENABLE_GUI */

#define DEFAULT_PORT 5004

static DhtClient* startup(GKeyFile *config)
{
    GError *error = NULL;
    g_autofree gchar *base_path = g_build_filename(g_get_home_dir(), ".nanotalk", NULL);
    g_autofree gchar *config_path = g_build_filename(base_path, "user.cfg", NULL);
    g_autofree gchar *key_path = g_build_filename(base_path, "user.key", NULL);
    g_mkdir_with_parents(base_path, 0775);

    // Load configuration
    if(!g_key_file_load_from_file(config, config_path, G_KEY_FILE_KEEP_COMMENTS, NULL))
    {
        g_key_file_set_integer(config, "network", "local-port", DEFAULT_PORT);
        g_key_file_set_string(config,  "network", "bootstrap-host", "");
        g_key_file_set_integer(config, "network", "bootstrap-port", DEFAULT_PORT);
        g_key_file_save_to_file(config, config_path, NULL);
    }

    // Load private key
    gsize key_len = 0;
    g_autofree gchar *key_data = NULL;
    g_file_get_contents(key_path, &key_data, &key_len, NULL);
    if(key_len != DHT_KEY_SIZE)
    {
        key_data = g_realloc(key_data, DHT_KEY_SIZE);
        dht_key_make_random((DhtKey*)key_data);
        g_file_set_contents(key_path, key_data, DHT_KEY_SIZE, NULL);
    }

    // Bind client
    DhtClient *client = dht_client_new((DhtKey*)key_data);
    guint16 local_port = g_key_file_get_integer(config, "network", "local-port", NULL);
    g_autoptr(GInetAddress) inaddr_any = g_inet_address_new_any(DHT_ADDRESS_FAMILY);
    g_autoptr(GSocketAddress) local_address = g_inet_socket_address_new(inaddr_any, local_port);
    if(!dht_client_bind(client, local_address, FALSE, &error))
    {
        g_printerr("%s\n", error->message);
        g_clear_error(&error);
    }

    // Bootstrap
    g_autofree gchar* bootstrap_host = g_key_file_get_string(config, "network", "bootstrap-host", NULL);
    guint16 bootstrap_port = g_key_file_get_integer(config, "network", "bootstrap-port", NULL);
    if(bootstrap_host && bootstrap_host[0] && bootstrap_port)
    {
        g_autoptr(GResolver) resolver = g_resolver_get_default();
        GList *list = g_resolver_lookup_by_name(resolver, bootstrap_host, NULL, &error);
        if(list)
        {
            g_autoptr(GSocketAddress) address = g_inet_socket_address_new(list->data, bootstrap_port);
            dht_client_bootstrap(client, address);
            g_resolver_free_addresses(list);
        }
        else
        {
            g_printerr("%s\n", error->message);
            g_clear_error(&error);
        }
    }

    return client;
}

int main(int argc, char *argv[])
{
    setlocale(LC_ALL, "");
    g_print("Nanotalk " VERSION "\n");

#ifdef ENABLE_NLS
    bindtextdomain(PACKAGE, LOCALEDIR);
    bind_textdomain_codeset(PACKAGE, "UTF-8");
    textdomain(PACKAGE);
#endif /* ENABLE_NLS */

#ifdef ENABLE_GUI
    application_init(argc, argv);
#endif /* ENABLE_GUI */

    GKeyFile *config = g_key_file_new();
    DhtClient *client = startup(config);

#ifdef ENABLE_GUI
    application_run(client, config);
#else
    (void)client;
    g_key_file_unref(config);
    g_main_loop_run(g_main_loop_new(NULL, FALSE));
#endif /* ENABLE_GUI */

    return 0;
}
