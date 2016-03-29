CFLAGS = -Wall -Werror -g -O2

.PHONY: all clean
all: server
clean:
	rm -f server server.h server.key

server: server.c server.h ../src/dhtclient.c ../src/dhtclient.h
	gcc $(CFLAGS) -I../src -o $@ server.c ../src/dhtclient.c $(shell pkg-config --cflags --libs libsodium gio-2.0)

server.h:
	dd if=/dev/random of=server.key bs=1 count=32
	xxd -i server.key server.h
