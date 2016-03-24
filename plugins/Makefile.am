plugin_LTLIBRARIES = libgstrtpcrypto.la

libgstrtpcrypto_la_SOURCES = gstrtpcrypto.c gstrtpencrypt.c gstrtpdecrypt.c
libgstrtpcrypto_la_CFLAGS = $(GST_CFLAGS) $(SODIUM_CFLAGS)
libgstrtpcrypto_la_LIBADD = $(GST_LIBS) $(SODIUM_LIBS)
libgstrtpcrypto_la_LDFLAGS = $(GST_PLUGIN_LDFLAGS)
libgstrtpcrypto_la_LIBTOOLFLAGS = --tag=disable-static

noinst_HEADERS = gstrtpencrypt.h gstrtpdecrypt.h
