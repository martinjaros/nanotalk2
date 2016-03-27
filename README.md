# Nanotalk distributed multimedia service
This is a simple, distributed voice client for Linux desktop.
It can be used for simple calls between nodes;
the peer node is specified by its ID (or an alias you assigned to this ID).
The client will search the network by querying nodes it already knows and once the target node is found,
an encrypted audio stream is created.

I made this project out of pure interest in the topic mostly for demonstration purposes.
If you actually want to use it practically there is a need for better GUI and more features,
but feel free to use this project or its parts for your own application.

## Build and install
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

## Running
First, you need a private key, which is a 32-byte random sequence

    dd if=/dev/random of=/path/to/private.key bs=32 count=1

An unique ID is assigned to each key. To find out what ID does your key have, run the client with a debugging option and check the console output

    G_MESSAGES_DEBUG=Nanotalk nanotalk -k /path/to/private.key

You may create a text file with list of aliases for IDs you know. For example

    Eq2dQfYkQU17HY7A5N4k0TFJBuU= Name of this node
    L+XtSrI1bBehyOppis7FCvvnw9U= Another name

In order to use the client it must be bootstrapped to a live node specified either by hostname or address

    nanotalk -k /path/to/private.key -a /path/to/aliases.txt -h 10.20.30.40 -p 5004

The client can play a sound file to indicate an incoming call; use the `--call-sound` option.  
For more options see `nanotalk --help`

## License
Use with GNU General Public License, version 2  
(or ask me for relicensing)
