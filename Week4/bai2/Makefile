CC = gcc
CFLAGS = -Wall -Wextra -std=c99

all: server client

server: server.c
	$(CC) $(CFLAGS) -o server server.c

client: client.c
	$(CC) $(CFLAGS) -o client client.c

clean:
	rm -f server client *.o
	rm -rf storage/.tmp_*

.PHONY: all clean
