/****************************************************************************
 * vendor/artinchip/chips/d13x/artinchip_start.c
 *
 * D13x 启动入口
 * 地址来源: luban-lite aic_soc.h + D13x User Manual
 * XSPI 寄存器来源: luban-lite xspi_hw_v1.0.h
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/arch.h>
#include <nuttx/irq.h>
#include <nuttx/init.h>
#include <arch/board/board.h>
#include "chip.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* CMU 寄存器地址 (来源: D13x User Manual §8.1 CMU) */

#define CMU_PLL_IN          (D13X_CMU_BASE + 0x0A4)
#define CMU_PLL_COM         (D13X_CMU_BASE + 0x0A0)
#define CMU_PLL_INT0_GEN    (D13X_CMU_BASE + 0x000)
#define CMU_PLL_INT0_CFG    (D13X_CMU_BASE + 0x040)
#define CMU_PLL_INT1_GEN    (D13X_CMU_BASE + 0x004)
#define CMU_PLL_INT1_CFG    (D13X_CMU_BASE + 0x044)
#define CMU_CLK_CPU         (D13X_CMU_BASE + 0x200)
#define CMU_CLK_AXI_AHB     (D13X_CMU_BASE + 0x100)
#define CMU_CLK_APB0        (D13X_CMU_BASE + 0x120)
#define CMU_CLK_UART0       (D13X_CMU_BASE + 0x840)
#define CMU_CLK_GPIO        (D13X_CMU_BASE + 0x83C)

/* PLL_INTx_GEN 位域 */

#define PLL_GEN_FACTOR_N_SHIFT   8
#define PLL_GEN_FACTOR_N_MASK    (0xFF << PLL_GEN_FACTOR_N_SHIFT)
#define PLL_GEN_LOCK_BIT         (1 << 17)
#define PLL_GEN_ENABLE           (1 << 16)  /* PLL_EN bit16, 非 bit0 (FACTOR_P) */

/* CLK 位域 */

#define CLK_SRC_PLL_INT1         (1 << 8)
#define CLK_SRC_PLL_INT0         (1 << 8)

/* UART0 时钟位域 */

#define UART_CLK_MOD_RSTN        (1 << 13)
#define UART_CLK_MOD_BUS_EN      (1 << 12)
#define UART_CLK_MOD_CLK_EN      (1 << 8)

/* eFuse 寄存器 */

#define EFUSE_CMU_REG       (D13X_CMU_BASE + 0x904)
#define EFUSE_218_REG       (D13X_SID_BASE + 0x218)

/* WDOG 寄存器 (来源: D13x User Manual §11.2) */

#define WDOG_CTL            (D13X_WDT_BASE + 0x000)
#define WDOG_CNT            (D13X_WDT_BASE + 0x004)
#define WDOG_IRQ_EN         (D13X_WDT_BASE + 0x008)
#define WDOG_IRQ_STA        (D13X_WDT_BASE + 0x00C)

/* XSPI 控制器寄存器 (来源: luban-lite xspi_hw_v1.0.h) */

#define XSPI_CTL            (D13X_XSPI_BASE + 0x000)
#define XSPI_CLK            (D13X_XSPI_BASE + 0x004)
#define XSPI_TCR            (D13X_XSPI_BASE + 0x008)
#define XSPI_STAS           (D13X_XSPI_BASE + 0x00C)
#define XSPI_FCR            (D13X_XSPI_BASE + 0x028)
#define XSPI_START          (D13X_XSPI_BASE + 0x030)
#define XSPI_ADDR           (D13X_XSPI_BASE + 0x034)
#define XSPI_LCKCR          (D13X_XSPI_BASE + 0x054)
#define XSPI_LUT_UP         (D13X_XSPI_BASE + 0x058)
#define XSPI_CS0_DCTL       (D13X_XSPI_BASE + 0x014)
#define XSPI_CS1_DCTL       (D13X_XSPI_BASE + 0x01C)
#define XSPI_CS0_IOCFG1     (D13X_XSPI_BASE + 0x070)
#define XSPI_CS0_IOCFG2     (D13X_XSPI_BASE + 0x074)
#define XSPI_CS0_IOCFG3     (D13X_XSPI_BASE + 0x078)
#define XSPI_CS0_IOCFG4     (D13X_XSPI_BASE + 0x07C)
#define XSPI_CS1_IOCFG1     (D13X_XSPI_BASE + 0x080)
#define XSPI_CS1_IOCFG2     (D13X_XSPI_BASE + 0x084)
#define XSPI_CS1_IOCFG3     (D13X_XSPI_BASE + 0x088)
#define XSPI_CS1_IOCFG4     (D13X_XSPI_BASE + 0x08C)
#define XSPI_TDR            (D13X_XSPI_BASE + 0x200)
#define XSPI_RDR            (D13X_XSPI_BASE + 0x300)
#define XSPI_LUTN(n)        (D13X_XSPI_BASE + 0x100 + 0x4 * (n))

