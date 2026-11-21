// リテラルプール: .ltorg、別のセクション
  .text
f:
  ldr w3, =0x12345678
  ldr x0, =sym
  ldr x1, =sym
  ldr x2, =5
  .section .text.b,"ax"
  ldr x3, =7
  ldr w4, =9
  .text
  ldr w5, =0x12345678
  .ltorg
  ldr x6, =5
  .section .text.b,"ax"
  nop
