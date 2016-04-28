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

#define G_LOG_DOMAIN "RtpDecrypt"

#include <string.h>
#include <sodium.h>
#include <gst/rtp/rtp.h>
#include "rtpdecrypt.h"

GST_DEBUG_CATEGORY_STATIC(gst_rtp_decrypt_debug);
#define GST_CAT_DEFAULT gst_rtp_decrypt_debug

enum
{
    PROP_0,
    PROP_KEY,
    PROP_TIMEOUT
};

static GstStaticPadTemplate src_template =
        GST_STATIC_PAD_TEMPLATE("src", GST_PAD_SRC, GST_PAD_ALWAYS, GST_STATIC_CAPS("application/x-rtp"));
static GstStaticPadTemplate sink_template =
        GST_STATIC_PAD_TEMPLATE("sink", GST_PAD_SINK, GST_PAD_ALWAYS, GST_STATIC_CAPS("application/x-rtp"));

G_DEFINE_TYPE(GstRtpDecrypt, gst_rtp_decrypt, GST_TYPE_ELEMENT)

static gboolean on_timeout(GstClock *clock, GstClockTime time, GstClockID id, gpointer object)
{
    GstRtpDecrypt *decrypt = GST_RTP_DECRYPT(object);
    if(!decrypt->validated)
    {
        GstMessage *message = gst_message_new_element(GST_OBJECT(object), gst_structure_new_empty("GstRtpTimeout"));
            gst_element_post_message(GST_ELEMENT(object), message);
    }

    decrypt->validated = FALSE;
    return TRUE;
}

static void gst_rtp_decrypt_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec);
static void gst_rtp_decrypt_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec);
static GstStateChangeReturn gst_rtp_decrypt_change_state(GstElement *element, GstStateChange transition);
static GstFlowReturn gst_rtp_decrypt_chain(GstPad *pad, GstObject *parent, GstBuffer *inbuf);
static void gst_rtp_decrypt_finalize(GObject *object);

static void gst_rtp_decrypt_class_init(GstRtpDecryptClass *klass)
{
    GObjectClass *object_class = (GObjectClass*)klass;
    object_class->set_property = gst_rtp_decrypt_set_property;
    object_class->get_property = gst_rtp_decrypt_get_property;
    object_class->finalize = gst_rtp_decrypt_finalize;

    g_object_class_install_property(object_class, PROP_KEY,
        g_param_spec_boxed("key", "Key", "Decryption key", G_TYPE_BYTES, G_PARAM_READWRITE));

    g_object_class_install_property(object_class, PROP_TIMEOUT,
        g_param_spec_uint64("timeout", "Timeout", "Source timeout (ns)", 0, G_MAXUINT64, 0, G_PARAM_READWRITE));

    GstElementClass *element_class = (GstElementClass*)klass;
    element_class->change_state = gst_rtp_decrypt_change_state;
    gst_element_class_add_pad_template(element_class, gst_static_pad_template_get(&src_template));
    gst_element_class_add_pad_template(element_class, gst_static_pad_template_get(&sink_template));
    gst_element_class_set_details_simple(element_class,
        "RTP Decrypt", "Filter/Network/RTP", "Decrypts RTP packets", "Martin Jaros <xjaros32@stud.feec.vutbr.cz>");

    GST_DEBUG_CATEGORY_INIT(gst_rtp_decrypt_debug, "rtpdecrypt", 0, "RTP Decrypt");
}

static void gst_rtp_decrypt_init(GstRtpDecrypt *decrypt)
{
    decrypt->src = gst_pad_new_from_static_template(&src_template, "src");
    GST_PAD_SET_PROXY_CAPS(decrypt->src);
    gst_element_add_pad(GST_ELEMENT(decrypt), decrypt->src);

    decrypt->sink = gst_pad_new_from_static_template(&sink_template, "sink");
    GST_PAD_SET_PROXY_CAPS(decrypt->sink);
    gst_pad_set_chain_function(decrypt->sink, gst_rtp_decrypt_chain);
    gst_element_add_pad(GST_ELEMENT(decrypt), decrypt->sink);
}