/* XSPI_CTL 位域 (来源: luban-lite xspi_hw_v1.0.h) */

#define CTL_XSPI_EN         (1 << 0)
#define CTL_XIP_EN          (1 << 2)
#define CTL_AXI_BURST       (1 << 3)
#define CTL_XSPI_MODE_SHIFT 4
#define CTL_XSPI_MODE_MASK  (0x3 << CTL_XSPI_MODE_SHIFT)
#define CTL_PARALLEL_MODE   (1 << 6)
#define CTL_TIMEOUT_EN      (1 << 7)
#define CTL_BOUNDARY_EN     (1 << 12)
#define CTL_BOUNDARY_CTL    (1 << 13)

/* XSPI 模式 */

#define XSPI_MODE_XCCELA    0
#define XSPI_MODE_HYPERBUS  1
#define XSPI_MODE_OPI       2
#define XSPI_MODE_SPI       3

/* XSPI_STAS 位域 */

#define STAS_BUSY            (1 << 0)

/* XSPI_FCR 位域 */

#define FCR_RF_RST           (1 << 15)
#define FCR_TF_RST           (1 << 31)

/* LUT 指令 (来源: luban-lite xspi_hw_v1.0.h) */

#define LUT_STOP             0x0
#define LUT_CMD              0x1
#define LUT_CMD_EX           0x2
#define LUT_ADDR             0x3
#define LUT_WRITE            0x4
#define LUT_READ             0x5
#define LUT_CMD_DDR          0x11
#define LUT_ADDR_DDR         0x13
#define LUT_WRITE_DDR        0x14
#define LUT_READ_DDR         0x15
#define LUT_DUMMY            0x10

/* LUT IO 模式 */

#define LUT_IO_1             0x0
#define LUT_IO_2             0x1
#define LUT_IO_4             0x2
#define LUT_IO_8             0x3

/* LUT 锁定 */

#define LUT_LOCK             0x1
#define LUT_UNLOCK           0x2

/* LUT 条目编码宏 */

#define LUT_ENTRY(ins_h, io_h, op_h, ins_l, io_l, op_l) \
  (((ins_h) << 26) | ((io_h) << 24) | ((op_h) << 16) | \
   ((ins_l) << 10) | ((io_l) << 8) | (op_l))

/* BSS 段符号 */

extern uint32_t _sbss[];
extern uint32_t _ebss[];
extern uint32_t _sdata[];
extern uint32_t _edata[];
extern uint32_t _eronly[];

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static inline void xspi_wait_idle(void)
{
  while (getreg32(XSPI_STAS) & STAS_BUSY);
}

static void d13x_clock_init(void)
{
  uint32_t reg;

  /* 1. 打开 24MHz 振荡器 */

  reg = getreg32(CMU_PLL_IN);
  reg |= (1 << 1);
  putreg32(reg, CMU_PLL_IN);

  /* 2. PLL 公共电路 LDO */

  reg = getreg32(CMU_PLL_COM);
  reg |= (1 << 31) | (1 << 0);
  putreg32(reg, CMU_PLL_COM);

  /* 3. 使能 PLL_INT1 → 1.2GHz (N=49) */

  reg = getreg32(CMU_PLL_INT1_GEN);
  reg |= PLL_GEN_ENABLE;
  putreg32(reg, CMU_PLL_INT1_GEN);
  while (!(getreg32(CMU_PLL_INT1_GEN) & PLL_GEN_LOCK_BIT));

  /* 4. 使能 PLL_INT0 → 480MHz (N=19) */

  reg = getreg32(CMU_PLL_INT0_GEN);
  reg &= ~PLL_GEN_FACTOR_N_MASK;
  reg |= (19 << PLL_GEN_FACTOR_N_SHIFT);
  reg |= PLL_GEN_ENABLE;
  putreg32(reg, CMU_PLL_INT0_GEN);
  while (!(getreg32(CMU_PLL_INT0_GEN) & PLL_GEN_LOCK_BIT));

  /* 5. AXI/AHB = 200MHz (PLL_INT1/(5+1)) */

  putreg32(CLK_SRC_PLL_INT1 | 5, CMU_CLK_AXI_AHB);

  /* 6. APB0 = 100MHz (PLL_INT1/(11+1)) */

  putreg32(CLK_SRC_PLL_INT1 | 11, CMU_CLK_APB0);

  /* 7. CPU = 480MHz (PLL_INT0/1) */

  putreg32(CLK_SRC_PLL_INT0 | 0, CMU_CLK_CPU);

  /* 8. GPIO 时钟 */

  reg = getreg32(CMU_CLK_GPIO);
  reg |= (1 << 13) | (1 << 12);
  putreg32(reg, CMU_CLK_GPIO);

  /* 9. UART0 = 60MHz (PLL_INT1/(19+1)), 含 MOD_BUS_EN */

  putreg32(UART_CLK_MOD_RSTN | UART_CLK_MOD_BUS_EN | UART_CLK_MOD_CLK_EN | 19,
           CMU_CLK_UART0);
}

