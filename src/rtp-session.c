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

#define DEFAULT_AUDIO_SOURCE        "pulsesrc"
#define DEFAULT_AUDIO_SINK          "pulsesink"
#define DEFAULT_AUDIO_ENCODER       "opusenc"
#define DEFAULT_AUDIO_DECODER       "opusdec"
#define DEFAULT_AUDIO_PAYLOADER     "rtpopuspay"
#define DEFAULT_AUDIO_DEPAYLOADER   "rtpopusdepay"

enum
{
    PROP_0,
    PROP_AUDIO_SOURCE,
    PROP_AUDIO_SINK,
    PROP_AUDIO_ENCODER,
    PROP_AUDIO_DECODER,
    PROP_AUDIO_PAYLOADER,
    PROP_AUDIO_DEPAYLOADER
};

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

    gchar *audio_source, *audio_sink, *audio_encoder, *audio_decoder, *audio_payloader, *audio_depayloader;
};

static guint rtp_session_signals[LAST_SIGNAL];

G_DEFINE_TYPE_WITH_PRIVATE(RtpSession, rtp_session, G_TYPE_OBJECT)

static void rtp_session_set_property(GObject *obj, guint prop_id, const GValue *value, GParamSpec *pspec);
static void rtp_session_get_property(GObject *obj, guint prop_id, GValue *value, GParamSpec *pspec);
static void rtp_session_finalize(GObject *obj);

static GstCaps* request_pt_map_cb(GstElement *element, guint pt, gpointer arg);
static void new_payload_type_cb(GstElement *element, guint pt, GstPad *pad, gpointer arg);
static gboolean bus_watch_cb(GstBus *bus, GstMessage *message, gpointer arg);

static inline GstElement* assert_element(const gchar *factory, const gchar *name)
{
    GstElement *element = gst_element_factory_make(factory, name);
    if(element == NULL) g_error(_("Missing GStreamer element: %s"), name);

    return element;
}

