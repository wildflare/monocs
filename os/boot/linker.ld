/* カーネルのリンカスクリプト。ファームウェアが kernel8.img を 0x80000 に読み込む */

ENTRY(_start)

SECTIONS
{
    . = 0x80000;

    .text : {
        KEEP(*(.text.boot))
        *(.text .text.*)
    }

    .rodata : ALIGN(16) {
        *(.rodata .rodata.*)
    }

    /* TLS（_Thread_local の変数）の初期値。各コアの TLS は .tls_space に置き、os/kernel/tls.c が
       ここから写す。.tbss はアドレスを使わない（次の .data は .tbss の先頭から） */
    .tdata : ALIGN(16) {
        __tdata_start = .;
        *(.tdata .tdata.*)
        __tdata_end = .;
    }

    .tbss : ALIGN(16) {
        __tbss_start = .;
        *(.tbss .tbss.*)
        __tbss_end = .;
    }
    __tls_align = MAX(ALIGNOF(.tdata), ALIGNOF(.tbss));

    .data : ALIGN(16) {
        *(.data .data.*)
    }

    .bss (NOLOAD) : ALIGN(16) {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    }

    /* コア 0 のスタック（8MB。コンパイラとアプリも同じスタックを使う。
       Linux の既定値に合わせた。slimcc の test/countof.c は約 1.4MB の VLA を使う）。
       いちばん下の 64KB はガードページ（os/kernel/mmu.c）。あふれると例外になる。
       ガードページを飛び越える大きな局所変数（64KB 超の配列・VLA）は捕まえられない */
    .stack (NOLOAD) : ALIGN(16) {
        . += 0x800000;
        __stack_top = .;
    }

    /* コア 1〜3 のスタック（8MB ずつ。コア n（1〜3）は [__stack_smp_base + (n-1) × 8MB、
       __stack_smp_base + n × 8MB) を使う。boot.S の secondary_entry が上端を計算する） */
    .stack_smp (NOLOAD) : ALIGN(16) {
        __stack_smp_base = .;
        . += 3 * 0x800000;
    }

    /* コアごとの TLS（1MB ずつ。コア n の TP は __tls_space + n × 1MB。os/kernel/tls.c） */
    .tls_space (NOLOAD) : ALIGN(4096) {
        __tls_space = .;
        . += 4 * 0x100000;
    }

    /* ヒープ（memory.c の sbrk が、ここから ARM 用 RAM の終わりまでを使う） */
    . = ALIGN(4096);
    __heap_start = .;

    /DISCARD/ : {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame .eh_frame_hdr)
    }
}
