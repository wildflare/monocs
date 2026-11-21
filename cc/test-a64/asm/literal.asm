// リテラルプール: 大きさごとのまとめ方、同じ値の共有、セクションをまたぐ場合
  .text
f:
  ldr x0, =0x30d00800
  ldr x1, =5
  ldr x2, =sym
  ldr w3, =0x12345678
  ldr x4, =0x30d00800
  ldr x5, =sym+8
  ldr x6, =0x1234567812345678
  ret
  .data
  .word 1
  .text
g:
  ldr x7, =other
  nop
