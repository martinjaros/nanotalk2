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

#ifndef __DHT_CLIENT_H__
#define __DHT_CLIENT_H__

#include "dht-common.h"

#define DHT_TYPE_CLIENT dht_client_get_type()
G_DECLARE_DERIVABLE_TYPE(DhtClient, dht_client, DHT, CLIENT, GObject)

struct _DhtClientClass
{
    GObjectClass parent_class;

    void (*new_connection)(DhtClient *client, DhtId *id, GSocket *socket, GSocketAddress *address, DhtKey *enc_key, DhtKey *dec_key);
};

DhtClient* dht_client_new(DhtKey *key);

gboolean dht_client_bind(DhtClient *client, GSocketAddress *address, gboolean allow_reuse, GError **error);

gboolean dht_client_bootstrap(DhtClient *client, GSocketAddress *address, GError **error);

void dht_client_lookup_async(DhtClient *client, const DhtId *id, GAsyncReadyCallback callback, gpointer user_data);

gboolean dht_client_lookup_finish(DhtClient *client, GAsyncResult *result,
        GSocket **socket, GSocketAddress **address, DhtKey *enc_key, DhtKey *dec_key, GError **error);

#endif /* __DHT_CLIENT_H__ */
