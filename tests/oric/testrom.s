; A 16K test ROM for the Oric machines (no Oric ROM needed) - exercises
; the ULA, the VIA's timer interrupt, the AY behind the VIA, the keyboard
; matrix and the tape signal, and leaves results in page 2 for tests/oric.c.
;
;   xa -o testrom.rom testrom.s
;
; $0200/1  T1 interrupts taken (100 Hz)
; $0210/1  row and column of the last key seen held ($ff - none)
; $0230    poke non-zero to read the tape - bytes after the first $24
;          following a $16 leader land at $0400, up to $0220/1 bytes
; $0222/3  bytes read; $0224 parity errors; $0225 set when done

VORB  = $0300
VORA  = $0301
DDRB = $0302
DDRA = $0303
T1CL = $0304
T1CH = $0305
T2CL = $0308
T2CH = $0309
ACR  = $030b
PCR  = $030c
IFR  = $030d
IER  = $030e

irqs = $0200
krow = $0210
kcol = $0211
tmax = $0220
tcnt = $0222
tpar = $0224
tdone = $0225
tgo  = $0230
ptr  = $00
bits = $02
byte = $03

* = $c000

reset:
    sei
    cld
    ldx #$ff
    txs
    lda #0
    sta irqs
    sta irqs+1
    sta tgo
    sta tdone
    lda #$ff
    sta krow
    sta kcol
    lda #<2000
    sta tmax
    lda #>2000
    sta tmax+1

    ; VIA - port A out to the AY, port B out except PB3, motor off
    lda #$ff
    sta DDRA
    lda #$f7
    sta DDRB
    lda #$00
    sta VORB
    lda #$dd
    sta PCR

    ; AY - tone A at period $100, full volume, port A output
    ldx #7
    lda #$7e
    jsr aywr
    ldx #0
    lda #$00
    jsr aywr
    ldx #1
    lda #$01
    jsr aywr
    ldx #8
    lda #$0f
    jsr aywr

    jsr font
    jsr screen

    ; T1 free-running every 10000 cycles
    lda #$40
    sta ACR
    lda #<9998
    sta T1CL
    lda #>9998
    sta T1CH
    lda #$c0
    sta IER
    cli

main:
    lda tgo
    beq main
    sei
    jsr tape
    lda #0
    sta tgo
    cli
    jmp main

; ---- AY register X = A, through the VIA ---------------------------------
aywr:
    stx VORA
    ldy #$ff            ; BDIR=1 BC1=1 - latch the register number
    sty PCR
    ldy #$dd
    sty PCR
    sta VORA
    ldy #$fd            ; BDIR=1 BC1=0 - write
    sty PCR
    ldy #$dd
    sty PCR
    rts

; ---- font - hex digits at '0'-'9', 'A'-'F', a block at $7f ---------------
font:
    ldx #0
f1: lda glyphs,x
    sta $b400+$30*8,x
    inx
    cpx #80
    bne f1
    ldx #0
f2: lda glyphs+80,x
    sta $b400+$41*8,x
    inx
    cpx #48
    bne f2
    ldx #7
    lda #$3f
f3: sta $b400+$7f*8,x
    dex
    bpl f3
    rts

; ---- the text screen ------------------------------------------------------
screen:
    ; clear $bb80-$bfdf to spaces (char $20, blank in this font)
    lda #<$bb80
    sta ptr
    lda #>$bb80
    sta ptr+1
    ldy #0
s1: lda #$20
    sta (ptr),y
    iny
    bne s1
    inc ptr+1
    lda ptr+1
    cmp #$c0
    bne s1
    ; row 0 - red paper, yellow ink, the digits
    lda #$11
    sta $bb80
    lda #$03
    sta $bb81
    ldx #15
s2: lda hex,x
    sta $bb82,x
    ora #$80            ; row 1 - the same inverted
    sta $bb80+40+2,x
    dex
    bpl s2
    lda #$0a            ; row 2 - double height
    sta $bb80+80
    lda #$0c            ; row 3 - blinking blocks
    sta $bb80+120
    ldx #7
    lda #$7f
s3: sta $bb80+120+2,x
    sta $bb80+80+2,x
    dex
    bpl s3
    rts

; ---- IRQ - count, scan the keyboard, show the key ---------------------------
irq:
    pha
    txa
    pha
    tya
    pha
    lda T1CL            ; clear the T1 flag
    inc irqs
    bne i1
    inc irqs+1
i1: jsr kbscan
    lda krow
    jsr hexout
    sta $bb80+160+2
    lda kcol
    jsr hexout
    sta $bb80+160+4
    pla
    tay
    pla
    tax
    pla
nmi:
    rti

