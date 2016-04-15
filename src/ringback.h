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

#ifndef __RINGBACK_H__
#define __RINGBACK_H__

#include <gst/audio/audio.h>

#define GST_TYPE_RINGBACK \
  (gst_ringback_get_type())
#define GST_RINGBACK(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_RINGBACK,GstRingback))
#define GST_RINGBACK_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_RINGBACK,GstRingbackClass))
#define GST_IS_RINGBACK(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_RINGBACK))
#define GST_IS_RINGBACK_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_RINGBACK))

typedef struct _GstRingback GstRingback;
typedef struct _GstRingbackClass GstRingbackClass;

struct _GstRingback
{
    GstAudioFilter filter;
    gboolean enabled;
    gfloat tone_ampl, tone_freq, tone_dur, pause_dur;
    gfloat time, rate, channels;
};

struct _GstRingbackClass
{
    GstAudioFilterClass parent_class;
};

GType gst_ringback_get_type(void);

#endif /* __RINGBACK_H__ */
