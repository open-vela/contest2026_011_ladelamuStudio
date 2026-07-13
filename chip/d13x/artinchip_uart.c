/****************************************************************************
 * vendor/artinchip/chips/d13x/artinchip_uart.c
 *
 * D13x 完整串口驱动
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/arch.h>
#include <nuttx/irq.h>
#include <nuttx/serial/serial.h>
#include <arch/irq.h>
#include "riscv_internal.h"
#include "chip.h"

#define D13X_GPIO_PA0_CFG          (D13X_GPIO_BASE + 0x80)
#define D13X_GPIO_PA1_CFG          (D13X_GPIO_BASE + 0x84)
#define D13X_GPIO_PINMUX_MASK      0x0004037f
#define D13X_GPIO_UART0_TX_CFG     0x00000035
#define D13X_GPIO_UART0_RX_CFG     0x00000035
#define D13X_UART_MCR_FUNC_MASK    0x000000c0

#ifdef CONFIG_D13X_UART0_DIAGNOSTIC
#  define D13X_GPIO_PA_INPUT       (D13X_GPIO_BASE + 0x00)
#  define D13X_GPIO_SPE_IE_FORCE   (1 << 18)
#  define D13X_CMU_UART0_CFG       (D13X_CMU_BASE + 0x840)
#  define D13X_CLIC_UART0_ENTRY    (D13X_E907_CLIC_BASE + 0x1000 + 76 * 4)
#  define D13X_UART_DIAG_TIMEOUT   1000000
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

#ifdef CONFIG_D13X_UART0_DIAGNOSTIC
static bool g_d13x_diag_idle_reported;
static bool g_d13x_diag_saw_low;
static bool g_d13x_diag_saw_rx_hw;
static uint32_t g_d13x_diag_gpio_last;
static uint32_t g_d13x_diag_edges;
static uint32_t g_d13x_diag_rx_count;
static uint32_t g_d13x_diag_last_rx;
static uint32_t g_d13x_diag_loopback;

static void d13x_uart_diag_putc(char ch)
{
  int timeout;

  for (timeout = 0; timeout < D13X_UART_DIAG_TIMEOUT; timeout++)
    {
      if ((getreg32(D13X_UART0_BASE + UART_LSR) & UART_LSR_THRE) != 0)
        {
          putreg32((uint32_t)ch, D13X_UART0_BASE + UART_RBR_THR_DLL);
          return;
        }
    }
}

static void d13x_uart_diag_puts(FAR const char *str)
{
  while (*str != '\0')
    {
      d13x_uart_diag_putc(*str++);
    }
}

static void d13x_uart_diag_hex(uint32_t value)
{
  static const char hex[] = "0123456789abcdef";
  int shift;

  for (shift = 28; shift >= 0; shift -= 4)
    {
      d13x_uart_diag_putc(hex[(value >> shift) & 0x0f]);
    }
}

static uint32_t d13x_uart_diag_read_mie(void)
{
  uint32_t value;

  __asm__ __volatile__("csrr %0, mie" : "=r"(value));
  return value;
}

static uint32_t d13x_uart_diag_read_mstatus(void)
{
  uint32_t value;

  __asm__ __volatile__("csrr %0, mstatus" : "=r"(value));
  return value;
}

static void d13x_uart_diag_field(FAR const char *name, uint32_t value)
{
  d13x_uart_diag_putc(' ');
  d13x_uart_diag_puts(name);
  d13x_uart_diag_putc('=');
  d13x_uart_diag_hex(value);
}

static void d13x_uart_diag_report(FAR const char *tag)
{
  d13x_uart_diag_puts("\r\n[D13RX:");
  d13x_uart_diag_puts(tag);
  d13x_uart_diag_putc(']');
  d13x_uart_diag_field("LB", g_d13x_diag_loopback);
  d13x_uart_diag_field("CFG", getreg32(D13X_GPIO_PA1_CFG));
  d13x_uart_diag_field("GPIO", getreg32(D13X_GPIO_PA_INPUT));
  d13x_uart_diag_field("EDGE", g_d13x_diag_edges);
  d13x_uart_diag_field("LOW", g_d13x_diag_saw_low ? 1 : 0);
  d13x_uart_diag_field("LSR", getreg32(D13X_UART0_BASE + UART_LSR));
  d13x_uart_diag_field("USR", getreg32(D13X_UART0_BASE + UART_USR));
  d13x_uart_diag_field("RFL", getreg32(D13X_UART0_BASE + UART_RFL));
  d13x_uart_diag_field("IER", getreg32(D13X_UART0_BASE + UART_DLH_IER));
  d13x_uart_diag_field("LCR", getreg32(D13X_UART0_BASE + UART_LCR));
  d13x_uart_diag_field("MCR", getreg32(D13X_UART0_BASE + UART_MCR));
  d13x_uart_diag_field("CLIC76", getreg32(D13X_CLIC_UART0_ENTRY));
  d13x_uart_diag_field("MIE", d13x_uart_diag_read_mie());
  d13x_uart_diag_field("MSTATUS", d13x_uart_diag_read_mstatus());
  d13x_uart_diag_field("CLK", getreg32(D13X_CMU_UART0_CFG));
  d13x_uart_diag_field("RXN", g_d13x_diag_rx_count);
  d13x_uart_diag_field("LAST", g_d13x_diag_last_rx);
  d13x_uart_diag_puts("\r\n");
}

static void d13x_uart_diag_loopback(void)
{
  uint32_t saved_ier;
  uint32_t saved_mcr;
  int timeout;

  g_d13x_diag_loopback = 0xffffffff;
  saved_ier = getreg32(D13X_UART0_BASE + UART_DLH_IER);
  saved_mcr = getreg32(D13X_UART0_BASE + UART_MCR);
  putreg32(0, D13X_UART0_BASE + UART_DLH_IER);

  for (timeout = 0; timeout < D13X_UART_DIAG_TIMEOUT; timeout++)
    {
      if ((getreg32(D13X_UART0_BASE + UART_LSR) & UART_LSR_TEMT) != 0)
        {
          break;
        }
    }

  if (timeout < D13X_UART_DIAG_TIMEOUT)
    {
      putreg32(saved_mcr | UART_MCR_LOOPBACK,
               D13X_UART0_BASE + UART_MCR);
      putreg32(0x55, D13X_UART0_BASE + UART_RBR_THR_DLL);

      for (timeout = 0; timeout < D13X_UART_DIAG_TIMEOUT; timeout++)
        {
          if ((getreg32(D13X_UART0_BASE + UART_LSR) & UART_LSR_DR) != 0)
            {
              g_d13x_diag_loopback =
                getreg32(D13X_UART0_BASE + UART_RBR_THR_DLL) & 0xff;
              break;
            }
        }
    }

  putreg32(saved_mcr, D13X_UART0_BASE + UART_MCR);
  putreg32(saved_ier, D13X_UART0_BASE + UART_DLH_IER);
}

static void d13x_uart_diag_received(int ch)
{
  g_d13x_diag_last_rx = (uint32_t)ch & 0xff;
  g_d13x_diag_rx_count++;
  d13x_uart_diag_puts("\r\n[D13RX:RX]");
  d13x_uart_diag_field("BYTE", g_d13x_diag_last_rx);
  d13x_uart_diag_field("COUNT", g_d13x_diag_rx_count);
  d13x_uart_diag_puts("\r\n");
}

static void d13x_uart_diag_idle_poll(void)
{
  uint32_t gpio;
  uint32_t lsr;
  uint32_t rfl;

  gpio = getreg32(D13X_GPIO_PA_INPUT) & (1 << 1);
  lsr = getreg32(D13X_UART0_BASE + UART_LSR);
  rfl = getreg32(D13X_UART0_BASE + UART_RFL);

  if (!g_d13x_diag_idle_reported)
    {
      g_d13x_diag_idle_reported = true;
      g_d13x_diag_gpio_last = gpio;
      d13x_uart_diag_report("IDLE");
    }

  if (gpio != g_d13x_diag_gpio_last)
    {
      g_d13x_diag_gpio_last = gpio;
      g_d13x_diag_edges++;
      if (gpio == 0 && !g_d13x_diag_saw_low)
        {
          g_d13x_diag_saw_low = true;
          d13x_uart_diag_report("PA1LOW");
        }
    }

  if (((lsr & UART_LSR_DR) != 0 || rfl != 0) &&
      !g_d13x_diag_saw_rx_hw)
    {
      g_d13x_diag_saw_rx_hw = true;
      d13x_uart_diag_report("RXHW");
    }
}
#endif

static int d13x_uart0_handler(int irq, FAR void *context, FAR void *arg)
{
  FAR struct uart_dev_s *dev = arg;
  uint32_t iir;
  int passes;

  /* Drain every pending DesignWare/16550 interrupt source. */

  for (passes = 0; passes < 256; passes++)
    {
      iir = getreg32(D13X_UART0_BASE + UART_FCR_IIR);
      if ((iir & 0x0f) == UART_IIR_BUSY)
        {
          (void)getreg32(D13X_UART0_BASE + UART_USR);
          continue;
        }

      if ((iir & UART_IIR_NO_INT) != 0)
        {
          break;
        }

      switch (iir & UART_IIR_ID_MASK)
        {
          case UART_IIR_RECV_DATA:
          case UART_IIR_CHAR_TIMEOUT:
            uart_recvchars(dev);
            break;

          case UART_IIR_THR_EMPTY:
            uart_xmitchars(dev);
            break;

          case UART_IIR_RECV_LINE:
            (void)getreg32(D13X_UART0_BASE + UART_LSR);
            break;

          case UART_IIR_MODEM_STATUS:
            (void)getreg32(D13X_UART0_BASE + UART_MSR);
            break;

          default:
            return OK;
        }
    }

  return OK;
}

