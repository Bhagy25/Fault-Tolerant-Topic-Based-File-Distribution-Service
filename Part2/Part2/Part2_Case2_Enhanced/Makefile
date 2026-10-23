CC = gcc
CFLAGS = -std=c11 -O2 -Wall -Wextra -Werror -pedantic -pthread -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -I../common
LDFLAGS = -pthread
COMMON = ../common/utils.c ../common/topics.c

.PHONY: all clean
all: server client

server: server.c $(COMMON) ../common/utils.h ../common/topics.h ../common/protocol.h
	$(CC) $(CFLAGS) -o $@ server.c $(COMMON) $(LDFLAGS)

client: client.c checkpoint.c checkpoint.h ../common/utils.c ../common/utils.h ../common/protocol.h
	$(CC) $(CFLAGS) -o $@ client.c checkpoint.c ../common/utils.c $(LDFLAGS)

clean:
	rm -f server client
