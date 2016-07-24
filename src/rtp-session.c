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

#define G_LOG_DOMAIN "RTP"

#ifdef HAVE_CANBERRA
#include <canberra-gtk.h>
#endif /* HAVE_CANBERRA */

#include <glib/gi18n.h>
#include "rtp-src.h"
#include "rtp-sink.h"
#include "rtp-session.h"

#define SOCKET_TIMEOUT 1 // 1 second
#define KEEPALIVE_PERIOD 100 // 0.1 seconds

#define REPLAY_PERIOD 5000 // 5 seconds

enum
{
    PROP_0,
    PROP_VIDEO_BITRATE,
    PROP_AUDIO_BITRATE,
    PROP_VOLUME
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

    GSocket *socket;
    guint io_timeout_source;

    gboolean enable_video, on_hold;

#ifdef HAVE_CANBERRA
    ca_context *ca_ctx;
    guint ca_timeout_source;
#endif /* HAVE_CANBERRA */
};

static guint rtp_session_signals[LAST_SIGNAL];

G_DEFINE_TYPE_WITH_PRIVATE(RtpSession, rtp_session, G_TYPE_OBJECT)

static void rtp_session_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec);
static void rtp_session_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec);
static void rtp_session_finalize(GObject *obj);

static GstCaps* request_pt_map_cb(GstElement *element, guint pt, gpointer arg);
static void new_payload_type_cb(GstElement *element, guint pt, GstPad *pad, gpointer arg);
static gboolean bus_watch_cb(GstBus *bus, GstMessage *message, gpointer arg);
static gboolean accept_cb(gpointer arg);
static gboolean io_timeout_cb(gpointer arg);

#ifdef HAVE_CANBERRA
static gboolean ca_timeout_cb(gpointer arg);
#endif /* HAVE_CANBERRA */

static inline GstElement* assert_element(GstBin *bin, const gchar *factory, const gchar *name)
{
    GstElement *element = gst_element_factory_make(factory, name);
    if(element)
        gst_bin_add(bin, element);
    else
        g_warning(_("Missing GStreamer element: %s"), factory);

    return element;
}

