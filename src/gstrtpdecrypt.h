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

#ifndef __GST_RTP_DECRYPT_H__
#define __GST_RTP_DECRYPT_H__

#include <gst/gst.h>

#define GST_TYPE_RTP_DECRYPT \
  (gst_rtp_decrypt_get_type())
#define GST_RTP_DECRYPT(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_RTP_DECRYPT,GstRtpDecrypt))
#define GST_RTP_DECRYPT_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_RTP_DECRYPT,GstRtpDecryptClass))
#define GST_IS_RTP_DECRYPT(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_RTP_DECRYPT))
#define GST_IS_RTP_DECRYPT_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_RTP_DECRYPT))

typedef struct _GstRtpDecrypt GstRtpDecrypt;
typedef struct _GstRtpDecryptClass GstRtpDecryptClass;

struct _GstRtpDecrypt
{
    GstElement element;
    GstPad *src;
    GstPad *sink;
    GBytes *key;
    guint64 roc;
    guint16 s_l;
};

struct _GstRtpDecryptClass
{
    GstElementClass parent_class;
};

GType gst_rtp_decrypt_get_type(void);

#endif /* __GST_RTP_DECRYPT_H__ */
