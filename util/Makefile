CFLAGS = -Wall -Werror -g -O2

.PHONY: all clean
all: server
clean:
	rm -f server server.h server.key

server: server.h ../src/dhtclient.h
server: server.c ../src/dhtclient.c
	gcc $(CFLAGS) -I../src -o $@ $^ $(shell pkg-config --cflags --libs libsodium gio-2.0)

server.h:
	dd if=/dev/random of=server.key bs=32 count=1
	xxd -i server.key server.h
