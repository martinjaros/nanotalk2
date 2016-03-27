bin_PROGRAMS = nanotalk
nanotalk_SOURCES = application.c dhtclient.c gstrtpencrypt.c gstrtpdecrypt.c
nanotalk_CFLAGS = $(SODIUM_CFLAGS) $(GTK_CFLAGS) $(GST_CFLAGS)
nanotalk_LDADD = $(SODIUM_LIBS) $(GTK_LIBS) $(GST_LIBS)
noinst_HEADERS = dhtclient.h glib-compat.h gstrtpencrypt.h gstrtpdecrypt.h