hexout:
    and #$0f
    tax
    lda hex,x
    rts

; Row in PB0-2, one column low on the AY's port A, PB3 high if held.
kbscan:
    lda #$ff
    sta krow
    sta kcol
    lda #0
    sta bits            ; row
k1: lda VORB
    and #$f8
    ora bits
    sta VORB
    lda #0
    sta byte            ; column
k2: ldx byte
    lda colmask,x
    ldx #14
    jsr aywr
    nop
    nop
    lda VORB
    and #$08
    beq k3
    lda bits
    sta krow
    lda byte
    sta kcol
k3: inc byte
    lda byte
    cmp #8
    bne k2
    inc bits
    lda bits
    cmp #8
    bne k1
    rts

; ---- tape - rising CB1 edges timed with T2 ---------------------------------
; A 1 is 416 cycles from rise to rise, a 0 is 832.
tape:
    lda #0
    sta tcnt
    sta tcnt+1
    sta tpar
    sta tdone
    lda #<$0400
    sta ptr
    lda #>$0400
    sta ptr+1
    lda PCR
    ora #$10            ; CB1 on rising edges, as the BASIC ROM does
    sta PCR
    lda #$ff            ; T2 runs down from $ffff
    sta T2CL
    sta T2CH
    lda VORB
    ora #$40            ; motor on
    sta VORB
    jsr rdbit             ; the first edge only starts the clock
    ; leader - bytes until $24
t1: jsr rdbyte
    cmp #$24
    bne t1
t2: jsr rdbyte
    ldy #0
    sta (ptr),y
    inc ptr
    bne t3
    inc ptr+1
t3: inc tcnt
    bne t4
    inc tcnt+1
t4: lda tcnt
    cmp tmax
    bne t2
    lda tcnt+1
    cmp tmax+1
    bne t2
    lda VORB
    and #$bf
    sta VORB
    lda #1
    sta tdone
    rts

; One framed byte - wait for the 0 after the 1s, eight bits, parity.
rdbyte:
r0: jsr rdbit
    bcs r0
    lda #0
    sta byte
    ldx #8
r1: jsr rdbit
    ror byte
    dex
    bne r1
    jsr rdbit             ; parity - odd over data and parity
    lda #0
    rol
    ldx #8
    ldy byte
r2: pha
    tya
    lsr
    tay
    pla
    adc #0
    dex
    bne r2
    and #1
    bne r3
    inc tpar
r3: lda byte
    rts

; Carry = the next bit. Uses the time since the last rising edge - T2
; ran down from $ffff, so its high byte is $ff less the elapsed pages
; (a 1 is 416 cycles, page 1; a 0 is 832, page 3).
rdbit:
    lda IFR
    and #$10
    beq rdbit
    lda T2CH
    ldy VORB             ; clears the CB1 flag
    ldy #$ff
    sty T2CL
    sty T2CH
    cmp #$fe            ; carry set (a 1) unless two or more pages passed
    rts

hex:
    .byt "0123456789ABCDEF"

colmask:
    .byt $fe, $fd, $fb, $f7, $ef, $df, $bf, $7f

; 6x8 hex digits, bit 5 leftmost
glyphs:
    .byt $1c,$22,$26,$2a,$32,$22,$1c,$00    ; 0
    .byt $08,$18,$08,$08,$08,$08,$1c,$00    ; 1
    .byt $1c,$22,$02,$0c,$10,$20,$3e,$00    ; 2
    .byt $3e,$02,$04,$0c,$02,$22,$1c,$00    ; 3
    .byt $04,$0c,$14,$24,$3e,$04,$04,$00    ; 4
    .byt $3e,$20,$3c,$02,$02,$22,$1c,$00    ; 5
    .byt $0c,$10,$20,$3c,$22,$22,$1c,$00    ; 6
    .byt $3e,$02,$04,$08,$10,$10,$10,$00    ; 7
    .byt $1c,$22,$22,$1c,$22,$22,$1c,$00    ; 8
    .byt $1c,$22,$22,$1e,$02,$04,$18,$00    ; 9
    .byt $08,$14,$22,$22,$3e,$22,$22,$00    ; A
    .byt $3c,$22,$22,$3c,$22,$22,$3c,$00    ; B
    .byt $1c,$22,$20,$20,$20,$22,$1c,$00    ; C
    .byt $3c,$22,$22,$22,$22,$22,$3c,$00    ; D
    .byt $3e,$20,$20,$3c,$20,$20,$3e,$00    ; E
    .byt $3e,$20,$20,$3c,$20,$20,$20,$00    ; F

    .dsb $fffa-*, $ff
    .word nmi, reset, irq
