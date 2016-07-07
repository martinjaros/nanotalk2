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
#include "rtp-src.h"

GST_DEBUG_CATEGORY_STATIC(rtp_src_debug);
#define GST_CAT_DEFAULT rtp_src_debug

#define PACKET_MTU 1500

#define DEFAULT_TIMEOUT 1000000 // 1 second

enum
{
    PROP_0,
    PROP_KEY,
    PROP_SOCKET,
    PROP_TIMEOUT
};

typedef struct _RtpStream RtpStream;

struct _RtpStream
{
    guint64 roc;
    guint16 seq_last;
};

static void rtp_stream_free(gpointer stream)
{
    g_slice_free(RtpStream, stream);
}

static GstStaticPadTemplate rtp_src_pad_template = GST_STATIC_PAD_TEMPLATE(
        "src", GST_PAD_SRC, GST_PAD_ALWAYS, GST_STATIC_CAPS("application/x-rtp"));

G_DEFINE_TYPE(RtpSrc, rtp_src, GST_TYPE_PUSH_SRC)

static void rtp_src_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec);
static void rtp_src_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec);
static gboolean rtp_src_negotiate(GstBaseSrc *basesrc);
static GstFlowReturn rtp_src_create(GstPushSrc *pushsrc, GstBuffer **outbuf);
static gboolean rtp_src_unlock(GstBaseSrc *basesrc);
static gboolean rtp_src_unlock_stop(GstBaseSrc *basesrc);
static void rtp_src_finalize(GObject *object);

