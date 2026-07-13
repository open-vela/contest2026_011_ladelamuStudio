    .section .text.startup
    .global _start

_start:
    /* 设置栈指针 */
    la sp, _estack
    andi sp, sp, ~0x7

    /* 清零 BSS 段 */
    la t0, _sbss
    la t1, _ebss
.Lclear_bss:
    bgeu t0, t1, .Lbss_done
    sw zero, 0(t0)
    addi t0, t0, 4
    j .Lclear_bss
.Lbss_done:

    /* 跳转到 main */
    call main

.Lhang:
    wfi
    j .Lhang

    .section .bss
    .align 4
_estack:
    .space 4096
