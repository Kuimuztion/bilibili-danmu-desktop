# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Kuimuztion

CC ?= gcc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O2
CPPFLAGS ?=
LDFLAGS ?=
LDLIBS ?=

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

ifeq ($(shell $(PKG_CONFIG) --exists gtk4-layer-shell-0 && echo yes),yes)
DEPS := gtk4-layer-shell-0 gtk4 libcurl zlib
CFLAGS += -DHAVE_GTK4_LAYER_SHELL=1
else
DEPS := gtk4 libcurl zlib
endif

DEP_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(DEPS))
DEP_LIBS := $(shell $(PKG_CONFIG) --libs $(DEPS)) -lm

SRC := src/main.c src/util.c src/avatar.c src/bilibili.c src/ui.c
OBJ := $(patsubst src/%.c,build/%.o,$(SRC))
DEP := $(OBJ:.o=.d)
TARGET := bin/danmu_desktop

.PHONY: all clean install uninstall

all: $(TARGET)

build bin:
	mkdir -p $@

$(TARGET): $(OBJ) | bin
	$(CC) $(LDFLAGS) $(CFLAGS) $(DEP_CFLAGS) -o $@ $^ $(DEP_LIBS) $(LDLIBS)

build/%.o: src/%.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEP_CFLAGS) -Iinclude \
		-MMD -MP -c -o $@ $<

clean:
	rm -rf build
	rm -f $(TARGET)

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/danmu_desktop

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/danmu_desktop

-include $(DEP)
