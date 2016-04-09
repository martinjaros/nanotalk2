AM_CFLAGS = $(SODIUM_CFLAGS) $(GLIB_CFLAGS)
LDADD = $(SODIUM_LIBS) $(GLIB_LIBS) $(LIBINTL)
DEFS += -DLOCALEDIR=\"$(localedir)\"

bin_PROGRAMS = nanotalk
nanotalk_SOURCES = main.c dhtclient.c
noinst_HEADERS = application.h dhtclient.h glib-compat.h gstrtpencrypt.h gstrtpdecrypt.h

if ENABLE_GUI
AM_CFLAGS += $(GTK_CFLAGS) $(GST_CFLAGS)
LDADD += $(GTK_LIBS) $(GST_LIBS)
nanotalk_SOURCES += application.c gstrtpencrypt.c gstrtpdecrypt.c
endif
