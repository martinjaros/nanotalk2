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

#ifndef __TONEGEN_H__
#define __TONEGEN_H__

#include <gst/audio/audio.h>

#define GST_TYPE_TONEGEN \
  (gst_tonegen_get_type())
#define GST_TONEGEN(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_TONEGEN,GstToneGen))
#define GST_TONEGEN_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_TONEGEN,GstToneGenClass))
#define GST_IS_TONEGEN(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_TONEGEN))
#define GST_IS_TONEGEN_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_TONEGEN))

typedef struct _GstToneGen GstToneGen;
typedef struct _GstToneGenClass GstToneGenClass;

struct _GstToneGen
{
    GstAudioFilter filter;
    gboolean enabled;
    gfloat tone_amplitude, tone_angular_rate, tone_duration, pause_duration;
    gfloat sample_phase, sample_time, sample_rate_inv;
    guint channels;
};

struct _GstToneGenClass
{
    GstAudioFilterClass parent_class;
};

GType gst_tonegen_get_type(void);

#endif /* __TONEGEN_H__ */
