/****************************************************************************
 * vendor/artinchip/chips/d13x/artinchip_oneshot.c
 *
 * D13x 定时器驱动（arch_alarm 模型）
 * GTC 寄存器来源: D13x User Manual §11.1 + luban-lite
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/timers/oneshot.h>
#include <nuttx/timers/arch_alarm.h>
#include <nuttx/irq.h>
#include <nuttx/arch.h>
#include "chip.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* GTC 寄存器偏移 (来源: D13x User Manual §11.1.3 表 11-1) */

#define GTC_CNTCR                (D13X_GTC_BASE + 0x0000)  /* 计数器控制 */
#define GTC_CNTSR                (D13X_GTC_BASE + 0x0004)  /* 计数器状态 */
#define GTC_CNTVL                (D13X_GTC_BASE + 0x0008)  /* 计数值低位 [31:0] */
#define GTC_CNTVH                (D13X_GTC_BASE + 0x000C)  /* 计数值高位 [51:32] */
#define GTC_CNTFID0              (D13X_GTC_BASE + 0x0020)  /* 计数频率 ID0 = 4MHz */
#define GTC_CNTFID1              (D13X_GTC_BASE + 0x0024)  /* 计数频率 ID1 = 1MHz */
#define GTC_CNTFID2              (D13X_GTC_BASE + 0x0028)  /* 计数频率 ID2 = 250KHz */
#define GTC_CONFG                (D13X_GTC_BASE + 0x00C0)  /* 配置: FDIV 分频 */
#define CMU_CLK_GTC              (D13X_CMU_BASE + 0x090c)

/* E907 core timer.  GTC supplies its 4 MHz time base; the interrupt compare
 * registers are part of CORET, not the GTC counter block.
 */

#define CORET_MTIMECMP_LO        (D13X_E907_CLINT_BASE + 0x4000)
#define CORET_MTIMECMP_HI        (D13X_E907_CLINT_BASE + 0x4004)
#define CORET_MTIME_LO           (D13X_E907_CLINT_BASE + 0xbff8)
#define CORET_MTIME_HI           (D13X_E907_CLINT_BASE + 0xbffc)

/* GTC_CNTCR 位定义 */

#define GTC_CNTCR_EN             (1 << 0)   /* 计数器使能 */
#define GTC_CNTCR_HDBG           (1 << 1)   /* 调试时保持 */

/* GTC 中断号: GTC 通过 CLIC 直接产生中断，IRQ 号取决于硬件连线
 * luban-lite 使用 CORET (IRQ 7) 作为系统 tick，GTC 仅用于使能计数
 * NuttX arch_alarm 模型需要一个 IRQ，使用 CORET_IRQn (7) 或
 * 通过 oneshot 软件比较实现。此处使用 CORET_IRQn 作为中断源。
 */

#define D13X_IRQ_GTC             RISCV_IRQ_MTIMER /* raw CORET_IRQn is 7 */

/* GTC 时钟频率: 基频 4MHz (D13x User Manual §11.1.1)
 * PCLK 可选 12/24/48/60 MHz，GTC 内部分频到 4MHz 基频
 * luban-lite system.c 中: *(volatile uint32_t *)GTC_BASE = 0x0001 (使能)
 * 计数频率由 GTC_CONFG 的 FDIV 字段控制
 */

