Nanotalk distributed voice client
=================================

This is a simple, distributed voice client.
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
 * GStreamer Good Plugins (for RTP)
 * GStreamer Bad Plugins (for Opus in pre-1.8 versions)

If you want to build a server without GTK+ and GStreamer use `./configure --disable-gui`.

Running
---------------------------------

Upon first startup a new key is generated and saved together with default configuration to

    $HOME/.nanotalk/user.cfg
    $HOME/.nanotalk/user.key
    $HOME/.nanotalk/aliases.txt

An unique ID is assigned to each key.
You may create a list of aliases for IDs you know. For example

    Eq2dQfYkQU17HY7A5N4k0TFJBuU= Name of this node
    L+XtSrI1bBehyOppis7FCvvnw9U= Another name

In order to use the client it must be bootstrapped to a live node specified either by hostname or address.
To make the client work behind NAT you need to either enable "full-cone" if the router supports it
or setup UDP port forwarding.
