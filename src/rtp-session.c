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

#include <glib/gi18n.h>
#include "rtp-src.h"
#include "rtp-sink.h"
#include "rtp-tone.h"
#include "rtp-session.h"

enum
{
    SIGNAL_HANGUP,
    LAST_SIGNAL
};

typedef struct _RtpSessionPrivate RtpSessionPrivate;

struct _RtpSessionPrivate
{
    GstElement *rx_pipeline, *tx_pipeline;
    guint rx_watch, tx_watch;
};

static guint rtp_session_signals[LAST_SIGNAL];

G_DEFINE_TYPE_WITH_PRIVATE(RtpSession, rtp_session, G_TYPE_OBJECT)

static void rtp_session_finalize(GObject *obj);

static GstCaps* request_pt_map_cb(GstElement *element, guint pt, gpointer arg);
static void new_payload_type_cb(GstElement *element, guint pt, GstPad *pad, gpointer arg);
static gboolean bus_watch_cb(GstBus *bus, GstMessage *message, gpointer arg);

static inline GstElement* assert_element(GstBin *bin, const gchar *factory, const gchar *name)
{
    GstElement *element = gst_element_factory_make(factory, name);
    if(element)
        gst_bin_add(bin, element);
    else
        g_warning(_("Missing GStreamer element: %s"), name);

    return element;
}

