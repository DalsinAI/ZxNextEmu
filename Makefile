# ZxNextEmu
#
#   make                 host build (any GCC; the project compiler is
#                        GCC 16.2.0-open3)
#   make test            CPU exercisers (needs tests-data/, see tests/README.md)
#   make amiga           AmigaOS 3.x build with m68k-amigaos-gcc 16.2.0-open3

CC      ?= gcc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c99 -Wall -Wextra -pedantic
BUILD   ?= build

CORE_SRC = src/z80/z80.c
STORE_SRC = src/storage/blockdev.c src/storage/vfat.c
DEV_SRC  = src/devices/esp8266.c
NEXT_SRC = $(STORE_SRC) $(DEV_SRC) src/next/next.c src/next/uart.c src/next/video.c src/next/dma.c src/next/sd.c src/next/bootrom.c
ZX_SRC   = src/zx/zx.c src/zx/ay.c src/zx/tape.c src/zx/snap.c src/frontend/media.c $(NEXT_SRC)
ZX_HDR   = $(wildcard src/z80/*.h src/zx/*.h src/next/*.h src/storage/*.h src/devices/*.h src/frontend/*.h)

.PHONY: all desktop test sst vfat esp clean

all: $(BUILD)/zxrun $(BUILD)/zex $(BUILD)/sst $(BUILD)/vfattool $(BUILD)/esp

$(BUILD)/zex: tests/zex.c $(CORE_SRC) src/z80/z80.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/zex.c $(CORE_SRC)

$(BUILD)/zxrun: src/frontend/headless.c src/frontend/net_posix.c $(CORE_SRC) $(ZX_SRC) $(ZX_HDR) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ src/frontend/headless.c src/frontend/net_posix.c $(CORE_SRC) $(ZX_SRC)

# Desktop front end (needs SDL2 development files).
SDL_CFLAGS ?= $(shell sdl2-config --cflags)
SDL_LIBS   ?= $(shell sdl2-config --libs)

desktop: $(BUILD)/zxnextemu

$(BUILD)/zxnextemu: src/frontend/sdl.c $(CORE_SRC) $(ZX_SRC) $(ZX_HDR) | $(BUILD)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -o $@ src/frontend/sdl.c src/frontend/net_posix.c $(CORE_SRC) $(ZX_SRC) $(SDL_LIBS)

$(BUILD):
	mkdir -p $@

test: $(BUILD)/zex
	$(BUILD)/zex tests-data/zexdoc.com
	$(BUILD)/zex tests-data/zexall.com

clean:
	rm -rf $(BUILD)

$(BUILD)/sst: tests/sst.c $(CORE_SRC) src/z80/z80.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/sst.c $(CORE_SRC)

sst: $(BUILD)/sst
	tests/run-sst.sh

$(BUILD)/vfattool: tests/vfattool.c $(STORE_SRC) src/storage/blockdev.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/vfattool.c $(STORE_SRC)

# Folder-backed SD cards (needs mtools and dosfstools).
vfat: $(BUILD)/vfattool
	tests/vfat.sh $(BUILD)/vfattool

$(BUILD)/esp: tests/esp.c src/frontend/net_posix.c $(CORE_SRC) $(ZX_SRC) $(ZX_HDR) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/esp.c src/frontend/net_posix.c $(CORE_SRC) $(ZX_SRC)

# The ESP8266 wifi module on the Next's UART, against a local echo server.
esp: $(BUILD)/esp
	tests/esp.sh $(BUILD)/esp
