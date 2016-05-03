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

#ifdef ENABLE_GUI
#include "application.h"
static Application *application = NULL;
#endif /* ENABLE_GUI */

#include <glib/gi18n.h>
#include <locale.h>
#include <stdlib.h>
#include "dhtclient.h"

#define DEFAULT_IPV6 FALSE
#define DEFAULT_PORT 5004

#define DEFAULT_ECHO_CANCEL     FALSE
#define DEFAULT_BITRATE         64000
#define DEFAULT_LATENCY         200

#define CONFIG_FILE     "user.cfg"
#define KEY_FILE        "user.key"
#define ALIASES_FILE    "aliases.txt"

static gboolean startup(int *argc, char ***argv, GError **error)
{
    gboolean print_version = FALSE;
    g_autofree gchar *config_file = NULL;
    g_autofree gchar *key_file = NULL;
    g_autofree gchar *aliases_file = NULL;
    GOptionEntry options[] =
    {
        { "version", 'v', 0, G_OPTION_ARG_NONE, &print_version, N_("Print program version"), NULL },
        { "config", 'c', 0, G_OPTION_ARG_FILENAME, &config_file, N_("Configuration file"), _("FILE") },
        { "key", 'k', 0, G_OPTION_ARG_FILENAME, &key_file, N_("Private key"), _("FILE") },

#ifdef ENABLE_GUI
        { "aliases", 'a', 0, G_OPTION_ARG_FILENAME, &aliases_file, N_("List of aliases"), _("FILE") },
#endif /* ENABLE_GUI */

        { NULL }
    };

#ifdef ENABLE_NLS
    setlocale(LC_ALL, "");
    bindtextdomain(PACKAGE, LOCALEDIR);
    bind_textdomain_codeset(PACKAGE, "UTF-8");
    textdomain(PACKAGE);
#endif /* ENABLE_NLS */

    g_autoptr(GOptionContext) context = g_option_context_new(NULL);
    g_option_context_add_main_entries(context, options, PACKAGE);
    g_option_context_set_summary(context, _("Nanotalk distributed voice client"));
    g_option_context_set_description(context, PACKAGE_BUGREPORT "\n" PACKAGE_URL);

#ifdef ENABLE_GUI
    application_add_option_group(context);
#endif /* ENABLE_GUI */

    if(!g_option_context_parse(context, argc, argv, error))
        return FALSE;

    if(print_version)
    {
        // Print program version
        g_print(PACKAGE " " VERSION "\n");
        exit(EXIT_SUCCESS);
    }

    // Set default paths
    g_autofree gchar *path = g_build_filename(g_get_home_dir(), ".nanotalk", NULL);
    if(!config_file || !key_file || !aliases_file)
        g_mkdir_with_parents(path, 0775);

    if(!config_file) config_file = g_build_filename(path, CONFIG_FILE, NULL);
    if(!key_file) key_file = g_build_filename(path, KEY_FILE, NULL);
    if(!aliases_file) aliases_file = g_build_filename(path, ALIASES_FILE, NULL);

    // Load configuration file
    g_autoptr(GKeyFile) config = g_key_file_new();
    if(!g_key_file_load_from_file(config, config_file, G_KEY_FILE_KEEP_COMMENTS, NULL))
    {
        // Save default configuration
        g_key_file_set_boolean(config, "network", "enable-ipv6", DEFAULT_IPV6);
        g_key_file_set_integer(config, "network", "local-port", DEFAULT_PORT);
        g_key_file_set_string(config,  "network", "bootstrap-host", "");
        g_key_file_set_integer(config, "network", "bootstrap-port", DEFAULT_PORT);

#ifdef ENABLE_GUI
        g_key_file_set_boolean(config, "audio", "echo-cancel", DEFAULT_ECHO_CANCEL);
        g_key_file_set_integer(config, "audio", "bitrate", DEFAULT_BITRATE);
        g_key_file_set_integer(config, "audio", "latency", DEFAULT_LATENCY);
#endif /* ENABLE_GUI */

        g_key_file_save_to_file(config, config_file, NULL);
    }

    gboolean enable_ipv6 = g_key_file_get_boolean(config, "network", "enable-ipv6", NULL);
    guint16 local_port = g_key_file_get_integer(config, "network", "local-port", NULL);
    g_autofree gchar* bootstrap_host = g_key_file_get_string(config, "network", "bootstrap-host", NULL);
    guint16 bootstrap_port = g_key_file_get_integer(config, "network", "bootstrap-port", NULL);
    g_autoptr(DhtClient) client = NULL;

    // Load key from file
    gsize key_len = 0;
    g_autofree gchar *key_data = NULL;
    if(g_file_get_contents(key_file, &key_data, &key_len, NULL))
    {
        // Use existing key
        g_autoptr(GBytes) key = g_bytes_new(key_data, key_len);
        client = dht_client_new(enable_ipv6 ? G_SOCKET_FAMILY_IPV6 : G_SOCKET_FAMILY_IPV4, local_port, key, error);
        if(!client) return FALSE;
    }
    else
    {
        // Generate new key
        client = dht_client_new(enable_ipv6 ? G_SOCKET_FAMILY_IPV6 : G_SOCKET_FAMILY_IPV4, local_port, NULL, error);
        if(!client) return FALSE;

        g_autoptr(GBytes) key = NULL;
        g_object_get(client, "key", &key, NULL);
        gconstpointer key_data = g_bytes_get_data(key, &key_len);

        // Save to file
        if(!g_file_set_contents(key_file, key_data, key_len, error))
            return FALSE;
    }

    if(bootstrap_host && bootstrap_host[0] && bootstrap_port)
        dht_client_bootstrap(client, bootstrap_host, bootstrap_port);

#ifdef ENABLE_GUI
    application = application_new(client, config, config_file, aliases_file, error);
    if(!application) return FALSE;
#else
    g_object_ref(client); // keep client alive
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

#ifdef ENABLE_GUI
    application_run();
    application_free(application);
#else
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);
#endif /* ENABLE_GUI */

    return EXIT_SUCCESS;
}
