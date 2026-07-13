/****************************************************************************
 * vendor/artinchip/chips/d13x/artinchip_allocateheap.c
 *
 * D13x 堆内存初始化
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/arch.h>
#include <nuttx/mm/mm.h>
#include "chip.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* .data 段之后是 .bss 段，.bss 段之后是 IDLE 栈，堆从 IDLE 栈之后开始 */

extern uint32_t _ebss[];
#define HEAP_BASE      ((uintptr_t)_ebss + CONFIG_IDLETHREAD_STACKSIZE)

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: up_allocate_heap
 *
 * Description:
 *   This function will be called to dynamically set aside the heap region.
 *
 ****************************************************************************/

void up_allocate_heap(void **heap_start, size_t *heap_size)
{
  /* 堆起始地址: _ebss + IDLE 栈大小 */

  *heap_start = (void *)HEAP_BASE;

  /* 堆大小: SRAM 结束地址 - 堆起始地址 */

  /* Luban-lite reserves the final 0x100 bytes of D13x SRAM_S0. */

  *heap_size = D13X_SRAM_BASE + D13X_SRAM_USABLE_SIZE - HEAP_BASE;
}

/****************************************************************************
 * Name: riscv_addregion
 *
 * Description:
 *   RAM may be added in non-contiguous chunks. This routine adds all chunks
 *   that may be used for heap.
 *
 ****************************************************************************/

#if CONFIG_MM_REGIONS > 1
void riscv_addregion(void)
{
  /* 添加 PSRAM 作为额外堆区域 */

  umm_addregion((void *)D13X_PSRAM_BASE, D13X_PSRAM_SIZE);
}
#endif
