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

#define G_LOG_DOMAIN "DhtClient"

#include <string.h>
#include <sodium.h>
#include "dhtclient.h"

#define DHT_NONCE_SIZE 32
#define DHT_HASH_SIZE  20
#define DHT_NODE_COUNT 16

#define DHT_CONCURRENCY 3
#define DHT_TIMEOUT_MS  1000  // 1 second
#define DHT_REFRESH_MS  60000 // 1 minute

/* Message codes */
#define MSG_LOOKUP_REQ     0xC0
#define MSG_LOOKUP_RES     0xC1
#define MSG_CONNECTION_REQ 0xC2
#define MSG_CONNECTION_RES 0xC3

#define MSG_BUFFER_SIZE 2048
#define ADDR_SIZE_MAX 16
#define ADDR_SIZE(family) (family == G_SOCKET_FAMILY_IPV4 ? 4 : ADDR_SIZE_MAX)

/* Properties */
enum
{
    PROP_0,
    PROP_KEY,
    PROP_SOCKET,
    PROP_KEY_SIZE,
    PROP_ID,
    PROP_PEERS,
    PROP_PUBLIC_ADDRESS,
};

/* Signals */
enum
{
    SIGNAL_ACCEPT_CONNECTION,
    SIGNAL_NEW_CONNECTION,
    SIGNAL_ON_ERROR,
    LAST_SIGNAL
};

typedef struct _msg_node MsgNode;
typedef struct _msg_lookup MsgLookup;
typedef struct _msg_connection MsgConnection;

typedef struct _dht_node DhtNode;
typedef struct _dht_bucket DhtBucket;
typedef struct _dht_query DhtQuery;
typedef struct _dht_lookup DhtLookup;
typedef struct _dht_connection DhtConnection;
typedef struct _dht_client_private DhtClientPrivate;

struct _msg_node
{
    guint8 id[DHT_HASH_SIZE];
    guint16 port; // network byte order
    guint8 addr[0]; // ADDR_SIZE(family)
}
__attribute__((packed));

struct _msg_lookup
{
    guint8 type; // MSG_LOOKUP_REQ, MSG_LOOKUP_RES
    guint8 srcid[DHT_HASH_SIZE];
    guint8 dstid[DHT_HASH_SIZE];
    guint8 nodes[0]; // (sizeof(MsgNode) + ADDR_SIZE(family)) * count
}
__attribute__((packed));

struct _msg_connection
{
    guint8 type; // MSG_CONNECTION_REQ
    guint8 pk[crypto_scalarmult_BYTES];
    guint8 nonce[DHT_NONCE_SIZE];
}
__attribute__((packed));

struct _dht_node
{
    guint8 id[DHT_HASH_SIZE];
    guint8 addr[ADDR_SIZE_MAX];
    guint16 port; // host byte order

    gboolean is_alive;
};

struct _dht_bucket
{
    DhtBucket *next, *prev; // nullable

    gsize count;
    DhtNode nodes[DHT_NODE_COUNT];
};

struct _dht_query // use slice allocator
{
    guint8 metric[DHT_HASH_SIZE]; // must be first member, used for sorting
    guint8 addr[ADDR_SIZE_MAX];
    guint16 port; // host byte order

    DhtLookup *parent;
    guint timeout_source; // GSource ID within default context or 0
    gboolean is_finished;
};

struct _dht_lookup
{
    guint8 id[DHT_HASH_SIZE];
    DhtClient *parent;

    GSequence *queries; // elements of type DhtQuery, sorted by metric
    gsize num_sources; // number of pending timeouts in the sequence
    gsize num_connections; // requested number of connections (0 for internal lookups)
};

struct _dht_connection
{
    guint8 id[DHT_HASH_SIZE];
    guint8 nonce[DHT_NONCE_SIZE];
    DhtClient *parent;
    GList *link; // list link within parent->connections

    GSocket *socket;
    guint io_source, timeout_source; // GSource ID within default context
};

struct _dht_client_private
{
    guint8 id[DHT_HASH_SIZE];
    DhtBucket root_bucket;

    guint8 pk[crypto_scalarmult_BYTES]; // public key
    guint8 sk[crypto_scalarmult_SCALARBYTES]; // secret key
    gsize key_size; // derived key size

    GHashTable *lookup_table; // elements type of DhtLookup
    GList *connections; // elements type of DhtConnection

    guint8 public_address[ADDR_SIZE_MAX];
    guint16 public_port;

    GSocket *socket;
    GSocketFamily family; // G_SOCKET_FAMILY_IPV4 or G_SOCKET_FAMILY_IPV6
    guint io_source, timeout_source; // GSource ID within default context
};

static guint dht_client_signals[LAST_SIGNAL];

G_DEFINE_TYPE_WITH_PRIVATE(DhtClient, dht_client, G_TYPE_OBJECT)

static void dht_client_set_property(GObject *obj, guint prop_id, const GValue *value, GParamSpec *pspec);
static void dht_client_get_property(GObject *obj, guint prop_id, GValue *value, GParamSpec *pspec);
static void dht_client_constructed(GObject *obj);
static void dht_client_finalize(GObject *obj);

static gboolean dht_client_refresh(gpointer arg);
static gboolean dht_client_receive(GSocket *socket, GIOCondition condition, gpointer arg);
static void dht_client_update(DhtClient *client, gconstpointer id, gboolean is_alive, gconstpointer addr, guint16 port);
static gsize dht_client_search(DhtClient *client, gconstpointer id, gpointer node_ptr);

