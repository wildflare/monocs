// .macro、SYS 命令の別名（tlbi・ic・dc・at）、ldr =（リテラルプール）
.macro ventry type
    .balign 0x80
    sub     sp, sp, #816
    mov     x0, #\type
    b       common
.endm
.macro two a, b=7
    add \a, \a, #\b
lbl\@: nop
.endm
    .section .text.vectors, "ax"
    .balign 0x800
v:
    ventry 0 + 4
    ventry 3
    two x1
    two x2, 9
    two x3 5
common:
    tlbi vmalle1
    tlbi vmalle1is
    tlbi vae1, x3
    tlbi alle2
    ic iallu
    ic ialluis
    ic ivau, x4
    dc civac, x0
    dc cvac, x1
    dc cvau, x2
    dc ivac, x3
    dc zva, x5
    dc isw, x6
    dc csw, x7
    dc cisw, x8
    at s1e1r, x9
    at s1e0w, x10
    ldr x0, =0x30d00800
    ldr x1, =v
    ldr x2, =ext+16
    ldr w3, =77
    ldr x9, local1
local1:
    ret
