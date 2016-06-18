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

#define G_LOG_DOMAIN "RTP"

#include <string.h>
#include <sodium.h>
#include "rtp-sink.h"

GST_DEBUG_CATEGORY_STATIC(rtp_sink_debug);
#define GST_CAT_DEFAULT rtp_sink_debug

enum
{
    PROP_0,
    PROP_SOCKET,
    PROP_KEY
};

static GstStaticPadTemplate rtp_sink_pad_template = GST_STATIC_PAD_TEMPLATE(
        "sink", GST_PAD_SINK, GST_PAD_ALWAYS, GST_STATIC_CAPS("application/x-rtp"));

G_DEFINE_TYPE(RtpSink, rtp_sink, GST_TYPE_BASE_SINK)

static void rtp_sink_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec);
static void rtp_sink_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec);
static GstFlowReturn rtp_sink_render(GstBaseSink *base, GstBuffer *buffer);
static void rtp_sink_finalize(GObject *object);

static void rtp_sink_class_init(RtpSinkClass *sink_class)
{
    GObjectClass *object_class = (GObjectClass*)sink_class;
    object_class->set_property = rtp_sink_set_property;
    object_class->get_property = rtp_sink_get_property;
    object_class->finalize = rtp_sink_finalize;

    g_object_class_install_property(object_class, PROP_SOCKET,
        g_param_spec_object("socket", "Socket", "Connected socket", G_TYPE_SOCKET, G_PARAM_READWRITE));

    g_object_class_install_property(object_class, PROP_KEY,
        g_param_spec_boxed("key", "Key", "Encryption key", G_TYPE_BYTES, G_PARAM_READWRITE));

    GstElementClass *element_class = (GstElementClass*)sink_class;
    gst_element_class_add_pad_template(element_class, gst_static_pad_template_get(&rtp_sink_pad_template));
    gst_element_class_set_static_metadata(element_class,
            "RTP sink", "Sink/Network/RTP", "RTP packet sender", "Martin Jaros <xjaros32@stud.feec.vutbr.cz>");

    GstBaseSinkClass *base_class = (GstBaseSinkClass*)sink_class;
    base_class->render = rtp_sink_render;

    GST_DEBUG_CATEGORY_INIT(rtp_sink_debug, "rtpsink", 0, "RTP sink");
}

static void rtp_sink_init(RtpSink *sink)
{

}

static void rtp_sink_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
    RtpSink *sink = RTP_SINK(object);

    switch(prop_id)
    {
        case PROP_SOCKET:
            g_clear_object(&sink->socket);
            sink->socket = g_object_ref(g_value_get_object(value));
            break;

        case PROP_KEY:
            sink->key = *((DhtKey*)g_value_get_boxed(value));
            sink->roc = 0;
            break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

static void rtp_sink_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
    RtpSink *sink = RTP_SINK(object);

    switch(prop_id)
    {
        case PROP_SOCKET:
            g_value_set_object(value, sink->socket);
            break;

        case PROP_KEY:
            g_value_set_boxed(value, &sink->key);
            break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

static GstFlowReturn rtp_sink_render(GstBaseSink *base, GstBuffer *buffer)
{
    RtpSink *sink = RTP_SINK(base);

    GstMapInfo map;
    if(sink->socket && (gst_buffer_get_size(buffer) > 12) && gst_buffer_map(buffer, &map, GST_MAP_READ))
    {
        guint16 seq = GST_READ_UINT16_BE(map.data + 2);
        guint32 ssrc = GST_READ_UINT32_BE(map.data + 8);

        guint8 nonce[12];
        GST_WRITE_UINT64_LE(nonce, sink->roc << 16 | (guint64)seq);
        GST_WRITE_UINT32_LE(nonce + 8, ssrc);

        if((seq == G_MAXUINT16) && (++sink->roc == 0x1000000000000))
            GST_ELEMENT_ERROR(sink, STREAM, DECRYPT, ("Key utilization limit was reached."), (NULL));

        guint8 packet[map.size + 16];
        memcpy(packet, map.data, 12);
        crypto_aead_chacha20poly1305_ietf_encrypt(packet + 12, NULL, map.data + 12, map.size - 12, map.data, 12, NULL, nonce, sink->key.data);

        GError *error = NULL;
        g_socket_send(sink->socket, (gchar*)packet, sizeof(packet), NULL, &error);
        if(error)
        {
            GST_ELEMENT_ERROR(sink, RESOURCE, WRITE, ("%s", error->message), (NULL));
            g_error_free(error);
        }

        gst_buffer_unmap(buffer, &map);
        gst_buffer_unref(buffer);
        return GST_FLOW_OK;
    }

    gst_buffer_unref(buffer);
    return GST_FLOW_ERROR;
}

static void rtp_sink_finalize(GObject *object)
{
    RtpSink *sink = RTP_SINK(object);

    if(sink->socket)
        g_object_unref(sink->socket);

    G_OBJECT_CLASS(rtp_sink_parent_class)->finalize(object);
}
