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

#define G_LOG_DOMAIN "Ringback"

#include <gst/math-compat.h>
#include "ringback.h"

#define DEFAULT_ENABLED     FALSE
#define DEFAULT_TONE_AMPL   0.75
#define DEFAULT_TONE_FREQ   425
#define DEFAULT_TONE_DUR    1
#define DEFAULT_PAUSE_DUR   4

enum
{
    PROP_0,
    PROP_ENABLED,
    PROP_TONE_AMPLITUDE,
    PROP_TONE_FREQUENCY,
    PROP_TONE_DURATION,
    PROP_PAUSE_DURATION,
};

G_DEFINE_TYPE(GstRingback, gst_ringback, GST_TYPE_AUDIO_FILTER)

static void gst_ringback_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec);
static void gst_ringback_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec);
static gboolean gst_ringback_setup(GstAudioFilter *filter, const GstAudioInfo *info);
static GstFlowReturn gst_ringback_transform_ip(GstBaseTransform *trans, GstBuffer *buf);

static void gst_ringback_class_init(GstRingbackClass *klass)
{
    GObjectClass *object_class = (GObjectClass*)klass;
    object_class->set_property = gst_ringback_set_property;
    object_class->get_property = gst_ringback_get_property;

    g_object_class_install_property(object_class, PROP_ENABLED,
        g_param_spec_boolean("enabled", "Enabled", "Enable tone generation", DEFAULT_ENABLED, G_PARAM_READWRITE));

    g_object_class_install_property(object_class, PROP_TONE_AMPLITUDE,
        g_param_spec_float("tone-amplitude", "Tone amplitude", "Tone amplitude", 0, 1, DEFAULT_TONE_AMPL, G_PARAM_READWRITE));

    g_object_class_install_property(object_class, PROP_TONE_FREQUENCY,
        g_param_spec_float("tone-frequency", "Tone frequency", "Tone frequency (Hz)", 0, G_MAXFLOAT, DEFAULT_TONE_FREQ, G_PARAM_READWRITE));

    g_object_class_install_property(object_class, PROP_TONE_DURATION,
        g_param_spec_float("tone-duration", "Tone duration", "Tone duration (s)", 0, G_MAXFLOAT, DEFAULT_TONE_DUR, G_PARAM_READWRITE));

    g_object_class_install_property(object_class, PROP_PAUSE_DURATION,
        g_param_spec_float("pause-duration", "Pause duration", "Pause duration (s)", 0, G_MAXFLOAT, DEFAULT_PAUSE_DUR, G_PARAM_READWRITE));

    GstElementClass *element_class = (GstElementClass*)klass;
    gst_element_class_set_details_simple(element_class,
        "Ringback", "Filter/Audio", "Generates tone over audio payload", "Martin Jaros <xjaros32@stud.feec.vutbr.cz>");

    GstBaseTransformClass *tranform_class = (GstBaseTransformClass*)klass;
    tranform_class->transform_ip = gst_ringback_transform_ip;

    GstAudioFilterClass *filter_class = (GstAudioFilterClass*)klass;
    filter_class->setup = gst_ringback_setup;

    GstCaps *caps = gst_caps_from_string(GST_AUDIO_CAPS_MAKE("S16LE")",layout=(string)interleaved");
    gst_audio_filter_class_add_pad_templates(filter_class, caps);
    gst_caps_unref(caps);
}

static void gst_ringback_init(GstRingback *ringback)
{
    ringback->enabled = DEFAULT_ENABLED;
    ringback->tone_ampl = DEFAULT_TONE_AMPL;
    ringback->tone_freq = DEFAULT_TONE_FREQ;
    ringback->tone_dur = DEFAULT_TONE_DUR;
    ringback->pause_dur = DEFAULT_PAUSE_DUR;
}

static void gst_ringback_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
    GstRingback *ringback = GST_RINGBACK(object);

    switch(prop_id)
    {
        case PROP_ENABLED:
            ringback->enabled = g_value_get_boolean(value);
            break;

        case PROP_TONE_AMPLITUDE:
            ringback->tone_ampl = g_value_get_float(value);
            break;

        case PROP_TONE_FREQUENCY:
            ringback->tone_freq = g_value_get_float(value);
            break;

        case PROP_TONE_DURATION:
            ringback->tone_dur = g_value_get_float(value);
            break;

        case PROP_PAUSE_DURATION:
            ringback->pause_dur = g_value_get_float(value);
            break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

static void gst_ringback_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
    GstRingback *ringback = GST_RINGBACK(object);

    switch(prop_id)
    {
        case PROP_ENABLED:
            g_value_set_boolean(value, ringback->enabled);
            break;

        case PROP_TONE_AMPLITUDE:
            g_value_set_float(value, ringback->tone_ampl);
            break;

        case PROP_TONE_FREQUENCY:
            g_value_set_float(value, ringback->tone_freq);
            break;

        case PROP_TONE_DURATION:
            g_value_set_float(value, ringback->tone_dur);
            break;

        case PROP_PAUSE_DURATION:
            g_value_set_float(value, ringback->pause_dur);
            break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

static gboolean gst_ringback_setup(GstAudioFilter *filter, const GstAudioInfo *info)
{
    GstRingback *ringback = GST_RINGBACK(filter);
    ringback->channels = info->channels;
    ringback->rate = info->rate;
    return TRUE;
}

static GstFlowReturn gst_ringback_transform_ip(GstBaseTransform *trans, GstBuffer *buf)
{
    GstRingback *ringback = GST_RINGBACK(trans);
    if(!ringback->enabled) return GST_FLOW_OK;

    GstMapInfo info = { };
    if(gst_buffer_map(buf, &info, GST_MAP_READWRITE))
    {
        gint16 *ptr = (gint16*)info.data;
        while((gpointer)ptr < (gpointer)info.data + info.size)
        {
            ringback->time = ringback->time > ringback->tone_dur + ringback->pause_dur ? 0 :
                ringback->time + 1 / ringback->rate;

            gint16 value = ringback->time > ringback->tone_dur ? 0 :
                G_MAXINT16 * sinf(2 * M_PI * ringback->tone_freq * ringback->time) * ringback->tone_ampl;

            gsize channels = ringback->channels;
            while(channels--) *ptr++ = value;
        }

        gst_buffer_unmap(buf, &info);
        return GST_FLOW_OK;
    }

    return GST_FLOW_ERROR;
}
