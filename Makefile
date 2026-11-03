CC=gcc
CFLAGS=-Wall -Wextra -std=c99 -pedantic -Iinclude

.PHONY: all
all: zeelib

zeelib: src/main.c
	$(CC) -o $@ $(CFLAGS) $^

clean:
	rm -rf zeelib