static void d13x_uart_init(void)
{
  uint32_t divisor;

  UART_WAIT_IDLE(D13X_UART0_BASE);
  putreg32(0, D13X_UART0_BASE + UART_DLH_IER);
  putreg32(UART_LCR_DLAB, D13X_UART0_BASE + UART_LCR);
  divisor = D13X_UART0_FREQ / (16 * D13X_UART0_BAUDRATE);
  putreg32(divisor & 0xFF, D13X_UART0_BASE + UART_RBR_THR_DLL);
  putreg32((divisor >> 8) & 0xFF, D13X_UART0_BASE + UART_DLH_IER);
  putreg32(UART_FCR_FIFOE | UART_FCR_RFIFOR | UART_FCR_XFIFOR,
           D13X_UART0_BASE + UART_FCR_IIR);
  putreg32(0x03, D13X_UART0_BASE + UART_LCR);
}

/****************************************************************************
 * PSRAM 驱动 (LUT-based XSPI 控制器)
 * 来源: luban-lite xspi_hw_v1.0.h + hal_xspi.c + xspi_psram.c
 ****************************************************************************/

static uint32_t d13x_psram_read_efuse(uint32_t reg_addr)
{
  putreg32(0x1100, EFUSE_CMU_REG);
  uint32_t val = getreg32(reg_addr);
  putreg32(0x0, EFUSE_CMU_REG);
  return val;
}

static uint32_t d13x_psram_get_size(void)
{
  uint32_t fuse_218 = d13x_psram_read_efuse(EFUSE_218_REG);
  uint8_t mark_id = fuse_218 & 0xFF;

  switch (mark_id)
    {
      case 0x00: return 0;
      case 0x01: return 4 * 1024 * 1024;
      case 0x02: case 0x03: case 0x04: case 0x05:
      case 0xA1: case 0xA2:
        return 8 * 1024 * 1024;
      case 0x06: case 0x07:
        return 16 * 1024 * 1024;
      default: return 8 * 1024 * 1024;
    }
}

/* 通过 LUT 执行 PSRAM 命令 (同步 CPU 模式)
 * LUT 编码: 每个条目 32 位, 包含两阶段 (ins_h/io_h/op_h + ins_l/io_l/op_l)
 */

static void d13x_xspi_lut_write(int lut_id,
                                 int ins_l, int io_l, int op_l,
                                 int ins_h, int io_h, int op_h)
{
  uint32_t entry = LUT_ENTRY(ins_h, io_h, op_h, ins_l, io_l, op_l);
  putreg32(entry, XSPI_LUTN(lut_id));
}

static void d13x_xspi_lut_update(void)
{
  putreg32(1, XSPI_LUT_UP);
}

static void __attribute__((unused)) d13x_xspi_lut_lock(int lock)
{
  putreg32(lock ? LUT_LOCK : LUT_UNLOCK, XSPI_LCKCR);
}

static void d13x_xspi_trigger(int lut_id)
{
  xspi_wait_idle();
  putreg32(lut_id, XSPI_START);
  xspi_wait_idle();
}

/* 通过 LUT 执行 PSRAM 命令 */

