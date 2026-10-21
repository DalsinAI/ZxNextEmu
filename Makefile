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
DEV_SRC  = src/devices/esp8266.c src/devices/nextpi.c
NEXT_SRC = $(STORE_SRC) $(DEV_SRC) src/next/next.c src/next/uart.c src/next/video.c src/next/dma.c src/next/sd.c src/next/bootrom.c
comma := ,
ZXM_SRC  = src/core/zxm.c
ZX_SRC   = src/zx/zx.c src/zx/beta.c src/zx/files.c src/zx/keys.c src/zx/ay.c src/zx/tape.c src/zx/snap.c src/frontend/media.c $(NEXT_SRC)
ZX_HDR   = $(wildcard src/z80/*.h src/zx/*.h src/next/*.h src/storage/*.h src/devices/*.h src/frontend/*.h)

.PHONY: all desktop test sst vfat esp nextpi ulaplus state beta wasm wasmtest web serve clean

all: $(BUILD)/zxrun $(BUILD)/zex $(BUILD)/sst $(BUILD)/vfattool $(BUILD)/esp $(BUILD)/zxm

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

# NextZXOS's Pi tools against the emulated NextPi (TBBLUE=the Next software).
nextpi: $(BUILD)/zxrun
	tests/nextpi.sh $(BUILD)/zxrun

$(BUILD)/zxm: tests/zxm.c $(ZXM_SRC) $(CORE_SRC) $(ZX_SRC) $(ZX_HDR) src/core/zxm.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/zxm.c $(ZXM_SRC) $(CORE_SRC) $(ZX_SRC)

$(BUILD)/ulaplus: tests/ulaplus.c $(ZXM_SRC) $(CORE_SRC) $(ZX_SRC) $(ZX_HDR) src/core/zxm.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/ulaplus.c $(ZXM_SRC) $(CORE_SRC) $(ZX_SRC)

ulaplus: $(BUILD)/ulaplus
	$(BUILD)/ulaplus roms

# Save, run on, load, run again: every machine must repeat itself exactly.
# The clones' ROMs are not in the repository: put them in CLONE_ROMS (with
# the others) to include them; without, they are skipped.
CLONE_ROMS ?= roms
MACHINES   = zx48 zx128 plus2 plus2a plus3 pentagon next scorpion atm2

state: $(BUILD)/zxm
	@for mc in $(MACHINES); do \
	    $(BUILD)/zxm $(CLONE_ROMS) $$mc 300 "print 2+2\n"; r=$$?; \
	    [ $$r = 0 ] || [ $$r = 77 ] || exit 1; done

# ---- zxcore.wasm: the core for ZX Cradle (clang + wasi-libc headers and
# malloc; the module imports nothing). -------------------------------------
WASM_CC      ?= clang
WASI_SYSROOT ?= /usr
WASM_SRC = $(ZXM_SRC) $(CORE_SRC) src/zx/zx.c src/zx/beta.c src/zx/keys.c src/zx/ay.c src/zx/tape.c src/zx/snap.c \
           src/next/next.c src/next/uart.c src/next/video.c src/next/dma.c src/next/sd.c src/next/bootrom.c
WASM_EXPORTS = zxm_lookup zxm_machine zxm_size zxm_create zxm_destroy zxm_info_of zxm_reset \
           zxm_run_frame zxm_framebuffer zxm_audio zxm_input zxm_type zxm_typing \
           zxm_media_insert zxm_media_eject zxm_state_size zxm_state_save zxm_state_load \
           zxm_debugger malloc free

wasm: $(BUILD)/zxcore.wasm

$(BUILD)/zxcore.wasm: $(WASM_SRC) $(ZX_HDR) src/core/zxm.h | $(BUILD)
	$(WASM_CC) --target=wasm32-wasi --sysroot=$(WASI_SYSROOT) -O2 -std=c99 -Wall -Wextra -pedantic \
	    -mexec-model=reactor $(addprefix -Wl$(comma)--export=,$(WASM_EXPORTS)) -o $@ $(WASM_SRC)

# The WebAssembly core must give exactly the native core's pictures and sound.
wasmtest: $(BUILD)/zxcore.wasm $(BUILD)/zxm
	@for mc in $(MACHINES); do \
	    a=$$($(BUILD)/zxm $(CLONE_ROMS) $$mc 300 'print 2+2\n' | cut -d' ' -f1-4) || exit 1; \
	    case "$$a" in *skipped*) echo "$$a"; continue;; esac; \
	    b=$$(node tests/wasm.mjs $(BUILD)/zxcore.wasm $(CLONE_ROMS) $$mc 300 'print 2+2\n') || exit 1; \
	    if [ "$$a" = "$$b" ]; then echo "$$b (native and wasm agree)"; \
	    else echo "MISMATCH: native '$$a' wasm '$$b'"; exit 1; fi; done

# ---- The browser runtime: build/web is a static site (open it through any
# web server; `make serve` uses port 7081, ZX Cradle's dev port). ----------
WEB_FILES = web/index.html web/app.js web/app.css web/worker.js web/audio.js

web: $(BUILD)/zxcore.wasm $(WEB_FILES)
	mkdir -p $(BUILD)/web/cradle
	cp $(WEB_FILES) $(BUILD)/zxcore.wasm $(BUILD)/web/
	cp docs/cradle/tokens/zxcradle.css docs/cradle/tokens/components.css $(BUILD)/web/cradle/
	python3 -c 'import base64,glob,json,os,sys; json.dump({os.path.basename(f): base64.b64encode(open(f,"rb").read()).decode() for f in sorted(glob.glob("roms/*.rom"))}, sys.stdout)' > $(BUILD)/web/roms.json

serve: web
	@echo "ZX Cradle: http://localhost:7081/"
	cd $(BUILD)/web && python3 -m http.server 7081

$(BUILD)/beta: tests/beta.c $(ZXM_SRC) $(CORE_SRC) $(ZX_SRC) $(ZX_HDR) src/core/zxm.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/beta.c $(ZXM_SRC) $(CORE_SRC) $(ZX_SRC)

# TR-DOS on the Beta 128 machines (needs python3 and the clones' ROMs).
beta: $(BUILD)/beta
	tests/beta.sh $(BUILD)/beta $(CLONE_ROMS)