static gboolean dht_query_timeout(gpointer arg);
static void dht_query_destroy(gpointer arg);

static gboolean dht_lookup_update(DhtLookup *lookup, gconstpointer node_ptr, gsize count, gboolean emit_error, GError **error);
static gboolean dht_lookup_dispatch(DhtLookup *lookup, gboolean emit_error, GError **error);
static void dht_lookup_destroy(gpointer arg);

static gboolean dht_connection_receive(GSocket *socket, GIOCondition condition, gpointer arg);
static gboolean dht_connection_timeout(gpointer arg);
static void dht_connection_destroy(gpointer arg);

static inline void dht_memxor(guint8 *metric, const guint8 *id1, const guint8 *id2)
{
    int i;
    for(i = 0; i < DHT_HASH_SIZE; i++)
        metric[i] = id1[i] ^ id2[i];
}

static guint dht_hash_func(gconstpointer key)
{
    return (((guint8*)key)[DHT_HASH_SIZE - 4] << 24) |
           (((guint8*)key)[DHT_HASH_SIZE - 3] << 16) |
           (((guint8*)key)[DHT_HASH_SIZE - 2] <<  8) |
           (((guint8*)key)[DHT_HASH_SIZE - 1]);
}

static gboolean dht_equal_func(gconstpointer a, gconstpointer b)
{
    return memcmp(a, b, DHT_HASH_SIZE) == 0;
}

static gint dht_compare_func(gconstpointer a, gconstpointer b, gpointer arg)
{
    return memcmp(a, b, DHT_HASH_SIZE);
}

static void dht_kdf(gconstpointer sk, gconstpointer pk, gconstpointer rx_nonce, gconstpointer tx_nonce,
        gpointer enc_key, gpointer dec_key, gsize key_size)
{
    crypto_generichash_state state;

    guint8 secret[crypto_scalarmult_BYTES];
    crypto_scalarmult(secret, sk, pk);

    crypto_generichash_init(&state, secret, crypto_scalarmult_BYTES, key_size);
    crypto_generichash_update(&state, rx_nonce, DHT_NONCE_SIZE);
    crypto_generichash_update(&state, tx_nonce, DHT_NONCE_SIZE);
    crypto_generichash_final(&state, enc_key, key_size);

    crypto_generichash_init(&state, secret, crypto_scalarmult_BYTES, key_size);
    crypto_generichash_update(&state, tx_nonce, DHT_NONCE_SIZE);
    crypto_generichash_update(&state, rx_nonce, DHT_NONCE_SIZE);
    crypto_generichash_final(&state, dec_key, key_size);
}

