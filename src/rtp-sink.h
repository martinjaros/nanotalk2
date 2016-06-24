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

#ifndef __RTP_SINK_H__
#define __RTP_SINK_H__

#include <gst/base/base.h>
#include "dht-common.h"

#define RTP_TYPE_SINK rtp_sink_get_type()
#define RTP_SINK(obj) G_TYPE_CHECK_INSTANCE_CAST((obj),RTP_TYPE_SINK,RtpSink)
#define RTP_SINK_CLASS(klass) G_TYPE_CHECK_CLASS_CAST((klass),RTP_TYPE_SINK,RtpSinkClass)
#define RTP_IS_SINK(obj) G_TYPE_CHECK_INSTANCE_TYPE((obj),RTP_TYPE_SINK)

typedef struct _RtpSink RtpSink;
typedef struct _RtpSinkClass RtpSinkClass;

struct _RtpSink
{
    GstBaseSink parent_instance;

    GSocket *socket;
    DhtKey key;
    guint64 roc;
};

struct _RtpSinkClass
{
    GstBaseSinkClass parent_class;
};

GstElement* rtp_sink_new(DhtKey *key, GSocket *socket, const gchar *name);

GType rtp_sink_get_type(void);

#endif /* __RTP_SINK_H__ */
