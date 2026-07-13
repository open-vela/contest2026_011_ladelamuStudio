/****************************************************************************
 * vendor/artinchip/chips/d13x/artinchip_irq.c
 *
 * D13x 中断系统适配（CLIC 中断控制器）
 * CLIC 寄存器来源: luban-lite core_rv32.h CLIC_Type 结构体
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/irq.h>
#include <nuttx/arch.h>
#include "riscv_internal.h"
#include "chip.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* CLIC 寄存器结构 (来源: luban-lite core_rv32.h CLIC_Type)
 *
 * CLIC 基地址: 0x20800000
 * 每个中断源占 4 字节: IP(1B), IE(1B), ATTR(1B), CTL(1B)
 * CLICINT[] 数组起始偏移: 0x1000
 *
 * struct CLIC_Type {
 *   uint32_t CLICCFG;      // offset 0x000
 *   uint32_t CLICINFO;     // offset 0x004
 *   uint32_t MINTTHRESH;   // offset 0x008
 *   uint32_t RESERVED[1021];
 *   CLIC_INT_Control CLICINT[4096];  // offset 0x1000
 * };
 *
 * struct CLIC_INT_Control {
 *   uint8_t IP;    // offset +0x000 (pending)
 *   uint8_t IE;    // offset +0x001 (enable)
 *   uint8_t ATTR;  // offset +0x002 (attribute: vector/shv)
 *   uint8_t CTL;   // offset +0x003 (control: priority)
 * };
 */

#define D13X_CLIC_BASE         D13X_E907_CLIC_BASE   /* 0x20800000 */
#define D13X_CLIC_CFG          (D13X_CLIC_BASE + 0x0000)
#define D13X_CLIC_INFO         (D13X_CLIC_BASE + 0x0004)
#define D13X_CLIC_MINTTHRESH   (D13X_CLIC_BASE + 0x0008)
#define D13X_CLIC_INT_BASE     (D13X_CLIC_BASE + 0x1000)  /* CLICINT[] 起始 */

/* T-Head E907 implementation-specific CSRs.  tinySPL enables the hardware
 * stack push/swap extensions, while NuttX exception_common builds and
 * restores its own software context frame.  They must not be active at the
 * same time.
 */

#define D13X_CSR_MXSTATUS      0x7c0
#define D13X_CSR_MEXSTATUS     0x7e1
#define D13X_MXSTATUS_MM       (1u << 15)
#define D13X_MXSTATUS_ISAEE    (1u << 22)
#define D13X_MEXSTATUS_SPUSHEN (1u << 16)
#define D13X_MEXSTATUS_SPSWAPEN (1u << 17)

/* 中断向量表地址（RISC-V 使用 __trap_vec） */

extern uint32_t __trap_vec[];
extern void riscv_exception_attach(void);
extern void d13x_boot_trace(char ch);
extern void d13x_boot_trace_hex(uint32_t value);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: d13x_clic_set_irq
 *
 * Description:
 *   设置指定中断的 IE/IP/ATTR 寄存器
 *
 ****************************************************************************/

static inline void d13x_clic_enable_irq(int irq)
{
  /* CLICINT[irq].IE = 1 */

  volatile uint8_t *ie = (volatile uint8_t *)(D13X_CLIC_INT_BASE + irq * 4 + 1);
  *ie = 1;
}

static inline void d13x_clic_disable_irq(int irq)
{
  /* CLICINT[irq].IE = 0 */

  volatile uint8_t *ie = (volatile uint8_t *)(D13X_CLIC_INT_BASE + irq * 4 + 1);
  *ie = 0;
}

static inline void d13x_clic_clear_pending(int irq)
{
  /* CLICINT[irq].IP = 0 (W1C) */

  volatile uint8_t *ip = (volatile uint8_t *)(D13X_CLIC_INT_BASE + irq * 4 + 0);
  *ip = 0;
}

