CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -pthread

all: server client

server: server.c
	$(CC) $(CFLAGS) -o server server.c

client: client.c
	$(CC) $(CFLAGS) -o client client.c

clean:
	rm -f server client *.o
	rm -rf server_storage *.tmp

.PHONY: all clean
