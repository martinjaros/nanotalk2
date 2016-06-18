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
#include "rtp-src.h"

GST_DEBUG_CATEGORY_STATIC(rtp_src_debug);
#define GST_CAT_DEFAULT rtp_src_debug

#define PACKET_MTU 1500

#define DEFAULT_TIMEOUT 1000000000LL // 1 second

enum
{
    PROP_0,
    PROP_SOCKET,
    PROP_TIMEOUT,
    PROP_KEY
};

typedef struct _RtpStream RtpStream;

struct _RtpStream
{
    guint64 roc;
    guint16 seq_last;
};

static void rtp_stream_free(gpointer ptr)
{
    g_slice_free(RtpStream, ptr);
}

static GstStaticPadTemplate rtp_src_pad_template = GST_STATIC_PAD_TEMPLATE(
        "src", GST_PAD_SRC, GST_PAD_ALWAYS, GST_STATIC_CAPS("application/x-rtp"));

G_DEFINE_TYPE(RtpSrc, rtp_src, GST_TYPE_PUSH_SRC)

static void rtp_src_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec);
static void rtp_src_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec);
static gboolean rtp_src_negotiate(GstBaseSrc *base);
static GstFlowReturn rtp_src_create(GstPushSrc *push, GstBuffer **buf);
static gboolean rtp_src_unlock(GstBaseSrc *base);
static gboolean rtp_src_unlock_stop(GstBaseSrc *base);
static void rtp_src_finalize(GObject *object);

static void rtp_src_class_init(RtpSrcClass *src_class)
{
    GObjectClass *object_class = (GObjectClass*)src_class;
    object_class->set_property = rtp_src_set_property;
    object_class->get_property = rtp_src_get_property;
    object_class->finalize = rtp_src_finalize;

    g_object_class_install_property(object_class, PROP_SOCKET,
        g_param_spec_object("socket", "Socket", "Connected socket", G_TYPE_SOCKET, G_PARAM_READWRITE));

    g_object_class_install_property(object_class, PROP_TIMEOUT,
        g_param_spec_int64("timeout", "Timeout", "Post EOS after timeout (ns)", -1, G_MAXINT64, DEFAULT_TIMEOUT, G_PARAM_READWRITE));

    g_object_class_install_property(object_class, PROP_KEY,
        g_param_spec_boxed("key", "Key", "Encryption key", G_TYPE_BYTES, G_PARAM_READWRITE));

    GstElementClass *element_class = (GstElementClass*)src_class;
    gst_element_class_add_pad_template(element_class, gst_static_pad_template_get(&rtp_src_pad_template));
    gst_element_class_set_static_metadata(element_class,
            "RTP source", "Source/Network/RTP", "RTP packet receiver", "Martin Jaros <xjaros32@stud.feec.vutbr.cz>");

    GstBaseSrcClass *base_class = (GstBaseSrcClass*)src_class;
    base_class->negotiate = rtp_src_negotiate;
    base_class->unlock = rtp_src_unlock;
    base_class->unlock_stop = rtp_src_unlock_stop;

    GstPushSrcClass *push_class = (GstPushSrcClass*)src_class;
    push_class->create = rtp_src_create;

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

static void rtp_src_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
    RtpSrc *src = RTP_SRC(object);

    switch(prop_id)
    {
        case PROP_SOCKET:
            g_clear_object(&src->socket);
            src->socket = g_object_ref(g_value_get_object(value));
            break;

        case PROP_TIMEOUT:
            src->timeout = g_value_get_int64(value);
            break;

        case PROP_KEY:
            src->key = *((DhtKey*)g_value_get_boxed(value));
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
        case PROP_SOCKET:
            g_value_set_object(value, src->socket);
            break;

        case PROP_TIMEOUT:
            g_value_set_int64(value, src->timeout);
            break;

        case PROP_KEY:
            g_value_set_boxed(value, &src->key);
            break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

static gboolean rtp_src_negotiate(GstBaseSrc *base)
{
    RtpSrc *src = RTP_SRC(base);

    if(GST_BASE_SRC_CLASS(rtp_src_parent_class)->negotiate(base))
    {
        g_clear_object(&src->allocator);
        gst_base_src_get_allocator(base, &src->allocator, &src->params);
        return TRUE;
    }

    return FALSE;
}

static GstFlowReturn rtp_src_create(GstPushSrc *push, GstBuffer **buf)
{
    RtpSrc *src = RTP_SRC(push);

    while(src->socket)
    {
        GError *error = NULL;
        g_socket_condition_timed_wait(src->socket, G_IO_IN, src->timeout, src->cancellable, &error);
        if(error)
        {
            GstFlowReturn ret = GST_FLOW_ERROR;
            if(g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
                ret = GST_FLOW_FLUSHING;
            else if(g_error_matches(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT))
                ret = GST_FLOW_EOS;
            else GST_ELEMENT_ERROR(src, RESOURCE, READ, ("%s", error->message), (NULL));

            g_error_free(error);
            return ret;
        }

        guint8 packet[PACKET_MTU];
        gssize len = g_socket_receive(src->socket, (gchar*)packet, sizeof(packet), NULL, &error);
        if(error)
        {
            GST_ELEMENT_ERROR(src, RESOURCE, READ, ("%s", error->message), (NULL));

            g_error_free(error);
            return GST_FLOW_ERROR;
        }

        if(len > 28)
        {
            GstBuffer *buffer = gst_buffer_new_allocate(src->allocator, len - 16, &src->params);
            if(!buffer) return GST_FLOW_ERROR;

            GstMapInfo map;
            if(!gst_buffer_map(buffer, &map, GST_MAP_WRITE))
            {
                gst_buffer_unref(buffer);
                return GST_FLOW_ERROR;
            }

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

            if(crypto_aead_chacha20poly1305_ietf_decrypt(map.data + 12, NULL, NULL, packet + 12, len - 12, packet, 12, nonce, src->key.data) == 0)
            {
                memcpy(map.data, packet, 12);
                gst_buffer_unmap(buffer, &map);

                if(!stream)
                {
                    stream = g_slice_new(RtpStream);
                    g_hash_table_insert(src->streams, GUINT_TO_POINTER(ssrc), stream);
                }

                // Update stream state
                stream->seq_last = seq_last;
                stream->roc = roc;

                *buf = buffer;
                return GST_FLOW_OK;
            }

            gst_buffer_unmap(buffer, &map);
            gst_buffer_unref(buffer);
        }
    }

    return GST_FLOW_ERROR;
}

static gboolean rtp_src_unlock(GstBaseSrc *base)
{
    RtpSrc *src = RTP_SRC(base);

    g_cancellable_cancel(src->cancellable);
    return TRUE;
}

static gboolean rtp_src_unlock_stop(GstBaseSrc *base)
{
    RtpSrc *src = RTP_SRC(base);

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