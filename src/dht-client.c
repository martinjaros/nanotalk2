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
typedef struct _msg_connection1 MsgConnection1;
typedef struct _msg_connection2 MsgConnection2;
typedef struct _msg_connection3 MsgConnection3;

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
    guint8 type; // MSG_LOOKUP_REQ, MSG_LOOKUP_RES
    DhtId srcid;
    DhtId dstid;
    MsgNode nodes[0]; // up to DHT_NODE_COUNT
};

struct _msg_connection1
{
    guint8 type; // MSG_CONNECTION_REQ
    DhtKey pubkey;
    DhtKey nonce;
};

struct _msg_connection2
{
    guint8 type; // MSG_CONNECTION_RES
    DhtKey pubkey;
    DhtKey nonce;
    DhtKey peer_nonce;
    DhtKey auth_tag;
};

struct _msg_connection3
{
    guint8 type; // MSG_CONNECTION_RES
    DhtKey peer_nonce;
    DhtKey auth_tag;
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

    GSequence *query_sequence; // <DhtQuery>
    GHashTable *query_table; // <DhtAddress, GSequenceIter>
    guint num_sources;

    GSList *results; // <GSimpleAsyncResult>
    DhtClient *client; // weak
};

struct _dht_connection
{
    DhtId id;
    DhtKey nonce;

    gboolean is_remote;
    guint timeout_source;

    GSocket *socket; // nullable
    GSocketAddress *sockaddr; // nullable
    GSimpleAsyncResult *result; // nullable
    DhtKey enc_key, dec_key, auth_tag;

    DhtClient *client; // weak
};

struct _dht_client_private
{
    DhtId id;
    DhtKey pubkey, privkey;

    GList *buckets; // <GList<DhtNode>>
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
            G_TYPE_NONE, 4, DHT_TYPE_ID, G_TYPE_SOCKET, DHT_TYPE_KEY, DHT_TYPE_KEY);
}