static void rtp_session_class_init(RtpSessionClass *session_class)
{
    GObjectClass *object_class = (GObjectClass*)session_class;
    object_class->finalize = rtp_session_finalize;

    rtp_session_signals[SIGNAL_HANGUP] = g_signal_new("hangup",
            RTP_TYPE_SESSION, G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET(RtpSessionClass, hangup), NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void rtp_session_init(RtpSession *session)
{
    RtpSessionPrivate *priv = rtp_session_get_instance_private(session);

    priv->rx_pipeline = gst_pipeline_new("rx_pipeline");
    GstBus *rx_bus = gst_pipeline_get_bus(GST_PIPELINE(priv->rx_pipeline));
    priv->rx_watch = gst_bus_add_watch(rx_bus, bus_watch_cb, session);
    gst_object_unref(rx_bus);

    priv->tx_pipeline = gst_pipeline_new("tx_pipeline");
    GstBus *tx_bus = gst_pipeline_get_bus(GST_PIPELINE(priv->tx_pipeline));
    priv->tx_watch = gst_bus_add_watch(tx_bus, bus_watch_cb, session);
    gst_object_unref(tx_bus);
}

RtpSession* rtp_session_new(GSocket *socket, DhtKey *enc_key, DhtKey *dec_key)
{
    RtpSession *session = g_object_new(RTP_TYPE_SESSION, NULL);
    RtpSessionPrivate *priv = rtp_session_get_instance_private(session);

    GstElement *audio_src = assert_element(GST_BIN(priv->tx_pipeline), "autoaudiosrc", "audio_src");
    GstElement *audio_enc = assert_element(GST_BIN(priv->tx_pipeline), "opusenc", "audio_enc");
    GstElement *audio_pay = assert_element(GST_BIN(priv->tx_pipeline), "rtpopuspay", "audio_pay");
    GstElement *rtp_demux = assert_element(GST_BIN(priv->rx_pipeline), "rtpptdemux", "rtp_demux");
    GstElement *audio_buffer = assert_element(GST_BIN(priv->rx_pipeline), "rtpjitterbuffer", "audio_buffer");
    GstElement *audio_depay = assert_element(GST_BIN(priv->rx_pipeline), "rtpopusdepay", "audio_depay");
    GstElement *audio_dec = assert_element(GST_BIN(priv->rx_pipeline), "opusdec", "audio_dec");
    GstElement *audio_volume = assert_element(GST_BIN(priv->rx_pipeline), "volume", "audio_volume");
    GstElement *audio_sink = assert_element(GST_BIN(priv->rx_pipeline), "autoaudiosink", "audio_sink");
    if(!audio_src || !audio_enc || !audio_pay || !rtp_demux || !audio_buffer || !audio_depay || !audio_dec || !audio_volume || !audio_sink)
        return session;

    // Receiving pipeline
    GstElement *rtp_src = rtp_src_new(dec_key, socket, "rtp_src");
    gst_bin_add(GST_BIN(priv->rx_pipeline), rtp_src);
    gst_element_link_many(audio_buffer, audio_depay, audio_dec, audio_volume, audio_sink, NULL);
    gst_element_link(rtp_src, rtp_demux);

    g_signal_connect(rtp_demux, "request-pt-map", (GCallback)request_pt_map_cb, session);
    g_signal_connect(rtp_demux, "new-payload-type", (GCallback)new_payload_type_cb, session);

#if !GST_CHECK_VERSION(1, 2, 6)
    gst_util_set_object_arg(G_OBJECT(audio_buffer), "mode", "none");
#endif

    // Transmitting pipeline
    GstElement *audio_tone = rtp_tone_new("audio_tone");
    GstElement *audio_sink_rtp = rtp_sink_new(enc_key, socket, "audio_sink");
    gst_bin_add_many(GST_BIN(priv->tx_pipeline), audio_tone, audio_sink_rtp, NULL);
    gst_element_link_many(audio_src, audio_tone, audio_enc, audio_pay, audio_sink_rtp, NULL);

    return session;
}

void rtp_session_bind_volume(RtpSession *session, gpointer source, const gchar *property)
{
    g_return_if_fail(RTP_IS_SESSION(session));
    RtpSessionPrivate *priv = rtp_session_get_instance_private(session);

    GstElement *audio_volume = gst_bin_get_by_name(GST_BIN(priv->rx_pipeline), "audio_volume");
    if(audio_volume)
    {
        g_object_bind_property(source, property, audio_volume, "volume", G_BINDING_SYNC_CREATE);
        gst_object_unref(audio_volume);
    }
}

void rtp_session_set_bitrate(RtpSession *session, guint bitrate, gboolean vbr)
{
    g_return_if_fail(RTP_IS_SESSION(session));
    RtpSessionPrivate *priv = rtp_session_get_instance_private(session);

    GstElement *audio_enc = gst_bin_get_by_name(GST_BIN(priv->tx_pipeline), "audio_enc");
    if(audio_enc)
    {
        g_object_set(audio_enc, "bitrate", bitrate, NULL);
        gst_util_set_object_arg(G_OBJECT(audio_enc), "bitrate-type", vbr ? "constrained-vbr" : "cbr");
        gst_object_unref(audio_enc);
    }
}

void rtp_session_set_tone(RtpSession *session, gboolean enable)
{
    g_return_if_fail(RTP_IS_SESSION(session));
    RtpSessionPrivate* priv = rtp_session_get_instance_private(session);

    GstElement *audio_tone = gst_bin_get_by_name(GST_BIN(priv->tx_pipeline), "audio_tone");
    if(audio_tone)
    {
        g_object_set(audio_tone, "enable", enable, NULL);
        gst_object_unref(audio_tone);
    }

    GstElement *audio_volume = gst_bin_get_by_name(GST_BIN(priv->rx_pipeline), "audio_volume");
    if(audio_volume)
    {
        g_object_set(audio_volume, "mute", enable, NULL);
        gst_object_unref(audio_volume);
    }
}

void rtp_session_play(RtpSession *session)
{
    g_return_if_fail(RTP_IS_SESSION(session));
    RtpSessionPrivate *priv = rtp_session_get_instance_private(session);

    gst_element_set_state(priv->rx_pipeline, GST_STATE_PLAYING);
    gst_element_set_state(priv->tx_pipeline, GST_STATE_PLAYING);
}

void rtp_session_destroy(RtpSession *session)
{
    g_return_if_fail(RTP_IS_SESSION(session));
    RtpSessionPrivate *priv = rtp_session_get_instance_private(session);

    gst_debug_bin_to_dot_file_with_ts(GST_BIN(priv->rx_pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "rx-pipeline");
    gst_debug_bin_to_dot_file_with_ts(GST_BIN(priv->tx_pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "tx-pipeline");

    gst_element_set_state(priv->rx_pipeline, GST_STATE_NULL);
    gst_element_set_state(priv->tx_pipeline, GST_STATE_NULL);

    g_object_unref(session);
}

static void rtp_session_finalize(GObject *obj)
{
    RtpSession *session = RTP_SESSION(obj);
    RtpSessionPrivate *priv = rtp_session_get_instance_private(session);

    gst_object_unref(priv->rx_pipeline);
    g_source_remove(priv->rx_watch);

    gst_object_unref(priv->tx_pipeline);
    g_source_remove(priv->tx_watch);

    G_OBJECT_CLASS(rtp_session_parent_class)->finalize(obj);
}

static GstCaps* request_pt_map_cb(GstElement *element, guint pt, gpointer arg)
{
    switch(pt)
    {
        case 96:
            return gst_caps_new_simple("application/x-rtp",
                    "media", G_TYPE_STRING, "audio",
                    "clock-rate", G_TYPE_INT, 48000,
                    "encoding-name", G_TYPE_STRING, "X-GST-OPUS-DRAFT-SPITTKA-00",
                    NULL);

        default:
            return NULL;
    }
}

static void new_payload_type_cb(GstElement *element, guint pt, GstPad *pad, gpointer arg)
{
    RtpSession *session = arg;
    RtpSessionPrivate *priv = rtp_session_get_instance_private(session);

    switch(pt)
    {
        case 96:
        {
            GstElement *audio_buffer = gst_bin_get_by_name(GST_BIN(priv->rx_pipeline), "audio_buffer");
            if(audio_buffer)
            {
                GstPad *sinkpad = gst_element_get_static_pad(audio_buffer, "sink");
                gst_pad_link(pad, sinkpad);
                gst_object_unref(sinkpad);
                gst_object_unref(audio_buffer);
            }

            break;
        }

        default:
        {
            g_debug("Unknown payload type %u", pt);

            GstElement *fakesink = gst_element_factory_make("fakesink", NULL);
            if(fakesink)
            {
                gst_bin_add(GST_BIN(priv->rx_pipeline), fakesink);

                GstPad *sinkpad = gst_element_get_static_pad(fakesink, "sink");
                gst_pad_link(pad, sinkpad);
                gst_object_unref(sinkpad);

                gst_element_sync_state_with_parent(fakesink);
            }
        }
    }
}

static gboolean bus_watch_cb(GstBus *bus, GstMessage *message, gpointer arg)
{
    RtpSession *session = arg;

    switch(message->type)
    {
        case GST_MESSAGE_ERROR:
        {
            g_autoptr(GError) error = NULL;
            gst_message_parse_error(message, &error, NULL);
            g_debug("%s", error->message);

            g_signal_emit(session, rtp_session_signals[SIGNAL_HANGUP], 0);
            break;
        }

        default:
            break;
    }

    return TRUE;
}