static int d13x_uart_setup(FAR struct uart_dev_s *dev)
{
  uint32_t divisor;

  /* Hengshan-Pi UART0 follows the D13x reference pinmux: PA0 TX, PA1 RX. */

  modifyreg32(D13X_GPIO_PA0_CFG, D13X_GPIO_PINMUX_MASK,
              D13X_GPIO_UART0_TX_CFG);
  modifyreg32(D13X_GPIO_PA1_CFG, D13X_GPIO_PINMUX_MASK,
              D13X_GPIO_UART0_RX_CFG);

  divisor = D13X_UART0_FREQ / (16 * D13X_UART0_BAUDRATE);

  /* 等待 UART 空闲 (来源: User Manual §12.4) */

  UART_WAIT_IDLE(D13X_UART0_BASE);

  /* 禁用中断 */

  putreg32(0, D13X_UART0_BASE + UART_DLH_IER);

  /* Select basic UART mode explicitly.  MCR[7:6] values 2 and 3 select
   * RS485 modes, where the RX pin no longer behaves as a normal input.
   */

  putreg32(0, D13X_UART0_BASE + UART_MCR);

  modifyreg32(D13X_UART0_BASE + UART_HALT, 0,
              UART_HALT_CHCFG_AT_BUSY);

  /* 设置 DLAB=1 以访问 DLL/DLH */

  putreg32(UART_LCR_DLAB, D13X_UART0_BASE + UART_LCR);

  /* 写入波特率除数 */

  putreg32(divisor & 0xFF, D13X_UART0_BASE + UART_RBR_THR_DLL);
  putreg32((divisor >> 8) & 0xFF, D13X_UART0_BASE + UART_DLH_IER);

  /* 使能 FIFO, 清除 DLAB, 设置 8N1 */

  putreg32(UART_FCR_FIFOE | UART_FCR_RFIFOR | UART_FCR_XFIFOR,
           D13X_UART0_BASE + UART_FCR_IIR);
  putreg32(0x03, D13X_UART0_BASE + UART_LCR);
  modifyreg32(D13X_UART0_BASE + UART_HALT, 0,
              UART_HALT_CHANGE_UPDATE);

  return OK;
}

