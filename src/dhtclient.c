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
#include <glib/gi18n.h>
#include "dhtclient.h"

#define DHT_NONCE_SIZE 32
#define DHT_HASH_SIZE  20

#define DHT_NODE_COUNT 16 // number of nodes per bucket
#define DHT_CONCURRENCY 3 // number of concurrent requests per lookup

#define DHT_TIMEOUT_MS   1000 // request timeout (1 second)
#define DHT_REFRESH_MS  60000 // refresh period (1 minute)
#define DHT_LINGER_MS 3600000 // dead node linger (1 hour)

/* Message codes */
#define MSG_LOOKUP_REQ     0xC0
#define MSG_LOOKUP_RES     0xC1
#define MSG_CONNECTION_REQ 0xC2
#define MSG_CONNECTION_RES 0xC3

#define MSG_BUFFER_SIZE 1500
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
    PROP_LAST_SEEN,
    PROP_PACKETS_RECEIVED,
    PROP_PACKETS_SENT,
    PROP_BYTES_RECEIVED,
    PROP_BYTES_SENT,
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
typedef struct _msg_connection_request MsgConnectionRequest;
typedef struct _msg_connection_response MsgConnectionResponse;

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

struct _msg_connection_request
{
    guint8 type; // MSG_CONNECTION_REQ
    guint8 srcid[DHT_HASH_SIZE];
    guint8 nonce[DHT_NONCE_SIZE];
}
__attribute__((packed));

struct _msg_connection_response
{
    guint8 type; // MSG_CONNECTION_RES
    guint8 pk[crypto_scalarmult_BYTES];
    guint8 nonce[DHT_NONCE_SIZE];
    guint8 peer_nonce[DHT_NONCE_SIZE];
}
__attribute__((packed));

struct _dht_node
{
    guint8 id[DHT_HASH_SIZE];
    guint8 addr[ADDR_SIZE_MAX];
    guint16 port; // host byte order

    gboolean is_alive;
    gint64 last_seen; // monotonic timestamp (microseconds)
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
    gboolean is_finished, is_alive;
};

struct _dht_lookup
{
    guint8 id[DHT_HASH_SIZE]; // hash table key
    DhtClient *parent;

    GSequence *queries; // elements of type DhtQuery, sorted by metric
    gsize num_sources; // number of pending timeouts in the sequence
    gsize num_connections; // requested number of connections (0 for internal lookups)
    guint bootstrap_source; // timeout for bootstrap (0 if not bootstrap)
};

struct _dht_connection
{
    guint8 id[DHT_HASH_SIZE];
    guint8 nonce[DHT_NONCE_SIZE]; // hash table key
    DhtClient *parent;

    GSocket *socket;
    GSocketAddress *sockaddr;
    guint timeout_source; // GSource ID within default context
};

struct _dht_client_private
{
    guint8 id[DHT_HASH_SIZE];
    DhtBucket root_bucket;

    guint8 pk[crypto_scalarmult_BYTES]; // public key
    guint8 sk[crypto_scalarmult_SCALARBYTES]; // secret key
    gsize key_size; // derived key size

    GHashTable *lookup_table; // elements type of DhtLookup
    GHashTable *connection_table; // elements type of DhtConnection

    GSocket *socket;
    GSocketFamily family; // G_SOCKET_FAMILY_IPV4 or G_SOCKET_FAMILY_IPV6
    guint io_source, timeout_source; // GSource ID within default context

