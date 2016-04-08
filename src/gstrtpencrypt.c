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

#define G_LOG_DOMAIN "RtpEncrypt"

#include <string.h>
#include <sodium.h>
#include <glib/gi18n.h>
#include <gst/rtp/rtp.h>
#include "gstrtpencrypt.h"

GST_DEBUG_CATEGORY_STATIC(gst_rtp_encrypt_debug);
#define GST_CAT_DEFAULT gst_rtp_encrypt_debug

enum
{
    PROP_0,
    PROP_KEY
};

static GstStaticPadTemplate src_template =
        GST_STATIC_PAD_TEMPLATE("src", GST_PAD_SRC, GST_PAD_ALWAYS, GST_STATIC_CAPS("application/x-rtp"));
static GstStaticPadTemplate sink_template =
        GST_STATIC_PAD_TEMPLATE("sink", GST_PAD_SINK, GST_PAD_ALWAYS, GST_STATIC_CAPS("application/x-rtp"));

G_DEFINE_TYPE(GstRtpEncrypt, gst_rtp_encrypt, GST_TYPE_ELEMENT)

static void gst_rtp_encrypt_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec);
static void gst_rtp_encrypt_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec);
static GstFlowReturn gst_rtp_encrypt_chain(GstPad *pad, GstObject *parent, GstBuffer *inbuf);
static void gst_rtp_encrypt_finalize(GObject *object);

static void gst_rtp_encrypt_class_init(GstRtpEncryptClass *klass)
{
    GObjectClass *object_class = (GObjectClass*)klass;
    object_class->set_property = gst_rtp_encrypt_set_property;
    object_class->get_property = gst_rtp_encrypt_get_property;
    object_class->finalize = gst_rtp_encrypt_finalize;

    g_object_class_install_property(object_class, PROP_KEY,
        g_param_spec_boxed("key", _("Key"), _("Encryption key"), G_TYPE_BYTES, G_PARAM_READWRITE));

    GstElementClass *element_class = (GstElementClass*)klass;
    gst_element_class_add_pad_template(element_class, gst_static_pad_template_get(&src_template));
    gst_element_class_add_pad_template(element_class, gst_static_pad_template_get(&sink_template));
    gst_element_class_set_details_simple(element_class,
        "RTP Encrypt", "Filter/Network/RTP", _("Encrypts RTP packets"), "Martin Jaros <xjaros32@stud.feec.vutbr.cz>");

    GST_DEBUG_CATEGORY_INIT(gst_rtp_encrypt_debug, "rtpencrypt", 0, "RTP Encrypt");
}

static void gst_rtp_encrypt_init(GstRtpEncrypt *encrypt)
{
    encrypt->src = gst_pad_new_from_static_template(&src_template, "src");
    GST_PAD_SET_PROXY_CAPS(encrypt->src);
    gst_element_add_pad(GST_ELEMENT(encrypt), encrypt->src);

    encrypt->sink = gst_pad_new_from_static_template(&sink_template, "sink");
    GST_PAD_SET_PROXY_CAPS(encrypt->sink);
    gst_pad_set_chain_function(encrypt->sink, gst_rtp_encrypt_chain);
    gst_element_add_pad(GST_ELEMENT(encrypt), encrypt->sink);
}

static void gst_rtp_encrypt_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
    GstRtpEncrypt *encrypt = GST_RTP_ENCRYPT(object);

    switch(prop_id)
    {
        case PROP_KEY:
            encrypt->key = g_bytes_ref(g_value_get_boxed(value));
            encrypt->roc = 0;
            break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

static void gst_rtp_encrypt_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
    GstRtpEncrypt *encrypt = GST_RTP_ENCRYPT(object);

    switch(prop_id)
    {
        case PROP_KEY:
            g_value_set_boxed(value, encrypt->key);
            break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

static GstFlowReturn gst_rtp_encrypt_chain(GstPad *pad, GstObject *parent, GstBuffer *inbuf)
{
    GstRtpEncrypt *encrypt = GST_RTP_ENCRYPT(parent);

    gsize key_len = 0;
    gconstpointer key = encrypt->key ? g_bytes_get_data(encrypt->key, &key_len) : NULL;
    if(key_len != crypto_aead_chacha20poly1305_KEYBYTES)
    {
        GST_ELEMENT_ERROR(encrypt, RESOURCE, SETTINGS, (_("Invalid key.")),
            (_("expected key size of %u bytes, but got %zu bytes"), crypto_aead_chacha20poly1305_KEYBYTES, key_len));

        gst_buffer_unref(inbuf);
        return GST_FLOW_ERROR;
    }

    GstRTPBuffer rtp_buffer = { };
    if(gst_rtp_buffer_map(inbuf, GST_MAP_READ, &rtp_buffer))
    {
        guint16 seq = gst_rtp_buffer_get_seq(&rtp_buffer);
        guint header_len = gst_rtp_buffer_get_header_len(&rtp_buffer);
        guint payload_len = gst_rtp_buffer_get_payload_len(&rtp_buffer);
        guint packet_len = gst_rtp_buffer_get_packet_len(&rtp_buffer);
        gst_rtp_buffer_unmap(&rtp_buffer);

        GstBuffer *outbuf = gst_buffer_new_allocate(NULL, packet_len + crypto_aead_chacha20poly1305_ABYTES, NULL);

        if(outbuf)
        {
            GstMapInfo inbuf_map;
            if(gst_buffer_map(inbuf, &inbuf_map, GST_MAP_READ))
            {
                GstMapInfo outbuf_map;
                if(gst_buffer_map(outbuf, &outbuf_map, GST_MAP_WRITE))
                {
                    guint64 roc = encrypt->roc;
                    if(seq == UINT16_MAX) encrypt->roc++;

                    guint64 index = GUINT64_TO_BE((roc << 16) | seq);
                    if(crypto_aead_chacha20poly1305_encrypt(
                            outbuf_map.data + header_len, NULL,
                            inbuf_map.data + header_len, payload_len,
                            inbuf_map.data, header_len, NULL,
                            (gconstpointer)&index, key) == 0)
                    {
                        memcpy(outbuf_map.data, inbuf_map.data, header_len);
                        gst_buffer_unmap(outbuf, &outbuf_map);
                        gst_buffer_unmap(inbuf, &inbuf_map);
                        gst_buffer_unref(inbuf);

                        GST_DEBUG_OBJECT(encrypt, "Pushing buffer roc=%lu seq=%hu", roc, seq);
                        return gst_pad_push(encrypt->src, outbuf);
                    }

                    gst_buffer_unmap(outbuf, &outbuf_map);
                }

                gst_buffer_unmap(inbuf, &inbuf_map);
            }

            gst_buffer_unref(outbuf);
        }
    }

    GST_WARNING_OBJECT(encrypt, "Buffer dropped");
    gst_buffer_unref(inbuf);
    return GST_FLOW_OK;
}

static void gst_rtp_encrypt_finalize(GObject *object)
{
    GstRtpEncrypt *encrypt = GST_RTP_ENCRYPT(object);
    g_bytes_unref(encrypt->key);

    G_OBJECT_CLASS(gst_rtp_encrypt_parent_class)->finalize(object);
}