static void d13x_uart_console_prepare(void)
{
  uint32_t rx_cfg = D13X_GPIO_UART0_RX_CFG;

  /* uart_open() deliberately skips setup() for a console because NuttX
   * expects early serial initialization to have configured the hardware.
   * tinySPL leaves TX usable, but it does not guarantee the PA1 RX mux.
   * Preserve its proven clock/divisor/FIFO state and only claim the RX pin.
   */

#ifdef CONFIG_D13X_UART0_DIAGNOSTIC
  rx_cfg |= D13X_GPIO_SPE_IE_FORCE;
#endif

  modifyreg32(D13X_GPIO_PA1_CFG, D13X_GPIO_PINMUX_MASK, rx_cfg);
  modifyreg32(D13X_UART0_BASE + UART_MCR, D13X_UART_MCR_FUNC_MASK, 0);

#ifdef CONFIG_D13X_UART0_DIAGNOSTIC
  d13x_uart_diag_loopback();
  g_d13x_diag_gpio_last = getreg32(D13X_GPIO_PA_INPUT) & (1 << 1);
  d13x_uart_diag_report("BOOT");
#endif
}

static void d13x_uart_shutdown(FAR struct uart_dev_s *dev)
{
  putreg32(0, D13X_UART0_BASE + UART_DLH_IER);
}

