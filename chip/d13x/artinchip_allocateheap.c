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

/* The application and IDLE stack execute from PSRAM.  SRAM is therefore the
 * primary heap, while the unused PSRAM tail is added as a second region.
 */

extern uint32_t _eidle_stack[];
#define PSRAM_HEAP_BASE \
  (((uintptr_t)_eidle_stack + 15u) & ~(uintptr_t)15u)
#define PSRAM_END       (D13X_PSRAM_BASE + D13X_PSRAM_SIZE)

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
  *heap_start = (void *)D13X_SRAM_BASE;
  *heap_size = D13X_SRAM_USABLE_SIZE;
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
  if (PSRAM_HEAP_BASE < PSRAM_END)
    {
      umm_addregion((void *)PSRAM_HEAP_BASE,
                    PSRAM_END - PSRAM_HEAP_BASE);
    }
}
#endif
