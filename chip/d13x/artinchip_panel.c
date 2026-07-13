/****************************************************************************
 * vendor/artinchip/chips/d13x/artinchip_panel.c
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/arch.h>

#include <stddef.h>

#include "chip.h"
#include "artinchip_panel.h"

#define PANEL_HACTIVE       1024u
#define PANEL_VACTIVE       600u
#define PANEL_HBP           160u
#define PANEL_HFP           160u
#define PANEL_HSYNC         20u
#define PANEL_VBP           12u
#define PANEL_VFP           20u
#define PANEL_VSYNC         2u
#define PANEL_PIXEL_CLOCK   52000000u

#define PANEL_GPIO_GROUP         4u
#define PANEL_ENABLE_PIN         13u
#define GPIO_GROUP_STRIDE        0x100u
#define GPIO_OUTPUT_CLEAR        0x10u
#define GPIO_OUTPUT_SET          0x14u
#define GPIO_PIN_CONFIG          0x80u
#define GPIO_PIN_STRIDE          0x04u
#define GPIO_FUNCTION_MASK       0x0fu
#define GPIO_DRIVE_MASK          (0x07u << 4)
#define GPIO_PULL_MASK           (0x03u << 8)
#define GPIO_DIRECTION_MASK      (0x03u << 16)
#define GPIO_FUNCTION_GPIO       1u
#define GPIO_DRIVE_LEVEL_3       (3u << 4)
#define GPIO_DIRECTION_OUTPUT    (2u << 16)

static uintptr_t d13x_panel_gpio_reg(uint32_t offset)
{
  return D13X_GPIO_BASE + PANEL_GPIO_GROUP * GPIO_GROUP_STRIDE + offset;
}

int d13x_panel_initialize(void)
{
  uintptr_t cfg = d13x_panel_gpio_reg(GPIO_PIN_CONFIG +
                                      PANEL_ENABLE_PIN * GPIO_PIN_STRIDE);
  uint32_t reg = getreg32(cfg);

  /* Luban-Lite's D13x demo88 board drives the LVDS backlight from PE13.
   * Keep it off until the LVDS and DE timing blocks are running.
   */

  reg &= ~(GPIO_FUNCTION_MASK | GPIO_DRIVE_MASK | GPIO_PULL_MASK |
           GPIO_DIRECTION_MASK);
  reg |= GPIO_FUNCTION_GPIO | GPIO_DRIVE_LEVEL_3 | GPIO_DIRECTION_OUTPUT;
  putreg32(reg, cfg);
  putreg32(1u << PANEL_ENABLE_PIN,
           d13x_panel_gpio_reg(GPIO_OUTPUT_CLEAR));

  return 0;
}

void d13x_panel_enable(void)
{
  putreg32(1u << PANEL_ENABLE_PIN,
           d13x_panel_gpio_reg(GPIO_OUTPUT_SET));
}

void d13x_panel_disable(void)
{
  putreg32(1u << PANEL_ENABLE_PIN,
           d13x_panel_gpio_reg(GPIO_OUTPUT_CLEAR));
}

void d13x_panel_get_timing(uint32_t *hactive, uint32_t *vactive,
                           uint32_t *hfp, uint32_t *hbp, uint32_t *hsync,
                           uint32_t *vfp, uint32_t *vbp, uint32_t *vsync,
                           uint32_t *pixel_clock)
{
  if (hactive != NULL)
    {
      *hactive = PANEL_HACTIVE;
    }

  if (vactive != NULL)
    {
      *vactive = PANEL_VACTIVE;
    }

  if (hfp != NULL)
    {
      *hfp = PANEL_HFP;
    }

  if (hbp != NULL)
    {
      *hbp = PANEL_HBP;
    }

  if (hsync != NULL)
    {
      *hsync = PANEL_HSYNC;
    }

  if (vfp != NULL)
    {
      *vfp = PANEL_VFP;
    }

  if (vbp != NULL)
    {
      *vbp = PANEL_VBP;
    }

  if (vsync != NULL)
    {
      *vsync = PANEL_VSYNC;
    }

  if (pixel_clock != NULL)
    {
      *pixel_clock = PANEL_PIXEL_CLOCK;
    }
}
