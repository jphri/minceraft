SOURCES=$(wildcard *.c)
OBJECTS=$(SOURCES:.c=.o)
CC=c99
CFLAGS=-Wall -Wextra -pedantic -O3 -march=native -mtune=native -g
LDFLAGS=-lglfw -lGL -lGLEW -lm

.PHONY: all clean

all: minceraft

clean:
	rm -f $(OBJECTS)
	rm -f minceraft

minceraft: $(OBJECTS)
	$(CC) $^ $(LDFLAGS) -o $@

%.o: %.c
	$(CC) -c $< $(CFLAGS) -o $@

