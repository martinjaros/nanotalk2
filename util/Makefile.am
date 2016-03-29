CFLAGS += $(SODIUM_CFLAGS) $(GLIB_CFLAGS)
LDADD = $(SODIUM_LIBS) $(GLIB_LIBS)

bin_PROGRAMS = nanotalk-server
nanotalk_server_SOURCES = server.c dhtclient.c
noinst_HEADERS = dhtclient.h glib-compat.h
