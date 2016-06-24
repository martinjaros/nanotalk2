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

#ifndef __RTP_TONE_H__
#define __RTP_TONE_H__

#include <gst/audio/audio.h>

#define RTP_TYPE_TONE \
  (rtp_tone_get_type())
#define RTP_TONE(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),RTP_TYPE_TONE,RtpTone))
#define RTP_TONE_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),RTP_TYPE_TONE,RtpToneClass))
#define RTP_IS_TONE(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),RTP_TYPE_TONE))
#define RTP_IS_TONEGEN_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),RTP_TYPE_TONE))

typedef struct _RtpTone RtpTone;
typedef struct _RtpToneClass RtpToneClass;

struct _RtpTone
{
    GstAudioFilter parent_instance;

    gboolean enable;
    gfloat tone_amplitude, tone_frequency, tone_duration, pause_duration;
    gfloat time, phase, rate;
};

struct _RtpToneClass
{
    GstAudioFilterClass parent_class;
};

GstElement* rtp_tone_new(const gchar *name);

GType rtp_tone_get_type(void);

#endif /* __RTP_TONE_H__ */