static void dht_client_init(DhtClient *client)
{
    DhtClientPrivate *priv = dht_client_get_instance_private(client);

    priv->buckets = g_list_alloc();
    priv->num_buckets = 1;

    priv->lookup_table = g_hash_table_new_full(dht_id_hash, dht_id_equal, NULL, dht_lookup_destroy_cb);
    priv->connection_table = g_hash_table_new_full(dht_key_hash, dht_key_equal, NULL, dht_connection_destroy_cb);

    // Create socket
    g_autoptr(GError) error = NULL;
    priv->socket = g_socket_new(DHT_ADDRESS_FAMILY, G_SOCKET_TYPE_DATAGRAM, G_SOCKET_PROTOCOL_UDP, &error);
    if(error)
    {
        g_warning("%s", error->message);
        return;
    }

    // Attach sources
    g_autoptr(GSource) source = g_socket_create_source(priv->socket, G_IO_IN, NULL);
    g_source_set_callback(source, (GSourceFunc)dht_client_receive_cb, client, NULL);
    priv->socket_source = g_source_attach(source, g_main_context_default());
    priv->timeout_source = g_timeout_add(DHT_REFRESH_MS, dht_client_refresh_cb, client);
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
            g_return_if_fail(key != NULL);

            priv->privkey = *key;
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

DhtClient* dht_client_new(DhtKey *key)
{
    g_return_val_if_fail(key != NULL, NULL);

    return g_object_new(DHT_TYPE_CLIENT, "key", key, NULL);
}

gboolean dht_client_bind(DhtClient *client, GSocketAddress *address, gboolean allow_reuse, GError **error)
{
    g_return_val_if_fail(DHT_IS_CLIENT(client), FALSE);
    DhtClientPrivate *priv = dht_client_get_instance_private(client);

    return g_socket_bind(priv->socket, address, allow_reuse, error);
}

void dht_client_bootstrap(DhtClient *client, GSocketAddress *address)
{
    g_return_if_fail(DHT_IS_CLIENT(client));
    g_return_if_fail(G_IS_INET_SOCKET_ADDRESS(address));
    DhtClientPrivate *priv = dht_client_get_instance_private(client);

    DhtLookup *lookup = g_hash_table_lookup(priv->lookup_table, &priv->id);
    if(!lookup)
    {
        lookup = g_slice_new(DhtLookup);
        lookup->client = client;
        lookup->id = priv->id;
        lookup->num_sources = 0;
        lookup->query_sequence = g_sequence_new(dht_query_destroy_cb);
        lookup->query_table = g_hash_table_new(dht_address_hash, dht_address_equal);
        lookup->results = NULL;
        g_hash_table_insert(priv->lookup_table, &lookup->id, lookup);
    }

    MsgNode node;
    memset(node.id.data, 0, DHT_ID_SIZE);
    dht_address_serialize(&node.addr, address);
    dht_lookup_update(lookup, &node, 1);
}

void dht_client_lookup_async(DhtClient *client, const DhtId *id, GAsyncReadyCallback callback, gpointer user_data)
{
    g_return_if_fail(DHT_IS_CLIENT(client));
    g_return_if_fail(id != NULL);
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
    lookup->query_sequence = g_sequence_new(dht_query_destroy_cb);
    lookup->query_table = g_hash_table_new(dht_address_hash, dht_address_equal);
    lookup->results = g_slist_prepend(NULL, result);
    g_hash_table_insert(priv->lookup_table, &lookup->id, lookup);

    // Dispatch lookup
    MsgNode nodes[DHT_NODE_COUNT];
    guint count = dht_client_search(client, id, nodes);
    dht_lookup_update(lookup, nodes, count);
}

gboolean dht_client_lookup_finish(DhtClient *client, GAsyncResult *result, GSocket **socket, DhtKey *enc_key, DhtKey *dec_key, GError **error)
{
    g_return_val_if_fail(DHT_IS_CLIENT(client), FALSE);
    if(g_simple_async_result_propagate_error(G_SIMPLE_ASYNC_RESULT(result), error))
        return FALSE;

    DhtConnection *connection = g_simple_async_result_get_op_res_gpointer(G_SIMPLE_ASYNC_RESULT(result));
    if(socket) *socket = g_object_ref(connection->socket);
    if(enc_key) *enc_key = connection->enc_key;
    if(dec_key) *dec_key = connection->dec_key;

    return TRUE;
}

static void dht_client_finalize(GObject *obj)
{
    DhtClient *client = DHT_CLIENT(obj);
    DhtClientPrivate *priv = dht_client_get_instance_private(client);

    g_hash_table_destroy(priv->lookup_table);
    g_hash_table_destroy(priv->connection_table);
    g_list_free_full(priv->buckets, dht_bucket_destroy_cb);

    if(priv->socket)
    {
        g_object_unref(priv->socket);
        g_source_remove(priv->socket_source);
        g_source_remove(priv->timeout_source);
    }

    G_OBJECT_CLASS(dht_client_parent_class)->finalize(obj);
}

static void dht_client_update(DhtClient *client, const DhtId *id, const DhtAddress *addr, gboolean is_alive)
{
    DhtClientPrivate *priv = dht_client_get_instance_private(client);
    g_debug("Update node %08x %s (%s)", dht_id_hash(id), dht_address_print(addr), is_alive ? "alive" : "timed-out");

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
    count++;

    priv->num_peers++;
    g_object_notify_by_pspec(G_OBJECT(client), dht_client_properties[PROP_PEERS]);

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
                    g_debug("Delete node %08x %s", dht_id_hash(&node->id), dht_address_print(&node->addr));

                    // Delete dead node
                    g_slice_free(DhtNode, node);
                    bucket->data = g_list_delete_link(bucket->data, link);

                    priv->num_peers--;
                    g_object_notify_by_pspec(G_OBJECT(client), dht_client_properties[PROP_PEERS]);
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

        // Check if this is the target node
        if(dht_id_equal(&node->id, &lookup->id))
        {
            while(lookup->results)
            {
                GSimpleAsyncResult *result = lookup->results->data;
                lookup->results = g_slist_delete_link(lookup->results, lookup->results);

                MsgConnection1 request;
                request.type = MSG_CONNECTION_REQ;
                request.pubkey = priv->pubkey;
                dht_key_make_random(&request.nonce);

                // Create connection
                DhtConnection *connection = g_slice_new(DhtConnection);
                connection->client = client;
                connection->id = lookup->id;
                connection->nonce = request.nonce;
                connection->is_remote = FALSE;
                connection->socket = NULL;
                connection->sockaddr = dht_address_deserialize(&node->addr);
                connection->result = result;
                connection->timeout_source = g_timeout_add(DHT_TIMEOUT_MS, dht_connection_timeout_cb, connection);
                g_hash_table_replace(priv->connection_table, &connection->nonce, connection);

                // Send request
                g_autoptr(GError) error = NULL;
                g_socket_send_to(priv->socket, connection->sockaddr, (gchar*)&request, sizeof(MsgConnection1), NULL, &error);
                if(error) g_debug("%s", error->message);
            }

            g_hash_table_remove(priv->lookup_table, &lookup->id);
            return;
        }

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

            GSequenceIter *iter = g_sequence_search(lookup->query_sequence, query, dht_id_compare, NULL);
            g_hash_table_insert(lookup->query_table, &query->addr, g_sequence_insert_before(iter, query));
        }
    }

    dht_lookup_dispatch(lookup);
}