    // Statistics
    GTimeVal last_seen;
    guint64 packets_received, packets_sent;
    guint64 bytes_received, bytes_sent;
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

static gboolean dht_lookup_update(DhtLookup *lookup, gconstpointer node_ptr, gsize count, GError **error);
static gboolean dht_lookup_dispatch(DhtLookup *lookup, GError **error);
static gboolean dht_lookup_timeout(gpointer arg);
static void dht_lookup_destroy(gpointer arg);

static gboolean dht_connection_timeout(gpointer arg);
static void dht_connection_destroy(gpointer arg);

static inline void dht_xor(guint8 *metric, const guint8 *id1, const guint8 *id2)
{
    int i;
    for(i = 0; i < DHT_HASH_SIZE; i++)
        metric[i] = id1[i] ^ id2[i];
}

static guint dht_id_hash(gconstpointer key)
{
    return (((guint8*)key)[DHT_HASH_SIZE - 4] << 24) |
           (((guint8*)key)[DHT_HASH_SIZE - 3] << 16) |
           (((guint8*)key)[DHT_HASH_SIZE - 2] <<  8) |
           (((guint8*)key)[DHT_HASH_SIZE - 1]);
}

static guint dht_nonce_hash(gconstpointer key)
{
    return (((guint8*)key)[DHT_NONCE_SIZE - 4] << 24) |
           (((guint8*)key)[DHT_NONCE_SIZE - 3] << 16) |
           (((guint8*)key)[DHT_NONCE_SIZE - 2] <<  8) |
           (((guint8*)key)[DHT_NONCE_SIZE - 1]);
}

static gboolean dht_id_equal(gconstpointer a, gconstpointer b)
{
    return memcmp(a, b, DHT_HASH_SIZE) == 0;
}

static gboolean dht_nonce_equal(gconstpointer a, gconstpointer b)
{
    return memcmp(a, b, DHT_NONCE_SIZE) == 0;
}

static gint dht_metric_compare(gconstpointer a, gconstpointer b, gpointer arg)
{
    return memcmp(a, b, DHT_HASH_SIZE);
}

static void dht_kdf(gconstpointer secret, gconstpointer rx_nonce, gconstpointer tx_nonce,
        gpointer enc_key, gpointer dec_key, gsize key_size)
{
    crypto_generichash_state state;

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
            g_param_spec_boxed("key", _("Key"), _("Client private key"), G_TYPE_BYTES,
                    G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    /**
     * DhtClient:socket:
     * IPv4 or IPv6 datagram socket used for IO operations.
     */
    g_object_class_install_property(object_class, PROP_SOCKET,
            g_param_spec_object("socket", _("Socket"), _("Client socket"), G_TYPE_SOCKET,
                    G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    /**
     * DhtClient:key-size:
     * Size of the derived keys passed to #DhtClient::new-connection signal.
     */
    g_object_class_install_property(object_class, PROP_KEY_SIZE,
            g_param_spec_uint("key-size", _("Key size"), _("Size of the derived keys"),
                    crypto_generichash_BYTES_MIN, crypto_generichash_BYTES_MAX, crypto_generichash_BYTES,
                    G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    /**
     * DhtClient:id:
     * Client ID derived from private key (20 bytes, Base64 encoded).
     */
    g_object_class_install_property(object_class, PROP_ID,
            g_param_spec_string("id", _("ID"), _("Client ID"), NULL,
                    G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

    /**
     * DhtClient:peers:
     * Number of alive peers the client knows about.
     */
    g_object_class_install_property(object_class, PROP_PEERS,
            g_param_spec_uint("peers", _("Peers"), _("Number of peers"), 0, G_MAXUINT, 0,
                    G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

    /**
     * DhtClient:last-seen:
     * Local time of last message reception.
     */
    g_object_class_install_property(object_class, PROP_LAST_SEEN,
            g_param_spec_boxed("last-seen", _("Last seen"), _("Time of last message"), G_TYPE_DATE_TIME,
                    G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

    /**
     * DhtClient:packets-received:
     * Total number of received packets.
     */
    g_object_class_install_property(object_class, PROP_PACKETS_RECEIVED,
            g_param_spec_uint64("packets-received", _("Packets received"), _("Number of received packets"), 0, G_MAXUINT64, 0,
                    G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

    /**
     * DhtClient:packets-sent:
     * Total number of sent packets.
     */
    g_object_class_install_property(object_class, PROP_PACKETS_SENT,
            g_param_spec_uint64("packets-sent", _("Packets sent"), _("Number of sent packets"), 0, G_MAXUINT64, 0,
                    G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

    /**
     * DhtClient:bytes-received:
     * Total number of received bytes.
     */
    g_object_class_install_property(object_class, PROP_BYTES_RECEIVED,
            g_param_spec_uint64("bytes-received", _("Bytes received"), _("Number of received bytes"), 0, G_MAXUINT64, 0,
                    G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

    /**
     * DhtClient:bytes-sent:
     * Total number of sent bytes.
     */
    g_object_class_install_property(object_class, PROP_BYTES_SENT,
            g_param_spec_uint64("bytes-sent", _("Bytes sent"), _("Number of sent bytes"), 0, G_MAXUINT64, 0,
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
    priv->lookup_table = g_hash_table_new_full(dht_id_hash, dht_id_equal, NULL, dht_lookup_destroy);
    priv->connection_table = g_hash_table_new_full(dht_nonce_hash, dht_nonce_equal, NULL, dht_connection_destroy);
    priv->key_size = DHT_KEY_SIZE;
}

DhtClient* dht_client_new(GSocketFamily family, guint16 port, GBytes *key, GError **error)
{
    if(key && (g_bytes_get_size(key) != crypto_scalarmult_SCALARBYTES))
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, _("Invalid key size"));
        return NULL;
    }

    g_autoptr(GSocket) socket = g_socket_new(family, G_SOCKET_TYPE_DATAGRAM, G_SOCKET_PROTOCOL_UDP, error);
    if(!socket) return NULL;

    // Bind socket
    g_autoptr(GInetAddress) in_addr = g_inet_address_new_any(family);
    g_autoptr(GSocketAddress) sockaddr = g_inet_socket_address_new(in_addr, port);
    if(!g_socket_bind(socket, sockaddr, FALSE, error))
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
    GList *list = g_resolver_lookup_by_name(resolver, host, NULL, error);
    if(!list) return FALSE;

    GList *item;
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
        DhtLookup *lookup = g_hash_table_lookup(priv->lookup_table, priv->id);
        if(!lookup)
        {
            // Create lookup
            lookup = g_new0(DhtLookup, 1);
            memcpy(lookup->id, priv->id, DHT_HASH_SIZE);
            lookup->parent = client;
            lookup->queries = g_sequence_new(dht_query_destroy);
            lookup->bootstrap_source = g_timeout_add(DHT_TIMEOUT_MS, dht_lookup_timeout, lookup);
            g_hash_table_insert(priv->lookup_table, lookup->id, lookup);
            g_debug("bootstrapping");
        }

        // Send request
        MsgLookup msg;
        msg.type = MSG_LOOKUP_REQ;
        memcpy(msg.srcid, priv->id, DHT_HASH_SIZE);
        memcpy(msg.dstid, priv->id, DHT_HASH_SIZE);
        gssize res = g_socket_send_to(priv->socket, sockaddr, (gchar*)&msg, sizeof(msg), NULL, error);
        if(res >= 0)
        {
            priv->packets_sent++;
            priv->bytes_sent += res;
            return TRUE;
        }

        return FALSE;
    }

    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_HOST_UNREACHABLE, _("Unknown host"));
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
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, _("Invalid ID format"));
        return FALSE;
    }

    if(memcmp(id, priv->id, DHT_HASH_SIZE) == 0)
    {
        // Fail if using own ID
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CONNECTION_REFUSED, _("Connection refused"));
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
    return dht_lookup_update(lookup, nodes, count, error);
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

        case PROP_LAST_SEEN:
        {
            g_value_take_boxed(value, g_date_time_new_from_timeval_local(&priv->last_seen));
            break;
        }

        case PROP_PACKETS_RECEIVED:
        {
            g_value_set_uint64(value, priv->packets_received);
            break;
        }

        case PROP_PACKETS_SENT:
        {
            g_value_set_uint64(value, priv->packets_sent);
            break;
        }

        case PROP_BYTES_RECEIVED:
        {
            g_value_set_uint64(value, priv->bytes_received);
            break;
        }

        case PROP_BYTES_SENT:
        {
            g_value_set_uint64(value, priv->bytes_sent);
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
    g_hash_table_destroy(priv->connection_table);
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
    dht_lookup_update(lookup, nodes, count, NULL);

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
        priv->packets_received++;
        priv->bytes_received += len;

        switch(*(guint8*)buffer)
        {
            case MSG_LOOKUP_REQ:
            {
                if(len != sizeof(MsgLookup)) break;
                MsgLookup *msg = (MsgLookup*)buffer;

                // Ignore own ID
                if(memcmp(msg->srcid, priv->id, DHT_HASH_SIZE) == 0) break;

                g_debug("received lookup request");
                dht_client_update(client, msg->srcid, TRUE, addr, port);

                // Send response
                msg->type = MSG_LOOKUP_RES;
                memcpy(msg->srcid, priv->id, DHT_HASH_SIZE);
                gsize count = dht_client_search(client, msg->dstid, msg->nodes);
                gssize res = g_socket_send_to(socket, sockaddr, buffer, len + count * node_size, NULL, NULL);
                if(res >= 0)
                {
                    priv->packets_sent++;
                    priv->bytes_sent += res;
                }

                break;
            }

            case MSG_LOOKUP_RES:
            {
                gsize count = (len - sizeof(MsgLookup)) / node_size;
                if(len != sizeof(MsgLookup) + node_size * count) break;
                MsgLookup *msg = (MsgLookup*)buffer;

                // Ignore own ID
                if(memcmp(msg->srcid, priv->id, DHT_HASH_SIZE) == 0) break;

                g_debug("received lookup response");
                dht_client_update(client, msg->srcid, TRUE, addr, port);

                // Find lookup
                DhtLookup *lookup = g_hash_table_lookup(priv->lookup_table, msg->dstid);
                if(lookup)
                {
                    guint8 metric[DHT_HASH_SIZE];
                    dht_xor(metric, msg->srcid, lookup->id);

                    GSequenceIter *iter = g_sequence_lookup(lookup->queries, metric, dht_metric_compare, NULL);
                    if(iter)
                    {
                        // Finalize existing query
                        DhtQuery *query = g_sequence_get(iter);
                        query->is_finished = TRUE;
                        query->is_alive = TRUE;
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
                        dht_xor(query->metric, msg->srcid, msg->dstid);
                        memcpy(query->addr, addr, addr_size);
                        query->port = port;
                        query->is_finished = TRUE;
                        query->is_alive = TRUE;
                        query->parent = lookup;
                        g_sequence_insert_sorted(lookup->queries, query, dht_metric_compare, NULL);
                    }

                    g_autoptr(GError) error = NULL;
                    if(!dht_lookup_update(lookup, msg->nodes, count, &error))
                    {
                        g_autofree gchar *id_base64 = g_base64_encode(lookup->id, DHT_HASH_SIZE);
                        g_signal_emit(client, dht_client_signals[SIGNAL_ON_ERROR], 0, id_base64, *error);
                    }
                }

                break;
            }

            case MSG_CONNECTION_REQ:
            {
                if(len != sizeof(MsgConnectionRequest)) break;
                MsgConnectionRequest *msg = (MsgConnectionRequest*)buffer;

                // Ignore own ID
                if(memcmp(msg->srcid, priv->id, DHT_HASH_SIZE) == 0) break;

                g_debug("received connection request");
                dht_client_update(client, msg->srcid, TRUE, addr, port);

                gboolean accept = FALSE;
                g_autofree gchar *id_base64 = g_base64_encode(msg->srcid, DHT_HASH_SIZE);
                g_signal_emit(client, dht_client_signals[SIGNAL_ACCEPT_CONNECTION], 0, id_base64, &accept);
                if(accept)
                {
                    DhtConnection *connection = g_new0(DhtConnection, 1);
                    randombytes_buf(connection->nonce, DHT_NONCE_SIZE);
                    memcpy(connection->id, msg->srcid, DHT_HASH_SIZE);
                    connection->parent = client;

                    MsgConnectionResponse response;
                    response.type = MSG_CONNECTION_RES;
                    memcpy(response.pk, priv->pk, crypto_scalarmult_BYTES);
                    memcpy(response.nonce, connection->nonce, DHT_NONCE_SIZE);
                    memcpy(response.peer_nonce, msg->nonce, DHT_NONCE_SIZE);

                    // Send response
                    connection->socket = g_socket_new(priv->family, G_SOCKET_TYPE_DATAGRAM, G_SOCKET_PROTOCOL_UDP, NULL);
                    g_socket_send_to(connection->socket, sockaddr, (gchar*)&response, sizeof(response), NULL, NULL);

                    connection->timeout_source = g_timeout_add(DHT_TIMEOUT_MS, dht_connection_timeout, connection);
                    g_hash_table_insert(priv->connection_table, connection->nonce, connection);
                }

                break;
            }

            case MSG_CONNECTION_RES:
            {
                if(len != sizeof(MsgConnectionResponse)) break;
                MsgConnectionResponse *msg = (MsgConnectionResponse*)buffer;

                DhtConnection *connection = g_hash_table_lookup(priv->connection_table, msg->peer_nonce);
                if(!connection) break;

                guint8 id[DHT_HASH_SIZE];
                crypto_generichash(id, DHT_HASH_SIZE, msg->pk, crypto_scalarmult_BYTES, NULL, 0);
                if(memcmp(id, connection->id, DHT_HASH_SIZE) != 0) break;

                guint8 secret[crypto_scalarmult_BYTES];
                if(crypto_scalarmult(secret, priv->sk, msg->pk) != 0) break;
                g_debug("received connection response");

                gboolean is_remote = connection->socket ? TRUE : FALSE;
                if(!is_remote)
                {
                    MsgConnectionResponse response;
                    response.type = MSG_CONNECTION_RES;
                    memcpy(response.pk, priv->pk, crypto_scalarmult_BYTES);
                    memcpy(response.nonce, connection->nonce, DHT_NONCE_SIZE);
                    memcpy(response.peer_nonce, msg->nonce, DHT_NONCE_SIZE);

                    // Send response
                    connection->socket = g_socket_new(priv->family, G_SOCKET_TYPE_DATAGRAM, G_SOCKET_PROTOCOL_UDP, NULL);
                    g_socket_send_to(connection->socket, connection->sockaddr, (gchar*)&response, sizeof(response), NULL, NULL);
                }

                // Derive keys
                guint8 enc_key[priv->key_size];
                guint8 dec_key[priv->key_size];
                dht_kdf(secret, msg->nonce, connection->nonce, enc_key, dec_key, priv->key_size);

                g_autoptr(GBytes) enc_key_bytes = g_bytes_new(enc_key, sizeof(enc_key));
                g_autoptr(GBytes) dec_key_bytes = g_bytes_new(dec_key, sizeof(dec_key));
                g_autofree gchar *id_base64 = g_base64_encode(connection->id, DHT_HASH_SIZE);
                g_signal_emit(client, dht_client_signals[SIGNAL_NEW_CONNECTION], 0,
                        id_base64, connection->socket, sockaddr, enc_key_bytes, dec_key_bytes, is_remote);

                g_hash_table_remove(priv->connection_table, connection->nonce);
            }
        }
    }

    return G_SOURCE_CONTINUE;
}

static void dht_client_update(DhtClient *client, gconstpointer id, gboolean is_alive, gconstpointer addr, guint16 port)
{
    DhtClientPrivate *priv = dht_client_get_instance_private(client);
    gsize addr_size = ADDR_SIZE(priv->family);

    g_debug("update node %08x (%s)", dht_id_hash(id), is_alive ? "alive" : "timed-out");
    if(is_alive) g_get_current_time(&priv->last_seen);

    guint8 metric[DHT_HASH_SIZE], nbits;
    dht_xor(metric, priv->id, id);

    DhtBucket *bucket = &priv->root_bucket;
    for(nbits = 0; bucket->next && !(metric[nbits / 8] & (0x80 >> nbits % 8)); nbits++)
        bucket = bucket->next;

    gsize i;
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
                bucket->nodes[i].last_seen = g_get_monotonic_time();
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
        bucket->nodes[bucket->count].is_alive = TRUE;
        bucket->nodes[bucket->count].last_seen = g_get_monotonic_time();

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
            bucket->nodes[i].is_alive = TRUE;
            bucket->nodes[i].last_seen = g_get_monotonic_time();

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

    gint64 current_time = g_get_monotonic_time();
    guint8 metric[DHT_HASH_SIZE], nbits = 0, dir = 1;
    dht_xor(metric, priv->id, id);

    gsize count = 0;
    DhtBucket *bucket = &priv->root_bucket;
    while(count < DHT_NODE_COUNT)
    {
        if(!(metric[nbits / 8] & (0x80 >> (nbits % 8))) == !dir)
        {
            gsize i;
            for(i = 0; (i < bucket->count) && (count < DHT_NODE_COUNT); i++)
            {
                // Ignore timed out nodes
                if(!bucket->nodes[i].is_alive && (current_time - bucket->nodes[i].last_seen > DHT_LINGER_MS * 1000L))
                    continue;

                MsgNode *node = node_ptr;
                node_ptr += node_size;

                // Copy node
                memcpy(node->id, bucket->nodes[i].id, DHT_HASH_SIZE);
                memcpy(node->addr, bucket->nodes[i].addr, addr_size);
                node->port = GUINT16_TO_BE(bucket->nodes[i].port);
                count++;
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
    dht_xor(id, query->metric, lookup->id);
    dht_client_update(client, id, FALSE, NULL, 0);
    query->is_finished = TRUE;

    // Update source counter
    query->timeout_source = 0;
    lookup->num_sources--;

    // Dispatch lookup
    g_autoptr(GError) error = NULL;
    if(!dht_lookup_dispatch(lookup, &error))
    {
        g_autofree gchar *id_base64 = g_base64_encode(lookup->id, DHT_HASH_SIZE);
        g_signal_emit(client, dht_client_signals[SIGNAL_ON_ERROR], 0, id_base64, *error);
    }

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

static gboolean dht_lookup_update(DhtLookup *lookup, gconstpointer node_ptr, gsize count, GError **error)
{
    DhtClient *client = lookup->parent;
    DhtClientPrivate *priv = dht_client_get_instance_private(client);
    gsize addr_size = ADDR_SIZE(priv->family);
    gsize node_size = sizeof(MsgNode) + addr_size;

    while(count--)
    {
        const MsgNode *node = node_ptr;
        node_ptr += node_size;

        // Skip own ID
        if(memcmp(node->id, priv->id, DHT_HASH_SIZE) == 0)
            continue;

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
                MsgConnectionRequest request;
                request.type = MSG_CONNECTION_REQ;
                memcpy(request.srcid, priv->id, DHT_HASH_SIZE);
                memcpy(request.nonce, connection->nonce, DHT_NONCE_SIZE);
                g_debug("sending connection request");

                g_autoptr(GInetAddress) in_addr = g_inet_address_new_from_bytes(node->addr, priv->family);
                connection->sockaddr = g_inet_socket_address_new(in_addr, GUINT16_FROM_BE(node->port));
                gssize res = g_socket_send_to(priv->socket, connection->sockaddr, (gchar*)&request, sizeof(request), NULL, NULL);
                if(res >= 0)
                {
                    priv->packets_sent++;
                    priv->bytes_sent += res;
                }

                connection->timeout_source = g_timeout_add(DHT_TIMEOUT_MS, dht_connection_timeout, connection);
                g_hash_table_insert(priv->connection_table, connection->nonce, connection);
            }

            // Destroy lookup
            g_hash_table_remove(priv->lookup_table, lookup->id);
            return TRUE;
        }

        guint8 metric[DHT_HASH_SIZE];
        dht_xor(metric, lookup->id, node->id);

        // Skip duplicates
        GSequenceIter *iter = g_sequence_search(lookup->queries, metric, dht_metric_compare, NULL);
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

    return dht_lookup_dispatch(lookup, error);
}

static gboolean dht_lookup_dispatch(DhtLookup *lookup, GError **error)
{
    DhtClient *client = lookup->parent;
    DhtClientPrivate *priv = dht_client_get_instance_private(client);
    g_debug("dispatching lookup %08x", dht_id_hash(lookup->id));

    gsize num_alive = 0;
    GSequenceIter *iter = g_sequence_get_begin_iter(lookup->queries);
    while(!g_sequence_iter_is_end(iter) && (lookup->num_sources < DHT_CONCURRENCY) && (num_alive < DHT_NODE_COUNT))
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
            gssize res = g_socket_send_to(priv->socket, sockaddr, (gchar*)&msg, sizeof(msg), NULL, NULL);
            if(res >= 0)
            {
                priv->packets_sent++;
                priv->bytes_sent += res;
            }

            // Attach timer
            query->timeout_source = g_timeout_add(DHT_TIMEOUT_MS, dht_query_timeout, query);
            lookup->num_sources++;
        }

        if(query->is_alive) num_alive++;
        iter = g_sequence_iter_next(iter);
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

        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_HOST_NOT_FOUND, _("DHT lookup failed"));
        g_hash_table_remove(priv->lookup_table, lookup->id);
        return FALSE;
    }

    return TRUE;
}

// Called for bootstrap lookup after DHT_TIMEOUT_MS
static gboolean dht_lookup_timeout(gpointer arg)
{
    DhtLookup *lookup = arg;
    DhtClient *client = lookup->parent;
    DhtClientPrivate *priv = dht_client_get_instance_private(client);

    lookup->bootstrap_source = 0;
    if(lookup->num_sources == 0)
    {
        g_hash_table_remove(priv->lookup_table, lookup->id);

        // Signal bootstrap error
        g_autofree gchar *id_base64 = g_base64_encode(priv->id, DHT_HASH_SIZE);
        g_autoptr(GError) error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_TIMED_OUT, _("Bootstrap timed out"));
        g_signal_emit(client, dht_client_signals[SIGNAL_ON_ERROR], 0, id_base64, error);
    }

    return G_SOURCE_REMOVE;
}

// Called by GHashTable destroy notifier
static void dht_lookup_destroy(gpointer arg)
{
    DhtLookup *lookup = arg;
    if(lookup->bootstrap_source) g_source_remove(lookup->bootstrap_source);
    g_sequence_free(lookup->queries);
    g_free(lookup);
}

static gboolean dht_connection_timeout(gpointer arg)
{
    DhtConnection *connection = arg;
    DhtClient *client = connection->parent;
    DhtClientPrivate *priv = dht_client_get_instance_private(client);

    g_autofree gchar *id_base64 = g_base64_encode(connection->id, DHT_HASH_SIZE);
    g_autoptr(GError) error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_TIMED_OUT, _("Connection timed out"));
    g_signal_emit(client, dht_client_signals[SIGNAL_ON_ERROR], 0, id_base64, error);

    g_hash_table_remove(priv->connection_table, connection->nonce);
    return G_SOURCE_REMOVE;
}

// Called by GList destroy notifier
static void dht_connection_destroy(gpointer arg)
{
    DhtConnection *connection = arg;

    if(connection->socket) g_object_unref(connection->socket);
    if(connection->sockaddr) g_object_unref(connection->sockaddr);
    g_source_remove(connection->timeout_source);
    g_free(connection);
}