static void dht_client_class_init(DhtClientClass *client_class)
{
    g_assert(DHT_KEY_SIZE == crypto_scalarmult_SCALARBYTES);
    g_assert(DHT_ID_LENGTH == ((4 * DHT_HASH_SIZE / 3 + 3) & ~3));

    GObjectClass *object_class = (GObjectClass*)client_class;
    object_class->set_property = dht_client_set_property;
    object_class->get_property = dht_client_get_property;
    object_class->constructed = dht_client_constructed;
    object_class->finalize = dht_client_finalize;

    /**
     * DhtClient:key:
     * 32-byte private key.
     */
    g_object_class_install_property(object_class, PROP_KEY,
            g_param_spec_boxed("key", "Key", "Client private key", G_TYPE_BYTES,
                    G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    /**
     * DhtClient:socket:
     * IPv4 or IPv6 datagram socket used for IO operations.
     */
    g_object_class_install_property(object_class, PROP_SOCKET,
            g_param_spec_object("socket", "Socket", "Client socket", G_TYPE_SOCKET,
                    G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    /**
     * DhtClient:key-size:
     * Size of the derived keys passed to #DhtClient::new-connection signal.
     */
    g_object_class_install_property(object_class, PROP_KEY_SIZE,
            g_param_spec_uint("key-size", "Key size", "Size of the derived keys", 16, 64, DHT_KEY_SIZE,
                    G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    /**
     * DhtClient:id:
     * Client ID derived from private key (20 bytes, Base64 encoded).
     */
    g_object_class_install_property(object_class, PROP_ID,
            g_param_spec_string("id", "ID", "Client ID", NULL,
                    G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

    /**
     * DhtClient:peers:
     * Number of alive peers the client knows about.
     */
    g_object_class_install_property(object_class, PROP_PEERS,
            g_param_spec_uint("peers", "Peers", "Number of peers", 0, G_MAXUINT, 0,
                    G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

    /**
     * DhtClient:public-address:
     * Externally visible address of the socket.
     */
    g_object_class_install_property(object_class, PROP_PUBLIC_ADDRESS,
            g_param_spec_object("public-address", "Public address", "Externally visible address", G_TYPE_SOCKET_ADDRESS,
                    G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

    /**
     * DhtClient::accept-connection:
     * @client: Object instance that emitted the signal
     * @id: ID of the remote peer (20 bytes, Base64 encoded)
     *
     * A signal emitted when a remote peer requests a connection.
     *
     * Returns: %TRUE if the connection should be accepted
     */
    dht_client_signals[SIGNAL_ACCEPT_CONNECTION] = g_signal_new("accept-connection",
             G_TYPE_FROM_CLASS(client_class), G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET(DhtClientClass, accept_connection),
             NULL, NULL, NULL, G_TYPE_BOOLEAN, 1, G_TYPE_STRING);

    /**
     * DhtClient::new-connection:
     * @client: Object instance that emitted the signal
     * @id: ID of the remote peer (20 bytes, Base64 encoded)
     * @socket: Socket allocated for this connection
     * @sockaddr: Remote peer address
     * @enc_key: Derived encryption key of #DhtClient:key-size bytes
     * @dec_key: Derived decryption key of #DhtClient:key-size bytes
     * @remote: %TRUE if the connection was initiated by the remote peer
     *
     * A signal emitted after establishing a new connection, see dht_client_lookup().
     */
    dht_client_signals[SIGNAL_NEW_CONNECTION] = g_signal_new("new-connection",
            G_TYPE_FROM_CLASS(client_class), G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET(DhtClientClass, new_connection),
            NULL, NULL, NULL, G_TYPE_NONE, 6, G_TYPE_STRING, G_TYPE_SOCKET, G_TYPE_SOCKET_ADDRESS, G_TYPE_BYTES, G_TYPE_BYTES, G_TYPE_BOOLEAN);

    /**
     * DhtClient::on-error:
     * @client: Object instance that emitted the signal
     * @id: ID of the remote peer (20 bytes, Base64 encoded)
     * @error: The error being reported
     *
     * A signal reporting an asynchronous IO error, see dht_client_lookup().
     */
    dht_client_signals[SIGNAL_ON_ERROR] = g_signal_new("on-error",
            G_TYPE_FROM_CLASS(client_class), G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET(DhtClientClass, on_error),
            NULL, NULL, NULL, G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_ERROR);
}

static void dht_client_init(DhtClient *client)
{
    DhtClientPrivate *priv = dht_client_get_instance_private(client);
    priv->lookup_table = g_hash_table_new_full(dht_hash_func, dht_equal_func, NULL, dht_lookup_destroy);
    priv->key_size = DHT_KEY_SIZE;
}

DhtClient* dht_client_new(GSocketFamily family, guint16 port, GBytes *key, GError **error)
{
    if(key && (g_bytes_get_size(key) != crypto_scalarmult_SCALARBYTES))
    {
        if(error) *error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid key size");
        return NULL;
    }

    g_autoptr(GSocket) socket = g_socket_new(family, G_SOCKET_TYPE_DATAGRAM, G_SOCKET_PROTOCOL_UDP, error);
    if(!socket) return NULL;

    // Bind socket
    g_autoptr(GInetAddress) in_addr = g_inet_address_new_any(family);
    g_autoptr(GSocketAddress) sockaddr = g_inet_socket_address_new(in_addr, port);
    if(!g_socket_bind(socket, sockaddr, TRUE, error))
        return NULL;

    return g_object_new(DHT_TYPE_CLIENT, "key", key, "socket", socket, NULL);
}

gboolean dht_client_bootstrap(DhtClient *client, const gchar *host, guint16 port, GError **error)
{
    g_return_val_if_fail(DHT_IS_CLIENT(client), FALSE);
    g_return_val_if_fail(host != NULL, FALSE);
    DhtClientPrivate *priv = dht_client_get_instance_private(client);

    // Resolve host
    g_autoptr(GSocketAddress) sockaddr = NULL;
    g_autoptr(GResolver) resolver = g_resolver_get_default();
    GList *item, *list = g_resolver_lookup_by_name(resolver, host, NULL, error);
    for(item = list; item; item = item->next)
    {
        // Filter out correct address family
        GInetAddress *in_addr = G_INET_ADDRESS(item->data);
        if(g_inet_address_get_family(in_addr) == priv->family)
        {
            sockaddr = g_inet_socket_address_new(in_addr, port);
            break;
        }
    }

    g_resolver_free_addresses(list);

    if(sockaddr)
    {
        // Send request
        MsgLookup msg;
        msg.type = MSG_LOOKUP_REQ;
        memcpy(msg.srcid, priv->id, DHT_HASH_SIZE);
        memcpy(msg.dstid, priv->id, DHT_HASH_SIZE);
        return g_socket_send_to(priv->socket, sockaddr, (gchar*)&msg, sizeof(msg), NULL, error) > 0;
    }

    if(error) *error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_HOST_UNREACHABLE, "Unknown host");
    return FALSE;
}

gboolean dht_client_lookup(DhtClient *client, const gchar *id_base64, GError **error)
{
    g_return_val_if_fail(DHT_IS_CLIENT(client), FALSE);
    g_return_val_if_fail(id_base64 != NULL, FALSE);
    DhtClientPrivate *priv = dht_client_get_instance_private(client);

    gsize idlen = 0;
    g_autofree gpointer id = g_base64_decode(id_base64, &idlen);
    if(idlen != DHT_HASH_SIZE)
    {
        if(error) *error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid ID format");
        return FALSE;
    }

    if(memcmp(id, priv->id, DHT_HASH_SIZE) == 0)
    {
        // Fail if using own ID
        if(error) *error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CONNECTION_REFUSED, "Connection refused");
        return FALSE;
    }

    DhtLookup *lookup = g_hash_table_lookup(priv->lookup_table, id);
    if(lookup)
    {
        // Lookup already exists, just add connections
        lookup->num_connections++;
        return TRUE;
    }

    // Create lookup
    lookup = g_new0(DhtLookup, 1);
    memcpy(lookup->id, id, DHT_HASH_SIZE);
    lookup->parent = client;
    lookup->num_connections = 1;
    lookup->queries = g_sequence_new(dht_query_destroy);
    g_hash_table_insert(priv->lookup_table, lookup->id, lookup);

    // Dispatch lookup
    guint8 nodes[DHT_NODE_COUNT * (sizeof(MsgNode) + ADDR_SIZE(priv->family))];
    gsize count = dht_client_search(client, id, nodes);
    return dht_lookup_update(lookup, nodes, count, FALSE, error);
}

static void dht_client_set_property(GObject *obj, guint prop_id, const GValue *value, GParamSpec *pspec)
{
    DhtClient *client = DHT_CLIENT(obj);
    DhtClientPrivate *priv = dht_client_get_instance_private(client);

    switch(prop_id)
    {
        case PROP_KEY:
        {
            GBytes *key = g_value_get_boxed(value);
            if(key)
            {
                gsize keylen = 0;
                gconstpointer data = g_bytes_get_data(key, &keylen);
                memcpy(priv->sk, data, MIN(keylen, crypto_scalarmult_SCALARBYTES));
                g_warn_if_fail(keylen == crypto_scalarmult_SCALARBYTES);
            }
            else randombytes_buf(priv->sk, crypto_scalarmult_SCALARBYTES);
            break;
        }

        case PROP_SOCKET:
        {
            priv->socket = g_value_dup_object(value) ?: g_socket_new(G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_DATAGRAM, 0, NULL);
            priv->family = g_socket_get_family(priv->socket);
            break;
        }

        case PROP_KEY_SIZE:
        {
            priv->key_size = g_value_get_uint(value);
            break;
        }

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, prop_id, pspec);
            break;
    }
}

static void dht_client_get_property(GObject *obj, guint prop, GValue *value, GParamSpec *pspec)
{
    DhtClient *client = DHT_CLIENT(obj);
    DhtClientPrivate *priv = dht_client_get_instance_private(client);

    switch(prop)
    {
        case PROP_KEY:
        {
            g_value_take_boxed(value, g_bytes_new(priv->sk, crypto_scalarmult_SCALARBYTES));
            break;
        }

        case PROP_SOCKET:
        {
            g_value_set_object(value, priv->socket);
            break;
        }

        case PROP_KEY_SIZE:
        {
            g_value_set_uint(value, priv->key_size);
            break;
        }

        case PROP_ID:
        {
            g_value_take_string(value, g_base64_encode(priv->id, DHT_HASH_SIZE));
            break;
        }

        case PROP_PEERS:
        {
            guint count = 0;
            DhtBucket *bucket = &priv->root_bucket;
            while(bucket)
            {
                gsize i;
                for(i = 0; i < bucket->count; i++)
                {
                    if(bucket->nodes[i].is_alive) // count alive nodes
                        count++;
                }

                bucket = bucket->next;
            }

            g_value_set_uint(value, count);
            break;
        }

        case PROP_PUBLIC_ADDRESS:
        {
            g_autoptr(GInetAddress) in_addr = g_inet_address_new_from_bytes(priv->public_address, priv->family);
            g_value_take_object(value, g_inet_socket_address_new(in_addr, priv->public_port));
            break;
        }

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, prop, pspec);
            break;
    }
}

static void dht_client_constructed(GObject *obj)
{
    DhtClient *client = DHT_CLIENT(obj);
    DhtClientPrivate *priv = dht_client_get_instance_private(client);

    // Derive ID
    crypto_scalarmult_base(priv->pk, priv->sk);
    crypto_generichash(priv->id, DHT_HASH_SIZE, priv->pk, crypto_scalarmult_BYTES, NULL, 0);

    // Create bootstrapping lookup
    DhtLookup *lookup = g_new0(DhtLookup, 1);
    memcpy(lookup->id, priv->id, DHT_HASH_SIZE);
    lookup->parent = client;
    lookup->queries = g_sequence_new(dht_query_destroy);
    g_hash_table_insert(priv->lookup_table, lookup->id, lookup);

    // Attach sources
    g_autoptr(GSource) source = g_socket_create_source(priv->socket, G_IO_IN, NULL);
    g_source_set_callback(source, (GSourceFunc)dht_client_receive, client, NULL);
    priv->io_source = g_source_attach(source, g_main_context_default());
    priv->timeout_source = g_timeout_add(DHT_REFRESH_MS, dht_client_refresh, client);

    // Chain to parent
    G_OBJECT_CLASS(dht_client_parent_class)->constructed(obj);
}

static void dht_client_finalize(GObject *obj)
{
    DhtClient *client = DHT_CLIENT(obj);
    DhtClientPrivate *priv = dht_client_get_instance_private(client);

    g_hash_table_destroy(priv->lookup_table);
    g_list_free_full(priv->connections, dht_connection_destroy);
    g_object_unref(priv->socket);
    g_source_remove(priv->io_source);
    g_source_remove(priv->timeout_source);
    while(priv->root_bucket.next)
    {
        DhtBucket *next = priv->root_bucket.next->next;
        g_free(priv->root_bucket.next);
        priv->root_bucket.next = next;
    }

    // Chain to parent
    G_OBJECT_CLASS(dht_client_parent_class)->finalize(obj);
}

// Called periodically at DHT_REFRESH_MS
static gboolean dht_client_refresh(gpointer arg)
{
    DhtClient *client = DHT_CLIENT(arg);
    DhtClientPrivate *priv = dht_client_get_instance_private(client);

    // Create lookup
    DhtLookup *lookup = g_new0(DhtLookup, 1);
    lookup->queries = g_sequence_new(dht_query_destroy);
    lookup->parent = client;

    // Count buckets
    gsize count = 1;
    DhtBucket *bucket = &priv->root_bucket;
    while(bucket->next)
    {
        bucket = bucket->next;
        count++;
    }

    // Select random ID evenly distributed along buckets
    gint i, nbits = g_random_int_range(0, count);
    for(i = 0; i < DHT_HASH_SIZE; i++, nbits -= 8)
    {
        if(nbits >= 8)
            lookup->id[i] = priv->id[i];
        else if(nbits > 0)
            lookup->id[i] = (priv->id[i] & (0xFF << (8 - nbits))) | g_random_int_range(0, 0xFF >> nbits);
        else
            lookup->id[i] = g_random_int_range(0, 0xFF);
    }

    g_debug("refresh timeout");
    g_hash_table_insert(priv->lookup_table, lookup->id, lookup);

    // Dispatch lookup
    guint8 nodes[DHT_NODE_COUNT * (sizeof(MsgNode) + ADDR_SIZE(priv->family))];
    count = dht_client_search(client, lookup->id, nodes);
    dht_lookup_update(lookup, nodes, count, FALSE, NULL);

    return G_SOURCE_CONTINUE;
}

// Socket source handler for DhtClient
static gboolean dht_client_receive(GSocket *socket, GIOCondition condition, gpointer arg)
{
    DhtClient *client = DHT_CLIENT(arg);
    DhtClientPrivate *priv = dht_client_get_instance_private(client);
    g_autoptr(GSocketAddress) sockaddr = NULL;

    gchar buffer[MSG_BUFFER_SIZE];
    gssize len = g_socket_receive_from(socket, &sockaddr, buffer, sizeof(buffer), NULL, NULL);
    if(len > 0)
    {
        GInetAddress *in_addr = g_inet_socket_address_get_address(G_INET_SOCKET_ADDRESS(sockaddr));
        if(g_inet_address_get_family(in_addr) != priv->family)
            return G_SOURCE_CONTINUE;

        gconstpointer addr = g_inet_address_to_bytes(in_addr);
        guint16 port = g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(sockaddr));
        gsize addr_size = ADDR_SIZE(priv->family);
        gsize node_size = sizeof(MsgNode) + addr_size;

        switch(*(guint8*)buffer)
        {
            case MSG_LOOKUP_REQ:
            {
                if(len != sizeof(MsgLookup)) break;
                MsgLookup *msg = (MsgLookup*)buffer;

                if(memcmp(msg->srcid, priv->id, DHT_HASH_SIZE) == 0) break;
                dht_client_update(client, msg->srcid, TRUE, addr, port);
                g_debug("received lookup request");

                // Send response
                msg->type = MSG_LOOKUP_RES;
                memcpy(msg->srcid, priv->id, DHT_HASH_SIZE);
                gsize count = dht_client_search(client, msg->dstid, msg->nodes);
                g_socket_send_to(socket, sockaddr, buffer, len + count * node_size, NULL, NULL);
                break;
            }

            case MSG_LOOKUP_RES:
            {
                gsize count = (len - sizeof(MsgLookup)) / node_size;
                if(len != sizeof(MsgLookup) + node_size * count) break;
                MsgLookup *msg = (MsgLookup*)buffer;

                if(memcmp(msg->srcid, priv->id, DHT_HASH_SIZE) == 0) break;
                dht_client_update(client, msg->srcid, TRUE, addr, port);
                g_debug("received lookup response");

                // Find lookup
                DhtLookup *lookup = g_hash_table_lookup(priv->lookup_table, msg->dstid);
                if(lookup)
                {
                    guint8 metric[DHT_HASH_SIZE];
                    dht_memxor(metric, msg->srcid, lookup->id);

                    GSequenceIter *iter = g_sequence_lookup(lookup->queries, metric, dht_compare_func, NULL);
                    if(iter)
                    {
                        // Finalize existing query
                        DhtQuery *query = g_sequence_get(iter);
                        query->is_finished = TRUE;
                        if(query->timeout_source)
                        {
                            g_source_remove(query->timeout_source);
                            query->timeout_source = 0;
                            lookup->num_sources--;
                        }
                    }
                    else
                    {
                        // Insert finalized query
                        DhtQuery *query = g_slice_new0(DhtQuery);
                        dht_memxor(query->metric, msg->srcid, msg->dstid);
                        memcpy(query->addr, addr, addr_size);
                        query->port = port;
                        query->is_finished = TRUE;
                        query->parent = lookup;
                        g_sequence_insert_sorted(lookup->queries, query, dht_compare_func, NULL);
                    }

                    g_autoptr(GError) error = NULL;
                    dht_lookup_update(lookup, msg->nodes, count, TRUE, &error);
                }

                break;
            }

            case MSG_CONNECTION_REQ:
            {
                if(len != sizeof(MsgConnection)) break;
                MsgConnection *msg = (MsgConnection*)buffer;

                guint8 id[DHT_HASH_SIZE];
                crypto_generichash(id, DHT_HASH_SIZE, msg->pk, crypto_scalarmult_BYTES, NULL, 0);
                g_autofree gchar *id_base64 = g_base64_encode(id, DHT_HASH_SIZE);
                g_debug("received connection request %s", id_base64);

                gboolean accept = FALSE;
                g_signal_emit(client, dht_client_signals[SIGNAL_ACCEPT_CONNECTION], 0, id_base64, &accept);
                if(accept)
                {
                    MsgConnection response;
                    response.type = MSG_CONNECTION_RES;
                    memcpy(response.pk, priv->pk, crypto_scalarmult_BYTES);
                    randombytes_buf(response.nonce, DHT_NONCE_SIZE);

                    // Send response
                    g_autoptr(GSocket) connection_socket = g_socket_new(priv->family, G_SOCKET_TYPE_DATAGRAM, G_SOCKET_PROTOCOL_UDP, NULL);
                    g_socket_send_to(connection_socket, sockaddr, (gchar*)&response, sizeof(response), NULL, NULL);
                    g_debug("connection accepted");

                    // Derive keys
                    guint8 enc_key[priv->key_size];
                    guint8 dec_key[priv->key_size];
                    dht_kdf(priv->sk, msg->pk, msg->nonce, response.nonce, enc_key, dec_key, priv->key_size);

                    // Signal new connection
                    g_autoptr(GBytes) enc_key_bytes = g_bytes_new(enc_key, sizeof(enc_key));
                    g_autoptr(GBytes) dec_key_bytes = g_bytes_new(dec_key, sizeof(dec_key));
                    g_signal_emit(client, dht_client_signals[SIGNAL_NEW_CONNECTION], 0,
                            id_base64, connection_socket, sockaddr, enc_key_bytes, dec_key_bytes, TRUE);
                }
                else
                {
                    // Send empty packet to reject the connection
                    g_socket_send_to(socket, sockaddr, NULL, 0, NULL, NULL);
                    g_debug("connection rejected");
                }

                break;
            }
        }
    }

    return G_SOURCE_CONTINUE;
}

static void dht_client_update(DhtClient *client, gconstpointer id, gboolean is_alive, gconstpointer addr, guint16 port)
{
    DhtClientPrivate *priv = dht_client_get_instance_private(client);
    gsize addr_size = ADDR_SIZE(priv->family);

    g_debug("update node %016lx (%s)", GUINT64_FROM_BE(*(guint64*)id), is_alive ? "alive" : "timed-out");
    guint8 metric[DHT_HASH_SIZE], nbits;
    dht_memxor(metric, priv->id, id);

    DhtBucket *bucket = &priv->root_bucket;
    for(nbits = 0; bucket->next && !(metric[nbits / 8] & (0x80 >> nbits % 8)); nbits++)
        bucket = bucket->next;

    int i;
    for(i = 0; i < bucket->count; i++)
    {
        if(memcmp(bucket->nodes[i].id, id, DHT_HASH_SIZE) == 0)
        {
            // Update existing node
            bucket->nodes[i].is_alive = is_alive;
            if(is_alive)
            {
                memcpy(bucket->nodes[i].addr, addr, addr_size);
                bucket->nodes[i].port = port;
            }

            return;
        }
    }

    if(!is_alive)
        return;

    while((bucket->count == DHT_NODE_COUNT) && !bucket->next)
    {
        // Split buckets
        DhtBucket *prev = bucket, *next = g_new0(DhtBucket, 1);
        gsize count = bucket->count;

        prev->count = 0;
        next->count = 0;
        for(i = 0; i < count; i++)
        {
            if((bucket->nodes[i].id[nbits / 8] ^ priv->id[nbits / 8]) & (0x80 >> nbits % 8))
                prev->nodes[prev->count++] = bucket->nodes[i];
            else
                next->nodes[next->count++] = bucket->nodes[i];
        }

        prev->next = next;
        next->prev = prev;
        if(!(metric[nbits / 8] & (0x80 >> nbits % 8)))
        {
            bucket = next;
            nbits++;
        }
    }

    if(bucket->count < DHT_NODE_COUNT)
    {
        // Append new node
        memcpy(bucket->nodes[bucket->count].id, id, DHT_HASH_SIZE);
        bucket->nodes[bucket->count].is_alive = is_alive;

        memcpy(bucket->nodes[bucket->count].addr, addr, addr_size);
        bucket->nodes[bucket->count].port = port;
        bucket->count++;
        return;
    }

    for(i = 0; i < bucket->count; i++)
    {
        if(!bucket->nodes[i].is_alive)
        {
            // Replace dead node
            memcpy(bucket->nodes[i].id, id, DHT_HASH_SIZE);
            bucket->nodes[i].is_alive = is_alive;

            memcpy(bucket->nodes[i].addr, addr, addr_size);
            bucket->nodes[i].port = port;
            return;
        }
    }

    return;
}

static gsize dht_client_search(DhtClient *client, gconstpointer id, gpointer node_ptr)
{
    DhtClientPrivate *priv = dht_client_get_instance_private(client);
    gsize addr_size = ADDR_SIZE(priv->family);
    gsize node_size = sizeof(MsgNode) + addr_size;

    guint8 metric[DHT_HASH_SIZE], nbits = 0, dir = 1;
    dht_memxor(metric, priv->id, id);

    gsize count = 0;
    DhtBucket *bucket = &priv->root_bucket;
    while(count < DHT_NODE_COUNT)
    {
        if(!(metric[nbits / 8] & (0x80 >> (nbits % 8))) == !dir)
        {
            int i;
            for(i = 0; (i < bucket->count) && (count < DHT_NODE_COUNT); i++, count++)
            {
                MsgNode *node = node_ptr;
                node_ptr += node_size;

                // Copy node
                memcpy(node->id, bucket->nodes[i].id, DHT_HASH_SIZE);
                memcpy(node->addr, bucket->nodes[i].addr, addr_size);
                node->port = GUINT16_TO_BE(bucket->nodes[i].port);
            }
        }

        if(dir)
        {
            if(bucket->next)
            {
                bucket = bucket->next;
                nbits++;
            }
            else dir = 0;
        }
        else
        {
            if(bucket->prev)
            {
                bucket = bucket->prev;
                nbits--;
            }
            else break;
        }
    }

    return count;
}

static gboolean dht_query_timeout(gpointer arg)
{
    DhtQuery *query = arg;
    DhtLookup *lookup = query->parent;
    DhtClient *client = lookup->parent;

    // Update node
    guint8 id[DHT_HASH_SIZE];
    dht_memxor(id, query->metric, lookup->id);
    dht_client_update(client, id, FALSE, NULL, 0);
    query->is_finished = TRUE;

    // Update source counter
    query->timeout_source = 0;
    lookup->num_sources--;

    // Dispatch lookup
    g_autoptr(GError) error = NULL;
    dht_lookup_dispatch(lookup, TRUE, &error);
    return G_SOURCE_REMOVE;
}

// Called by GSequence destroy notify
static void dht_query_destroy(gpointer arg)
{
    DhtQuery *query = arg;

    // Abort timeout if any
    if(query->timeout_source)
        g_source_remove(query->timeout_source);

    g_slice_free(DhtQuery, query);
}

static gboolean dht_lookup_update(DhtLookup *lookup, gconstpointer node_ptr, gsize count, gboolean emit_error, GError **error)
{
    DhtClient *client = lookup->parent;
    DhtClientPrivate *priv = dht_client_get_instance_private(client);
    gsize addr_size = ADDR_SIZE(priv->family);
    gsize node_size = sizeof(MsgNode) + addr_size;

    while(count--)
    {
        const MsgNode *node = node_ptr;
        node_ptr += node_size;

        // Skip own ID, save address
        if(memcmp(node->id, priv->id, DHT_HASH_SIZE) == 0)
        {
            priv->public_port = GUINT16_FROM_BE(node->port);
            memcpy(priv->public_address, node->addr, addr_size);
            continue;
        }

        // Check if this is the target node
        if(memcmp(node->id, lookup->id, DHT_HASH_SIZE) == 0)
        {
            // Create connections
            while(lookup->num_connections--)
            {
                DhtConnection *connection = g_new0(DhtConnection, 1);
                memcpy(connection->id, lookup->id, DHT_HASH_SIZE);
                randombytes_buf(connection->nonce, DHT_NONCE_SIZE);
                connection->parent = client;

                // Send request
                MsgConnection request;
                request.type = MSG_CONNECTION_REQ;
                memcpy(request.pk, priv->pk, crypto_scalarmult_BYTES);
                memcpy(request.nonce, connection->nonce, DHT_NONCE_SIZE);
                g_debug("sending connection request");

                g_autoptr(GInetAddress) in_addr = g_inet_address_new_from_bytes(node->addr, priv->family);
                g_autoptr(GSocketAddress) sockaddr = g_inet_socket_address_new(in_addr, GUINT16_FROM_BE(node->port));
                connection->socket = g_socket_new(priv->family, G_SOCKET_TYPE_DATAGRAM, G_SOCKET_PROTOCOL_UDP, NULL);
                g_socket_send_to(connection->socket, sockaddr, (gchar*)&request, sizeof(request), NULL, NULL);

                // Attach socket source
                g_autoptr(GSource) source = g_socket_create_source(connection->socket, G_IO_IN, NULL);
                g_source_set_callback(source, (GSourceFunc)dht_connection_receive, connection, NULL);
                connection->io_source =  g_source_attach(source, g_main_context_default());

                // Attach timer, link with parent
                connection->timeout_source = g_timeout_add(DHT_TIMEOUT_MS, dht_connection_timeout, connection);
                connection->link = g_list_prepend(priv->connections, connection);
                priv->connections = connection->link;
            }

            // Destroy lookup
            g_hash_table_remove(priv->lookup_table, lookup->id);
            return TRUE;
        }

        guint8 metric[DHT_HASH_SIZE];
        dht_memxor(metric, lookup->id, node->id);

        // Skip duplicates
        GSequenceIter *iter = g_sequence_search(lookup->queries, metric, dht_compare_func, NULL);
        if(!g_sequence_iter_is_begin(iter) && !memcmp(g_sequence_get(g_sequence_iter_prev(iter)), metric, DHT_HASH_SIZE))
            continue;

        // Insert query
        DhtQuery *query = g_slice_new0(DhtQuery);
        memcpy(query->metric, metric, DHT_HASH_SIZE);
        memcpy(query->addr, node->addr, addr_size);
        query->port = GUINT16_FROM_BE(node->port);
        query->parent = lookup;

        g_sequence_insert_before(iter, query);
    }

    return dht_lookup_dispatch(lookup, emit_error, error);
}

static gboolean dht_lookup_dispatch(DhtLookup *lookup, gboolean emit_error, GError **error)
{
    DhtClient *client = lookup->parent;
    DhtClientPrivate *priv = dht_client_get_instance_private(client);

    gsize count = 0;
    GSequenceIter *iter = g_sequence_get_begin_iter(lookup->queries);
    while(!g_sequence_iter_is_end(iter) && (lookup->num_sources < DHT_CONCURRENCY) && (count < DHT_NODE_COUNT))
    {
        DhtQuery *query = g_sequence_get(iter);
        if(!query->is_finished && !query->timeout_source)
        {
            // Send request
            MsgLookup msg;
            msg.type = MSG_LOOKUP_REQ;
            memcpy(msg.srcid, priv->id, DHT_HASH_SIZE);
            memcpy(msg.dstid, lookup->id, DHT_HASH_SIZE);

            g_autoptr(GInetAddress) in_addr = g_inet_address_new_from_bytes(query->addr, priv->family);
            g_autoptr(GSocketAddress) sockaddr = g_inet_socket_address_new(in_addr, query->port);
            g_socket_send_to(priv->socket, sockaddr, (gchar*)&msg, sizeof(msg), NULL, NULL);

            // Attach timer
            query->timeout_source = g_timeout_add(DHT_TIMEOUT_MS, dht_query_timeout, query);
            lookup->num_sources++;
        }

        iter = g_sequence_iter_next(iter);
        count++;
    }

    if(lookup->num_sources == 0)
    {
        // No more queries, finalize lookup
        if(lookup->num_connections == 0)
        {
            // This is an internal lookup, suppress error
            g_hash_table_remove(priv->lookup_table, lookup->id);
            return TRUE;
        }

        if(error)
        {
            g_debug("lookup failed");
            *error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_HOST_NOT_FOUND, "DHT lookup failed");
            if(emit_error)
            {
                g_autofree gchar *id_base64 = g_base64_encode(lookup->id, DHT_HASH_SIZE);
                g_signal_emit(client, dht_client_signals[SIGNAL_ON_ERROR], 0, id_base64, *error);
            }
        }

        g_hash_table_remove(priv->lookup_table, lookup->id);
        return FALSE;
    }

    return TRUE;
}

// Called by GHashTable destroy notifier
static void dht_lookup_destroy(gpointer arg)
{
    DhtLookup *lookup = arg;
    g_sequence_free(lookup->queries);
    g_free(lookup);
}

// Socket source handler for DhtConnection
static gboolean dht_connection_receive(GSocket *socket, GIOCondition condition, gpointer arg)
{
    DhtConnection *connection = arg;
    DhtClient *client = connection->parent;
    DhtClientPrivate *priv = dht_client_get_instance_private(client);

    g_autofree gchar *id_base64 = g_base64_encode(connection->id, DHT_HASH_SIZE);
    g_autoptr(GSocketAddress) sockaddr = NULL;
    g_autoptr(GError) error = NULL;

    gchar buffer[MSG_BUFFER_SIZE];
    gssize len = g_socket_receive_from(socket, &sockaddr, buffer, sizeof(buffer), NULL, &error);
    if(error)
    {
        // Signal error
        g_signal_emit(client, dht_client_signals[SIGNAL_ON_ERROR], 0, id_base64, error);
        goto finalize;
    }

    // Check message type
    MsgConnection *msg = (MsgConnection*)buffer;
    if((len == sizeof(MsgConnection)) && (msg->type == MSG_CONNECTION_RES))
    {
        // Derive keys
        guint8 enc_key[priv->key_size];
        guint8 dec_key[priv->key_size];
        dht_kdf(priv->sk, msg->pk, msg->nonce, connection->nonce, enc_key, dec_key, priv->key_size);

        // Verify ID
        guint8 id[DHT_HASH_SIZE];
        crypto_generichash(id, DHT_HASH_SIZE, msg->pk, crypto_scalarmult_BYTES, NULL, 0);
        if(memcmp(id, connection->id, DHT_HASH_SIZE) == 0)
        {
            g_debug("received connection response %s", id_base64);
            g_autoptr(GBytes) enc_key_bytes = g_bytes_new(enc_key, sizeof(enc_key));
            g_autoptr(GBytes) dec_key_bytes = g_bytes_new(dec_key, sizeof(dec_key));
            g_signal_emit(client, dht_client_signals[SIGNAL_NEW_CONNECTION], 0,
                    id_base64, socket, sockaddr, enc_key_bytes, dec_key_bytes, FALSE);

            goto finalize;
        }
    }

    // Invalid response, signal error
    error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CONNECTION_REFUSED, "Connection refused");
    g_signal_emit(client, dht_client_signals[SIGNAL_ON_ERROR], 0, id_base64, error);

finalize:
    priv->connections = g_list_delete_link(priv->connections, connection->link);
    dht_connection_destroy(connection);
    return G_SOURCE_REMOVE;
}

static gboolean dht_connection_timeout(gpointer arg)
{
    DhtConnection *connection = arg;
    DhtClient *client = connection->parent;
    DhtClientPrivate *priv = dht_client_get_instance_private(client);

    g_debug("connection timeout");
    g_autofree gchar *id_base64 = g_base64_encode(connection->id, DHT_HASH_SIZE);
    g_autoptr(GError) error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "Connection timed out");
    g_signal_emit(client, dht_client_signals[SIGNAL_ON_ERROR], 0, id_base64, error);

    // Destroy connection
    priv->connections = g_list_delete_link(priv->connections, connection->link);
    dht_connection_destroy(connection);
    return G_SOURCE_REMOVE;
}

// Called by GList destroy notifier
static void dht_connection_destroy(gpointer arg)
{
    DhtConnection *connection = arg;

    g_source_remove(connection->io_source);
    g_source_remove(connection->timeout_source);
    g_object_unref(connection->socket);
    g_free(connection);
}
