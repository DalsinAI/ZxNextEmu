; vdptest: an MSX2 cartridge that draws a SCREEN 5 picture with the V9938
; command engine (HMMV, LMMV, LINE, LMMM, HMMM, PSET) and shows two
; sprites, one of them two-coloured with the CC bit. Built with pasmo:
;   pasmo tests/msx/vdptest.asm vdptest.rom
; RAM C000 on: the number of polls of CE each command took, 16-bit each.

VDPD    equ     98h
VDPC    equ     99h
VDPP    equ     9Ah
VDPI    equ     9Bh

        org     4000h
        db      "AB"
        dw      init
        dw      0, 0, 0, 0, 0, 0

init:   di
        ld      sp, 0F000h
        ld      ix, 0C000h
        ; switch page 2 and 3 to RAM: C-BIOS left them there already

        ld      hl, regs
        ld      b, (regs_end - regs) / 2
setr:   ld      a, (hl)
        out     (VDPC), a
        inc     hl
        ld      a, (hl)
        or      80h
        out     (VDPC), a
        inc     hl
        djnz    setr

        ; palette: colour 1 dark grey, 2 orange
        xor     a
        out     (VDPC), a
        ld      a, 16 + 80h
        out     (VDPC), a
        ld      hl, palette
        ld      bc, 32 * 256 + VDPP
        otir

        ld      hl, c_clear
        call    cmd
        ld      hl, c_box
        call    cmd
        ld      hl, c_line1
        call    cmd
        ld      hl, c_line2
        call    cmd
        ld      hl, c_copy
        call    cmdsx
        ld      hl, c_hcopy
        call    cmdsx
        ld      hl, c_pset
        call    cmd
        call    wait

        ; sprites: patterns at 7800, colours at 7400, attributes at 7600
        ld      hl, 7800h
        call    vaddr
        ld      hl, sprpat
        ld      bc, 64 * 256 + VDPD
        otir
        ld      hl, 7400h
        call    vaddr
        ld      b, 16
sc0:    ld      a, 0Fh              ; sprite 0: white
        out     (VDPD), a
        djnz    sc0
        ld      b, 16
sc1:    ld      a, 40h + 02h        ; sprite 1: CC, OR-ed onto sprite 0
        out     (VDPD), a
        djnz    sc1
        ld      hl, 7600h
        call    vaddr
        ld      hl, sprattr
        ld      bc, 12 * 256 + VDPD
        otir

loop:   jr      loop

; HL -> 11 bytes for R#36..R#46. Counts the polls of CE before it starts.
cmd:    call    wait
        ld      a, 36
        out     (VDPC), a
        ld      a, 17 + 80h
        out     (VDPC), a
        ld      bc, 11 * 256 + VDPI
        otir
        ret

; HL -> 15 bytes for R#32..R#46.
cmdsx:  call    wait
        ld      a, 32
        out     (VDPC), a
        ld      a, 17 + 80h
        out     (VDPC), a
        ld      bc, 15 * 256 + VDPI
        otir
        ret

; Wait for CE to clear; store the number of polls at (IX).
wait:   ld      a, 2
        out     (VDPC), a
        ld      a, 15 + 80h
        out     (VDPC), a
        ld      de, 0
w1:     inc     de
        in      a, (VDPC)
        rrca
        jr      c, w1
        xor     a
        out     (VDPC), a
        ld      a, 15 + 80h
        out     (VDPC), a
        ld      (ix + 0), e
        ld      (ix + 1), d
        inc     ix
        inc     ix
        ret

; Set the VRAM write address to HL (in the first 64K).
vaddr:  ld      a, h
        rlca
        rlca
        and     3
        out     (VDPC), a
        ld      a, 14 + 80h
        out     (VDPC), a
        ld      a, l
        out     (VDPC), a
        ld      a, h
        and     3Fh
        or      40h
        out     (VDPC), a
        ret

regs:   db      06h, 0              ; G4
        db      42h, 1              ; display on, 16x16 sprites
        db      1Fh, 2              ; page 0
        db      0EFh, 5             ; sprite attributes 7600
        db      0Fh, 6              ; sprite patterns 7800
        db      04h, 7              ; border colour 4
        db      08h, 8              ; 64K VRAM, sprites on
        db      80h, 9              ; 212 lines, 60 Hz
        db      00h, 10
        db      00h, 11
        db      00h, 14
        db      00h, 23
regs_end:

palette:
        db      00h, 0, 22h, 2, 72h, 3, 00h, 0
        db      17h, 1, 27h, 3, 51h, 1, 27h, 6
        db      71h, 1, 73h, 3, 61h, 6, 64h, 6
        db      11h, 4, 65h, 2, 55h, 5, 77h, 7

;                DX      DY      NX      NY      CLR  ARG  CMD
c_clear:  dw     0,      0,      256,    212
          db     11h, 00h, 0C0h                      ; HMMV colour 1
c_box:    dw     16,     16,     64,     48
          db     08h, 00h, 80h                       ; LMMV red
c_line1:  dw     0,      0,      255,    211
          db     0Fh, 00h, 70h                       ; LINE white, X major
c_line2:  dw     200,    10,     150,    40
          db     0Ah, 01h, 70h                       ; LINE yellow, Y major
;                SX      SY      DX      DY      NX      NY      CLR  ARG  CMD
c_copy:   dw     16,     16,     128,    120,    64,     48
          db     00h, 00h, 93h                       ; LMMM XOR
c_hcopy:  dw     16,     16,     20,     150,    64,     40
          db     00h, 00h, 0D0h                      ; HMMM
c_pset:   dw     250,    200,    0,      0
          db     0Eh, 00h, 50h                       ; PSET

sprpat:                                             ; pattern 0: a solid block
        ds      32, 0FFh
                                                    ; pattern 4: a frame
        db      0FFh, 80h, 80h, 80h, 80h, 80h, 80h, 80h
        db      80h, 80h, 80h, 80h, 80h, 80h, 80h, 0FFh
        db      0FFh, 01h, 01h, 01h, 01h, 01h, 01h, 01h
        db      01h, 01h, 01h, 01h, 01h, 01h, 01h, 0FFh

sprattr:
        db      99, 100, 0, 0               ; y + 1 = line 100
        db      99, 100, 4, 0
        db      216, 0, 0, 0                ; end of list

        ds      8000h - $, 0FFh
