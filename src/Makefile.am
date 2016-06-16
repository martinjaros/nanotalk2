AM_CFLAGS = $(SODIUM_CFLAGS) $(GLIB_CFLAGS)
LDADD = $(SODIUM_LIBS) $(GLIB_LIBS) $(LIBINTL)
DEFS += -DLOCALEDIR=\"$(localedir)\"

bin_PROGRAMS = nanotalk test
nanotalk_SOURCES = main.c dhtclient.c
noinst_HEADERS = application.h dhtclient.h glib-compat.h tonegen.h rtpencrypt.h rtpdecrypt.h

if ENABLE_GUI
AM_CFLAGS += $(GTK_CFLAGS) $(GST_CFLAGS)
LDADD += $(GTK_LIBS) $(GST_LIBS)
nanotalk_SOURCES += application.c tonegen.c rtpencrypt.c rtpdecrypt.c
endif

test_SOURCES = dht-common.c dht-client.c