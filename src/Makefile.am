AM_CFLAGS = $(SODIUM_CFLAGS) $(GLIB_CFLAGS)
LDADD = $(SODIUM_LIBS) $(GLIB_LIBS) $(LIBINTL)
DEFS += -DLOCALEDIR=\"$(localedir)\"

bin_PROGRAMS = nanotalk
nanotalk_SOURCES = main.c dht-common.c dht-client.c
noinst_HEADERS = dht-common.h dht-client.h glib-compat.h

if ENABLE_GUI
AM_CFLAGS += $(GTK_CFLAGS) $(GST_CFLAGS) $(CANBERRA_CFLAGS)
LDADD += $(GTK_LIBS) $(GST_LIBS) $(CANBERRA_LIBS)
nanotalk_SOURCES += application.c rtp-session.c rtp-src.c rtp-sink.c
noinst_HEADERS += application.h rtp-session.h rtp-src.h rtp-sink.h
endif