static void rtp_src_class_init(RtpSrcClass *src_class)
{
    GObjectClass *object_class = (GObjectClass*)src_class;
    object_class->set_property = rtp_src_set_property;
    object_class->get_property = rtp_src_get_property;
    object_class->finalize = rtp_src_finalize;

    g_object_class_install_property(object_class, PROP_KEY,
        g_param_spec_boxed("key", "Key", "Encryption key", DHT_TYPE_KEY,
                G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    g_object_class_install_property(object_class, PROP_SOCKET,
        g_param_spec_object("socket", "Socket", "Connected socket", G_TYPE_SOCKET,
                G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    g_object_class_install_property(object_class, PROP_TIMEOUT,
        g_param_spec_int64("timeout", "Timeout", "Socket timeout (us)", -1, G_MAXINT64, DEFAULT_TIMEOUT,
                G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    GstElementClass *element_class = (GstElementClass*)src_class;
    gst_element_class_add_pad_template(element_class, gst_static_pad_template_get(&rtp_src_pad_template));
    gst_element_class_set_static_metadata(element_class,
            "RTP source", "Source/Network/RTP", "RTP packet receiver", "Martin Jaros <xjaros32@stud.feec.vutbr.cz>");

    GstBaseSrcClass *basesrc_class = (GstBaseSrcClass*)src_class;
    basesrc_class->negotiate = rtp_src_negotiate;
    basesrc_class->unlock = rtp_src_unlock;
    basesrc_class->unlock_stop = rtp_src_unlock_stop;

    GstPushSrcClass *pushsrc_class = (GstPushSrcClass*)src_class;
    pushsrc_class->create = rtp_src_create;

    GST_DEBUG_CATEGORY_INIT(rtp_src_debug, "rtpsrc", 0, "RTP source");
}

static void rtp_src_init(RtpSrc *src)
{
    src->streams = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, rtp_stream_free);
    src->cancellable = g_cancellable_new();
    src->timeout = DEFAULT_TIMEOUT;

    gst_base_src_set_live(GST_BASE_SRC(src), TRUE);
    gst_base_src_set_format(GST_BASE_SRC(src), GST_FORMAT_TIME);
    gst_base_src_set_do_timestamp(GST_BASE_SRC(src), TRUE);
}

GstElement* rtp_src_new(DhtKey *key, GSocket *socket, const gchar *name)
{
    g_return_val_if_fail(key != NULL, NULL);
    g_return_val_if_fail(socket != NULL, NULL);

    return g_object_new(RTP_TYPE_SRC, "key", key, "socket", socket, "name", name, NULL);
}

static void rtp_src_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
    RtpSrc *src = RTP_SRC(object);

    switch(prop_id)
    {
        case PROP_KEY:
        {
            DhtKey *key = g_value_get_boxed(value);
            g_return_if_fail(key != NULL);

            src->key = *key;
            break;
        }

        case PROP_SOCKET:
        {
            GSocket *socket = g_value_get_object(value);
            g_return_if_fail(socket != NULL);

            src->socket = g_object_ref(socket);
            break;
        }

        case PROP_TIMEOUT:
            src->timeout = g_value_get_int64(value);
            break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

static void rtp_src_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
    RtpSrc *src = RTP_SRC(object);

    switch(prop_id)
    {
        case PROP_KEY:
            g_value_set_boxed(value, &src->key);
            break;

        case PROP_SOCKET:
            g_value_set_object(value, src->socket);
            break;

        case PROP_TIMEOUT:
            g_value_set_int64(value, src->timeout);
            break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

static gboolean rtp_src_negotiate(GstBaseSrc *basesrc)
{
    RtpSrc *src = RTP_SRC(basesrc);

    if(GST_BASE_SRC_CLASS(rtp_src_parent_class)->negotiate(basesrc))
    {
        g_clear_object(&src->allocator);
        gst_base_src_get_allocator(basesrc, &src->allocator, &src->params);
        return TRUE;
    }

    return FALSE;
}

static GstFlowReturn rtp_src_create(GstPushSrc *pushsrc, GstBuffer **outbuf)
{
    RtpSrc *src = RTP_SRC(pushsrc);

    while(1)
    {
        g_autoptr(GError) error = NULL;
        g_socket_condition_timed_wait(src->socket, G_IO_IN, src->timeout, src->cancellable, &error);
        if(error)
        {
            if(g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
                return GST_FLOW_FLUSHING;

            GST_ELEMENT_ERROR(src, RESOURCE, READ, ("%s", error->message), (NULL));
            return GST_FLOW_ERROR;
        }

        guint8 packet[PACKET_MTU];
        gssize len = g_socket_receive(src->socket, (gchar*)packet, sizeof(packet), NULL, &error);
        if(error)
        {
            GST_ELEMENT_ERROR(src, RESOURCE, READ, ("%s", error->message), (NULL));
            return GST_FLOW_ERROR;
        }

        if((len > 28) && (packet[0] == 0x80))
        {
            guint16 seq = GST_READ_UINT16_BE(packet + 2);
            guint32 ssrc = GST_READ_UINT32_BE(packet + 8);

            // Get stream for SSRC
            RtpStream *stream = g_hash_table_lookup(src->streams, GUINT_TO_POINTER(ssrc));
            guint16 seq_last = stream ? stream->seq_last : 0;
            guint64 roc = stream ? stream->roc : 0;

            // Roll-over counter logic
            if((seq_last < 0x8000) && (seq_last + 0x8000 < seq) && (roc > 0x0000000000000)) roc--;
            if((seq_last > 0x7FFF) && (seq_last - 0x8000 > seq) && (roc < 0x1000000000000)) roc++;

            guint8 nonce[12];
            GST_WRITE_UINT64_LE(nonce, roc << 16 | (guint64)seq);
            GST_WRITE_UINT32_LE(nonce + 8, ssrc);

            len -= 16;
            if(dht_aead_verify(packet + len, packet, 12, packet + 12, len - 12, nonce, &src->key))
            {
                GstBuffer *buffer = gst_buffer_new_allocate(src->allocator, len, &src->params);
                if(!buffer) return GST_FLOW_ERROR;

                GstMapInfo map;
                if(!gst_buffer_map(buffer, &map, GST_MAP_WRITE))
                {
                    gst_buffer_unref(buffer);
                    return GST_FLOW_ERROR;
                }

                memcpy(map.data, packet, 12);
                dht_aead_xor(map.data + 12, packet + 12, len - 12, nonce, &src->key);
                gst_buffer_unmap(buffer, &map);

                if(!stream)
                {
                    stream = g_slice_new(RtpStream);
                    g_hash_table_insert(src->streams, GUINT_TO_POINTER(ssrc), stream);
                }

                // Update stream state
                stream->seq_last = seq;
                stream->roc = roc;

                *outbuf = buffer;
                return GST_FLOW_OK;
            }
            else GST_WARNING_OBJECT(src, "Authentication failed");
        }
        else GST_WARNING_OBJECT(src, "Invalid packet");

        if(len < 0) break;
    }

    return GST_FLOW_ERROR;
}

static gboolean rtp_src_unlock(GstBaseSrc *basesrc)
{
    RtpSrc *src = RTP_SRC(basesrc);

    g_cancellable_cancel(src->cancellable);

    return TRUE;
}

static gboolean rtp_src_unlock_stop(GstBaseSrc *basesrc)
{
    RtpSrc *src = RTP_SRC(basesrc);

    g_cancellable_reset(src->cancellable);

    return TRUE;
}

static void rtp_src_finalize(GObject *object)
{
    RtpSrc *src = RTP_SRC(object);

    if(src->allocator)
        g_object_unref(src->allocator);

    if(src->socket)
        g_object_unref(src->socket);

    g_object_unref(src->cancellable);
    g_hash_table_destroy(src->streams);

    G_OBJECT_CLASS(rtp_src_parent_class)->finalize(object);
}
