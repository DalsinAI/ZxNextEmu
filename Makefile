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
DEV_SRC  = src/devices/esp8266.c src/devices/nextpi.c src/devices/saa1099.c src/devices/sid.c src/devices/gs.c src/devices/ata.c
NEXT_SRC = $(STORE_SRC) $(DEV_SRC) src/next/next.c src/next/uart.c src/next/video.c src/next/dma.c src/next/sd.c src/next/bootrom.c
comma := ,
SAM_SRC  = src/sam/sam.c src/devices/floppy.c src/devices/wd177x.c
MSX_SRC  = src/msx/msx.c src/msx/vdp.c src/msx/vdpcmd.c src/msx/cart.c src/msx/scc.c src/msx/rtc.c \
           src/msx/fdc.c src/msx/ide.c
ZXM_SRC  = src/core/zxm.c src/core/zxm_zx.c src/core/zxm_sam.c src/core/zxm_msx.c $(SAM_SRC) $(MSX_SRC)
ZX_SRC   = src/spec256/spec256.c src/spec256/palette.c src/zx/zx.c src/zx/beta.c src/zx/expansion.c src/zx/files.c src/zx/keys.c src/zx/ay.c src/zx/tape.c src/zx/snap.c src/frontend/media.c $(NEXT_SRC)
ZX_HDR   = $(wildcard src/spec256/*.h src/z80/*.h src/zx/*.h src/next/*.h src/sam/*.h src/storage/*.h src/devices/*.h src/frontend/*.h src/msx/*.h src/core/*.h)

.PHONY: all desktop test sst sam vfat esp nextpi ulaplus mb03 esxdos state beta wasm wasmtest web serve clean

all: $(BUILD)/zxrun $(BUILD)/zxmrun $(BUILD)/zex $(BUILD)/sst $(BUILD)/vfattool $(BUILD)/esp $(BUILD)/zxm $(BUILD)/mb03

$(BUILD)/zex: tests/zex.c $(CORE_SRC) src/z80/z80.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/zex.c $(CORE_SRC)

$(BUILD)/zxrun: src/frontend/headless.c src/frontend/net_posix.c $(CORE_SRC) $(ZX_SRC) $(ZX_HDR) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ src/frontend/headless.c src/frontend/net_posix.c $(CORE_SRC) $(ZX_SRC)

# Any machine through zxm (zxmrun -l lists them).
$(BUILD)/zxmrun: src/frontend/zxmrun.c $(ZXM_SRC) $(CORE_SRC) $(ZX_SRC) $(ZX_HDR) src/core/zxm.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ src/frontend/zxmrun.c $(ZXM_SRC) $(CORE_SRC) $(ZX_SRC)

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

$(BUILD)/sst: tests/sst.c tests/json.h $(CORE_SRC) src/z80/z80.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/sst.c $(CORE_SRC)

sst: $(BUILD)/sst
	tests/run-sst.sh


$(BUILD)/vfattool: tests/vfattool.c $(STORE_SRC) src/storage/blockdev.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/vfattool.c $(STORE_SRC)

# Folder-backed SD cards (needs mtools and dosfstools).
vfat: $(BUILD)/vfattool
	tests/vfat.sh $(BUILD)/vfattool
	tests/vfat.sh $(BUILD)/vfattool 16

$(BUILD)/esp: tests/esp.c src/frontend/net_posix.c $(CORE_SRC) $(ZX_SRC) $(ZX_HDR) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/esp.c src/frontend/net_posix.c $(CORE_SRC) $(ZX_SRC)

# The ESP8266 wifi module on the Next's UART, against a local echo server.
esp: $(BUILD)/esp
	tests/esp.sh $(BUILD)/esp

# NextZXOS's Pi tools against the emulated NextPi (TBBLUE=the Next software).
nextpi: $(BUILD)/zxrun
	tests/nextpi.sh $(BUILD)/zxrun

$(BUILD)/sam: tests/sam.c $(ZXM_SRC) $(CORE_SRC) $(ZX_SRC) $(ZX_HDR) src/core/zxm.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/sam.c $(ZXM_SRC) $(CORE_SRC) $(ZX_SRC)

$(BUILD)/mkmgt: tests/mkmgt.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/mkmgt.c

# SAM Coupé: BOOT, SAVE and read back, SAA1099 pitch (fetches SAMDOS 2).
sam: $(BUILD)/sam $(BUILD)/mkmgt
	tests/sam.sh $(BUILD)

$(BUILD)/zxm: tests/zxm.c $(ZXM_SRC) $(CORE_SRC) $(ZX_SRC) $(ZX_HDR) src/core/zxm.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/zxm.c $(ZXM_SRC) $(CORE_SRC) $(ZX_SRC)

$(BUILD)/ulaplus: tests/ulaplus.c $(ZXM_SRC) $(CORE_SRC) $(ZX_SRC) $(ZX_HDR) src/core/zxm.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/ulaplus.c $(ZXM_SRC) $(CORE_SRC) $(ZX_SRC)

ulaplus: $(BUILD)/ulaplus
	$(BUILD)/ulaplus roms

# Save, run on, load, run again: every machine must repeat itself exactly.
ESXDOS_ROM ?= tests-data/esxdos/esxmmc.bin
GS_ROM     ?= tests-data/gs/gs105a.rom

# The clones' ROMs are not in the repository: put them in CLONE_ROMS (with
# the others) to include them; without, they are skipped.
CLONE_ROMS ?= roms
MACHINES   = zx48 zx128 plus2 plus2a plus3 pentagon next scorpion atm2 spec256 sam msx1 msx1eu msx2 msx2eu

state: $(BUILD)/zxm
	@for mc in $(MACHINES); do \
	    $(BUILD)/zxm $(CLONE_ROMS) $$mc 300 "print 2+2\n"; r=$$?; \
	    [ $$r = 0 ] || [ $$r = 77 ] || exit 1; done
	@if [ -f $(ESXDOS_ROM) ]; then for mc in zx48 zx128 plus3 pentagon; do \
	    ZXM_IFACE=mb03 ZXM_IFACE_ROM=$(ESXDOS_ROM) $(BUILD)/zxm roms $$mc 300 "print 2+2\n" || exit 1; done; \
	else echo "(no $(ESXDOS_ROM): skipping the MB03+ machines)"; fi

$(BUILD)/mb03: tests/mb03.c $(CORE_SRC) $(ZX_SRC) $(ZX_HDR) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/mb03.c $(CORE_SRC) $(ZX_SRC)

# The expansion-port interfaces (divMMC, MB03+ Ultimate).
mb03: $(BUILD)/mb03
	$(BUILD)/mb03 roms

# esxDOS on a divMMC, from a folder SD card (needs tests-data/esxdos/, see
# tests/README.md).
esxdos: $(BUILD)/zxrun
	tests/esxdos.sh $(BUILD)/zxrun

# ---- zxcore.wasm: the core for ZX Cradle (clang + wasi-libc headers and
# malloc; the module imports nothing). -------------------------------------
WASM_CC      ?= clang
WASI_SYSROOT ?= /usr
WASM_SRC = $(ZXM_SRC) $(CORE_SRC) src/spec256/spec256.c src/spec256/palette.c src/zx/zx.c src/zx/beta.c src/zx/expansion.c src/zx/keys.c src/zx/ay.c src/zx/tape.c src/zx/snap.c \
           src/next/next.c src/next/uart.c src/next/video.c src/next/dma.c src/next/sd.c src/next/bootrom.c \
           src/devices/saa1099.c src/devices/sid.c src/devices/gs.c src/devices/ata.c
WASM_EXPORTS = zxm_lookup zxm_machine zxm_size zxm_create zxm_destroy zxm_info_of zxm_reset \
           zxm_run_frame zxm_framebuffer zxm_fb_width zxm_audio zxm_input zxm_type zxm_typing \
           zxm_media_insert zxm_media_eject zxm_media_active zxm_media_progress zxm_interface_lookup zxm_interface_at zxm_attach zxm_slots zxm_inputs \
           zxm_state_size zxm_state_save zxm_state_load \
           zxm_debugger malloc free

wasm: $(BUILD)/zxcore.wasm

$(BUILD)/zxcore.wasm: $(WASM_SRC) $(ZX_HDR) src/core/zxm.h | $(BUILD)
	$(WASM_CC) --target=wasm32-wasi --sysroot=$(WASI_SYSROOT) -O2 -std=c99 -Wall -Wextra -pedantic \
	    -mexec-model=reactor $(addprefix -Wl$(comma)--export=,$(WASM_EXPORTS)) -o $@ $(WASM_SRC)

# The WebAssembly core must give exactly the native core's pictures and sound.
$(BUILD)/mb03tune: tests/mb03tune.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/mb03tune.c

wasmtest: $(BUILD)/zxcore.wasm $(BUILD)/zxm $(BUILD)/mb03tune
	@for mc in $(MACHINES); do rd=$(CLONE_ROMS); \
	    a=$$($(BUILD)/zxm $$rd $$mc 300 'print 2+2\n' | cut -d' ' -f1-4) || exit 1; \
	    case "$$a" in *skipped*) echo "$$a"; continue;; esac; \
	    b=$$(node tests/wasm.mjs $(BUILD)/zxcore.wasm $$rd $$mc 300 'print 2+2\n') || exit 1; \
	    if [ "$$a" = "$$b" ]; then echo "$$b (native and wasm agree)"; \
	    else echo "MISMATCH: native '$$a' wasm '$$b'"; exit 1; fi; done
	@if [ -f $(ESXDOS_ROM) ]; then for mc in zx48 zx128; do \
	    export ZXM_IFACE=mb03 ZXM_IFACE_ROM=$(ESXDOS_ROM); \
	    a=$$($(BUILD)/zxm roms $$mc 300 'print 2+2\n' | cut -d' ' -f1-4) || exit 1; \
	    b=$$(node tests/wasm.mjs $(BUILD)/zxcore.wasm roms $$mc 300 'print 2+2\n') || exit 1; \
	    if [ "$$a" = "$$b" ]; then echo "$$b with an MB03+ (native and wasm agree)"; \
	    else echo "MISMATCH: native '$$a' wasm '$$b'"; exit 1; fi; done; \
	    $(BUILD)/mb03tune $(BUILD)/mb03tune.sna || exit 1; \
	    export ZXM_SNAP=$(BUILD)/mb03tune.sna; \
	    if [ -f $(GS_ROM) ]; then export ZXM_GS_ROM=$(GS_ROM); fi; \
	    a=$$($(BUILD)/zxm roms zx48 100 | cut -d' ' -f1-4) || exit 1; \
	    b=$$(node tests/wasm.mjs $(BUILD)/zxcore.wasm roms zx48 100) || exit 1; \
	    if [ "$$a" = "$$b" ]; then echo "$$b, MB03+ SID, SAA1099$${ZXM_GS_ROM:+ and General Sound} (native and wasm agree)"; \
	    else echo "MISMATCH: native '$$a' wasm '$$b'"; exit 1; fi; fi

# ---- The browser runtime: build/web is a static site (open it through any
# web server; `make serve` uses port 7081, ZX Cradle's dev port). ----------
WEB_FILES = web/index.html web/app.js web/app.css web/worker.js web/audio.js web/library.js web/libview.js web/cradle.js

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
