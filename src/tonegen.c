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

#include "tonegen.h"

#define PI 3.14159265358979323846

#define DEFAULT_ENABLED         FALSE
#define DEFAULT_TONE_AMPLITUDE  0.5
#define DEFAULT_TONE_FREQUENCY  425
#define DEFAULT_TONE_DURATION   1
#define DEFAULT_PAUSE_DURATION  4

enum
{
    PROP_0,
    PROP_ENABLED,
    PROP_TONE_AMPLITUDE,
    PROP_TONE_FREQUENCY,
    PROP_TONE_DURATION,
    PROP_PAUSE_DURATION,
};

G_DEFINE_TYPE(GstToneGen, gst_tonegen, GST_TYPE_AUDIO_FILTER)

static void gst_tonegen_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec);
static void gst_tonegen_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec);
static gboolean gst_tonegen_setup(GstAudioFilter *filter, const GstAudioInfo *info);
static GstFlowReturn gst_tonegen_transform_ip(GstBaseTransform *transform, GstBuffer *buffer);

static void gst_tonegen_class_init(GstToneGenClass *klass)
{
    GObjectClass *object_class = (GObjectClass*)klass;
    object_class->set_property = gst_tonegen_set_property;
    object_class->get_property = gst_tonegen_get_property;

    g_object_class_install_property(object_class, PROP_ENABLED,
        g_param_spec_boolean("enabled", "Enabled", "Enable tone generation", DEFAULT_ENABLED, G_PARAM_READWRITE));

    g_object_class_install_property(object_class, PROP_TONE_AMPLITUDE,
        g_param_spec_float("tone-amplitude", "Tone amplitude", "Tone amplitude", 0, 1, DEFAULT_TONE_AMPLITUDE, G_PARAM_READWRITE));

    g_object_class_install_property(object_class, PROP_TONE_FREQUENCY,
        g_param_spec_float("tone-frequency", "Tone frequency", "Tone frequency (Hz)", 0, G_MAXFLOAT, DEFAULT_TONE_FREQUENCY, G_PARAM_READWRITE));

    g_object_class_install_property(object_class, PROP_TONE_DURATION,
        g_param_spec_float("tone-duration", "Tone duration", "Tone duration (s)", 0, G_MAXFLOAT, DEFAULT_TONE_DURATION, G_PARAM_READWRITE));

    g_object_class_install_property(object_class, PROP_PAUSE_DURATION,
        g_param_spec_float("pause-duration", "Pause duration", "Pause duration (s)", 0, G_MAXFLOAT, DEFAULT_PAUSE_DURATION, G_PARAM_READWRITE));

    GstElementClass *element_class = (GstElementClass*)klass;
    gst_element_class_set_details_simple(element_class,
        "ToneGen", "Filter/Audio", "Generates tones", "Martin Jaros <xjaros32@stud.feec.vutbr.cz>");

    GstBaseTransformClass *tranform_class = (GstBaseTransformClass*)klass;
    tranform_class->transform_ip = gst_tonegen_transform_ip;

    GstAudioFilterClass *filter_class = (GstAudioFilterClass*)klass;
    filter_class->setup = gst_tonegen_setup;

    GstCaps *caps = gst_caps_from_string(GST_AUDIO_CAPS_MAKE("S16LE")",layout=(string)interleaved");
    gst_audio_filter_class_add_pad_templates(filter_class, caps);
    gst_caps_unref(caps);
}

static void gst_tonegen_init(GstToneGen *tonegen)
{
    tonegen->enabled = DEFAULT_ENABLED;
    tonegen->tone_frequency = DEFAULT_TONE_FREQUENCY;
    tonegen->tone_amplitude = DEFAULT_TONE_AMPLITUDE;
    tonegen->tone_duration = DEFAULT_TONE_DURATION;
    tonegen->pause_duration = DEFAULT_PAUSE_DURATION;
}

static void gst_tonegen_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
    GstToneGen *tonegen = GST_TONEGEN(object);

    switch(prop_id)
    {
        case PROP_ENABLED:
            tonegen->enabled = g_value_get_boolean(value);
            break;

        case PROP_TONE_AMPLITUDE:
            tonegen->tone_amplitude = g_value_get_float(value);
            break;

        case PROP_TONE_FREQUENCY:
            tonegen->tone_frequency = g_value_get_float(value);
            break;

        case PROP_TONE_DURATION:
            tonegen->tone_duration = g_value_get_float(value);
            break;

        case PROP_PAUSE_DURATION:
            tonegen->pause_duration = g_value_get_float(value);
            break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

static void gst_tonegen_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
    GstToneGen *tonegen = GST_TONEGEN(object);

    switch(prop_id)
    {
        case PROP_ENABLED:
            g_value_set_boolean(value, tonegen->enabled);
            break;

        case PROP_TONE_AMPLITUDE:
            g_value_set_float(value, tonegen->tone_amplitude);
            break;

        case PROP_TONE_FREQUENCY:
            g_value_set_float(value, tonegen->tone_frequency);
            break;

        case PROP_TONE_DURATION:
            g_value_set_float(value, tonegen->tone_duration);
            break;

        case PROP_PAUSE_DURATION:
            g_value_set_float(value, tonegen->pause_duration);
            break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

static gboolean gst_tonegen_setup(GstAudioFilter *filter, const GstAudioInfo *info)
{
    GstToneGen *tonegen = GST_TONEGEN(filter);
    tonegen->sample_rate = info->rate;
    tonegen->channels = info->channels;
    return TRUE;
}

static GstFlowReturn gst_tonegen_transform_ip(GstBaseTransform *transform, GstBuffer *buffer)
{
    GstToneGen *tonegen = GST_TONEGEN(transform);
    if(!tonegen->enabled) return GST_FLOW_OK;

    GstMapInfo info = { };
    if(gst_buffer_map(buffer, &info, GST_MAP_READWRITE))
    {
        gint16 *ptr = (gint16*)info.data;
        while((gpointer)ptr < (gpointer)info.data + info.size)
        {
            gint16 value = 0;
            if(tonegen->sample_time < tonegen->tone_duration)
            {
                // Quadratic curve sine approximation
                float s, p = tonegen->sample_phase;
                s = p < 0 ? (4. / PI) * p + 4. / (PI * PI) * p * p : (4. / PI) * p - 4. / (PI * PI) * p * p;
                s = s < 0 ? .225 * (s * -s - s) + s : .225 * (s *  s - s) + s;
                value = G_MAXINT16 * tonegen->tone_amplitude * s;

                tonegen->sample_phase += 2. * PI * tonegen->tone_frequency / tonegen->sample_rate;
                if(tonegen->sample_phase > PI) tonegen->sample_phase -= 2. * PI;
            }

            guint channels = tonegen->channels;
            while(channels--) *ptr++ = value;

            tonegen->sample_time += 1. / tonegen->sample_rate;
            if(tonegen->sample_time > tonegen->tone_duration + tonegen->pause_duration)
                tonegen->sample_time = 0;
        }

        gst_buffer_unmap(buffer, &info);
        return GST_FLOW_OK;
    }

    return GST_FLOW_ERROR;
}
