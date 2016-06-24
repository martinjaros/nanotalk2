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

#include "rtp-tone.h"

#define PI 3.14159265358979323846

#define DEFAULT_ENABLE          FALSE
#define DEFAULT_TONE_AMPLITUDE  0.5
#define DEFAULT_TONE_FREQUENCY  425
#define DEFAULT_TONE_DURATION   1
#define DEFAULT_PAUSE_DURATION  4

enum
{
    PROP_0,
    PROP_ENABLE,
    PROP_TONE_AMPLITUDE,
    PROP_TONE_FREQUENCY,
    PROP_TONE_DURATION,
    PROP_PAUSE_DURATION,
};

G_DEFINE_TYPE(RtpTone, rtp_tone, GST_TYPE_AUDIO_FILTER)

static void rtp_tone_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec);
static void rtp_tone_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec);
static gboolean rtp_tone_setup(GstAudioFilter *filter, const GstAudioInfo *info);
static GstFlowReturn rtp_tone_transform_ip(GstBaseTransform *transform, GstBuffer *buffer);

static void rtp_tone_class_init(RtpToneClass *tone_class)
{
    GObjectClass *object_class = (GObjectClass*)tone_class;
    object_class->set_property = rtp_tone_set_property;
    object_class->get_property = rtp_tone_get_property;

    g_object_class_install_property(object_class, PROP_ENABLE,
        g_param_spec_boolean("enable", "Enable", "Enable tone", DEFAULT_ENABLE,
                G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    g_object_class_install_property(object_class, PROP_TONE_AMPLITUDE,
        g_param_spec_float("tone-amplitude", "Tone amplitude", "Tone amplitude", 0, 1, DEFAULT_TONE_AMPLITUDE,
                G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    g_object_class_install_property(object_class, PROP_TONE_FREQUENCY,
        g_param_spec_float("tone-frequency", "Tone frequency", "Tone frequency (Hz)", 0, G_MAXFLOAT, DEFAULT_TONE_FREQUENCY,
                G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    g_object_class_install_property(object_class, PROP_TONE_DURATION,
        g_param_spec_float("tone-duration", "Tone duration", "Tone duration (s)", 0, G_MAXFLOAT, DEFAULT_TONE_DURATION,
                G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    g_object_class_install_property(object_class, PROP_PAUSE_DURATION,
        g_param_spec_float("pause-duration", "Pause duration", "Pause duration (s)", 0, G_MAXFLOAT, DEFAULT_PAUSE_DURATION,
                G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    GstElementClass *element_class = (GstElementClass*)tone_class;
    gst_element_class_set_static_metadata(element_class,
        "RTP tone", "Filter/Audio", "Tone generator", "Martin Jaros <xjaros32@stud.feec.vutbr.cz>");

    GstBaseTransformClass *tranform_class = (GstBaseTransformClass*)tone_class;
    tranform_class->transform_ip = rtp_tone_transform_ip;

    GstAudioFilterClass *filter_class = (GstAudioFilterClass*)tone_class;
    filter_class->setup = rtp_tone_setup;

    GstCaps *caps = gst_caps_from_string(GST_AUDIO_CAPS_MAKE("S16LE")",layout=(string)interleaved");
    gst_audio_filter_class_add_pad_templates(filter_class, caps);
    gst_caps_unref(caps);
}

static void rtp_tone_init(RtpTone *tone)
{
    tone->enable = DEFAULT_ENABLE;
    tone->tone_frequency = DEFAULT_TONE_FREQUENCY;
    tone->tone_amplitude = DEFAULT_TONE_AMPLITUDE;
    tone->tone_duration = DEFAULT_TONE_DURATION;
    tone->pause_duration = DEFAULT_PAUSE_DURATION;
}

GstElement* rtp_tone_new(const gchar *name)
{
    return g_object_new(RTP_TYPE_TONE, "name", name, NULL);
}

static void rtp_tone_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
    RtpTone *tone = RTP_TONE(object);

    switch(prop_id)
    {
        case PROP_ENABLE:
            tone->enable = g_value_get_boolean(value);
            break;

        case PROP_TONE_AMPLITUDE:
            tone->tone_amplitude = g_value_get_float(value);
            break;

        case PROP_TONE_FREQUENCY:
            tone->tone_frequency = g_value_get_float(value);
            break;

        case PROP_TONE_DURATION:
            tone->tone_duration = g_value_get_float(value);
            break;

        case PROP_PAUSE_DURATION:
            tone->pause_duration = g_value_get_float(value);
            break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

static void rtp_tone_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
    RtpTone *tone = RTP_TONE(object);

    switch(prop_id)
    {
        case PROP_ENABLE:
            g_value_set_boolean(value, tone->enable);
            break;

        case PROP_TONE_AMPLITUDE:
            g_value_set_float(value, tone->tone_amplitude);
            break;

        case PROP_TONE_FREQUENCY:
            g_value_set_float(value, tone->tone_frequency);
            break;

        case PROP_TONE_DURATION:
            g_value_set_float(value, tone->tone_duration);
            break;

        case PROP_PAUSE_DURATION:
            g_value_set_float(value, tone->pause_duration);
            break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

static gboolean rtp_tone_setup(GstAudioFilter *filter, const GstAudioInfo *info)
{
    RtpTone *tone = RTP_TONE(filter);

    tone->rate = info->rate * info->channels;
    return TRUE;
}

static GstFlowReturn rtp_tone_transform_ip(GstBaseTransform *transform, GstBuffer *buffer)
{
    RtpTone *tone = RTP_TONE(transform);
    if(!tone->enable) return GST_FLOW_OK;

    GstMapInfo info = { };
    if(gst_buffer_map(buffer, &info, GST_MAP_READWRITE))
    {
        gint16 *sample = (gint16*)info.data;
        while((gpointer)sample < (gpointer)info.data + info.size)
        {
            gint16 value = 0;
            if(tone->time < tone->tone_duration)
            {
                const gfloat A = 0.5;
                const gfloat B = 2.0 * PI - 5.0;
                const gfloat C = PI - 3.0;

                if(tone->phase < 1.0)
                {
                    gfloat z = tone->phase;
                    gfloat z2 = z * z;
                    gfloat y = A * z * (PI - z2 * (B - C * z2));

                    value = G_MAXINT16 * tone->tone_amplitude * y;
                }
                else
                {
                    gfloat z = tone->phase - 2.0;
                    gfloat z2 = z * z;
                    gfloat y = -A * z * (PI - z2 * (B - C * z2));

                    value = G_MAXINT16 * tone->tone_amplitude * y;
                }

                tone->phase += 4.0 * tone->tone_frequency / tone->rate;
                if(tone->phase > 3.0) tone->phase -= 4.0;
            }

            *sample++ = value;
            tone->time += 1.0 / tone->rate;
            if(tone->time > tone->tone_duration + tone->pause_duration)
                tone->time -= tone->tone_duration + tone->pause_duration;
        }

        gst_buffer_unmap(buffer, &info);
        return GST_FLOW_OK;
    }

    return GST_FLOW_ERROR;
}
