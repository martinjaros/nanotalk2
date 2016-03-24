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

#include "gstrtpencrypt.h"
#include "gstrtpdecrypt.h"

static gboolean plugin_init(GstPlugin *plugin)
{
    return gst_element_register(plugin, "rtpencrypt", GST_RANK_NONE, GST_TYPE_RTP_ENCRYPT) &&
           gst_element_register(plugin, "rtpdecrypt", GST_RANK_NONE, GST_TYPE_RTP_DECRYPT);
}

GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR,
    rtpcrypto, "RTP encryption/decryption", plugin_init, "1.0",
    "GPL", "Distributed multimedia service", "http://github.com/martinjaros")
