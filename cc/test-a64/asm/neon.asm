// SIMD（picolibc の memcpy.S・strlen.S などが使うもの）と ccmp・ccmn、マクロのキーワード引数
.macro def_fn f p2align=0
  .text
  .p2align \p2align
  .global \f
  .type \f, %function
\f:
.endm
def_fn neon_test p2align=5
  add v0.4s, v1.4s, v2.4s
  add v3.16b, v4.16b, v5.16b
  add v6.2d, v7.2d, v8.2d
  sub v1.8h, v2.8h, v3.8h
  addp v0.16b, v1.16b, v2.16b
  addp v0.2d, v1.2d, v2.2d
  addp v31.4s, v30.4s, v29.4s
  cmeq v0.16b, v1.16b, v2.16b
  cmeq v0.8b, v1.8b, v2.8b
  cmeq v0.16b, v1.16b, 0
  cmeq v0.8b, v1.8b, #0
  cmeq v5.4s, v6.4s, #0
  cmhs v0.16b, v1.16b, v2.16b
  cmhi v0.8h, v1.8h, v2.8h
  cmgt v0.4s, v1.4s, v2.4s
  cmge v0.2d, v1.2d, v2.2d
  cmgt v0.16b, v1.16b, #0
  umaxp v0.16b, v1.16b, v2.16b
  uminp v0.8b, v1.8b, v2.8b
  umax v0.4s, v1.4s, v2.4s
  umin v0.8h, v1.8h, v2.8h
  bif v0.16b, v1.16b, v2.16b
  bit v0.8b, v1.8b, v2.8b
  bsl v0.16b, v1.16b, v2.16b
  shrn v0.8b, v1.8h, 4
  shrn v0.4h, v1.4s, #16
  shrn v0.2s, v1.2d, #1
  shrn2 v0.16b, v1.8h, #8
  rshrn v0.8b, v1.8h, #3
  dup v0.16b, w1
  dup v0.8h, w2
  dup v0.4s, w3
  dup v0.2d, x4
  dup v0.8b, w5
  dup v0.4s, v1.s[2]
  dup v0.16b, v1.b[15]
  ld1 {v0.16b}, [x0]
  ld1 {v0.16b, v1.16b}, [x1]
  ld1 {v0.16b, v1.16b}, [x1], #32
  ld1 {v0.8b, v1.8b, v2.8b}, [x2], #24
  ld1 {v4.4s-v7.4s}, [x3], #64
  ld1 {v0.2d}, [x4], x5
  ld1 {v31.16b, v0.16b}, [sp]
  st1 {v0.16b}, [x0]
  st1 {v0.16b, v1.16b}, [x1], #32
  st1 {v2.8h, v3.8h, v4.8h, v5.8h}, [x6]
  ccmp x0, x1, 0, eq
  ccmp w0, w1, #4, ne
  ccmp x2, 0, 0, hs
  ccmp x3, #31, #15, hi
  ccmn x4, x5, #2, lt
  ccmn w6, #3, #0, ge
  mov x0, v1.d[0]
  mov x1, v2.d[1]
  fmov x2, d3
  ldr q0, [x1, 16]
  ldr q1, [x2, #32]!
  str q2, [x3], 32
  ldp q0, q1, [x1, -32]
  stp q2, q3, [sp, #64]!
  ret