#define GTC_BASE_FREQ            4000000    /* 4MHz 基频 */

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct d13x_oneshot_lowerhalf_s
{
  struct oneshot_lowerhalf_s lower;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct d13x_oneshot_lowerhalf_s g_d13x_oneshot;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: d13x_oneshot_current
 *
 * Description:
 *   Get the current timer value (clkcnt_t API)
 *
 ****************************************************************************/

static uint64_t d13x_coret_current(void)
{
  uint32_t high1, low, high2;

  do
    {
      high1 = getreg32(CORET_MTIME_HI);
      low   = getreg32(CORET_MTIME_LO);
      high2 = getreg32(CORET_MTIME_HI);
    }
  while (high1 != high2);

  return ((uint64_t)high1 << 32) | low;
}

static void d13x_coret_set_compare(uint64_t value)
{
  irqstate_t flags = up_irq_save();

  /* Avoid a transient compare match while updating a 64-bit register on
   * RV32, following the RISC-V privileged specification sequence.
   */

  putreg32(UINT32_MAX, CORET_MTIMECMP_HI);
  putreg32((uint32_t)value, CORET_MTIMECMP_LO);
  putreg32((uint32_t)(value >> 32), CORET_MTIMECMP_HI);
  up_irq_restore(flags);
}

static clkcnt_t d13x_oneshot_current(struct oneshot_lowerhalf_s *lower)
{
  return (clkcnt_t)d13x_coret_current();
}

/****************************************************************************
 * Name: d13x_oneshot_start
 *
 * Description:
 *   Start the oneshot timer with a relative delay (clkcnt_t API)
 *
 ****************************************************************************/

static void d13x_oneshot_start(struct oneshot_lowerhalf_s *lower,
                               clkcnt_t delay)
{
  d13x_coret_set_compare(d13x_coret_current() + (uint64_t)delay);
}

/****************************************************************************
 * Name: d13x_oneshot_start_absolute
 *
 * Description:
 *   Start the oneshot timer with an absolute counter value (clkcnt_t API)
 *
 ****************************************************************************/

static void d13x_oneshot_start_absolute(struct oneshot_lowerhalf_s *lower,
                                        clkcnt_t cnt)
{
  d13x_coret_set_compare((uint64_t)cnt);
}

/****************************************************************************
 * Name: d13x_oneshot_cancel
 *
 * Description:
 *   Cancel the oneshot timer (clkcnt_t API)
 *
 ****************************************************************************/

static void d13x_oneshot_cancel(struct oneshot_lowerhalf_s *lower)
{
  d13x_coret_set_compare(UINT64_MAX);
}

/****************************************************************************
 * Name: d13x_oneshot_maxdelay
 *
 * Description:
 *   Get the maximum delay value (clkcnt_t API)
 *
 ****************************************************************************/

static clkcnt_t d13x_oneshot_maxdelay(struct oneshot_lowerhalf_s *lower)
{
  return (clkcnt_t)UINT64_MAX;
}

/****************************************************************************
 * Name: d13x_oneshot_handler
 *
 * Description:
 *   Timer interrupt handler (CORET_IRQn)
 *
 ****************************************************************************/

static int d13x_oneshot_handler(int irq, FAR void *context, FAR void *arg)
{
  struct oneshot_lowerhalf_s *lower = arg;

  /* Mask this one-shot until the upper half programs its next deadline. */

  d13x_coret_set_compare(UINT64_MAX);

  if (lower->callback)
    {
      lower->callback(lower, lower->arg);
    }

  return OK;
}

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct oneshot_operations_s g_d13x_oneshot_ops =
{
  .current        = d13x_oneshot_current,
  .start          = d13x_oneshot_start,
  .start_absolute = d13x_oneshot_start_absolute,
  .cancel         = d13x_oneshot_cancel,
  .max_delay      = d13x_oneshot_maxdelay,
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: up_timer_initialize
 *
 * Description:
 *   This function is called during start-up to initialize the timer
 *   interrupt.
 *
 ****************************************************************************/

void up_timer_initialize(void)
{
  /* Match luban-lite aic_gtc_enable(): release reset, enable the GTC clock,
   * then start the 4 MHz global counter feeding E907 CORET.
   */

  putreg32(0x3100, CMU_CLK_GTC);
  putreg32(GTC_CNTCR_EN, GTC_CNTCR);

  g_d13x_oneshot.lower.ops = &g_d13x_oneshot_ops;
  oneshot_count_init(&g_d13x_oneshot.lower, GTC_BASE_FREQ);

  d13x_coret_set_compare(UINT64_MAX);

  irq_attach(D13X_IRQ_GTC, d13x_oneshot_handler, &g_d13x_oneshot.lower);

  /* The alarm upper half reads MTIME and programs the first deadline. */

  up_alarm_set_lowerhalf(&g_d13x_oneshot.lower);

  /* Keep raw CORET IRQ 7 masked.  The E907 CLIC return path currently
   * restores a bootloader-era 0x010xxxxx stack pointer after this IRQ,
   * corrupting the running NuttX task.  The free-running counter remains
   * available, while serial polling and synchronous task wakeups continue
   * to work without the unsafe interrupt.
   */

  up_disable_irq(D13X_IRQ_GTC);
}
