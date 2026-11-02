# Build the quick start with pkg-config against an installed (or unpacked)
# Rerinku Device SDK:
#
#   make PKG_CONFIG_PATH=<sdk dir>/lib/pkgconfig
#   make PKG_CONFIG_PATH=<sdk dir>/lib/pkgconfig CC=arm-rockchip830-linux-uclibcgnueabihf-gcc
#
# `quickstart` links the static archive (it holds every dependency and needs
# only the C library and pthreads); `quickstart-shared` links the .so.
CC ?= cc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra $(shell $(PKG_CONFIG) --cflags rerinku_device)
SDK_LIBDIR := $(shell $(PKG_CONFIG) --variable=libdir rerinku_device)

all: quickstart

quickstart: quickstart.c
	$(CC) $(CFLAGS) -o $@ $< $(SDK_LIBDIR)/librerinku_device.a -pthread

quickstart-shared: quickstart.c
	$(CC) $(CFLAGS) -o $@ $< $(shell $(PKG_CONFIG) --libs rerinku_device) -Wl,-rpath,$(SDK_LIBDIR)

clean:
	rm -f quickstart quickstart-shared

.PHONY: all clean
