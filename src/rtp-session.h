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

#ifndef __RTP_SESSION_H__
#define __RTP_SESSION_H__

#include "dht-common.h"

#define RTP_TYPE_SESSION rtp_session_get_type()
G_DECLARE_DERIVABLE_TYPE(RtpSession, rtp_session, RTP, SESSION, GObject)

struct _RtpSessionClass
{
    GObjectClass parent_class;

    void (*hangup)(RtpSession *session);
};

RtpSession* rtp_session_new();

void rtp_session_prepare(RtpSession *session, GSocket *socket, DhtKey *enc_key, DhtKey *dec_key);

void rtp_session_echo_cancel(RtpSession *session);

void rtp_session_set_bitrate(RtpSession *session, guint bitrate, gboolean vbr);

void rtp_session_set_volume(RtpSession *session, gdouble volume);

void rtp_session_set_tone(RtpSession *session, gboolean enable);

void rtp_session_play(RtpSession *session);

void rtp_session_destroy(RtpSession *session);

#endif /* __RTP_SESSION_H__ */
