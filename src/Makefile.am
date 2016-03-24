bin_PROGRAMS = nanotalk

nanotalk_SOURCES = main.c dhtclient.c
nanotalk_CFLAGS = $(SODIUM_CFLAGS) $(GTK_CFLAGS) $(GST_CFLAGS)
nanotalk_LDFLAGS = $(SODIUM_LIBS) $(GTK_LIBS) $(GST_LIBS)

noinst_HEADERS = dhtclient.h glib-compat.h
DEFS += -DPACKAGE_DATA_DIR=\"$(pkgdatadir)\" -DPACKAGE_PLUGIN_DIR=\"$(plugindir)\"
