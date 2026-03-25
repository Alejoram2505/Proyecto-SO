CC = gcc
CFLAGS = -Wall -Wextra -Werror -std=c11 -pedantic
LDFLAGS = -pthread

COMMON_OBJS = common.o

all: servidor cliente

servidor: servidor.o $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

cliente: cliente.o $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

servidor.o: servidor.c common.h protocolo.h
cliente.o: cliente.c common.h protocolo.h
common.o: common.c common.h protocolo.h

clean:
	rm -f *.o servidor cliente

.PHONY: all clean
