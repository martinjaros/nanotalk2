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

#include <string.h>
#include <sodium.h>
#include "dht-common.h"

G_DEFINE_BOXED_TYPE(DhtKey, dht_key, dht_key_copy, dht_key_free)
G_DEFINE_BOXED_TYPE(DhtId, dht_id, dht_id_copy, dht_id_free)
G_DEFINE_BOXED_TYPE(DhtAddress, dht_address, dht_address_copy, dht_address_free)

__attribute__((constructor))
static int init()
{
    return sodium_init();
}

void dht_key_make_random(DhtKey *key)
{
    randombytes_buf(key->data, DHT_KEY_SIZE);
}

void dht_key_make_public(DhtKey *pubkey, const DhtKey *privkey)
{
    crypto_scalarmult_base(pubkey->data, privkey->data);
}

gboolean dht_key_make_shared(DhtKey *shared, const DhtKey *privkey, const DhtKey *pubkey)
{
    return crypto_scalarmult(shared->data, privkey->data, pubkey->data) == 0;
}

void dht_key_derive(DhtKey *key, DhtKey *tag, const DhtKey *shared, const DhtKey *tx_nonce, const DhtKey *rx_nonce)
{
    crypto_generichash_state state;
    DhtKey result[2];

    crypto_generichash_init(&state, shared->data, DHT_KEY_SIZE, 2 * DHT_KEY_SIZE);
    crypto_generichash_update(&state, tx_nonce->data, DHT_KEY_SIZE);
    crypto_generichash_update(&state, rx_nonce->data, DHT_KEY_SIZE);
    crypto_generichash_final(&state, result[0].data, 2 * DHT_KEY_SIZE);

    *key = result[0];
    *tag = result[1];
}

void dht_id_from_pubkey(DhtId *id, const DhtKey *pubkey)
{
    crypto_generichash(id->data, DHT_ID_SIZE, pubkey->data, DHT_KEY_SIZE, NULL, 0);
}

gboolean dht_id_from_string(DhtId *id, const gchar *str)
{
    gsize len = strlen(str);
    if(len != (((4 * DHT_ID_SIZE / 3) + 3) & ~3))
        return FALSE;

#if (DHT_ID_SIZE % 3 > 0)
    if(str[len - 1] != '=') return FALSE;
#if (DHT_ID_SIZE % 3 == 1)
    if(str[len - 2] != '=') return FALSE;
#endif
#endif

    gint state = 0;
    guint save = 0;
    return g_base64_decode_step(str, len, id->data, &state, &save) == DHT_ID_SIZE;
}

gchar* dht_id_to_string(const DhtId *id)
{
    return g_base64_encode(id->data, DHT_ID_SIZE);
}

void dht_id_xor(DhtId *res, const DhtId *a, const DhtId *b)
{
    int i;
    for(i = 0; i < DHT_ID_SIZE; i++)
        res->data[i] = a->data[i] ^ b->data[i];
}

void dht_address_serialize(DhtAddress *addr, GSocketAddress *sockaddr)
{
    guint16 port = g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(sockaddr));
    GInetAddress *inaddr = g_inet_socket_address_get_address(G_INET_SOCKET_ADDRESS(sockaddr));

    addr->data[0] = port >> 8;
    addr->data[1] = port & 0xFF;
    memcpy(addr->data + 2, g_inet_address_to_bytes(inaddr), DHT_ADDRESS_SIZE - 2);
}

GSocketAddress* dht_address_deserialize(const DhtAddress *addr)
{
    guint16 port = ((guint16)addr->data[0] << 8) | (guint16)addr->data[1];
    g_autoptr(GInetAddress) inaddr = g_inet_address_new_from_bytes(addr->data + 2, DHT_ADDRESS_FAMILY);

    return g_inet_socket_address_new(inaddr, port);
}

gpointer dht_key_copy(gpointer key)
{
    return g_slice_dup(DhtKey, key);
}

gpointer dht_id_copy(gpointer id)
{
    return g_slice_dup(DhtId, id);
}

gpointer dht_address_copy(gpointer addr)
{
    return g_slice_dup(DhtAddress, addr);
}

guint dht_key_hash(gconstpointer key)
{
    return (((guint8*)key)[DHT_KEY_SIZE - 4] << 24) |
           (((guint8*)key)[DHT_KEY_SIZE - 3] << 16) |
           (((guint8*)key)[DHT_KEY_SIZE - 2] <<  8) |
           (((guint8*)key)[DHT_KEY_SIZE - 1]);
}

guint dht_id_hash(gconstpointer id)
{
    return (((guint8*)id)[DHT_ID_SIZE - 4] << 24) |
           (((guint8*)id)[DHT_ID_SIZE - 3] << 16) |
           (((guint8*)id)[DHT_ID_SIZE - 2] <<  8) |
           (((guint8*)id)[DHT_ID_SIZE - 1]);
}

guint dht_address_hash(gconstpointer addr)
{
    guint hash;
    crypto_generichash((gpointer)&hash, sizeof(hash), addr, DHT_ADDRESS_SIZE, NULL, 0);
    return hash;
}

gboolean dht_key_equal(gconstpointer a, gconstpointer b)
{
    return memcmp(a, b, DHT_KEY_SIZE) == 0;
}

gboolean dht_id_equal(gconstpointer a, gconstpointer b)
{
    return memcmp(a, b, DHT_ID_SIZE) == 0;
}

gboolean dht_address_equal(gconstpointer a, gconstpointer b)
{
    return memcmp(a, b, DHT_ADDRESS_SIZE) == 0;
}

gint dht_id_compare(gconstpointer a, gconstpointer b, gpointer arg)
{
    (void)arg;
    return memcmp(a, b, DHT_ID_SIZE);
}

void dht_key_free(gpointer key)
{
    memset(key, 0, DHT_KEY_SIZE);
    g_slice_free(DhtKey, key);
}

void dht_id_free(gpointer id)
{
    g_slice_free(DhtId, id);
}

void dht_address_free(gpointer addr)
{
    g_slice_free(DhtAddress, addr);
}