static void d13x_psram_write_mr(uint8_t mr_addr, uint8_t mr_val)
{
  /* LUT[0]: CMD 0xC0 (WRITE_MR) on 1-line */
  d13x_xspi_lut_write(0, LUT_CMD, LUT_IO_1, 0xC0, LUT_STOP, 0, 0);
  /* LUT[1]: ADDR 1-byte on 1-line */
  d13x_xspi_lut_write(1, LUT_ADDR, LUT_IO_1, 1, LUT_STOP, 0, 0);
  /* LUT[2]: WRITE 1-byte on 1-line */
  d13x_xspi_lut_write(2, LUT_WRITE, LUT_IO_1, 1, LUT_STOP, 0, 0);
  d13x_xspi_lut_update();

  /* 设置地址 = MR 地址 */

  putreg32(mr_addr, XSPI_ADDR);

  /* 写入数据到 TDR */

  putreg32((uint32_t)mr_val, XSPI_TDR);

  /* 触发 LUT 序列: LUT[0] → LUT[1] → LUT[2] → STOP */

  d13x_xspi_trigger(0);
}

static void d13x_psram_read_mr(uint8_t mr_addr, uint8_t *mr_val)
{
  /* LUT[0]: CMD 0x40 (READ_MR) on 1-line */
  d13x_xspi_lut_write(0, LUT_CMD, LUT_IO_1, 0x40, LUT_STOP, 0, 0);
  /* LUT[1]: ADDR 1-byte on 1-line */
  d13x_xspi_lut_write(1, LUT_ADDR, LUT_IO_1, 1, LUT_STOP, 0, 0);
  /* LUT[2]: READ 1-byte on 1-line */
  d13x_xspi_lut_write(2, LUT_READ, LUT_IO_1, 1, LUT_STOP, 0, 0);
  d13x_xspi_lut_update();

  putreg32(mr_addr, XSPI_ADDR);
  d13x_xspi_trigger(0);

  *mr_val = (uint8_t)getreg32(XSPI_RDR);
}

static void d13x_psram_device_reset(void)
{
  /* LUT[0]: CMD 0xFF (RESET) on 1-line */

  d13x_xspi_lut_write(0, LUT_CMD, LUT_IO_1, 0xFF, LUT_STOP, 0, 0);
  d13x_xspi_lut_update();
  d13x_xspi_trigger(0);
}

static void d13x_psram_device_init(void)
{
  uint8_t mr4;

  /* 读取 MR4 检查当前配置 */

  d13x_psram_read_mr(0x04, &mr4);

  /* 配置 MR0: Burst Length = 32, Latency = 3 */

  d13x_psram_write_mr(0x00, 0x19);

  /* 配置 MR4: 使能 XIP */

  d13x_psram_write_mr(0x04, 0x80);
}

static void d13x_psram_xip_enable(void)
{
  uint32_t ctl;

  /* 配置 XIP LUT (OPI 模式下的 XIP 读/写序列)
   * LUT[16]: XIP READ (OPI 8-line, DDR)
   * LUT[0]:  XIP WRITE (OPI 8-line, DDR)
   * 这里使用简化配置: 启用 XIP 位, 由硬件处理 LUT 路由
   */

  /* 使能 XIP (CTL bit2) */

  xspi_wait_idle();
  ctl = getreg32(XSPI_CTL);
  ctl |= CTL_XIP_EN;
  putreg32(ctl, XSPI_CTL);
}

static uint32_t d13x_psram_verify(void)
{
  volatile uint32_t *psram = (volatile uint32_t *)D13X_PSRAM_BASE;
  uint32_t psram_size = d13x_psram_get_size();

  if (psram_size == 0)
    return 0;

  psram[0] = 0x12345678;
  psram[1] = 0x9ABCDEF0;

  if (psram[0] != 0x12345678)
    return 0;
  if (psram[1] != 0x9ABCDEF0)
    return 0;

  psram[0] = 0;
  psram[1] = 0;
  return psram_size;
}

