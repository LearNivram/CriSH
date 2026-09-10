# CriSH - a shell for macOS that runs Linux scripts unchanged.
# SPDX-License-Identifier: GPL-3.0-or-later

CC      ?= cc
PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin

WARN    := -Wall -Wextra -Wno-unused-parameter -Wshadow -Wpointer-arith \
           -Wstrict-prototypes -Wmissing-prototypes -Wwrite-strings
CFLAGS  ?= -O2
CFLAGS  += -std=c11 -D_DARWIN_C_SOURCE $(WARN)
LDFLAGS ?=

SRC := $(wildcard src/*.c) $(wildcard src/gnu/*.c)
OBJ := $(SRC:.c=.o)
BIN := build/crish

.PHONY: all clean install uninstall test universal fmt

all: $(BIN)

$(BIN): $(OBJ)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)
	@echo "  built $@ ($$(du -h $@ | cut -f1))"

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# One clang invocation emits both slices; no lipo needed.
universal:
	@mkdir -p build
	$(CC) $(CFLAGS) -arch arm64 -arch x86_64 -o build/crish $(SRC) $(LDFLAGS)
	@file build/crish

test: $(BIN)
	@./$(BIN) tests/run.crsh

clean:
	rm -f $(OBJ)
	rm -rf build

install: $(BIN)
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(BIN) $(DESTDIR)$(BINDIR)/crish

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/crish

fmt:
	@command -v clang-format >/dev/null && clang-format -i $(SRC) src/*.h src/gnu/*.h \
		|| echo "clang-format not installed, skipping"

$(OBJ): src/shell.h src/util.h
