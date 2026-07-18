/****************************************************************************
 * vendor/artinchip/boards/d13x-hengshan-pi/include/board.h
 *
 * D13x Hengshan Pi 板级宏定义
 *
 ****************************************************************************/

#ifndef __VENDOR_ARTINCHIP_BOARDS_D13X_HENGSHAN_PI_INCLUDE_BOARD_H
#define __VENDOR_ARTINCHIP_BOARDS_D13X_HENGSHAN_PI_INCLUDE_BOARD_H

#include <stddef.h>
#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* 系统时钟频率 */

#define BOARD_SYSCLK              480000000
#define BOARD_AHBCLK              200000000
#define BOARD_APB0CLK             100000000
#define BOARD_UARTCLK             60000000

/* 板级标识 */

#define BOARD_NAME                "D13x Hengshan Pi"
#define BOARD_VERSION             "1.0"

/* The complete MiSans character set is stored as a precompiled LVGL BinFont
 * in a raw, read-only partition. Keeping it outside the loaded NuttX image
 * avoids consuming image space and runtime TTF rasterization memory.
 */

#define BOARD_FONT_FLASH_OFFSET   0x00400000u
#define BOARD_FONT_FLASH_SIZE     1141148u

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int board_persist_read(void *buffer, size_t capacity, size_t *length);
int board_persist_write(const void *buffer, size_t length);
int board_flash_read(uint32_t address, void *buffer, size_t length);

#endif /* __VENDOR_ARTINCHIP_BOARDS_D13X_HENGSHAN_PI_INCLUDE_BOARD_H */