static int d13x_uart_attach(FAR struct uart_dev_s *dev)
{
  int ret;

  ret = irq_attach(D13X_UART0_IRQ, d13x_uart0_handler, dev);
  if (ret == OK)
    {
      /* UART0 runs in polling mode until D13x CLIC context switching is
       * fixed.  Leave its source attached for later work, but masked.
       */

      up_disable_irq(D13X_UART0_IRQ);
      modifyreg32(D13X_UART0_BASE + UART_DLH_IER,
                  UART_IER_ERBFI | UART_IER_ETBEI, 0);
    }

  return ret;
}

static void d13x_uart_detach(FAR struct uart_dev_s *dev)
{
  up_disable_irq(D13X_UART0_IRQ);
  irq_detach(D13X_UART0_IRQ);
}

static int d13x_uart_ioctl(FAR struct file *filep, int cmd,
                            unsigned long arg)
{
  return -ENOTTY;
}

static int d13x_uart_receive(FAR struct uart_dev_s *dev,
                              FAR unsigned int *status)
{
  int ch;

  *status = getreg32(D13X_UART0_BASE + UART_LSR);
  ch = (int)(getreg32(D13X_UART0_BASE + UART_RBR_THR_DLL) & 0xff);

#ifdef CONFIG_D13X_UART0_DIAGNOSTIC
  d13x_uart_diag_received(ch);
#endif

  return ch;
}

static void d13x_uart_rxint(FAR struct uart_dev_s *dev, bool enable)
{
  uint32_t ier = getreg32(D13X_UART0_BASE + UART_DLH_IER);

  /* RX IRQ entry/return is not yet safe on the D13x CLIC port: waking NSH
   * from uart_recvchars() inside the ISR restores an invalid task PC.  Keep
   * ERBFI disabled and let d13x_uart_rxpoll() feed the normal NuttX RX ring
   * from idle task context.  TX interrupts remain available.
   */

  ier &= ~UART_IER_ERBFI;

  putreg32(ier, D13X_UART0_BASE + UART_DLH_IER);
  (void)getreg32(D13X_UART0_BASE + UART_DLH_IER);
}

static bool d13x_uart_rxavailable(FAR struct uart_dev_s *dev)
{
  return (getreg32(D13X_UART0_BASE + UART_LSR) & UART_LSR_DR) != 0;
}

