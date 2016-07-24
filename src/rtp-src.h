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

#ifndef __RTP_SRC_H__
#define __RTP_SRC_H__

#include <gst/base/base.h>
#include "dht-common.h"

#define RTP_TYPE_SRC rtp_src_get_type()
#define RTP_SRC(obj) G_TYPE_CHECK_INSTANCE_CAST((obj),RTP_TYPE_SRC,RtpSrc)
#define RTP_SRC_CLASS(klass) G_TYPE_CHECK_CLASS_CAST((klass),RTP_TYPE_SRC,RtpSrcClass)
#define RTP_IS_SRC(obj) G_TYPE_CHECK_INSTANCE_TYPE((obj),RTP_TYPE_SRC)

typedef struct _RtpSrc RtpSrc;
typedef struct _RtpSrcClass RtpSrcClass;

struct _RtpSrc
{
    GstPushSrc parent_instance;

    GstAllocator *allocator;
    GstAllocationParams params;

    GSocket *socket;
    GCancellable *cancellable;

    GHashTable *streams;
    DhtKey key;

    gboolean enable;
};

struct _RtpSrcClass
{
    GstPushSrcClass parent_class;
};

GstElement* rtp_src_new(DhtKey *key, GSocket *socket, const gchar *name);

GType rtp_src_get_type(void);

#endif /* __RTP_SRC_H__ */
