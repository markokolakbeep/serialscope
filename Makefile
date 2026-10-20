CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -Werror -Wshadow -Wstrict-prototypes
PREFIX  ?= /usr/local

SRCS := src/main.c src/serial.c src/output.c
BIN  := build/serialscope

.PHONY: all test install uninstall clean

all: $(BIN)

$(BIN): $(SRCS) src/serial.h src/output.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SRCS) -o $@

# Tests drive the binary through a pseudo-terminal, so no hardware is needed
test: $(BIN)
	python3 tests/test_serialscope.py

install: $(BIN)
	install -Dm755 $(BIN) $(DESTDIR)$(PREFIX)/bin/serialscope

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/serialscope

clean:
	rm -rf build