static inline void d13x_clic_set_attr(int irq, uint8_t attr)
{
  /* CLICINT[irq].ATTR = attr */

  volatile uint8_t *attrp = (volatile uint8_t *)(D13X_CLIC_INT_BASE + irq * 4 + 2);
  *attrp = attr;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: up_irqinitialize
 *
 * Description:
 *   Initialize the interrupt subsystem
 *
 ****************************************************************************/

void up_irqinitialize(void)
{
  int i;
  uint32_t mxstatus;
  uint32_t mexstatus;

  /* Match the proven D12x NuttX port before installing the NuttX trap
   * entry.  In particular, undo the automatic E907 stack operations left
   * enabled by the Luban bootloader; otherwise an interrupt can restore a
   * hardware-modified SP into a NuttX software context.
   */

  __asm__ __volatile__("csrci mstatus, 0x8");
  __asm__ __volatile__("csrw mie, zero");
  __asm__ __volatile__("csrr %0, 0x7c0" : "=r"(mxstatus));
  mxstatus |= D13X_MXSTATUS_MM | D13X_MXSTATUS_ISAEE;
  __asm__ __volatile__("csrw 0x7c0, %0" :: "r"(mxstatus));

  __asm__ __volatile__("csrr %0, 0x7e1" : "=r"(mexstatus));
  mexstatus &= ~(D13X_MEXSTATUS_SPUSHEN | D13X_MEXSTATUS_SPSWAPEN);
  __asm__ __volatile__("csrw 0x7e1, %0" :: "r"(mexstatus));

  /* Configure the common CLIC trap entry and the E907 vector-table CSR. */

  /* Every CLIC source below uses ATTR.SHV=0 and therefore enters through
   * one common NuttX handler.  E907 CLIC mode 2 is the non-vectored mode;
   * mode 3 is reserved for the Luban hardware-vector entry model and keeps
   * using its bootloader stack state.
   */

  uint32_t mtvec = (uint32_t)__trap_vec | 0x2;
  __asm__ __volatile__("csrw mtvec, %0" :: "r"(mtvec));

  /* mtvt is CSR 0x307.  CSR 0x7c0 is T-Head MXSTATUS. */

  uint32_t mtvt = (uint32_t)__trap_vec;
  __asm__ __volatile__("csrw 0x307, %0" :: "r"(mtvt));

  riscv_exception_attach();

  /* 初始化 CLIC 中断控制器 (来源: luban-lite system.c SystemInit) */

  /* 读取 CLIC 配置信息 */

  volatile uint32_t *cliccfg = (volatile uint32_t *)D13X_CLIC_CFG;
  volatile uint32_t *clicinfo = (volatile uint32_t *)D13X_CLIC_INFO;

  uint32_t info = *clicinfo;
  uint32_t nlbits = (info >> 21) & 0xF;

  *cliccfg = (nlbits << 1) & 0x1E;

  /* 初始化所有中断: 清除 pending, 使能向量中断 */

  for (i = 0; i < MAX_IRQn; i++)
    {
      d13x_clic_disable_irq(i);
      d13x_clic_clear_pending(i);
      /* NuttX provides one common __trap_vec entry, not a hardware vector
       * table.  Keep SHV clear so all IRQs use the common entry.
       */

      d13x_clic_set_attr(i, 0);
    }

  /* tspend 使用正向中断 (来源: luban-lite system.c) */

  d13x_clic_set_attr(Machine_Software_IRQn, 0x2);

  /* 使能全局中断 */

  up_irq_enable();
}

/****************************************************************************
 * Name: up_enable_irq
 *
 * Description:
 *   Enable the interrupt specified by 'irq'
 *
 ****************************************************************************/

void up_enable_irq(int irq)
{
  int raw_irq = irq - RISCV_IRQ_ASYNC;

  if ((unsigned int)raw_irq < MAX_IRQn)
    {
      d13x_clic_enable_irq(raw_irq);
    }
}

/****************************************************************************
 * Name: up_disable_irq
 *
 * Description:
 *   Disable the interrupt specified by 'irq'
 *
 ****************************************************************************/

void up_disable_irq(int irq)
{
  int raw_irq = irq - RISCV_IRQ_ASYNC;

  if ((unsigned int)raw_irq < MAX_IRQn)
    {
      d13x_clic_disable_irq(raw_irq);
    }
}

/****************************************************************************
 * Name: riscv_dispatch_irq
 *
 * Description:
 *   Process interrupt and dispatch to registered handler
 *
 ****************************************************************************/

void *riscv_dispatch_irq(uint32_t mcause, uint32_t *regs)
{
  int irq = (int)(mcause & 0x3ff);

  /* Syslog is not available during early boot.  Report synchronous traps
   * directly so their cause and faulting instruction are still observable.
   */

  if ((mcause & RISCV_IRQ_BIT) == 0 && irq != RISCV_IRQ_ECALLM)
    {
      uint32_t mepc;
      uint32_t mtval;

      __asm__ __volatile__("csrr %0, mepc" : "=r"(mepc));
      __asm__ __volatile__("csrr %0, mtval" : "=r"(mtval));
      d13x_boot_trace('[');
      d13x_boot_trace_hex(mcause);
      d13x_boot_trace(':');
      d13x_boot_trace_hex(mepc);
      d13x_boot_trace(':');
      d13x_boot_trace_hex(mtval);
      d13x_boot_trace(']');
    }

  /* 读取中断号 */

  /* E907 CLIC stores previous privilege/status bits in mcause[30:28].
   * The architectural interrupt/exception number is the low 10 bits.
   */

  if ((mcause & RISCV_IRQ_BIT) != 0)
    {
      irq += RISCV_IRQ_ASYNC;
    }

  /* 调用中断处理函数 */

  return riscv_doirq(irq, regs);
}

/****************************************************************************
 * Name: up_irq_enable
 *
 * Description:
 *   Enable all interrupts
 *
 ****************************************************************************/

irqstate_t up_irq_enable(void)
{
  irqstate_t flags;
  uint32_t meie = 1u << 11;

  /* 读取当前中断状态 */

  __asm__ __volatile__("csrr %0, mstatus" : "=r"(flags));

  /* D12x/D13x CLIC peripheral interrupts are gated by mie.MEIE in
   * addition to the global mstatus.MIE bit.
   */

  __asm__ __volatile__("csrs mie, %0" :: "r"(meie));

  /* 使能全局中断 */

  __asm__ __volatile__("csrsi mstatus, 0x8");

  return flags;
}