static void d13x_uart_send(FAR struct uart_dev_s *dev, int ch)
{
  while (!(getreg32(D13X_UART0_BASE + UART_LSR) & UART_LSR_THRE));
  putreg32((uint32_t)ch, D13X_UART0_BASE + UART_RBR_THR_DLL);
}

static void d13x_uart_poll_tx(FAR struct uart_dev_s *dev)
{
  unsigned int nsent = 0;

  while (dev->xmit.head != dev->xmit.tail)
    {
      d13x_uart_send(dev, dev->xmit.buffer[dev->xmit.tail]);
      dev->xmit.tail++;
      if (dev->xmit.tail >= dev->xmit.size)
        {
          dev->xmit.tail = 0;
        }

      nsent++;
    }

  if (nsent > 0)
    {
      uart_datasent(dev);
    }
}

static void d13x_uart_txint(FAR struct uart_dev_s *dev, bool enable)
{
  uint32_t ier = getreg32(D13X_UART0_BASE + UART_DLH_IER);

  ier &= ~UART_IER_ETBEI;
  putreg32(ier, D13X_UART0_BASE + UART_DLH_IER);

  if (enable)
    {
      d13x_uart_poll_tx(dev);
    }

  (void)getreg32(D13X_UART0_BASE + UART_DLH_IER);
}

static bool d13x_uart_txready(FAR struct uart_dev_s *dev)
{
  /* d13x_uart_send() performs the bounded hardware wait.  Returning true
   * here makes direct/IRQ-context writes drain their complete buffer.
   */

  return true;
}

static bool d13x_uart_txempty(FAR struct uart_dev_s *dev)
{
  return (getreg32(D13X_UART0_BASE + UART_LSR) & UART_LSR_TEMT) != 0;
}

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct uart_ops_s g_d13x_uart_ops =
{
  .setup        = d13x_uart_setup,
  .shutdown     = d13x_uart_shutdown,
  .attach       = d13x_uart_attach,
  .detach       = d13x_uart_detach,
  .ioctl        = d13x_uart_ioctl,
  .receive      = d13x_uart_receive,
  .rxint        = d13x_uart_rxint,
  .rxavailable  = d13x_uart_rxavailable,
  .send         = d13x_uart_send,
  .txint        = d13x_uart_txint,
  .txready      = d13x_uart_txready,
  .txempty      = d13x_uart_txempty,
};

static char g_d13x_uart0_rxbuf[256];
static char g_d13x_uart0_txbuf[256];

static uart_dev_t g_d13x_uart0 =
{
  .ops      = &g_d13x_uart_ops,
  .isconsole = true,
  .recv     =
  {
    .size   = sizeof(g_d13x_uart0_rxbuf),
    .buffer = g_d13x_uart0_rxbuf,
  },
  .xmit     =
  {
    .size   = sizeof(g_d13x_uart0_txbuf),
    .buffer = g_d13x_uart0_txbuf,
  },
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

#ifdef USE_EARLYSERIALINIT
void riscv_earlyserialinit(void)
{
  g_d13x_uart0.isconsole = true;
  d13x_uart_setup(&g_d13x_uart0);
}
#endif

void d13x_uart_rxpoll(void)
{
#ifdef CONFIG_D13X_UART0_DIAGNOSTIC
  d13x_uart_diag_idle_poll();
#endif

  if (d13x_uart_rxavailable(&g_d13x_uart0))
    {
      uart_recvchars(&g_d13x_uart0);
    }
}

/****************************************************************************
 * Name: riscv_serialinit
 *
 * Description:
 *   Register serial console and serial ports
 *
 ****************************************************************************/

void riscv_serialinit(void)
{
  d13x_uart_console_prepare();
  uart_register("/dev/console", &g_d13x_uart0);
  uart_register("/dev/ttyS0", &g_d13x_uart0);
}
