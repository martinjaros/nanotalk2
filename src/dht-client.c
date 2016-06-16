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

#define G_LOG_DOMAIN "DHT"

#include <string.h>
#include <glib/gi18n.h>
#include "dht-client.h"

#define DHT_NODE_COUNT 16 // number of nodes per bucket
#define DHT_CONCURRENCY 3 // number of concurrent requests per lookup

#define DHT_TIMEOUT_MS 1000 // request timeout (1 second)
#define DHT_REFRESH_MS 60000 // refresh period (1 minute)
#define DHT_LINGER_US 3600000000LL // dead node linger (1 hour)

#define MSG_MTU 1500 // message buffer size

enum
{
    MSG_LOOKUP_REQ = 0xC0,
    MSG_LOOKUP_RES = 0xC1,
    MSG_CONNECTION_REQ = 0xC2,
    MSG_CONNECTION_RES = 0xC3
};

enum
{
    PROP_0,
    PROP_KEY,
    PROP_ID,
    PROP_PEERS,
    PROP_LISTEN,
    PROP_LAST
};

enum
{
    SIGNAL_NEW_CONNECTION,
    SIGNAL_LAST
};

typedef struct _msg_node MsgNode;
typedef struct _msg_lookup MsgLookup;
typedef struct _msg_connection MsgConnection;

typedef struct _dht_node DhtNode;
typedef struct _dht_query DhtQuery;
typedef struct _dht_lookup DhtLookup;
typedef struct _dht_connection DhtConnection;
typedef struct _dht_client_private DhtClientPrivate;

struct _msg_node
{
    DhtId id;
    DhtAddress addr;
};

struct _msg_lookup
{
    guint8 type;
    DhtId srcid;
    DhtId dstid;
    MsgNode nodes[0]; // up to DHT_NODE_COUNT
};

struct _msg_connection
{
    guint8 type;
    DhtKey pubkey;
    DhtKey nonces[0]; // source nonce, peer nonce (response only)
};

struct _dht_node
{
    DhtId id;
    DhtAddress addr;
    gint64 timestamp;
    gboolean is_alive;
};

struct _dht_query
{
    DhtId metric;
    DhtAddress addr;

    guint timeout_source;
    gboolean is_finished, is_alive;

    DhtLookup *lookup; // weak
};

struct _dht_lookup
{
    DhtId id;

    GHashTable *query_table; // <DhtAddress, DhtQuery>
    GSequence *query_sequence; // <weak DhtQuery> (sorted by metric)
    guint num_sources;

    GSList *results; // <GSimpleAsyncResult>
    DhtClient *client; // weak
};

struct _dht_connection
{
    DhtId id;
    DhtKey nonce;

    guint timeout_source;

    GSocket *socket;
    GSocketAddress *sockaddr;
    DhtKey enc_key, dec_key;

    GSimpleAsyncResult *result;
    DhtClient *client; // weak
};

struct _dht_client_private
{
    DhtId id;
    DhtKey pubkey, privkey;

    GList *buckets; // <GList<DhtNode>>
    GHashTable *cache; // <DhtId, DhtNode>

    GHashTable *lookup_table; // <DhtId, DhtLookup>
    GHashTable *connection_table; // <DhtKey, DhtConnection>

    gboolean listen;
    guint num_buckets;
    guint num_peers;

    GSocket *socket;
    guint socket_source;
    guint timeout_source;
};

static GParamSpec *dht_client_properties[PROP_LAST];
static guint dht_client_signals[SIGNAL_LAST];

G_DEFINE_TYPE_WITH_PRIVATE(DhtClient, dht_client, G_TYPE_OBJECT)

static void dht_client_set_property(GObject *obj, guint prop_id, const GValue *value, GParamSpec *pspec);
static void dht_client_get_property(GObject *obj, guint prop_id, GValue *value, GParamSpec *pspec);
static void dht_client_finalize(GObject *obj);

static void dht_client_update(DhtClient *client, const DhtId *id, const DhtAddress *addr, gboolean is_alive);
static guint dht_client_search(DhtClient *client, const DhtId *id, MsgNode *nodes);
static void dht_lookup_update1(DhtLookup *lookup, const DhtId *id, const DhtAddress *addr);
static void dht_lookup_update(DhtLookup *lookup, const MsgNode *nodes, guint count);
static void dht_lookup_dispatch(DhtLookup *lookup);

static gboolean dht_client_refresh_cb(gpointer arg);
static gboolean dht_client_receive_cb(GSocket *socket, GIOCondition condition, gpointer arg);
static gboolean dht_query_timeout_cb(gpointer arg);
static gboolean dht_connection_timeout_cb(gpointer arg);