static void gst_rtp_decrypt_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
    GstRtpDecrypt *decrypt = GST_RTP_DECRYPT(object);

    switch(prop_id)
    {
        case PROP_KEY:
            decrypt->key = g_bytes_ref(g_value_get_boxed(value));
            decrypt->roc = 0;
            decrypt->s_l = 0;
            break;

        case PROP_TIMEOUT:
            decrypt->timeout = g_value_get_uint64(value);
            break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

static void gst_rtp_decrypt_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
    GstRtpDecrypt *decrypt = GST_RTP_DECRYPT(object);

    switch(prop_id)
    {
        case PROP_KEY:
            g_value_set_boxed(value, decrypt->key);
            break;

        case PROP_TIMEOUT:
            g_value_set_uint64(value, decrypt->timeout);
            break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

static GstStateChangeReturn gst_rtp_decrypt_change_state(GstElement *element, GstStateChange transition)
{
    GstRtpDecrypt *decrypt = GST_RTP_DECRYPT(element);

    switch(transition)
    {
        case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
            if(decrypt->timeout)
            {
                GstClock *clock = gst_element_get_clock(element);
                decrypt->clock_id = gst_clock_new_periodic_id(clock, gst_clock_get_time(clock) + decrypt->timeout, decrypt->timeout);
                gst_clock_id_wait_async(decrypt->clock_id, on_timeout, decrypt, NULL);
                gst_object_unref(clock);
            }

            break;

        case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
            if(decrypt->clock_id)
            {
                gst_clock_id_unschedule(decrypt->clock_id);
                gst_clock_id_unref(decrypt->clock_id);
                decrypt->clock_id = NULL;
            }

            break;

        default: break;
    }

    return GST_ELEMENT_CLASS(gst_rtp_decrypt_parent_class)->change_state(element, transition);
}

static GstFlowReturn gst_rtp_decrypt_chain(GstPad *pad, GstObject *parent, GstBuffer *inbuf)
{
    GstRtpDecrypt *decrypt = GST_RTP_DECRYPT(parent);

    gsize key_len = 0;
    gconstpointer key = decrypt->key ? g_bytes_get_data(decrypt->key, &key_len) : NULL;
    if(key_len != crypto_aead_chacha20poly1305_KEYBYTES)
    {
        GST_ELEMENT_ERROR(decrypt, LIBRARY, SETTINGS, ("Invalid key."),
            ("expected key size of %u bytes, but got %zu bytes", crypto_aead_chacha20poly1305_KEYBYTES, key_len));

        gst_buffer_unref(inbuf);
        return GST_FLOW_ERROR;
    }

    GstRTPBuffer rtp_buffer = { };
    if(gst_rtp_buffer_map(inbuf, GST_MAP_READ, &rtp_buffer))
    {
        guint16 seq = gst_rtp_buffer_get_seq(&rtp_buffer);
        guint32 ssrc = gst_rtp_buffer_get_ssrc(&rtp_buffer);
        guint header_len = gst_rtp_buffer_get_header_len(&rtp_buffer);
        guint payload_len = gst_rtp_buffer_get_payload_len(&rtp_buffer);
        guint packet_len = gst_rtp_buffer_get_packet_len(&rtp_buffer);
        gst_rtp_buffer_unmap(&rtp_buffer);

        GstBuffer *outbuf = (packet_len < crypto_aead_chacha20poly1305_ABYTES) ? NULL :
            gst_buffer_new_allocate(NULL, packet_len - crypto_aead_chacha20poly1305_ABYTES, NULL);

        if(outbuf)
        {
            GstMapInfo inbuf_map;
            if(gst_buffer_map(inbuf, &inbuf_map, GST_MAP_READ))
            {
                GstMapInfo outbuf_map;
                if(gst_buffer_map(outbuf, &outbuf_map, GST_MAP_WRITE))
                {
                    guint64 roc = decrypt->roc, s_l = decrypt->s_l;
                    if((s_l < 0x8000) && (s_l + 0x8000 < seq) && (roc > 0x0000000000000)) roc--;
                    if((s_l > 0x7FFF) && (s_l - 0x8000 > seq) && (roc < 0x1000000000000)) roc++;

                    guint8 nonce[12];
                    GST_WRITE_UINT32_BE(nonce, ssrc);
                    GST_WRITE_UINT64_BE(nonce + 4, roc << 16 | (guint64)seq);
                    if(crypto_aead_chacha20poly1305_ietf_decrypt(
                            outbuf_map.data + header_len, NULL, NULL,
                            inbuf_map.data + header_len, payload_len,
                            inbuf_map.data, header_len,
                            (gconstpointer)nonce, key) == 0)
                    {
                        decrypt->validated = TRUE;
                        decrypt->roc = roc;
                        decrypt->s_l = seq;

                        memcpy(outbuf_map.data, inbuf_map.data, header_len);
                        gst_buffer_unmap(outbuf, &outbuf_map);
                        gst_buffer_unmap(inbuf, &inbuf_map);
                        gst_buffer_unref(inbuf);

                        GST_DEBUG_OBJECT(decrypt, "Pushing buffer ssrc=%u roc=%lu seq=%hu", ssrc, roc, seq);
                        return gst_pad_push(decrypt->src, outbuf);
                    }
                    gst_buffer_unmap(outbuf, &outbuf_map);
                }

                gst_buffer_unmap(inbuf, &inbuf_map);
            }

            gst_buffer_unref(outbuf);
        }
    }

    GST_WARNING_OBJECT(decrypt, "Buffer dropped");
    gst_buffer_unref(inbuf);
    return GST_FLOW_OK;
}

static void gst_rtp_decrypt_finalize(GObject *object)
{
    GstRtpDecrypt *decrypt = GST_RTP_DECRYPT(object);
    g_bytes_unref(decrypt->key);

    G_OBJECT_CLASS(gst_rtp_decrypt_parent_class)->finalize(object);
}