static void rtp_session_class_init(RtpSessionClass *session_class)
{
    GObjectClass *object_class = (GObjectClass*)session_class;
    object_class->set_property = rtp_session_set_property;
    object_class->get_property = rtp_session_get_property;
    object_class->finalize = rtp_session_finalize;

    g_object_class_install_property(object_class, PROP_VIDEO_BITRATE,
       g_param_spec_int("video-bitrate", "Video bitrate", "Video encoder bitrate", 0, G_MAXINT, 256000,
               G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    g_object_class_install_property(object_class, PROP_AUDIO_BITRATE,
       g_param_spec_int("audio-bitrate", "Audio bitrate", "Audio encoder bitrate", 4000, 650000, 64000,
               G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    g_object_class_install_property(object_class, PROP_VOLUME,
       g_param_spec_double("volume", "Volume", "Playback volume", 0, 1, 1,
               G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

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

#ifdef HAVE_CANBERRA
    ca_context_create(&priv->ca_ctx);
#endif /* HAVE_CANBERRA */
}

static void rtp_session_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
    RtpSession *session = RTP_SESSION(object);
    RtpSessionPrivate *priv = rtp_session_get_instance_private(session);

    switch(prop_id)
    {
        case PROP_VIDEO_BITRATE:
            if(priv->enable_video)
                gst_child_proxy_set_property(GST_CHILD_PROXY(priv->tx_pipeline), "video_enc::target-bitrate", value);

            break;

        case PROP_AUDIO_BITRATE:
            gst_child_proxy_set_property(GST_CHILD_PROXY(priv->tx_pipeline), "audio_enc::bitrate", value);
            break;

        case PROP_VOLUME:
            gst_child_proxy_set_property(GST_CHILD_PROXY(priv->rx_pipeline), "volume::volume", value);
            break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

static void rtp_session_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
    RtpSession *session = RTP_SESSION(object);
    RtpSessionPrivate *priv = rtp_session_get_instance_private(session);

    switch(prop_id)
    {
        case PROP_VIDEO_BITRATE:
            if(priv->enable_video)
                gst_child_proxy_get_property(GST_CHILD_PROXY(priv->tx_pipeline), "video_enc::target-bitrate", value);

            break;

        case PROP_AUDIO_BITRATE:
            gst_child_proxy_get_property(GST_CHILD_PROXY(priv->tx_pipeline), "audio_enc::bitrate", value);
            break;

        case PROP_VOLUME:
            gst_child_proxy_get_property(GST_CHILD_PROXY(priv->rx_pipeline), "volume::volume", value);
            break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

RtpSession* rtp_session_new(GSocket *socket, DhtKey *enc_key, DhtKey *dec_key, gboolean enable_video)
{
    RtpSession *session = g_object_new(RTP_TYPE_SESSION, NULL);
    RtpSessionPrivate *priv = rtp_session_get_instance_private(session);

    g_socket_set_timeout(socket, SOCKET_TIMEOUT);
    priv->socket = g_object_ref(socket);

    GstElement *rtp_src = rtp_src_new(dec_key, socket, "rtp_src");
    gst_bin_add(GST_BIN(priv->rx_pipeline), rtp_src);

    GstElement *rtp_demux = assert_element(GST_BIN(priv->rx_pipeline), "rtpptdemux", "rtp_demux");
    g_signal_connect(rtp_demux, "request-pt-map", (GCallback)request_pt_map_cb, session);
    g_signal_connect(rtp_demux, "new-payload-type", (GCallback)new_payload_type_cb, session);

    gst_element_link(rtp_src, rtp_demux);

    GstElement *audio_buffer = assert_element(GST_BIN(priv->rx_pipeline), "rtpjitterbuffer", "audio_buffer");
    GstElement *audio_depay = assert_element(GST_BIN(priv->rx_pipeline), "rtpopusdepay", "audio_depay");
    GstElement *audio_dec = assert_element(GST_BIN(priv->rx_pipeline), "opusdec", "audio_dec");
    GstElement *volume = assert_element(GST_BIN(priv->rx_pipeline), "volume", "volume");
    GstElement *audio_sink = assert_element(GST_BIN(priv->rx_pipeline), "autoaudiosink", "audio_sink");

    gst_element_link_many(audio_buffer, audio_depay, audio_dec, volume, audio_sink, NULL);

    GstElement *audio_src = assert_element(GST_BIN(priv->tx_pipeline), "autoaudiosrc", "audio_src");
    GstElement *audio_enc = assert_element(GST_BIN(priv->tx_pipeline), "opusenc", "audio_enc");
    GstElement *audio_pay = assert_element(GST_BIN(priv->tx_pipeline), "rtpopuspay", "audio_pay");
    GstElement *audio_rtp_sink = rtp_sink_new(enc_key, socket, "audio_sink");
    gst_bin_add(GST_BIN(priv->tx_pipeline), audio_rtp_sink);

#if !GST_CHECK_VERSION(1, 2, 8)
    g_object_set(audio_enc, "audio", FALSE, NULL);
#else
    gst_util_set_object_arg(G_OBJECT(audio_enc), "audio-type", "voice");
#endif

    gst_element_link_many(audio_src, audio_enc, audio_pay, audio_rtp_sink, NULL);

    if(enable_video)
    {
        priv->enable_video = TRUE;
        GstElement *video_src = assert_element(GST_BIN(priv->tx_pipeline), "autovideosrc", "video_src");
        GstElement *video_enc = assert_element(GST_BIN(priv->tx_pipeline), "vp8enc", "video_enc");
        GstElement *video_pay = assert_element(GST_BIN(priv->tx_pipeline), "rtpvp8pay", "video_pay");
        GstElement *video_rtp_sink = rtp_sink_new(enc_key, socket, "video_sink");
        gst_bin_add(GST_BIN(priv->tx_pipeline), video_rtp_sink);

        g_object_set(video_enc, "deadline", 1, "cpu-used", 5, NULL);
        g_object_set(video_pay, "pt", 97, NULL);

        gst_element_link_many(video_src, video_enc, video_pay, video_rtp_sink, NULL);
    }

    return session;
}

void rtp_session_launch(RtpSession *session, gboolean on_hold)
{
    g_return_if_fail(RTP_IS_SESSION(session));
    RtpSessionPrivate *priv = rtp_session_get_instance_private(session);

#ifdef HAVE_CANBERRA
    priv->ca_timeout_source = g_timeout_add(REPLAY_PERIOD, ca_timeout_cb, session);
    ca_context_play(priv->ca_ctx, 1, CA_PROP_EVENT_ID, on_hold ? "phone-incoming-call" : "phone-outgoing-calling", NULL);
#endif /* HAVE_CANBERRA */

    if(on_hold)
    {
        priv->on_hold = TRUE;
        gst_child_proxy_set(GST_CHILD_PROXY(priv->rx_pipeline), "rtp_src::enable", FALSE, NULL);
    }

    if(priv->io_timeout_source == 0)
    {
        priv->io_timeout_source = g_timeout_add(KEEPALIVE_PERIOD, io_timeout_cb, session);

        gst_element_set_state(priv->tx_pipeline, GST_STATE_READY);
        gst_element_set_state(priv->rx_pipeline, GST_STATE_PLAYING);
    }
}

void rtp_session_accept(RtpSession *session)
{
    g_return_if_fail(RTP_IS_SESSION(session));
    RtpSessionPrivate *priv = rtp_session_get_instance_private(session);

    if(priv->on_hold)
    {
        priv->on_hold = FALSE;
        gst_child_proxy_set(GST_CHILD_PROXY(priv->rx_pipeline), "rtp_src::enable", TRUE, NULL);
    }

    if(priv->io_timeout_source > 0)
    {
        g_source_remove(priv->io_timeout_source);
        priv->io_timeout_source = 0;

        gst_element_set_state(priv->tx_pipeline, GST_STATE_PLAYING);
    }

#ifdef HAVE_CANBERRA
    if(priv->ca_timeout_source > 0)
    {
        g_source_remove(priv->ca_timeout_source);
        priv->ca_timeout_source = 0;

        ca_context_cancel(priv->ca_ctx, 1);
    }
#endif /* HAVE_CANBERRA */
}

void rtp_session_destroy(RtpSession *session)
{
    g_return_if_fail(RTP_IS_SESSION(session));
    RtpSessionPrivate *priv = rtp_session_get_instance_private(session);

    gst_debug_bin_to_dot_file_with_ts(GST_BIN(priv->rx_pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "rx-pipeline");
    gst_debug_bin_to_dot_file_with_ts(GST_BIN(priv->tx_pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "tx-pipeline");

    gst_element_set_state(priv->rx_pipeline, GST_STATE_NULL);
    gst_element_set_state(priv->tx_pipeline, GST_STATE_NULL);

    if(priv->io_timeout_source > 0)
        g_source_remove(priv->io_timeout_source);

#ifdef HAVE_CANBERRA
    if(priv->ca_timeout_source > 0)
    {
        g_source_remove(priv->ca_timeout_source);
        ca_context_cancel(priv->ca_ctx, 1);
    }
#endif /* HAVE_CANBERRA */

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

    g_object_unref(priv->socket);

#ifdef HAVE_CANBERRA
    ca_context_destroy(priv->ca_ctx);
#endif /* HAVE_CANBERRA */

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
#if GST_CHECK_VERSION(1, 2, 6)
                    "encoding-name", G_TYPE_STRING, "OPUS",
#else
                    "encoding-name", G_TYPE_STRING, "X-GST-OPUS-DRAFT-SPITTKA-00",
#endif
                    NULL);

        case 97:
            return gst_caps_new_simple("application/x-rtp",
                    "media", G_TYPE_STRING, "video",
                    "clock-rate", G_TYPE_INT, 90000,
#if GST_CHECK_VERSION(1, 2, 6)
                    "encoding-name", G_TYPE_STRING, "VP8",
#else
                    "encoding-name", G_TYPE_STRING, "VP8-DRAFT-IETF-01",
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

            g_idle_add(accept_cb, session);
            break;
        }

        case 97:
        {
            GstElement *video_buffer = assert_element(GST_BIN(priv->rx_pipeline), "rtpjitterbuffer", "video_buffer");
            GstElement *video_depay = assert_element(GST_BIN(priv->rx_pipeline), "rtpvp8depay", "video_depay");
            GstElement *video_dec = assert_element(GST_BIN(priv->rx_pipeline), "vp8dec", "video_dec");
            GstElement *video_sink = assert_element(GST_BIN(priv->rx_pipeline), "autovideosink", "video_sink");
            gst_element_link_many(video_buffer, video_depay, video_dec, video_sink, NULL);

            GstPad *sinkpad = gst_element_get_static_pad(video_buffer, "sink");
            gst_pad_link(pad, sinkpad);
            gst_object_unref(sinkpad);

            gst_element_sync_state_with_parent(video_buffer);
            gst_element_sync_state_with_parent(video_depay);
            gst_element_sync_state_with_parent(video_dec);
            gst_element_sync_state_with_parent(video_sink);
            break;
        }

        default:
        {
            g_debug("Unknown payload %u", pt);

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
    RtpSessionPrivate *priv = rtp_session_get_instance_private(session);

    switch(message->type)
    {
        case GST_MESSAGE_ERROR:
        {
            g_autoptr(GError) error = NULL;
            gst_message_parse_error(message, &error, NULL);
            g_debug("%s", error->message);

            if(g_str_has_prefix(message->src->name, "video_") && (priv->io_timeout_source > 0))
            {
                if(priv->enable_video)
                {
                    priv->enable_video = FALSE;

                    GstElement *video_src = gst_bin_get_by_name(GST_BIN(priv->tx_pipeline), "video_src");
                    GstElement *video_enc = gst_bin_get_by_name(GST_BIN(priv->tx_pipeline), "video_enc");
                    GstElement *video_pay = gst_bin_get_by_name(GST_BIN(priv->tx_pipeline), "video_pay");
                    GstElement *video_sink = gst_bin_get_by_name(GST_BIN(priv->tx_pipeline), "video_sink");

                    // Try to recover from broken video pipeline
                    gst_element_set_state(priv->tx_pipeline, GST_STATE_NULL);
                    gst_bin_remove_many(GST_BIN(priv->tx_pipeline), video_src, video_enc, video_pay, video_sink, NULL);
                    gst_element_set_state(priv->tx_pipeline, GST_STATE_READY);

                    gst_object_unref(video_src);
                    gst_object_unref(video_enc);
                    gst_object_unref(video_pay);
                    gst_object_unref(video_sink);
                }

                break;
            }

            g_signal_emit(session, rtp_session_signals[SIGNAL_HANGUP], 0);
            break;
        }

        default:
            break;
    }

    return TRUE;
}

static gboolean accept_cb(gpointer arg)
{
    RtpSession *session = arg;

    rtp_session_accept(session);

    return G_SOURCE_REMOVE;
}

static gboolean io_timeout_cb(gpointer arg)
{
    RtpSession *session = arg;
    RtpSessionPrivate *priv = rtp_session_get_instance_private(session);

    gchar buffer[0];
    g_socket_send(priv->socket, buffer, 0, NULL, NULL);

    return G_SOURCE_CONTINUE;
}

#ifdef HAVE_CANBERRA
static gboolean ca_timeout_cb(gpointer arg)
{
    RtpSession *session = arg;
    RtpSessionPrivate *priv = rtp_session_get_instance_private(session);

    gboolean is_playing = FALSE;
    ca_context_playing(priv->ca_ctx, 1, &is_playing);

    if(!is_playing)
        ca_context_play(priv->ca_ctx, 1, CA_PROP_EVENT_ID, priv->on_hold ? "phone-incoming-call" : "phone-outgoing-calling", NULL);

    return G_SOURCE_CONTINUE;
}
#endif /* HAVE_CANBERRA */