static void d13x_psram_init(void)
{
  uint32_t reg;

  /* 首先验证 PSRAM 是否已由 bootloader 初始化 */

  if (d13x_psram_verify() > 0)
    return;

  /* 1. 使能 XSPI 时钟 (CMU_BASE + 0x45C) */

  reg = getreg32(D13X_CMU_BASE + 0x45C);
  reg |= (1 << 13) | (1 << 12) | (1 << 8);
  putreg32(reg, D13X_CMU_BASE + 0x45C);

  /* 2. 复位 XSPI 控制器 (写 0 到 CTL) */

  putreg32(0, XSPI_CTL);

  /* 3. 配置 XSPI 控制器 (来源: luban-lite hal_xspi_init) */

  /* 时钟分频: div=0 → 最高频率 */

  putreg32(0, XSPI_CLK);

  /* TCR: 写保持=8, 读保持=2 */

  xspi_wait_idle();
  putreg32((8 << 16) | (2 << 20), XSPI_TCR);

  /* XSPI 模式 = OPI (bits[5:4] = 2) */

  xspi_wait_idle();
  reg = getreg32(XSPI_CTL);
  reg |= (XSPI_MODE_OPI << CTL_XSPI_MODE_SHIFT);
  putreg32(reg, XSPI_CTL);

  /* 使能 wrap burst split */

  reg = getreg32(XSPI_CTL);
  reg |= CTL_AXI_BURST;
  putreg32(reg, XSPI_CTL);

  /* Boundary = 1K */

  reg = getreg32(XSPI_CTL);
  reg |= CTL_BOUNDARY_EN | CTL_BOUNDARY_CTL;
  putreg32(reg, XSPI_CTL);

  /* IO 驱动配置 */

  putreg32(0x36363636, XSPI_CS0_IOCFG1);
  putreg32(0x36363636, XSPI_CS0_IOCFG2);
  putreg32(0x36363636, XSPI_CS0_IOCFG3);
  putreg32(0x37, XSPI_CS0_IOCFG4);
  putreg32(0x36363636, XSPI_CS1_IOCFG1);
  putreg32(0x36363636, XSPI_CS1_IOCFG2);
  putreg32(0x36363636, XSPI_CS1_IOCFG3);
  putreg32(0x37, XSPI_CS1_IOCFG4);

  /* DLL 配置 (来源: luban-lite xspi_hw_set_dll_ctl) */

  /* CS0 DLL: ICP=150-200MHz, Phase=90度 */

  reg = getreg32(XSPI_CS0_DCTL);
  reg |= (2 << 12) | (3 << 8) | (1 << 4) | (1 << 5);  /* ICP_150_200M, phase_90b, LDO, LVS */
  putreg32(reg, XSPI_CS0_DCTL);

  /* 使能 DLL/VCDL/CP, 禁用 BYPASS */

  reg |= (1 << 0) | (1 << 1) | (1 << 2);  /* EN_DLL, EN_VCDL, EN_CP */
  reg &= ~(1 << 3);  /* EN_BYPASS = 0 */
  putreg32(reg, XSPI_CS0_DCTL);

  /* CS1 DLL: 同上 */

  reg = getreg32(XSPI_CS1_DCTL);
  reg |= (2 << 12) | (3 << 8) | (1 << 4) | (1 << 5);
  putreg32(reg, XSPI_CS1_DCTL);
  reg |= (1 << 0) | (1 << 1) | (1 << 2);
  reg &= ~(1 << 3);
  putreg32(reg, XSPI_CS1_DCTL);

  /* 4. 使能 XSPI 模块 */

  xspi_wait_idle();
  reg = getreg32(XSPI_CTL);
  reg |= CTL_XSPI_EN;
  putreg32(reg, XSPI_CTL);

  /* 5. 复位 PSRAM 设备 */

  d13x_psram_device_reset();

  /* 6. 配置 PSRAM 模式寄存器 */

  d13x_psram_device_init();

  /* 7. 使能 XIP 模式 */

  d13x_psram_xip_enable();

  /* 8. 验证 */

  d13x_psram_verify();
}

/****************************************************************************
 * 看门狗处理
 ****************************************************************************/

static void d13x_watchdog_init(void)
{
  putreg32(0, WDOG_CTL);
}

/* Raw UART trace used before the serial and syslog subsystems exist. */

void d13x_boot_trace(char ch)
{
  putreg32((uint32_t)ch, D13X_UART0_BASE + UART_RBR_THR_DLL);
}

void d13x_boot_trace_hex(uint32_t value)
{
  static const char hex[] = "0123456789abcdef";
  int shift;

  for (shift = 28; shift >= 0; shift -= 4)
    {
      d13x_boot_trace(hex[(value >> shift) & 0xf]);
    }
}

/****************************************************************************
 * C 入口 (由 d13x_head.S 调用)
 ****************************************************************************/

void __start_c(void)
{
  /* PBP/tinySPL has already initialized clocks, PSRAM, QSPI and UART before
   * loading this SRAM image.  Reinitializing those live blocks here can hang
   * on a PLL lock wait or invalidate the PSRAM configuration.
   */

  d13x_watchdog_init();

  nx_start();

  for (; ; );
}