static void dht_lookup_dispatch(DhtLookup *lookup)
{
    DhtClient *client = lookup->client;
    DhtClientPrivate *priv = dht_client_get_instance_private(client);
    g_debug("Dispatch lookup %08x", dht_id_hash(&lookup->id));

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

            g_autoptr(GError) error = NULL;
            g_autoptr(GSocketAddress) sockaddr = dht_address_deserialize(&query->addr);
            g_socket_send_to(priv->socket, sockaddr, (gchar*)&msg, sizeof(msg), NULL, &error);
            if(error) g_debug("%s", error->message);

            query->timeout_source = g_timeout_add(DHT_TIMEOUT_MS, dht_query_timeout_cb, query);
            lookup->num_sources++;
        }

        if(query->is_alive) num_alive++;
        iter = g_sequence_iter_next(iter);
    }

    if(lookup->num_sources == 0)
    {
        while(lookup->results)
        {
            // Lookup failed, report error
            GSimpleAsyncResult *result = lookup->results->data;
            g_simple_async_result_set_error(result, G_IO_ERROR, G_IO_ERROR_HOST_NOT_FOUND, _("Lookup failed"));
            g_simple_async_result_complete_in_idle(result);
            g_object_unref(result);

            lookup->results = g_slist_delete_link(lookup->results, lookup->results);
        }

        g_hash_table_remove(priv->lookup_table, &lookup->id);
    }
}

static gboolean dht_client_refresh_cb(gpointer arg)
{
    DhtClient *client = arg;
    DhtClientPrivate *priv = dht_client_get_instance_private(client);

    // Create lookup
    DhtLookup *lookup = g_slice_new(DhtLookup);
    lookup->client = client;
    lookup->num_sources = 0;
    lookup->query_sequence = g_sequence_new(dht_query_destroy_cb);
    lookup->query_table = g_hash_table_new(dht_address_hash, dht_address_equal);
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

    return G_SOURCE_CONTINUE;
}

