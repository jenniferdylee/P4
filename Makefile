CC = gcc
CFLAGS = -Wall -Wextra -pthread -O2

all: chatd client tesh_client

chatd: chatd.c protocol.h
	$(CC) $(CFLAGS) -o chatd chatd.c

client: client.c protocol.h
	$(CC) $(CFLAGS) -o client client.c

tesh_client: tesh_client.c protocol.h
	$(CC) $(CFLAGS) -o tesh_client tesh_client.c

clean:
	rm -f chatd client tesh_client