/****************************************************************************
 * vendor/artinchip/chips/d13x/d13x_head.S
 *
 * D13x RISC-V 启动汇编 (来源: NuttX rv32m1_head.S 模式 + luban-lite startup_gcc.S)
 * 职责: GP/SP 初始化, 中断禁用, BSS 清零, 跳转 C 入口
 *
 ****************************************************************************/

#include <nuttx/config.h>
#include <arch/irq.h>
#include "chip.h"

  .global __start
  .global _sbss
  .global _ebss
  .global _sdata
  .global _edata
  .global _eronly

  .section .text.vectors

/****************************************************************************
 * __start - 复位入口点
 ****************************************************************************/

__start:

  /* 禁用全局中断 */

  csrci mstatus, 0x8
  csrw mie, zero

  /* 初始化 GP 寄存器 (来源: luban-lite startup_gcc.S) */

  .option push
  .option norelax
  la    gp, __global_pointer$
  .option pop

#ifdef CONFIG_ARCH_FPU
  li    t0, 0x00006000
  csrs  mstatus, t0
  fscsr zero
#endif

  /* 清零 BSS 段 */

  la    t0, _sbss
  la    t1, _ebss
.Lclear_bss:
  bgeu  t0, t1, .Lbss_done
  sw    zero, 0(t0)
  addi  t0, t0, 4
  j     .Lclear_bss
.Lbss_done:

  /* ITB loads the complete flat binary at its linked SRAM address, so .data
   * is already initialized.  Put the IDLE stack immediately after BSS.
   */

  la    sp, _ebss
  li    t0, CONFIG_IDLETHREAD_STACKSIZE
  add   sp, sp, t0
  andi  sp, sp, -16

  /* 跳转到 NuttX C 入口 (artinchip_start.c 中的 __start_c) */

  jal   x1, __start_c

  /* 不应到达此处 */

.Lhang:
  wfi
  j     .Lhang
