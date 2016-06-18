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

#ifndef __DHT_COMMON_H__
#define __DHT_COMMON_H__

#include <gio/gio.h>
#include "glib-compat.h"

#define DHT_KEY_SIZE 32
#define DHT_ID_SIZE 20

#define DHT_ADDRESS_FAMILY G_SOCKET_FAMILY_IPV4
#define DHT_ADDRESS_SIZE (2+4) // port + IP address

#define DHT_TYPE_KEY dht_key_get_type()
#define DHT_TYPE_ID dht_id_get_type()
#define DHT_TYPE_ADDRESS dht_address_get_type()

typedef struct _DhtKey DhtKey;
typedef struct _DhtId DhtId;
typedef struct _DhtAddress DhtAddress;

struct _DhtKey
{
    guint8 data[DHT_KEY_SIZE];
};

struct _DhtId
{
    guint8 data[DHT_ID_SIZE];
};

struct _DhtAddress
{
    guint8 data[DHT_ADDRESS_SIZE];
};

void dht_key_make_random(DhtKey *key);
void dht_key_make_public(DhtKey *pubkey, const DhtKey *privkey);
gboolean dht_key_make_shared(DhtKey *shared, const DhtKey *privkey, const DhtKey *pubkey);
void dht_key_derive(DhtKey *key, DhtKey *auth_tag, const DhtKey *secret, const DhtKey *tx_nonce, const DhtKey *rx_nonce);

void dht_id_from_pubkey(DhtId *id, const DhtKey *pubkey);
gboolean dht_id_from_string(DhtId *id, const gchar *str);
gchar* dht_id_to_string(const DhtId *id);
void dht_id_xor(DhtId *res, const DhtId *a, const DhtId *b);

void dht_address_serialize(DhtAddress *addr, const GSocketAddress *sockaddr);
GSocketAddress* dht_address_deserialize(const DhtAddress *addr);

gpointer dht_key_copy(gpointer key);
gpointer dht_id_copy(gpointer id);
gpointer dht_address_copy(gpointer addr);

guint dht_key_hash(gconstpointer key);
guint dht_id_hash(gconstpointer id);
guint dht_address_hash(gconstpointer addr);

gboolean dht_key_equal(gconstpointer a, gconstpointer b);
gboolean dht_id_equal(gconstpointer a, gconstpointer b);
gboolean dht_address_equal(gconstpointer a, gconstpointer b);

gint dht_id_compare(gconstpointer a, gconstpointer b, gpointer arg);

void dht_key_free(gpointer key);
void dht_id_free(gpointer id);
void dht_address_free(gpointer addr);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(DhtKey, dht_key_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(DhtId, dht_id_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(DhtAddress, dht_address_free)

GType dht_key_get_type(void);
GType dht_id_get_type(void);
GType dht_address_get_type(void);

#endif /* __DHT_COMMON_H__ */
