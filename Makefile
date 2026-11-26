CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2 -Iinclude -D_POSIX_C_SOURCE=200809L
LDFLAGS ?=

LIB = libmonitor.a
LIB_OBJ = lib/bd_episodios.o lib/monitor_util.o
SERVER_OBJ = server.o
CLIENT_OBJ = client.o

.PHONY: all clean test

all: $(LIB) server client

$(LIB): $(LIB_OBJ)
	ar rcs $@ $^

lib/%.o: lib/%.c include/bd_episodios.h include/monitor_util.h
	$(CC) $(CFLAGS) -c $< -o $@

server.o: server.c include/bd_episodios.h include/monitor_util.h
	$(CC) $(CFLAGS) -c $< -o $@

client.o: client.c include/bd_episodios.h include/monitor_util.h
	$(CC) $(CFLAGS) -c $< -o $@

server: $(SERVER_OBJ) $(LIB)
	$(CC) $(SERVER_OBJ) $(LIB) -o $@ $(LDFLAGS)

client: $(CLIENT_OBJ) $(LIB)
	$(CC) $(CLIENT_OBJ) $(LIB) -o $@ $(LDFLAGS)

test: all
	./server --test-run

clean:
	rm -f lib/*.o *.o $(LIB) server client
