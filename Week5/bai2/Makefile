CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -I.

all: server client

server: server.c
	$(CC) $(CFLAGS) -o server server.c

client: client.c
	$(CC) $(CFLAGS) -o client client.c

clean:
	rm -f server client *.o
	rm -rf storage/* downloaded_*

.PHONY: all clean