static void dht_node_destroy_cb(gpointer arg);
static void dht_bucket_destroy_cb(gpointer arg);
static void dht_query_destroy_cb(gpointer arg);
static void dht_lookup_destroy_cb(gpointer arg);
static void dht_connection_destroy_cb(gpointer arg);
static void dht_result_destroy_cb(gpointer arg);

static void dht_client_class_init(DhtClientClass *client_class)
{
    GObjectClass *object_class = (GObjectClass*)client_class;
    object_class->set_property = dht_client_set_property;
    object_class->get_property = dht_client_get_property;
    object_class->finalize = dht_client_finalize;

    dht_client_properties[PROP_KEY] = g_param_spec_boxed("key", "Key", "Private key", DHT_TYPE_KEY,
            G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    dht_client_properties[PROP_ID] = g_param_spec_boxed("id", "ID", "Public ID", DHT_TYPE_ID,
            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    dht_client_properties[PROP_PEERS] = g_param_spec_uint("peers", "Peers", "Number of peers", 0, G_MAXUINT, 0,
            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    dht_client_properties[PROP_LISTEN] = g_param_spec_boolean("listen", "Listen", "Listen for incoming connections", FALSE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties(object_class, PROP_LAST, dht_client_properties);

    dht_client_signals[SIGNAL_NEW_CONNECTION] = g_signal_new("new-connection",
            DHT_TYPE_CLIENT, G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET(DhtClientClass, new_connection), NULL, NULL, NULL,
            G_TYPE_NONE, 5, DHT_TYPE_ID, G_TYPE_SOCKET, G_TYPE_SOCKET_ADDRESS, DHT_TYPE_KEY, DHT_TYPE_KEY);
}

static void dht_client_init(DhtClient *client)
{
    DhtClientPrivate *priv = dht_client_get_instance_private(client);

    priv->buckets = g_list_alloc();
    priv->num_buckets = 1;

    priv->cache = g_hash_table_new_full(dht_id_hash, dht_id_equal, NULL, dht_node_destroy_cb);
    priv->lookup_table = g_hash_table_new_full(dht_id_hash, dht_id_equal, NULL, dht_lookup_destroy_cb);
    priv->connection_table = g_hash_table_new_full(dht_key_hash, dht_key_equal, NULL, dht_connection_destroy_cb);

    // Create socket
    g_autoptr(GError) error = NULL;
    priv->socket = g_socket_new(DHT_ADDRESS_FAMILY, G_SOCKET_TYPE_DATAGRAM, G_SOCKET_PROTOCOL_UDP, &error);
    if(error) g_error("%s", error->message);

    // Attach sources
    g_autoptr(GSource) source = g_socket_create_source(priv->socket, G_IO_IN, NULL);
    g_source_set_callback(source, (GSourceFunc)dht_client_receive_cb, client, NULL);
    priv->socket_source = g_source_attach(source, g_main_context_default());
    priv->timeout_source = g_timeout_add(DHT_REFRESH_MS, dht_client_refresh_cb, client);
}

DhtClient* dht_client_new(DhtKey *key)
{
    return g_object_new(DHT_TYPE_CLIENT, "key", key, NULL);
}

static void dht_client_set_property(GObject *obj, guint prop_id, const GValue *value, GParamSpec *pspec)
{
    DhtClient *client = DHT_CLIENT(obj);
    DhtClientPrivate *priv = dht_client_get_instance_private(client);

    switch(prop_id)
    {
        case PROP_KEY:
        {
            DhtKey *key = g_value_get_boxed(value);
            if(key) priv->privkey = *key; else dht_key_make_random(&priv->privkey);
            dht_key_make_public(&priv->pubkey, &priv->privkey);
            dht_id_from_pubkey(&priv->id, &priv->pubkey);
            break;
        }

        case PROP_LISTEN:
            priv->listen = g_value_get_boolean(value);
            break;

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
            g_value_set_boxed(value, &priv->privkey);
            break;

        case PROP_ID:
            g_value_set_boxed(value, &priv->id);
            break;

        case PROP_PEERS:
            g_value_set_uint(value, priv->num_peers);
            break;

        case PROP_LISTEN:
            g_value_set_boolean(value, priv->listen);
            break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, prop, pspec);
            break;
    }
}

static void dht_client_finalize(GObject *obj)
{
    DhtClient *client = DHT_CLIENT(obj);
    DhtClientPrivate *priv = dht_client_get_instance_private(client);

    g_hash_table_destroy(priv->cache);
    g_hash_table_destroy(priv->lookup_table);
    g_hash_table_destroy(priv->connection_table);
    g_list_free_full(priv->buckets, dht_bucket_destroy_cb);

    g_object_unref(priv->socket);
    g_source_remove(priv->socket_source);
    g_source_remove(priv->timeout_source);

    G_OBJECT_CLASS(dht_client_parent_class)->finalize(obj);
}

gboolean dht_client_bind(DhtClient *client, GSocketAddress *address, gboolean allow_reuse, GError **error)
{
    g_return_val_if_fail(DHT_IS_CLIENT(client), FALSE);
    DhtClientPrivate *priv = dht_client_get_instance_private(client);

    return g_socket_bind(priv->socket, address, allow_reuse, error);
}

gboolean dht_client_bootstrap(DhtClient *client, GSocketAddress *address, GError **error)
{
    g_return_val_if_fail(DHT_IS_CLIENT(client), FALSE);
    DhtClientPrivate *priv = dht_client_get_instance_private(client);

    MsgLookup msg;
    msg.type = MSG_LOOKUP_REQ;
    msg.srcid = priv->id;
    msg.dstid = priv->id;
    return g_socket_send_to(priv->socket, address, (gchar*)&msg, sizeof(msg), NULL, error);
}

void dht_client_lookup_async(DhtClient *client, const DhtId *id, GAsyncReadyCallback callback, gpointer user_data)
{
    g_return_if_fail(DHT_IS_CLIENT(client));
    DhtClientPrivate *priv = dht_client_get_instance_private(client);

    // Fail if using own ID
    if(dht_id_equal(id, &priv->id))
    {
        g_simple_async_report_error_in_idle(G_OBJECT(client), callback, user_data, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, _("Invalid ID"));
        return;
    }

    GSimpleAsyncResult *result = g_simple_async_result_new(G_OBJECT(client), callback, user_data, NULL);
    DhtLookup *lookup = g_hash_table_lookup(priv->lookup_table, id);
    if(lookup)
    {
        // Lookup already exists, insert callback
        lookup->results = g_slist_prepend(lookup->results, result);
        return;
    }

    // Create lookup
    lookup = g_slice_new(DhtLookup);
    lookup->client = client;
    lookup->id = *id;
    lookup->num_sources = 0;
    lookup->query_sequence = g_sequence_new(NULL);
    lookup->query_table = g_hash_table_new_full(dht_address_hash, dht_address_equal, NULL, dht_query_destroy_cb);
    lookup->results = g_slist_prepend(NULL, result);
    g_hash_table_insert(priv->lookup_table, &lookup->id, lookup);

    // Dispatch lookup
    MsgNode nodes[DHT_NODE_COUNT];
    guint count = dht_client_search(client, id, nodes);
    dht_lookup_update(lookup, nodes, count);
    dht_lookup_dispatch(lookup);
}

gboolean dht_client_lookup_finish(DhtClient *client, GAsyncResult *result,
        GSocket **socket, GSocketAddress **address, DhtKey *enc_key, DhtKey *dec_key, GError **error)
{
    g_return_val_if_fail(DHT_IS_CLIENT(client), FALSE);

    if(g_simple_async_result_propagate_error(G_SIMPLE_ASYNC_RESULT(result), error))
        return FALSE;

    DhtConnection *connection = g_simple_async_result_get_op_res_gpointer(G_SIMPLE_ASYNC_RESULT(result));
    if(socket) *socket = g_object_ref(connection->socket);
    if(address) *address = g_object_ref(connection->sockaddr);
    if(enc_key) *enc_key = connection->enc_key;
    if(dec_key) *dec_key = connection->dec_key;

    return TRUE;
}

static void dht_client_update(DhtClient *client, const DhtId *id, const DhtAddress *addr, gboolean is_alive)
{
    DhtClientPrivate *priv = dht_client_get_instance_private(client);
    g_debug("update node %08x (%s)", dht_id_hash(id), is_alive ? "alive" : "timed-out");

    DhtId metric;
    dht_id_xor(&metric, &priv->id, id);

    guint nbits;
    GList *bucket = priv->buckets;
    for(nbits = 0; bucket->next && !(metric.data[nbits / 8] & (0x80 >> nbits % 8)); nbits++)
        bucket = bucket->next;

    // Iterate nodes
    guint count = 0;
    DhtNode *replaceable = NULL;
    GList *link = bucket->data;
    while(link)
    {
        DhtNode *node = link->data;
        if(dht_id_equal(&node->id, id))
        {
            // Update existing node
            if(is_alive)
            {
                node->timestamp = g_get_monotonic_time();
                node->is_alive = TRUE;
                node->addr = *addr;
            }
            else if(dht_address_equal(&node->addr, addr))
                node->is_alive = FALSE;

            return;
        }

        if(!node->is_alive)
            replaceable = node; // mark node for replacement

        link = link->next;
        count++;
    }

    if(!is_alive) return;

    if(count == DHT_NODE_COUNT)
    {
        if(replaceable)
        {
            // Replace existing node
            replaceable->timestamp = g_get_monotonic_time();
            replaceable->is_alive = TRUE;
            replaceable->addr = *addr;
            replaceable->id = *id;
        }

        return;
    }

    // Insert new node
    DhtNode *node = g_slice_new(DhtNode);
    node->timestamp = g_get_monotonic_time();
    node->is_alive = TRUE;
    node->addr = *addr;
    node->id = *id;

    bucket->data = g_list_prepend(bucket->data, node);
    priv->num_peers++;
    count++;

    // Split buckets
    while((count == DHT_NODE_COUNT) && !bucket->next)
    {
        GList *first = NULL, *last = NULL;

        count = 0;
        link = bucket->data;
        while(link)
        {
            // Redistribute nodes
            GList *next = link->next;
            DhtNode *node = link->data;
            if(!((node->id.data[nbits / 8] ^ priv->id.data[nbits / 8]) & (0x80 >> nbits % 8)))
            {
                bucket->data = g_list_remove_link(bucket->data, link);
                first = g_list_concat(last, link);
                last = link;
                count++;
            }

            link = next;
        }

        bucket = g_list_append(bucket, first)->next;
        priv->num_buckets++;
        nbits++;
    }

    return;
}

static guint dht_client_search(DhtClient *client, const DhtId *id, MsgNode *nodes)
{
    DhtClientPrivate *priv = dht_client_get_instance_private(client);

    DhtId metric;
    dht_id_xor(&metric, &priv->id, id);

    gint64 timestamp = g_get_monotonic_time();
    guint count = 0, nbits = 0, dir = 1;

    GList *bucket = priv->buckets;
    while(bucket)
    {
        if(!(metric.data[nbits / 8] & (0x80 >> (nbits % 8))) == !dir)
        {
            // Iterate nodes
            GList *link = bucket->data;
            while(link)
            {
                GList *next = link->next;
                DhtNode *node = link->data;
                if(node->is_alive || (timestamp - node->timestamp < DHT_LINGER_US))
                {
                    // Copy alive node
                    nodes->id = node->id;
                    nodes->addr = node->addr;
                    nodes++;

                    if(++count == DHT_NODE_COUNT)
                        return count;
                }
                else
                {
                    g_debug("delete node %08x", dht_id_hash(&node->id));

                    // Delete dead node
                    g_slice_free(DhtNode, node);
                    bucket->data = g_list_delete_link(bucket->data, link);
                    priv->num_peers--;
                }

                link = next;
            }
        }

        // Iterate buckets
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
            bucket = bucket->prev;
            nbits--;
        }
    }

    return count;
}

static void dht_lookup_update1(DhtLookup *lookup, const DhtId *id, const DhtAddress *addr)
{
    DhtQuery *query = g_hash_table_lookup(lookup->query_table, addr);
    if(query)
    {
        // Finalize existing query
        query->is_alive = TRUE;
        query->is_finished = TRUE;
        if(query->timeout_source > 0)
        {
            g_source_remove(query->timeout_source);
            query->timeout_source = 0;
            lookup->num_sources--;
        }
    }
    else
    {
        DhtId metric;
        dht_id_xor(&metric, id, &lookup->id);

        // Insert new finalized query
        query = g_slice_new(DhtQuery);
        query->lookup = lookup;
        query->metric = metric;
        query->addr = *addr;
        query->is_alive = TRUE;
        query->is_finished = TRUE;
        query->timeout_source = 0;

        g_hash_table_insert(lookup->query_table, &query->addr, query);
        g_sequence_insert_sorted(lookup->query_sequence, query, dht_id_compare, NULL);
    }
}

static void dht_lookup_update(DhtLookup *lookup, const MsgNode *nodes, guint count)
{
    DhtClient *client = lookup->client;
    DhtClientPrivate *priv = dht_client_get_instance_private(client);

    while(count--)
    {
        const MsgNode *node = nodes++;

        // Skip own ID
        if(dht_id_equal(&node->id, &priv->id))
            continue;

        if(!g_hash_table_contains(lookup->query_table, &node->addr))
        {
            DhtId metric;
            dht_id_xor(&metric, &lookup->id, &node->id);

            // Insert query
            DhtQuery *query = g_slice_new(DhtQuery);
            query->lookup = lookup;
            query->metric = metric;
            query->addr = node->addr;
            query->is_alive = FALSE;
            query->is_finished = FALSE;
            query->timeout_source = 0;

            g_hash_table_insert(lookup->query_table, &query->addr, query);
            g_sequence_insert_sorted(lookup->query_sequence, query, dht_id_compare, NULL);
        }
    }
}

static void dht_lookup_dispatch(DhtLookup *lookup)
{
    DhtClient *client = lookup->client;
    DhtClientPrivate *priv = dht_client_get_instance_private(client);
    g_debug("dispatching lookup %08x", dht_id_hash(&lookup->id));

    guint num_alive = 0;
    GSequenceIter *iter = g_sequence_get_begin_iter(lookup->query_sequence);
    while(!g_sequence_iter_is_end(iter) && (lookup->num_sources < DHT_CONCURRENCY) && (num_alive < DHT_NODE_COUNT))
    {
        DhtQuery *query = g_sequence_get(iter);
        if(!query->is_finished && (query->timeout_source == 0))
        {
            // Send request
            MsgLookup msg;
            msg.type = MSG_LOOKUP_REQ;
            msg.srcid = priv->id;
            msg.dstid = lookup->id;

            g_autoptr(GSocketAddress) sockaddr = dht_address_deserialize(&query->addr);
            g_socket_send_to(priv->socket, sockaddr, (gchar*)&msg, sizeof(msg), NULL, NULL);
            query->timeout_source = g_timeout_add(DHT_TIMEOUT_MS, dht_query_timeout_cb, query);
            lookup->num_sources++;
        }

        if(query->is_alive) num_alive++;
        iter = g_sequence_iter_next(iter);
    }

    if(lookup->num_sources == 0)
        g_hash_table_remove(priv->lookup_table, &lookup->id);
}

static gboolean dht_client_refresh_cb(gpointer arg)
{
    g_autoptr(DhtClient) client = g_object_ref(arg); // keep internal reference
    DhtClientPrivate *priv = dht_client_get_instance_private(client);

    // Clear cache
    g_hash_table_remove_all(priv->cache);

    // Create lookup
    DhtLookup *lookup = g_slice_new(DhtLookup);
    lookup->client = client;
    lookup->num_sources = 0;
    lookup->query_sequence = g_sequence_new(NULL);
    lookup->query_table = g_hash_table_new_full(dht_address_hash, dht_address_equal, NULL, dht_query_destroy_cb);
    lookup->results = NULL;

    // Generate ID
    gint i, nbits = g_random_int_range(0, priv->num_buckets);
    for(i = 0; i < DHT_ID_SIZE; i++, nbits -= 8)
    {
        if(nbits >= 8)
            lookup->id.data[i] = priv->id.data[i];
        else if(nbits > 0)
            lookup->id.data[i] = (priv->id.data[i] & (0xFF << (8 - nbits))) | g_random_int_range(0, 0xFF >> nbits);
        else
            lookup->id.data[i] = g_random_int_range(0, 0xFF);
    }

    g_hash_table_replace(priv->lookup_table, &lookup->id, lookup);

    // Dispatch lookup
    MsgNode nodes[DHT_NODE_COUNT];
    guint count = dht_client_search(client, &lookup->id, nodes);
    dht_lookup_update(lookup, nodes, count);
    dht_lookup_dispatch(lookup);

    return G_SOURCE_CONTINUE;
}

static gboolean dht_client_receive_cb(GSocket *socket, GIOCondition condition, gpointer arg)
{
    g_autoptr(DhtClient) client = g_object_ref(arg); // keep internal reference
    DhtClientPrivate *priv = dht_client_get_instance_private(client);

    guint8 buffer[MSG_MTU];
    g_autoptr(GSocketAddress) sockaddr = NULL;
    gssize len = g_socket_receive_from(socket, &sockaddr, (gchar*)buffer, sizeof(buffer), NULL, NULL);
    if(len < 1) return G_SOURCE_CONTINUE;

    switch(buffer[0])
    {
        case MSG_LOOKUP_REQ:
        {
            if(len != sizeof(MsgLookup)) break;
            MsgLookup *msg = (MsgLookup*)buffer;

            // Ignore own source ID
            if(dht_id_equal(&msg->srcid, &priv->id)) break;

            DhtAddress addr;
            dht_address_serialize(&addr, sockaddr);

            if(dht_id_equal(&msg->dstid, &priv->id))
            {
                // Cache node
                DhtNode *node = g_slice_new(DhtNode);
                node->timestamp = g_get_monotonic_time();
                node->id = msg->srcid;
                node->addr = addr;
                node->is_alive = TRUE;

                g_hash_table_replace(priv->cache, &node->id, node);
            }

            g_debug("lookup request %08x -> %08x", dht_id_hash(&msg->srcid), dht_id_hash(&msg->dstid));
            dht_client_update(client, &msg->srcid, &addr, TRUE);

            // Send response
            msg->type = MSG_LOOKUP_RES;
            msg->srcid = priv->id;
            guint count = dht_client_search(client, &msg->dstid, msg->nodes);
            g_socket_send_to(socket, sockaddr, (gchar*)buffer, len + count * sizeof(MsgNode), NULL, NULL);
            break;
        }

        case MSG_LOOKUP_RES:
        {
            guint count = (len - sizeof(MsgLookup)) / sizeof(MsgNode);
            if(len != sizeof(MsgLookup) + sizeof(MsgNode) * count) break;
            MsgLookup *msg = (MsgLookup*)buffer;

            // Ignore own source ID
            if(dht_id_equal(&msg->srcid, &priv->id)) break;

            DhtAddress addr;
            dht_address_serialize(&addr, sockaddr);

            DhtLookup *lookup = NULL;
            if((priv->num_peers == 0) && dht_id_equal(&msg->dstid, &priv->id))
            {
                // Bootstrap
                lookup = g_slice_new(DhtLookup);
                lookup->client = client;
                lookup->id = priv->id;
                lookup->num_sources = 0;
                lookup->query_sequence = g_sequence_new(NULL);
                lookup->query_table = g_hash_table_new_full(dht_address_hash, dht_address_equal, NULL, dht_query_destroy_cb);
                lookup->results = NULL;

                g_hash_table_replace(priv->lookup_table, &lookup->id, lookup);
            }

            g_debug("lookup response %08x -> %08x", dht_id_hash(&msg->srcid), dht_id_hash(&msg->dstid));
            dht_client_update(client, &msg->srcid, &addr, TRUE);

            lookup = lookup ?: g_hash_table_lookup(priv->lookup_table, &msg->dstid);
            if(lookup)
            {
                // Check if this is the target node
                if(dht_id_equal(&msg->srcid, &lookup->id))
                {
                    while(lookup->results)
                    {
                        GSimpleAsyncResult *result = lookup->results->data;
                        lookup->results = g_slist_delete_link(lookup->results, lookup->results);

                        GError *error = NULL;
                        GSocket *socket = g_socket_new(DHT_ADDRESS_FAMILY, G_SOCKET_TYPE_DATAGRAM, G_SOCKET_PROTOCOL_UDP, &error);
                        if(error)
                        {
                            g_simple_async_result_take_error(result, error);
                            g_simple_async_result_complete_in_idle(result);
                            g_object_unref(result);
                            continue;
                        }

                        // Send request
                        MsgConnection *msg = (MsgConnection*)buffer;
                        msg->type = MSG_CONNECTION_REQ;
                        msg->pubkey = priv->pubkey;
                        dht_key_make_random(&msg->nonces[0]);
                        g_socket_send_to(socket, sockaddr, (gchar*)buffer, sizeof(MsgConnection) + DHT_KEY_SIZE, NULL, &error);
                        if(error)
                        {
                            g_object_unref(socket);
                            g_simple_async_result_take_error(result, error);
                            g_simple_async_result_complete_in_idle(result);
                            g_object_unref(result);
                            continue;
                        }

                        // Create connection
                        DhtConnection *connection = g_slice_new(DhtConnection);
                        connection->client = client;
                        connection->nonce = msg->nonces[0];
                        connection->id = lookup->id;
                        connection->sockaddr = NULL;
                        connection->socket = socket;
                        connection->result = result;
                        connection->timeout_source = g_timeout_add(DHT_TIMEOUT_MS, dht_connection_timeout_cb, connection);
                        g_hash_table_replace(priv->connection_table, &connection->nonce, connection);
                    }

                    g_hash_table_remove(priv->lookup_table, &lookup->id);
                }
                else
                {
                    dht_lookup_update1(lookup, &msg->srcid, &addr);
                    dht_lookup_update(lookup, msg->nodes, count);
                    dht_lookup_dispatch(lookup);
                }
            }

            break;
        }

        case MSG_CONNECTION_REQ:
        {
            if(len != sizeof(MsgConnection) + sizeof(DhtKey)) break;
            MsgConnection *msg = (MsgConnection*)buffer;

            DhtId id;
            dht_id_from_pubkey(&id, &msg->pubkey);
            if(dht_id_equal(&id, &priv->id)) break;

            g_debug("connection request %08x", dht_id_hash(&id));
            if(priv->listen)
            {
                DhtNode *node = g_hash_table_lookup(priv->cache, &id);
                if(!node) break;

                DhtKey shared;
                if(!dht_key_make_shared(&shared, &priv->privkey, &msg->pubkey)) break;

                g_autoptr(GSocketAddress) destaddr = dht_address_deserialize(&node->addr);
                g_autoptr(GSocket) socket = g_socket_new(DHT_ADDRESS_FAMILY, G_SOCKET_TYPE_DATAGRAM, G_SOCKET_PROTOCOL_UDP, NULL);
                if(!socket) break;

                // Send response
                msg->type = MSG_CONNECTION_RES;
                msg->pubkey = priv->pubkey;
                msg->nonces[1] = msg->nonces[0];
                dht_key_make_random(&msg->nonces[0]);
                g_socket_send_to(socket, destaddr, (gchar*)buffer, len + sizeof(DhtKey), NULL, NULL);

                DhtKey enc_key, dec_key;
                dht_key_derive(&enc_key, &shared, &msg->nonces[0], &msg->nonces[1]);
                dht_key_derive(&dec_key, &shared, &msg->nonces[1], &msg->nonces[0]);
                g_signal_emit(client, dht_client_signals[SIGNAL_NEW_CONNECTION], 0, &id, socket, destaddr, &enc_key, &dec_key);
            }

            break;
        }

        case MSG_CONNECTION_RES:
        {
            if(len != sizeof(MsgConnection) + 2 * sizeof(DhtKey)) break;
            MsgConnection *msg = (MsgConnection*)buffer;

            DhtId id;
            dht_id_from_pubkey(&id, &msg->pubkey);

            g_debug("connection response %08x", dht_id_hash(&id));
            DhtConnection *connection = g_hash_table_lookup(priv->connection_table, &msg->nonces[1]);
            if(connection && dht_id_equal(&connection->id, &id))
            {
                DhtKey shared;
                if(!dht_key_make_shared(&shared, &priv->privkey, &msg->pubkey)) break;
                dht_key_derive(&connection->enc_key, &shared, &msg->nonces[1], &msg->nonces[0]);
                dht_key_derive(&connection->dec_key, &shared, &msg->nonces[0], &msg->nonces[1]);

                connection->sockaddr = g_object_ref(sockaddr);
                g_hash_table_steal(priv->connection_table, &connection->nonce);
                g_simple_async_result_set_op_res_gpointer(connection->result, connection, dht_connection_destroy_cb);
                g_simple_async_result_complete(connection->result);
                g_clear_object(&connection->result);
            }

            break;
        }

        default:
            g_debug("unknown message code 0x%x", buffer[0]);
    }

    return G_SOURCE_CONTINUE;
}

static gboolean dht_query_timeout_cb(gpointer arg)
{
    DhtQuery *query = arg;
    DhtLookup *lookup = query->lookup;
    g_autoptr(DhtClient) client = g_object_ref(lookup->client); // keep internal reference

    DhtId id;
    dht_id_xor(&id, &query->metric, &lookup->id);

    // Update node
    dht_client_update(client, &id, NULL, FALSE);
    query->is_finished = TRUE;

    // Update source counter
    query->timeout_source = 0;
    lookup->num_sources--;

    // Dispatch lookup
    dht_lookup_dispatch(lookup);
    return G_SOURCE_REMOVE;
}

static gboolean dht_connection_timeout_cb(gpointer arg)
{
    DhtConnection *connection = arg;
    g_autoptr(DhtClient) client = g_object_ref(connection->client); // keep internal reference
    DhtClientPrivate *priv = dht_client_get_instance_private(client);

    g_simple_async_result_set_error(connection->result, G_IO_ERROR, G_IO_ERROR_TIMED_OUT, _("Operation timed out"));
    g_simple_async_result_complete_in_idle(connection->result);
    g_clear_object(&connection->result);

    connection->timeout_source = 0;
    g_hash_table_remove(priv->connection_table, &connection->nonce);
    return G_SOURCE_REMOVE;
}

static void dht_node_destroy_cb(gpointer arg)
{
    g_slice_free(DhtNode, arg);
}

static void dht_bucket_destroy_cb(gpointer arg)
{
    g_list_free_full(arg, dht_node_destroy_cb);
}

static void dht_query_destroy_cb(gpointer arg)
{
    DhtQuery *query = arg;

    if(query->timeout_source > 0)
        g_source_remove(query->timeout_source);

    g_slice_free(DhtQuery, query);
}

static void dht_lookup_destroy_cb(gpointer arg)
{
    DhtLookup *lookup = arg;

    g_sequence_free(lookup->query_sequence);
    g_hash_table_destroy(lookup->query_table);
    g_slist_free_full(lookup->results, dht_result_destroy_cb);
    g_slice_free(DhtLookup, lookup);
}

static void dht_connection_destroy_cb(gpointer arg)
{
    DhtConnection *connection = arg;

    if(connection->timeout_source > 0)
        g_source_remove(connection->timeout_source);

    g_clear_object(&connection->socket);
    g_clear_object(&connection->sockaddr);
    g_clear_pointer(&connection->result, dht_result_destroy_cb);
    g_slice_free(DhtConnection, connection);
}

static void dht_result_destroy_cb(gpointer arg)
{
    GSimpleAsyncResult *result = arg;

    g_simple_async_result_set_error(result, G_IO_ERROR, G_IO_ERROR_HOST_NOT_FOUND, _("Lookup failed"));
    g_simple_async_result_complete_in_idle(result);
    g_object_unref(result);
}

//////////////////////////////////////////////////////////////////////////////////////

static GMainLoop *main_loop = NULL;
static DhtClient *test_client1 = NULL;
static DhtClient *test_client2 = NULL;

static void test_connection_cb(DhtClient *client, DhtId *id, GSocket *socket, GSocketAddress *address, DhtKey *enc_key, DhtKey *dec_key, gpointer arg)
{
    g_message("new connection");
}

static void test_lookup_cb(GObject *obj, GAsyncResult *result, gpointer arg)
{
    GError *error = NULL;
    dht_client_lookup_finish(DHT_CLIENT(obj), result, NULL, NULL, NULL, NULL, &error);
    if(error)
    {
        g_warning("%s", error->message);
        g_clear_error(&error);
    }

    g_message("lookup finished");
    g_clear_object(&test_client2);
    g_main_loop_quit(main_loop);
}

static gboolean test_timeout_cb(gpointer arg)
{
    g_message("lookup start");

    g_autoptr(DhtId) id;
    g_object_get(test_client2, "id", &id, NULL);
    //id->data[0] = 0;

    dht_client_lookup_async(test_client1, id, test_lookup_cb, NULL);

    g_clear_object(&test_client1);
    return G_SOURCE_REMOVE;
}

static void test_run(GError **error)
{
    DhtKey key1, key2;
    dht_key_make_random(&key1);
    dht_key_make_random(&key2);

    g_message("test start");
    test_client1 = dht_client_new(&key1);
    test_client2 = dht_client_new(&key2);
    g_signal_connect(test_client2, "new-connection", (GCallback)test_connection_cb, NULL);
    g_object_set(test_client2, "listen", TRUE, NULL);

    g_autoptr(GInetAddress) inaddr_any = g_inet_address_new_any(DHT_ADDRESS_FAMILY);
    g_autoptr(GSocketAddress) local_address1 = g_inet_socket_address_new(inaddr_any, 5004);
    g_autoptr(GSocketAddress) local_address2 = g_inet_socket_address_new(inaddr_any, 5005);
    if(!dht_client_bind(test_client1, local_address1, FALSE, error)) return;
    if(!dht_client_bind(test_client2, local_address2, FALSE, error)) return;

    g_autoptr(GInetAddress) inaddr_loopback = g_inet_address_new_loopback(DHT_ADDRESS_FAMILY);
    g_autoptr(GSocketAddress) remote_address = g_inet_socket_address_new(inaddr_loopback, 5005);
    if(!dht_client_bootstrap(test_client1, remote_address, error)) return;

    g_timeout_add(100, test_timeout_cb, NULL);
}

int main()
{
    main_loop = g_main_loop_new(NULL, FALSE);

    GError *error = NULL;
    test_run(&error);

    if(error)
    {
        g_warning("%s", error->message);
        g_clear_error(&error);
    }
    else g_main_loop_run(main_loop);

    g_message("test finished");
    g_clear_pointer(&main_loop, g_main_loop_unref);
    return 0;
}
