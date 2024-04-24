CFLAGS=-O3 -march=native -mtune=native
CC=c99

all: stb.o

clean:
	rm -f stb.o

stb.o: stb.c
	$(CC) $< $(CFLAGS) -c -o $@
