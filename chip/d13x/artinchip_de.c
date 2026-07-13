/****************************************************************************
 * vendor/artinchip/chips/d13x/artinchip_de.c
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/arch.h>

#include <errno.h>

#include "chip.h"
#include "artinchip_de.h"

#define BIT(n)                    (1u << (n))

#define DE_CONFIG_UPDATE          0x008u
#define VIDEO_LAYER_CTRL          0x020u
#define UI_LAYER_CTRL             0x0a0u
#define UI_LAYER_SIZE             0x0a4u
#define UI_LAYER_RECT_CTRL        0x0b0u
#define UI_RECT_INPUT_SIZE        0x0c0u
#define UI_RECT_OFFSET            0x0c4u
#define UI_RECT_STRIDE            0x0c8u
#define UI_RECT_ADDR              0x0ccu
#define BLENDING_BG_COLOR         0x100u
#define BLENDING_OUTPUT_SIZE      0x104u
#define UI_LAYER_OFFSET           0x10cu
#define WB_CTRL                   0x170u
#define TIMING_CTRL               0x1d0u
#define TIMING_INIT               0x1d4u
#define TIMING_STATUS             0x1d8u
#define TIMING_LINE_SET           0x1dcu
#define TIMING_ACTIVE_SIZE        0x1e0u
#define TIMING_H_PORCH            0x1e4u
#define TIMING_V_PORCH            0x1e8u
#define TIMING_SYNC_PULSE         0x1ecu
#define TIMING_POLARITY           0x1f0u
#define QOS_UI                    0x88cu
#define QOS_URGENT                0x890u

#define UI_FORMAT_RGB565          0x0eu
#define UI_LAYER_ENABLE           BIT(0)
#define UI_ALPHA_ENABLE           BIT(2)
#define TIMING_ENABLE             BIT(0)
#define DE_SOFT_RESET_ENABLE      BIT(16)

#define CMU_CLK_DE                (D13X_CMU_BASE + 0x08c0u)
#define MOD_RESET_DEASSERT        BIT(13)
#define MOD_BUS_ENABLE            BIT(12)
#define MOD_CLOCK_ENABLE          BIT(8)

static inline void de_write(uint32_t offset, uint32_t value)
{
  putreg32(value, D13X_DE_BASE + offset);
}

static inline uint32_t de_size(uint32_t width, uint32_t height)
{
  return ((height & 0x1fffu) << 16) | (width & 0x1fffu);
}

int d13x_de_initialize(uint32_t width, uint32_t height,
                       uint32_t hfp, uint32_t hbp, uint32_t hsync,
                       uint32_t vfp, uint32_t vbp, uint32_t vsync,
                       uint32_t stride)
{
  uint32_t reg;

  if (width == 0 || height == 0 || width > 0x1fff || height > 0x1fff ||
      stride > 0x7fff)
    {
      return -EINVAL;
    }

  /* D13x DE v1.1 runs at 150 MHz from PLL_INT1 / 8. */

  reg = getreg32(CMU_CLK_DE);
  reg &= ~0x3fu;
  reg |= 7u | MOD_RESET_DEASSERT | MOD_BUS_ENABLE | MOD_CLOCK_ENABLE;
  putreg32(reg, CMU_CLK_DE);

  de_write(TIMING_CTRL, 0);
  de_write(VIDEO_LAYER_CTRL, 0);
  de_write(UI_LAYER_CTRL, 0);
  de_write(UI_LAYER_RECT_CTRL, 0);
  de_write(TIMING_INIT, 0);
  de_write(TIMING_STATUS, 0xffffffffu);

  de_write(TIMING_ACTIVE_SIZE, de_size(width, height));
  de_write(TIMING_H_PORCH, ((hbp & 0x1fffu) << 16) |
                            (hfp & 0x1fffu));
  de_write(TIMING_V_PORCH, ((vbp & 0x1fffu) << 16) |
                            (vfp & 0x1fffu));
  de_write(TIMING_SYNC_PULSE, ((vsync & 0x7ffu) << 16) |
                               (hsync & 0x7ffu));
  de_write(TIMING_POLARITY, 0);
  de_write(TIMING_LINE_SET, 2u << 16);

  de_write(BLENDING_BG_COLOR, 0);
  de_write(BLENDING_OUTPUT_SIZE, de_size(width, height));
  de_write(UI_LAYER_SIZE, de_size(width, height));
  de_write(UI_LAYER_OFFSET, 0);
  de_write(UI_RECT_INPUT_SIZE, de_size(width, height));
  de_write(UI_RECT_OFFSET, 0);
  de_write(UI_RECT_STRIDE, stride);

  reg = (0xffu << 24) | (UI_FORMAT_RGB565 << 8) |
        UI_ALPHA_ENABLE | UI_LAYER_ENABLE;
  de_write(UI_LAYER_CTRL, reg);
  de_write(UI_LAYER_RECT_CTRL, BIT(0));

  reg = getreg32(D13X_DE_BASE + WB_CTRL);
  de_write(WB_CTRL, reg | DE_SOFT_RESET_ENABLE);

  de_write(QOS_UI, (0xdu << 28) | (0x60u << 16) |
                   (0xdu << 12) | 0x40u);
  de_write(QOS_URGENT, BIT(15) | BIT(14) | 0x40u);
  de_write(DE_CONFIG_UPDATE, BIT(0));
  return OK;
}

void d13x_de_set_framebuffer(uintptr_t address)
{
  de_write(UI_RECT_ADDR, (uint32_t)address);
  de_write(DE_CONFIG_UPDATE, BIT(0));
}

void d13x_de_enable(void)
{
  uint32_t reg = getreg32(D13X_DE_BASE + TIMING_CTRL);
  de_write(TIMING_CTRL, reg | TIMING_ENABLE);
  de_write(DE_CONFIG_UPDATE, BIT(0));
}

void d13x_de_disable(void)
{
  uint32_t reg = getreg32(D13X_DE_BASE + TIMING_CTRL);
  de_write(TIMING_CTRL, reg & ~TIMING_ENABLE);
  de_write(DE_CONFIG_UPDATE, BIT(0));
}