static void rtp_session_class_init(RtpSessionClass *session_class)
{
    GObjectClass *object_class = (GObjectClass*)session_class;
    object_class->finalize = rtp_session_finalize;
    object_class->set_property = rtp_session_set_property;
    object_class->get_property = rtp_session_get_property;

    g_object_class_install_property(object_class, PROP_AUDIO_SOURCE,
            g_param_spec_string("audio-source", "Audio source", "Audio source element", DEFAULT_AUDIO_SOURCE,
                    G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    g_object_class_install_property(object_class, PROP_AUDIO_SINK,
            g_param_spec_string("audio-sink", "Audio sink", "Audio sink element", DEFAULT_AUDIO_SINK,
                    G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    g_object_class_install_property(object_class, PROP_AUDIO_ENCODER,
            g_param_spec_string("audio-encoder", "Audio encoder", "Audio encoder element", DEFAULT_AUDIO_ENCODER,
                    G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    g_object_class_install_property(object_class, PROP_AUDIO_DECODER,
            g_param_spec_string("audio-decoder", "Audio decoder", "Audio decoder element", DEFAULT_AUDIO_DECODER,
                    G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    g_object_class_install_property(object_class, PROP_AUDIO_PAYLOADER,
            g_param_spec_string("audio-payloader", "Audio payloader", "Audio payloader element", DEFAULT_AUDIO_PAYLOADER,
                    G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    g_object_class_install_property(object_class, PROP_AUDIO_DEPAYLOADER,
            g_param_spec_string("audio-depayloader", "Audio depayloader", "Audio depayloader element", DEFAULT_AUDIO_DEPAYLOADER,
                    G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

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

static void rtp_session_set_property(GObject *obj, guint prop_id, const GValue *value, GParamSpec *pspec)
{
    RtpSession *session = RTP_SESSION(obj);
    RtpSessionPrivate *priv = rtp_session_get_instance_private(session);

    switch(prop_id)
    {
        case PROP_AUDIO_SOURCE:
            g_free(priv->audio_source);
            priv->audio_source = g_strdup(g_value_get_string(value));
            break;

        case PROP_AUDIO_SINK:
            g_free(priv->audio_sink);
            priv->audio_sink = g_strdup(g_value_get_string(value));
            break;

        case PROP_AUDIO_ENCODER:
            g_free(priv->audio_encoder);
            priv->audio_encoder = g_strdup(g_value_get_string(value));
            break;

        case PROP_AUDIO_DECODER:
            g_free(priv->audio_decoder);
            priv->audio_decoder = g_strdup(g_value_get_string(value));
            break;

        case PROP_AUDIO_PAYLOADER:
            g_free(priv->audio_payloader);
            priv->audio_payloader = g_strdup(g_value_get_string(value));
            break;

        case PROP_AUDIO_DEPAYLOADER:
            g_free(priv->audio_depayloader);
            priv->audio_depayloader = g_strdup(g_value_get_string(value));
            break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, prop_id, pspec);
    }
}

static void rtp_session_get_property(GObject *obj, guint prop_id, GValue *value, GParamSpec *pspec)
{
    RtpSession *session = RTP_SESSION(obj);
    RtpSessionPrivate *priv = rtp_session_get_instance_private(session);

    switch(prop_id)
    {
        case PROP_AUDIO_SOURCE:
            g_value_set_string(value, priv->audio_source);
            break;

        case PROP_AUDIO_SINK:
            g_value_set_string(value, priv->audio_sink);
            break;

        case PROP_AUDIO_ENCODER:
            g_value_set_string(value, priv->audio_encoder);
            break;

        case PROP_AUDIO_DECODER:
            g_value_set_string(value, priv->audio_decoder);
            break;

        case PROP_AUDIO_PAYLOADER:
            g_value_set_string(value, priv->audio_payloader);
            break;

        case PROP_AUDIO_DEPAYLOADER:
            g_value_set_string(value, priv->audio_depayloader);
            break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, prop_id, pspec);
    }
}

RtpSession* rtp_session_new()
{
    return g_object_new(RTP_TYPE_SESSION, NULL, NULL);
}

void rtp_session_prepare(RtpSession *session, GSocket *socket, DhtKey *enc_key, DhtKey *dec_key)
{
    g_return_if_fail(RTP_IS_SESSION(session));
    RtpSessionPrivate *priv = rtp_session_get_instance_private(session);

    // Receiving pipeline
    GstElement *rtp_src = rtp_src_new(dec_key, socket, "rtp_src");
    GstElement *rtp_demux = assert_element("rtpptdemux", "rtp_demux");
    GstElement *audio_buffer = assert_element("rtpjitterbuffer", "audio_buffer");
    GstElement *audio_depay = assert_element(priv->audio_depayloader, "audio_depay");
    GstElement *audio_dec = assert_element(priv->audio_decoder, "audio_dec");
    GstElement *audio_volume = assert_element("volume", "audio_volume");
    GstElement *audio_sink = assert_element(priv->audio_sink, "audio_sink");
    gst_bin_add_many(GST_BIN(priv->rx_pipeline), rtp_src, rtp_demux, audio_buffer, audio_depay, audio_dec, audio_volume, audio_sink, NULL);
    gst_element_link_many(audio_buffer, audio_depay, audio_dec, audio_volume, audio_sink, NULL);
    gst_element_link(rtp_src, rtp_demux);

    g_signal_connect(rtp_demux, "request-pt-map", (GCallback)request_pt_map_cb, session);
    g_signal_connect(rtp_demux, "new-payload-type", (GCallback)new_payload_type_cb, session);

#if !GST_CHECK_VERSION(1, 2, 6)
    gst_util_set_object_arg(G_OBJECT(audio_buffer), "mode", "none");
#endif

    // Transmitting pipeline
    GstElement *audio_src = assert_element(priv->audio_source, "audio_src");
    GstElement *audio_tone = rtp_tone_new("audio_tone");
    GstElement *audio_enc = assert_element(priv->audio_encoder, "audio_enc");
    GstElement *audio_pay = assert_element(priv->audio_payloader, "audio_pay");
    GstElement *audio_sink_rtp = rtp_sink_new(enc_key, socket, "audio_sink");
    gst_bin_add_many(GST_BIN(priv->tx_pipeline), audio_src, audio_tone, audio_enc, audio_pay, audio_sink_rtp, NULL);
    gst_element_link_many(audio_src, audio_tone, audio_enc, audio_pay, audio_sink_rtp, NULL);
}

void rtp_session_echo_cancel(RtpSession *session)
{
    g_return_if_fail(RTP_IS_SESSION(session));
    RtpSessionPrivate* priv = rtp_session_get_instance_private(session);

    GstStructure *props = gst_structure_new("props",
            "filter.want", G_TYPE_STRING, "echo-cancel",
            NULL);

    GstElement *audio_src = gst_bin_get_by_name(GST_BIN(priv->tx_pipeline), "audio_src");
    if(audio_src)
    {
        g_object_set(audio_src, "stream-properties", props, NULL);
        g_object_unref(audio_src);
    }

    GstElement *audio_sink = gst_bin_get_by_name(GST_BIN(priv->rx_pipeline), "audio_sink");
    if(audio_sink)
    {
        g_object_set(audio_sink, "stream-properties", props, NULL);
        g_object_unref(audio_sink);
    }

    gst_structure_free(props);
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
}

void rtp_session_set_volume(RtpSession *session, gdouble value)
{
    g_return_if_fail(RTP_IS_SESSION(session));
    RtpSessionPrivate *priv = rtp_session_get_instance_private(session);

    GstElement *audio_volume = gst_bin_get_by_name(GST_BIN(priv->rx_pipeline), "audio_volume");
    if(audio_volume)
    {
        g_object_set(audio_volume, "volume", value, NULL);
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

void rtp_session_start(RtpSession *session)
{
    g_return_if_fail(RTP_IS_SESSION(session));
    RtpSessionPrivate *priv = rtp_session_get_instance_private(session);

    gst_element_set_state(priv->rx_pipeline, GST_STATE_PLAYING);
    gst_element_set_state(priv->tx_pipeline, GST_STATE_PLAYING);
}

void rtp_session_stop(RtpSession *session)
{
    g_return_if_fail(RTP_IS_SESSION(session));
    RtpSessionPrivate *priv = rtp_session_get_instance_private(session);

    gst_debug_bin_to_dot_file_with_ts(GST_BIN(priv->rx_pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "rx-pipeline");
    gst_debug_bin_to_dot_file_with_ts(GST_BIN(priv->tx_pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "tx-pipeline");

    gst_element_set_state(priv->rx_pipeline, GST_STATE_NULL);
    gst_element_set_state(priv->tx_pipeline, GST_STATE_NULL);
}

static void rtp_session_finalize(GObject *obj)
{
    RtpSession *session = RTP_SESSION(obj);
    RtpSessionPrivate *priv = rtp_session_get_instance_private(session);

    gst_object_unref(priv->rx_pipeline);
    g_source_remove(priv->rx_watch);

    gst_object_unref(priv->tx_pipeline);
    g_source_remove(priv->tx_watch);

    g_free(priv->audio_source);
    g_free(priv->audio_sink);
    g_free(priv->audio_encoder);
    g_free(priv->audio_decoder);
    g_free(priv->audio_payloader);
    g_free(priv->audio_depayloader);

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
#if GST_CHECK_VERSION(1, 2, 8)
                    "encoding-name", G_TYPE_STRING, "OPUS",
#else
                    "encoding-name", G_TYPE_STRING, "X-GST-OPUS-DRAFT-SPITTKA-00",
#endif
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
            GstPad *sinkpad = gst_element_get_static_pad(audio_buffer, "sink");
            gst_pad_link(pad, sinkpad);
            gst_object_unref(sinkpad);
            gst_object_unref(audio_buffer);
            break;
        }

        default:
        {
            g_debug("Unknown payload type %u", pt);
            GstElement *fakesink = gst_element_factory_make("fakesink", NULL);
            gst_bin_add(GST_BIN(priv->rx_pipeline), fakesink);

            GstPad *sinkpad = gst_element_get_static_pad(fakesink, "sink");
            gst_pad_link(pad, sinkpad);
            gst_object_unref(sinkpad);

            gst_element_sync_state_with_parent(fakesink);
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
