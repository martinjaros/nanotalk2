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

#include <gio/gio.h>
#include "glib-compat.h"

/**
 * DHT_KEY_SIZE:
 * Size of the private key in bytes, see #DhtClient:key and dht_client_new().
 */
#define DHT_KEY_SIZE 32

/**
 * DHT_ID_LENGTH:
 * Length of the ID string, see #DhtClient:id and dht_client_lookup().
 */
#define DHT_ID_LENGTH 28

/**
 * SECTION:dhtclient
 * Distributed Hash Table client for real-time applications.
 */
#define DHT_TYPE_CLIENT dht_client_get_type()
G_DECLARE_DERIVABLE_TYPE(DhtClient, dht_client, DHT, CLIENT, GObject)

struct _DhtClientClass
{
    GObjectClass parent;

    /**
     * DhtClientClass::accept_connection:
     * @client: Object instance that emitted the signal
     * @id: ID of the remote peer (20 bytes, Base64 encoded)
     *
     * A virtual method implementing the #DhtClient::accept-connection signal.
     *
     * Returns: %TRUE if the connection should be accepted or %FALSE to reject the connection
     */
    gboolean (*accept_connection)(DhtClient *client, const gchar *id);

    /**
     * DhtClientClass::new_connection:
     * @client: Object instance that emitted the signal
     * @id: ID of the remote peer (20 bytes, Base64 encoded)
     * @socket: Socket allocated for this connection
     * @sockaddr: Remote peer address
     * @enc_key: Derived encryption key of #DhtClient:key-size bytes
     * @dec_key: Derived decryption key of #DhtClient:key-size bytes
     * @remote: %TRUE if the connection was initiated by the remote peer
     *
     * A virtual method implementing the #DhtClient::new-connection signal.
     */
    void (*new_connection)(DhtClient *client, const gchar *id,
            GSocket *socket, GSocketAddress *sockaddr, GBytes *enc_key, GBytes *dec_key, gboolean remote);

    /**
     * DhtClientClass::on_error:
     * @client: Object instance that emitted the signal
     * @id: ID of the remote peer (20 bytes, Base64 encoded)
     * @error: The error being reported
     *
     * A virtual method implementing the #DhtClient::on-error signal.
     */
    void (*on_error)(DhtClient *client, const gchar *id, GError *error);
};

/**
 * dht_client_new:
 * @family: #GSocketFamily.IPV4 or #GSocketFamily.IPV6
 * @port: Local port number
 * @key: (allow-none): 32-byte private key
 * @error: (allow-none): Output location for errors
 *
 * Creates new #DhtClient instance bound to specified @port with specified address @family.
 * If @key is %NULL, then a random key is generated.
 *
 * Returns: #DhtClient instance or %NULL
 */
DhtClient* dht_client_new(GSocketFamily family, guint16 port, GBytes *key, GError **error);

/**
 * dht_client_bootstrap:
 * @client: Object instance
 * @host: Hostname or address
 * @port: Destination port
 * @error: (allow-none): Output location for synchronous errors
 *
 * Bootstraps the client by sending a request to the specified address, this is necessary in order to join the distributed network.
 * This method may be called repeatedly, the #DhtClient:peers property will hold the number of peers found.
 * After this initial process the client will automatically take over with its own internal periodic refreshing.
 * Asynchronous errors will be reported via #DhtClient::on-error signal with the client's own ID.
 *
 * Returns: %TRUE on success
 */
gboolean dht_client_bootstrap(DhtClient *client, const gchar *host, guint16 port, GError **error);

/**
 * dht_client_lookup:
 * @client: Object instance
 * @id: ID of the remote peer (20 bytes, Base64 encoded)
 * @error: (allow-none): Output location for synchronous errors
 *
 * Finds host by @id and creates a connection, having its own socket socket pair and securely derived keys.
 * The newly allocated connections are signaled with #DhtClient::new-connection,
 * the user should either connect the signal or override the class method.
 * Asynchronous errors will be reported via #DhtClient::on-error signal:
 *
 *   #GIOError.HOST_NOT_FOUND - Lookup for a host with the specified ID failed
 *
 *   #GIOError.HOST_UNREACHABLE - The host address cannot be reached
 *
 *   #GIOError.CONNECTION_REFUSED - The connection was rejected
 *
 *   #GIOError.TIMED_OUT - The connection timed out
 *
 * Returns: %TRUE on success
 */
gboolean dht_client_lookup(DhtClient *client, const gchar *id, GError **error);

#endif /* __DHT_CLIENT_H__ */
