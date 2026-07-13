/****************************************************************************
 * vendor/artinchip/chips/d13x/include/irq.h
 *
 * D13x 中断号定义 (来源: luban-lite aic_soc.h)
 *
 ****************************************************************************/

#ifndef __VENDOR_ARTINCHIP_CHIPS_D13X_INCLUDE_IRQ_H
#define __VENDOR_ARTINCHIP_CHIPS_D13X_INCLUDE_IRQ_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <arch/irq.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* RISC-V 基础中断数 */

#define RISCV_NIRQ_INTERRUPTS    16

/* D13x 外设中断号 (来源: luban-lite aic_soc.h IRQn_Type) */

#define D13X_CLIC_IRQ(raw)       (RISCV_IRQ_ASYNC + (raw))

#define D13X_IRQ_DMA             D13X_CLIC_IRQ(32)
#define D13X_IRQ_CE              D13X_CLIC_IRQ(33)
#define D13X_IRQ_USB_DEV         D13X_CLIC_IRQ(34)
#define D13X_IRQ_USB_HOST0       D13X_CLIC_IRQ(35)
#define D13X_IRQ_GMAC0           D13X_CLIC_IRQ(39)
#define D13X_IRQ_SPI_ENC         D13X_CLIC_IRQ(41)
#define D13X_IRQ_QSPI2           D13X_CLIC_IRQ(42)
#define D13X_IRQ_QSPI3           D13X_CLIC_IRQ(43)
#define D13X_IRQ_QSPI0           D13X_CLIC_IRQ(44)
#define D13X_IRQ_QSPI1           D13X_CLIC_IRQ(45)
#define D13X_IRQ_SDMC0           D13X_CLIC_IRQ(46)
#define D13X_IRQ_SDMC1           D13X_CLIC_IRQ(47)
#define D13X_IRQ_XSPI            D13X_CLIC_IRQ(49)
#define D13X_IRQ_RTC             D13X_CLIC_IRQ(50)
#define D13X_IRQ_MTOP            D13X_CLIC_IRQ(51)
#define D13X_IRQ_AUDIO           D13X_CLIC_IRQ(54)
#define D13X_IRQ_LCD             D13X_CLIC_IRQ(55)
#define D13X_IRQ_DE              D13X_CLIC_IRQ(59)
#define D13X_IRQ_GE              D13X_CLIC_IRQ(60)
#define D13X_IRQ_VE              D13X_CLIC_IRQ(61)
#define D13X_IRQ_WDT             D13X_CLIC_IRQ(64)
#define D13X_IRQ_GPIO            D13X_CLIC_IRQ(68) /* raw 68-75 */
#define D13X_IRQ_UART0           D13X_CLIC_IRQ(76)
#define D13X_IRQ_UART1           D13X_CLIC_IRQ(77)
#define D13X_IRQ_UART2           D13X_CLIC_IRQ(78)
#define D13X_IRQ_UART3           D13X_CLIC_IRQ(79)
#define D13X_IRQ_UART4           D13X_CLIC_IRQ(80)
#define D13X_IRQ_UART5           D13X_CLIC_IRQ(81)
#define D13X_IRQ_UART6           D13X_CLIC_IRQ(82)
#define D13X_IRQ_UART7           D13X_CLIC_IRQ(83)
#define D13X_IRQ_I2C0            D13X_CLIC_IRQ(84)
#define D13X_IRQ_I2C1            D13X_CLIC_IRQ(85)
#define D13X_IRQ_I2C2            D13X_CLIC_IRQ(86)
#define D13X_IRQ_CAN0            D13X_CLIC_IRQ(88)
#define D13X_IRQ_CAN1            D13X_CLIC_IRQ(89)
#define D13X_IRQ_PWM             D13X_CLIC_IRQ(90)
#define D13X_IRQ_GPAI            D13X_CLIC_IRQ(92)
#define D13X_IRQ_RTP             D13X_CLIC_IRQ(93)
#define D13X_IRQ_TSEN            D13X_CLIC_IRQ(94)
#define D13X_IRQ_CIR             D13X_CLIC_IRQ(95)

/* 总中断数 */

#define NR_IRQS                  128

/* CLIC 初始化所需的标准 RISC-V 中断 ID (来源: luban-lite aic_soc.h) */

#define Machine_Software_IRQn    3
#define MAX_IRQn                 96   /* CIR_IRQn (95) + 1 */

/* 中断优先级 */

#define D13X_IRQ_PRIORITY_LOW    0
#define D13X_IRQ_PRIORITY_HIGH   15

#endif /* __VENDOR_ARTINCHIP_CHIPS_D13X_INCLUDE_IRQ_H */
