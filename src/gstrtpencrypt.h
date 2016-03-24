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

#ifndef __GST_RTP_ENCRYPT_H__
#define __GST_RTP_ENCRYPT_H__

#include <gst/gst.h>

#define GST_TYPE_RTP_ENCRYPT \
  (gst_rtp_encrypt_get_type())
#define GST_RTP_ENCRYPT(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_RTP_ENCRYPT,GstRtpEncrypt))
#define GST_RTP_ENCRYPT_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_RTP_ENCRYPT,GstRtpEncryptClass))
#define GST_IS_RTP_ENCRYPT(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_RTP_ENCRYPT))
#define GST_IS_RTP_ENCRYPT_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_RTP_ENCRYPT))

typedef struct _GstRtpEncrypt GstRtpEncrypt;
typedef struct _GstRtpEncryptClass GstRtpEncryptClass;

struct _GstRtpEncrypt
{
    GstElement element;
    GstPad *src;
    GstPad *sink;
    GBytes *key;
    guint64 roc;
};

struct _GstRtpEncryptClass
{
    GstElementClass parent_class;
};

GType gst_rtp_encrypt_get_type(void);

#endif /* __GST_RTP_ENCRYPT_H__ */
