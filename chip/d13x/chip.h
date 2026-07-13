/****************************************************************************
 * vendor/artinchip/chips/d13x/chip.h
 *
 * D13x 芯片宏定义 (基于 luban-lite aic_soc.h 权威地址)
 *
 ****************************************************************************/

#ifndef __VENDOR_ARTINCHIP_CHIPS_D13X_CHIP_H
#define __VENDOR_ARTINCHIP_CHIPS_D13X_CHIP_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* 寄存器访问宏 */

#define getreg32(a)     (*(volatile uint32_t *)(a))
#define putreg32(v,a)   (*(volatile uint32_t *)(a) = (v))

/* 芯片型号 */

#define D13X_CHIP_D133EBS          1

/* 内存布局 (来源: luban-lite aic_soc.h + D13x User Manual) */

#define D13X_BROM_BASE             0x30000000  /* BROM 64KB */
#define D13X_SRAM_BASE             0x30040000  /* SRAM_S0 1MB */
#define D13X_SRAM_SIZE             (1024 * 1024)  /* 1MB physical SRAM */
#define D13X_SRAM_RESERVED         0x100
#define D13X_SRAM_USABLE_SIZE      (D13X_SRAM_SIZE - D13X_SRAM_RESERVED)
#define D13X_PSRAM_BASE            0x40000000  /* PSRAM (up to 512MB address space) */
#define D13X_PSRAM_SIZE            (8 * 1024 * 1024)  /* 8MB for D133EBS */
#define D13X_FLASH_XIP_BASE        0x60000000  /* Flash XIP */

/* 外设基地址 (来源: luban-lite aic_soc.h) */

#define D13X_DMA_BASE              0x10000000
#define D13X_CE_BASE               0x10020000
#define D13X_USB_DEV_BASE          0x10200000
#define D13X_USB_HOST0_BASE        0x10210000
#define D13X_GMAC0_BASE            0x10280000
#define D13X_XSPI_BASE             0x10300000  /* XSPI (PSRAM 控制器) */
#define D13X_QSPI0_BASE            0x10400000
#define D13X_QSPI1_BASE            0x10410000
#define D13X_QSPI2_BASE            0x10420000
#define D13X_QSPI3_BASE            0x10430000
#define D13X_SDMC0_BASE            0x10440000
#define D13X_SDMC1_BASE            0x10450000
#define D13X_SYSCFG_BASE           0x18000000
#define D13X_CMU_BASE              0x18020000
#define D13X_SPI_ENC_BASE          0x18100000
#define D13X_I2S0_BASE             0x18600000
#define D13X_AUDIO_BASE            0x18610000
#define D13X_GPIO_BASE             0x18700000
#define D13X_UART0_BASE            0x18710000
#define D13X_LCD_BASE              0x18800000
#define D13X_DE_BASE               0x18A00000
#define D13X_GE_BASE               0x18B00000
#define D13X_VE_BASE               0x18C00000
#define D13X_WDT_BASE              0x19000000  /* Watchdog */
#define D13X_WRI_BASE              0x1900F000
#define D13X_SID_BASE              0x19010000  /* eFuse */
#define D13X_RTC_BASE              0x19030000
#define D13X_GTC_BASE              0x19050000  /* General Timer Counter */
#define D13X_I2C0_BASE             0x19220000
#define D13X_I2C1_BASE             0x19221000
#define D13X_I2C2_BASE             0x19222000
#define D13X_CAN0_BASE             0x19230000
#define D13X_CAN1_BASE             0x19231000
#define D13X_PWM_BASE              0x19240000

/* 中断控制器 (来源: luban-lite core_rv32.h) */

#define D13X_E907_CLINT_BASE       0x20000000  /* Core-Local Interrupt */
#define D13X_E907_CLIC_BASE        0x20800000  /* Core-Local Interrupt Controller */

/* 时钟频率 (来源: D13x User Manual + luban-lite) */

#define D13X_OSC24M_FREQ           24000000
#define D13X_PLL_INT0_FREQ         480000000
#define D13X_PLL_INT1_FREQ         1200000000
#define D13X_AXI_AHB_FREQ          200000000
#define D13X_APB0_FREQ             100000000
#define D13X_UART0_FREQ            60000000

/* UART0 配置 */

#define D13X_UART0_BAUDRATE        115200
#define D13X_UART0_IRQ             92  /* RISCV_IRQ_ASYNC + raw CLIC IRQ 76 */

/* 16550 UART 寄存器偏移 (来源: D13x User Manual §12.4) */

#define UART_RBR_THR_DLL           0x00
#define UART_DLH_IER               0x04
#define UART_FCR_IIR               0x08
#define UART_LCR                   0x0C
#define UART_MCR                   0x10
#define UART_LSR                   0x14
#define UART_MSR                   0x18
#define UART_USR                   0x7C   /* UART Status Register (busy flag) */
#define UART_RFL                   0x84   /* Receive FIFO level */
#define UART_HALT                  0xA4

/* LSR 位定义 */

#define UART_LSR_DR                (1 << 0)
#define UART_LSR_OE                (1 << 1)
#define UART_LSR_PE                (1 << 2)
#define UART_LSR_FE                (1 << 3)
#define UART_LSR_BI                (1 << 4)
#define UART_LSR_THRE              (1 << 5)
#define UART_LSR_TEMT              (1 << 6)

/* Interrupt enable and identification bits */

#define UART_IER_ERBFI             (1 << 0)
#define UART_IER_ETBEI             (1 << 1)
#define UART_IIR_NO_INT            (1 << 0)
#define UART_IIR_ID_MASK           0x0e
#define UART_IIR_MODEM_STATUS      0x00
#define UART_IIR_THR_EMPTY         0x02
#define UART_IIR_RECV_DATA         0x04
#define UART_IIR_RECV_LINE         0x06
#define UART_IIR_CHAR_TIMEOUT      0x0c
#define UART_IIR_BUSY              0x07

/* USR 位定义 */

#define UART_USR_BUSY              (1 << 0)
#define UART_USR_RFNE              (1 << 3)

/* MCR bits */

#define UART_MCR_LOOPBACK          (1 << 4)

/* LCR 位定义 */

#define UART_LCR_DLAB              (1 << 7)

/* HALT bits required when updating the DesignWare UART divisor. */

#define UART_HALT_CHCFG_AT_BUSY    (1 << 1)
#define UART_HALT_CHANGE_UPDATE    (1 << 2)

/* FCR 位定义 */

#define UART_FCR_FIFOE             (1 << 0)
#define UART_FCR_RFIFOR            (1 << 1)
#define UART_FCR_XFIFOR            (1 << 2)

/* 等待 UART 空闲 (来源: luban-lite aic_hal_uart.c WAIT_USART_IDLE) */

#define UART_WAIT_IDLE(base) \
  do { while (getreg32((base) + UART_USR) & UART_USR_BUSY); } while (0)

#endif /* __VENDOR_ARTINCHIP_CHIPS_D13X_CHIP_H */