static gboolean dht_client_receive_cb(GSocket *socket, GIOCondition condition, gpointer arg)
{
    DhtClient *client = arg;
    DhtClientPrivate *priv = dht_client_get_instance_private(client);

    guint8 buffer[MSG_MTU];

    g_autoptr(GError) error = NULL;
    g_autoptr(GSocketAddress) sockaddr = NULL;
    gssize len = g_socket_receive_from(socket, &sockaddr, (gchar*)buffer, sizeof(buffer), NULL, &error);
    if(error) g_debug("%s", error->message);

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

            g_debug("Lookup request %08x -> %08x", dht_id_hash(&msg->srcid), dht_id_hash(&msg->dstid));
            dht_client_update(client, &msg->srcid, &addr, TRUE);

            // Send response
            msg->type = MSG_LOOKUP_RES;
            msg->srcid = priv->id;
            guint count = dht_client_search(client, &msg->dstid, msg->nodes);
            g_socket_send_to(socket, sockaddr, (gchar*)buffer, len + count * sizeof(MsgNode), NULL, &error);
            if(error) g_debug("%s", error->message);
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

            g_debug("Lookup response %08x -> %08x", dht_id_hash(&msg->srcid), dht_id_hash(&msg->dstid));
            dht_client_update(client, &msg->srcid, &addr, TRUE);

            // Find lookup
            DhtLookup *lookup = g_hash_table_lookup(priv->lookup_table, &msg->dstid);
            if(!lookup) break;

            // Find query
            GSequenceIter *iter = g_hash_table_lookup(lookup->query_table, &addr);
            if(!iter) break;

            // Update query
            DhtQuery *query = g_sequence_get(iter);
            query->is_finished = TRUE;
            query->is_alive = TRUE;
            if(query->timeout_source > 0)
            {
                g_source_remove(query->timeout_source);
                query->timeout_source = 0;
                lookup->num_sources--;
            }

            DhtId metric;
            dht_id_xor(&metric, &msg->srcid, &lookup->id);
            if(!dht_id_equal(&metric, &query->metric))
            {
                query->metric = metric;
                g_sequence_sort_changed(iter, dht_id_compare, NULL);
            }

            // Update lookup
            dht_lookup_update(lookup, msg->nodes, count);
            break;
        }

        case MSG_CONNECTION_REQ:
        {
            if(len != sizeof(MsgConnection1)) break;
            MsgConnection1 *msg = (MsgConnection1*)buffer;

            DhtId id;
            dht_id_from_pubkey(&id, &msg->pubkey);
            if(dht_id_equal(&id, &priv->id)) break;

            g_debug("Connection request %08x", dht_id_hash(&id));
            if(priv->listen)
            {
                DhtKey shared;
                if(!dht_key_make_shared(&shared, &priv->privkey, &msg->pubkey)) break;

                GSocket *socket = g_socket_new(DHT_ADDRESS_FAMILY, G_SOCKET_TYPE_DATAGRAM, G_SOCKET_PROTOCOL_UDP, &error);
                if(error)
                {
                    g_debug("%s", error->message);
                    break;
                }

                MsgConnection2 response;
                response.type = MSG_CONNECTION_RES;
                response.pubkey = priv->pubkey;
                response.peer_nonce = msg->nonce;
                dht_key_make_random(&response.nonce);

                // Create connection
                DhtConnection *connection = g_slice_new(DhtConnection);
                dht_key_derive(&connection->enc_key, &response.auth_tag, &shared, &response.nonce, &msg->nonce);
                dht_key_derive(&connection->dec_key, &connection->auth_tag, &shared, &msg->nonce, &response.nonce);
                connection->nonce = response.nonce;
                connection->client = client;
                connection->id = id;
                connection->is_remote = TRUE;
                connection->result = NULL;
                connection->sockaddr = NULL;
                connection->socket = socket;
                connection->timeout_source = g_timeout_add(DHT_TIMEOUT_MS, dht_connection_timeout_cb, connection);
                g_hash_table_replace(priv->connection_table, &connection->nonce, connection);

                // Send response
                g_socket_send_to(socket, sockaddr, (gchar*)&response, sizeof(MsgConnection2), NULL, &error);
                if(error) g_debug("%s", error->message);
            }

            break;
        }

        case MSG_CONNECTION_RES:
        {
            if(len == sizeof(MsgConnection2))
            {
                MsgConnection2 *msg = (MsgConnection2*)buffer;

                DhtId id;
                dht_id_from_pubkey(&id, &msg->pubkey);
                g_debug("Connection response 1 %08x", dht_id_hash(&id));

                // Find connection
                DhtConnection *connection = g_hash_table_lookup(priv->connection_table, &msg->peer_nonce);
                if(!connection || connection->is_remote || !dht_id_equal(&connection->id, &id)) break;

                DhtKey shared;
                if(!dht_key_make_shared(&shared, &priv->privkey, &msg->pubkey)) break;

                MsgConnection3 response;
                response.type = MSG_CONNECTION_RES;
                response.peer_nonce = msg->nonce;

                // Derive keys and verify authentication tag
                dht_key_derive(&connection->enc_key, &response.auth_tag, &shared, &connection->nonce, &msg->nonce);
                dht_key_derive(&connection->dec_key, &connection->auth_tag, &shared, &msg->nonce, &connection->nonce);
                if(!dht_key_equal(&connection->auth_tag, &msg->auth_tag)) break;

                connection->socket = g_socket_new(DHT_ADDRESS_FAMILY, G_SOCKET_TYPE_DATAGRAM, G_SOCKET_PROTOCOL_UDP, &error);
                if(error)
                {
                    g_debug("%s", error->message);
                    break;
                }

                // Send response
                g_socket_send_to(connection->socket, connection->sockaddr, (gchar*)&response, sizeof(MsgConnection3), NULL, &error);
                if(error) g_debug("%s", error->message);

                g_source_remove(connection->timeout_source);
                connection->timeout_source = 0;

                // Complete result
                g_socket_connect(connection->socket, sockaddr, NULL, NULL);
                g_hash_table_steal(priv->connection_table, &connection->nonce);
                g_simple_async_result_set_op_res_gpointer(connection->result, connection, dht_connection_destroy_cb);
                g_simple_async_result_complete(connection->result);
                g_clear_object(&connection->result);
            }
            else if(len == sizeof(MsgConnection3))
            {
                MsgConnection3 *msg = (MsgConnection3*)buffer;

                // Find connection
                DhtConnection *connection = g_hash_table_lookup(priv->connection_table, &msg->peer_nonce);
                if(!connection || !connection->is_remote || !dht_key_equal(&connection->auth_tag, &msg->auth_tag)) break;
                g_debug("Connection response 2 %08x", dht_id_hash(&connection->id));

                g_hash_table_steal(priv->connection_table, &connection->nonce);
                if(priv->listen)
                {
                    // Signal result
                    g_socket_connect(connection->socket, sockaddr, NULL, NULL);
                    g_signal_emit(client, dht_client_signals[SIGNAL_NEW_CONNECTION], 0,
                            &connection->id, connection->socket, &connection->enc_key, &connection->dec_key);
                }

                dht_connection_destroy_cb(connection);
            }

            break;
        }

        default:
            g_debug("Unknown message code 0x%x", buffer[0]);
    }

    return G_SOURCE_CONTINUE;
}

static gboolean dht_query_timeout_cb(gpointer arg)
{
    DhtQuery *query = arg;
    DhtLookup *lookup = query->lookup;
    DhtClient *client = lookup->client;

    DhtId id;
    dht_id_xor(&id, &query->metric, &lookup->id);

    // Update node
    dht_client_update(client, &id, &query->addr, FALSE);
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
    DhtClient *client = connection->client;
    DhtClientPrivate *priv = dht_client_get_instance_private(client);

    if(connection->result)
    {
        g_simple_async_result_set_error(connection->result, G_IO_ERROR, G_IO_ERROR_TIMED_OUT, _("Operation timed out"));
        g_simple_async_result_complete_in_idle(connection->result);
        g_clear_object(&connection->result);
    }

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

    g_simple_async_result_set_error(result, G_IO_ERROR, G_IO_ERROR_CANCELLED, _("Operation cancelled"));
    g_simple_async_result_complete_in_idle(result);
    g_object_unref(result);
}
