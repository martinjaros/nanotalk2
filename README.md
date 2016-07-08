Nanotalk distributed voice client
=================================

This is a simple, distributed multimedia client.
It can be used for simple calls between nodes;
the peer node is specified by its ID (or an alias you assigned to this ID).
The client will search the network by querying nodes it already knows and once the target node is found,
an encrypted audio stream is created.

Build and install
---------------------------------

If building from git, you must first initialize the build system with `autoreconf -i`.

    ./configure
    make
    make install

Dependencies:

 * libsodium
 * GTK+ 3
 * GStreamer 1.0
 * GStreamer Base Plugins
 * GStreamer Good Plugins
 * GStreamer Bad Plugins (only for pre-1.8 versions)
 * libcanberra (optional)

If you want to build a server without GTK+ and GStreamer use `./configure --disable-gui`.

Running
---------------------------------

Upon first startup a new key is generated and saved together with default configuration to

    $HOME/.nanotalk/user.cfg
    $HOME/.nanotalk/user.key
    $HOME/.nanotalk/aliases.txt

An unique ID is assigned to each key.
You may create a list of aliases for IDs you know. For example

    Eq2dQfYkQU17HY7A5N4k0TFJBuU= John Doe

In order to use the client it must be bootstrapped to a live node specified either by hostname or address.
To make the client work behind NAT or VPN you need to setup UDP port forwarding (5004 by default).

To get more debugging output try setting `G_MESSAGES_DEBUG=RTP` or `GST_DEBUG=*:WARNING` in your environment.

PulseAudio supports acoustic echo cancellation with `PULSE_PROP="filter.want=echo-cancel"`.

