
#define G_LOG_DOMAIN "TEST"

#include <gst/gst.h>
#include "dht-client.h"
#include "rtp-session.h"

static GMainLoop *main_loop = NULL;
static DhtClient *test_client1 = NULL;
static DhtClient *test_client2 = NULL;
static RtpSession *test_session1 = NULL;
static RtpSession *test_session2 = NULL;

static gboolean test_timeout3_cb(gpointer arg)
{
    rtp_session_stop(test_session1);
    rtp_session_stop(test_session2);
    g_main_loop_quit(main_loop);

    return G_SOURCE_REMOVE;
}

static gboolean test_timeout2_cb(gpointer arg)
{
    g_message("Accept session");
    rtp_session_set_tone(test_session2, FALSE);
    rtp_session_set_volume(test_session2, 1.0);

    return G_SOURCE_REMOVE;
}

static void test_connection_cb(DhtClient *client, DhtId *id, GSocket *socket, DhtKey *enc_key, DhtKey *dec_key, gpointer arg)
{
    g_autoptr(GInetSocketAddress) local_address = G_INET_SOCKET_ADDRESS(g_socket_get_local_address(socket, NULL));
    g_autofree gchar *local_str = g_inet_address_to_string(g_inet_socket_address_get_address(local_address));
    guint16 local_port = g_inet_socket_address_get_port(local_address);

    g_autoptr(GInetSocketAddress) remote_address = G_INET_SOCKET_ADDRESS(g_socket_get_remote_address(socket, NULL));
    g_autofree gchar *remote_str = g_inet_address_to_string(g_inet_socket_address_get_address(remote_address));
    guint16 remote_port = g_inet_socket_address_get_port(remote_address);

    g_message("New connection %s:%u (%08x) -> %s:%u (%08x) %08x",
            local_str, local_port, dht_key_hash(enc_key),
            remote_str, remote_port, dht_key_hash(dec_key),
            dht_id_hash(id));

    test_session2 = rtp_session_new();
    rtp_session_prepare(test_session2, socket, enc_key, dec_key);
    rtp_session_echo_cancel(test_session2);
    rtp_session_set_tone(test_session2, TRUE);
    rtp_session_set_volume(test_session2, 0);
    rtp_session_start(test_session2);

    g_timeout_add(6000, test_timeout2_cb, NULL);
}

static void test_lookup_cb(GObject *obj, GAsyncResult *result, gpointer arg)
{
    DhtKey enc_key, dec_key;
    g_autoptr(GError) error = NULL;
    g_autoptr(GSocket) socket = NULL;
    dht_client_lookup_finish(DHT_CLIENT(obj), result, &socket, &enc_key, &dec_key, &error);
    if(error) g_warning("%s", error->message);

    g_autoptr(GInetSocketAddress) local_address = G_INET_SOCKET_ADDRESS(g_socket_get_local_address(socket, NULL));
    g_autofree gchar *local_str = g_inet_address_to_string(g_inet_socket_address_get_address(local_address));
    guint16 local_port = g_inet_socket_address_get_port(local_address);

    g_autoptr(GInetSocketAddress) remote_address = G_INET_SOCKET_ADDRESS(g_socket_get_remote_address(socket, NULL));
    g_autofree gchar *remote_str = g_inet_address_to_string(g_inet_socket_address_get_address(remote_address));
    guint16 remote_port = g_inet_socket_address_get_port(remote_address);

    g_message("Lookup finished %s:%u (%08x) -> %s:%u (%08x)",
            local_str, local_port, dht_key_hash(&enc_key),
            remote_str, remote_port, dht_key_hash(&dec_key));

    test_session1 = rtp_session_new();
    rtp_session_prepare(test_session1, socket, &enc_key, &dec_key);
    rtp_session_echo_cancel(test_session1);
    rtp_session_start(test_session1);
}

static gboolean test_timeout1_cb(gpointer arg)
{
    g_autoptr(DhtId) id1, id2;
    g_object_get(test_client1, "id", &id1, NULL);
    g_object_get(test_client2, "id", &id2, NULL);

    g_message("Lookup %08x -> %08x", dht_id_hash(id1), dht_id_hash(id2));
    dht_client_lookup_async(test_client1, id2, test_lookup_cb, NULL);

    return G_SOURCE_REMOVE;
}

static void test_run(GError **error)
{
    DhtKey key1, key2;
    dht_key_make_random(&key1);
    dht_key_make_random(&key2);

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
    dht_client_bootstrap(test_client1, remote_address);

    g_timeout_add(100, test_timeout1_cb, NULL);
}

int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);
    main_loop = g_main_loop_new(NULL, FALSE);

    g_autoptr(GError) error = NULL;
    test_run(&error);
    if(error) g_error("%s", error->message);

    g_timeout_add(10000, test_timeout3_cb, NULL);
    g_main_loop_run(main_loop);

    g_message("Test finished");
    g_object_unref(test_session1);
    g_object_unref(test_session2);
    g_object_unref(test_client1);
    g_object_unref(test_client2);
    g_main_loop_unref(main_loop);
    return 0;
}
