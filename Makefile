SOURCES=$(wildcard *.c)
OBJECTS=$(SOURCES:.c=.o)
DEPS=$(SOURCES:.c=.d)

CC=c99
CFLAGS=-Wall -Wextra -pedantic -O3 -march=native -mtune=native -g
CFLAGS += -Ilib/stb/include
CFLAGS += -Ilib/linmath/include
CFLAGS += -MP -MD
LDFLAGS=-lglfw -lGL -lGLEW -lm

.PHONY: all clean FORCE

all: minceraft

clean:
	rm -f $(OBJECTS)
	rm -f $(DEPS)
	rm -f minceraft

nuke:
	$(MAKE) -C . clean
	$(MAKE) -C lib/stb clean

minceraft: $(OBJECTS) lib/stb/stb.o
	$(CC) $^ $(LDFLAGS) -o $@

lib/stb/stb.o: FORCE
	$(MAKE) -C lib/stb stb.o

%.o: %.c
	$(CC) -c $< $(CFLAGS) -o $@

FORCE: ;

-include $(DEPS)
