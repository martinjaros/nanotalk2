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

/**
 * SECTION:dhtclient
 * Distributed hash table client for real-time applications.
 */
#define DHT_TYPE_CLIENT dht_client_get_type()
G_DECLARE_DERIVABLE_TYPE(DhtClient, dht_client, DHT, CLIENT, GObject)

struct _DhtClientClass
{
    GObjectClass parent_class;

    /**
     * DhtClientClass::new_connection:
     * @client: Object instance that emitted the signal
     * @id: ID of the remote peer
     * @socket: Connected socket
     * @enc_key: Encryption key
     * @dec_key: Decryption key
     *
     * A virtual method implementing the #DhtClient::new-connection signal.
     */
    void (*new_connection)(DhtClient *client, DhtId *id, GSocket *socket, DhtKey *enc_key, DhtKey *dec_key);
};

/**
 * dht_client_new:
 * @key: Private key
 *
 * Creates new #DhtClient instance.
 *
 * Returns: #DhtClient instance or %NULL on failure
 */
DhtClient* dht_client_new(DhtKey *key);

/**
 * dht_client_bind:
 * @client: Object instance
 * @address: Local address
 * @allow_reuse: Allow port reuse
 * @error: (allow-none): Output location for errors
 *
 * Binds the internal socket to the specified address, see g_socket_bind().
 *
 * Returns: %TRUE on success, %FALSE otherwise
 */
gboolean dht_client_bind(DhtClient *client, GSocketAddress *address, gboolean allow_reuse, GError **error);

/**
 * dht_client_bootstrap:
 * @client: Object instance
 * @address: Bootstrap address

 * Bootstraps the client by sending a request to the specified address, this is necessary in order to join the distributed network.
 * The #DhtClient:peers property holds the actual number of peers found.
 */
void dht_client_bootstrap(DhtClient *client, GSocketAddress *address);

/**
 * dht_client_lookup_async:
 * @client: Object instance
 * @id: ID of the remote peer
 * @callback: Callback handler
 * @user_data: User specified argument for @callback
 *
 * Asynchronously finds host by @id and creates a connection.
 * Eventually calls @callback, which must call dht_client_lookup_finish() to get the result.
 */
void dht_client_lookup_async(DhtClient *client, const DhtId *id, GAsyncReadyCallback callback, gpointer user_data);

/**
 * dht_client_lookup_finish:
 * @client: Object instance
 * @result: The result passed to #GAsyncReadyCallback
 * @socket: (allow-none): Connected socket
 * @enc_key: (allow-none): Encryption key
 * @dec_key: (allow-none): Decryption key
 * @error: (allow-none): Output location for errors
 *
 * Retrieves the results from dht_client_lookup_async().
 *
 * Returns: %TRUE on success, %FALSE otherwise
 */
gboolean dht_client_lookup_finish(DhtClient *client, GAsyncResult *result, GSocket **socket, DhtKey *enc_key, DhtKey *dec_key, GError **error);

#endif /* __DHT_CLIENT_H__ */
