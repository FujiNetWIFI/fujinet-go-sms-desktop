; irq.asm -- timing probe: when VINT and line interrupts are taken.
;
; VINT on, line interrupts every 16 lines. The IM 1 handler latches the H
; counter, reads the V counter and the status register (acknowledging) and
; appends the three bytes at $C100+; $C000/$C001 is the write pointer. The
; main loop runs NOPs for the first 120 records and HALT afterwards, the two
; ways programs wait for an interrupt.
        org     0
        di
        im      1
        ld      sp, $dff0
        jp      main
        defs    $38 - ASMPC, $ff
        push    af
        push    hl
        ld      a, $00          ; TH outputs low
        out     ($3f), a
        ld      a, $0a          ; TH inputs: latch H
        out     ($3f), a
        ld      hl, ($c000)
        in      a, ($7e)
        ld      (hl), a
        inc     hl
        in      a, ($7f)
        ld      (hl), a
        inc     hl
        in      a, ($bf)        ; status, acknowledges
        ld      (hl), a
        inc     hl
        ld      ($c000), hl
        pop     hl
        pop     af
        ei
        ret
        defs    $66 - ASMPC, $ff
        retn
main:
        ld      hl, $c100
        ld      ($c000), hl
        ld      a, $10          ; R0: line interrupts on, mode 4 off (TMS)
        out     ($bf), a
        ld      a, $80
        out     ($bf), a
        ld      a, $0f          ; R10: every 16 lines
        out     ($bf), a
        ld      a, $8a
        out     ($bf), a
        ld      a, $e0          ; R1: display on, VINT on
        out     ($bf), a
        ld      a, $81
        out     ($bf), a
        in      a, ($bf)
        ei
spin:
        nop
        nop
        nop
        ld      a, ($c001)      ; until the pointer passes $C268 (120 records)
        cp      $c2
        jr      c, spin
        ld      a, ($c000)
        cp      $68
        jr      c, spin
sleep:
        halt
        ld      a, ($c001)
        cp      $c5
        jr      c, sleep
        di
done:
        jr      done
        defs    $7ff0 - ASMPC, $ff
        defm    "TMR SEGA"
        defs    $8000 - ASMPC, $00
