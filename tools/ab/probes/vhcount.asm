; vhcount.asm -- timing probe: V and H counter reads at every phase of a line.
;
; After VBLANK, reads the V counter 256 times at a 37 T-state stride (37 and
; the 228-T line are coprime, so the samples visit every phase), then latches
; and reads the H counter 256 times at a 73 T stride (also coprime with 228).
; Results at $C100 (V) and $C200 (H); $C000 counts completed passes. The
; whole run repeats once a frame for three frames, at a different VBLANK
; phase each time (the poll loop's 30 T quantum against the 228 T line).
        org     0
        di
        im      1
        ld      sp, $dff0
        jp      main
        defs    $38 - ASMPC, $ff
        ei
        reti
        defs    $66 - ASMPC, $ff
        retn
main:
        xor     a
        ld      ($c000), a
pass:
        in      a, ($bf)        ; clear a pending VINT flag
wait:
        in      a, ($bf)
        and     $80
        jr      z, wait
        ld      a, ($c000)
        add     a, a            ; pass * 2 -> page offset in $C1..$C6
        add     a, $c1
        ld      h, a
        ld      l, 0
        ld      b, 0
vloop:
        in      a, ($7e)        ; 11
        ld      (hl), a         ;  7
        inc     hl              ;  6
        djnz    vloop           ; 13
        ld      b, 0
hloop:
        ld      a, $00          ;  7  TH1/TH2 outputs, low
        out     ($3f), a        ; 11
        ld      a, $0a          ;  7  TH1/TH2 inputs: the rising edge latches
        out     ($3f), a        ; 11
        in      a, ($7f)        ; 11
        ld      (hl), a         ;  7
        inc     hl              ;  6  (+ djnz 13: 73 T a sample)
        djnz    hloop
        ld      a, ($c000)
        inc     a
        ld      ($c000), a
        cp      3
        jr      nz, pass
done:
        jr      done
        defs    $7ff0 - ASMPC, $ff
        defm    "TMR SEGA"
        defs    $8000 - ASMPC, $00
