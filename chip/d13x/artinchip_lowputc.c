/****************************************************************************
 * vendor/artinchip/chips/d13x/artinchip_lowputc.c
 *
 * D13x 低级串口输出
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/arch.h>
#include <nuttx/irq.h>
#include "chip.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: artinchip_lowputc
 *
 * Description:
 *   Output one byte on the serial console
 *
 ****************************************************************************/

void artinchip_lowputc(char ch)
{
  /* 等待 UART 空闲 + 发送缓冲区空 (来源: luban-lite "fix hardware bug") */

  UART_WAIT_IDLE(D13X_UART0_BASE);
  while (!(getreg32(D13X_UART0_BASE + UART_LSR) & UART_LSR_THRE));

  /* 写入数据 */

  putreg32((uint32_t)ch, D13X_UART0_BASE + UART_RBR_THR_DLL);
}

/****************************************************************************
 * Name: up_putc
 *
 * Description:
 *   Provide priority, low-level access to support OS debug writes
 *
 ****************************************************************************/

void up_putc(int ch)
{
  artinchip_lowputc((char)ch);
}
