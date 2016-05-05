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

#ifndef __PREFERENCES_H__
#define __PREFERENCES_H__

#include <glib.h>

#define PREF_NETWORK_ENABLE_IPV6    "network", "enable-ipv6"
#define PREF_NETWORK_LOCAL_PORT     "network", "local-port"
#define PREF_NETWORK_BOOTSTRAP_HOST "network", "bootstrap-host"
#define PREF_NETWORK_BOOTSTRAP_PORT "network", "bootstrap-port"

#define PREF_AUDIO_ECHO_CANCEL      "audio", "echo-cancel"
#define PREF_AUDIO_LATENCY          "audio", "latency"
#define PREF_AUDIO_BITRATE          "audio", "bitrate"
#define PREF_AUDIO_ENABLE_VBR       "audio", "enable-vbr"
#define PREF_AUDIO_BUFFER_MODE      "audio", "buffer-mode"

static inline void preferences_default(GKeyFile *prefs)
{
    g_key_file_set_boolean(prefs, PREF_NETWORK_ENABLE_IPV6, FALSE);
    g_key_file_set_integer(prefs, PREF_NETWORK_LOCAL_PORT, 5004);
    g_key_file_set_string(prefs,  PREF_NETWORK_BOOTSTRAP_HOST, "");
    g_key_file_set_integer(prefs, PREF_NETWORK_BOOTSTRAP_PORT, 5004);

    g_key_file_set_boolean(prefs, PREF_AUDIO_ECHO_CANCEL, FALSE);
    g_key_file_set_integer(prefs, PREF_AUDIO_LATENCY, 200);
    g_key_file_set_integer(prefs, PREF_AUDIO_BITRATE, 64000);
    g_key_file_set_boolean(prefs, PREF_AUDIO_ENABLE_VBR, FALSE);
    // PREF_AUDIO_BUFFER_MODE unset
}

#endif /* __PREFERENCES_H__ */
