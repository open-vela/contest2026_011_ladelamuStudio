/****************************************************************************
 * vendor/artinchip/chips/d13x/artinchip_lvds.c
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/arch.h>

#include <errno.h>

#include "chip.h"
#include "artinchip_lvds.h"

#define BIT(n)                    (1u << (n))

#define D13X_LVDS_BASE            0x18810000u

#define LVDS_CTL                  0x00u
#define LVDS_0_SWAP               0x20u
#define LVDS_1_SWAP               0x24u
#define LVDS_0_POL_CTL            0x28u
#define LVDS_1_POL_CTL            0x2cu
#define LVDS_0_PHY_CTL            0x30u
#define LVDS_1_PHY_CTL            0x34u

#define LVDS_CTL_MODE(x)          (((x) & 0x3u) << 8)
#define LVDS_CTL_LINK(x)          (((x) & 0x3u) << 4)
#define LVDS_CTL_SYNC_MODE        BIT(1)
#define LVDS_CTL_EN               BIT(0)

#define LVDS_LINES                0x43210u
#define LVDS_PHY                  0xfau

#define CMU_PLL_FRA2_GEN          (D13X_CMU_BASE + 0x0028u)
#define CMU_PLL_FRA2_SDM          (D13X_CMU_BASE + 0x0088u)
#define CMU_CLK_DISP              (D13X_CMU_BASE + 0x0220u)
#define CMU_CLK_LVDS              (D13X_CMU_BASE + 0x0884u)

#define PLL_ENABLE                BIT(16)
#define PLL_OUT_SYS               BIT(18)
#define PLL_OUT_MUX               BIT(20)
#define PLL_FACTOR_M_EN           BIT(19)

#define MOD_RESET_DEASSERT        BIT(13)
#define MOD_BUS_ENABLE            BIT(12)
#define MOD_CLOCK_ENABLE          BIT(8)

#define GPIO_GROUP_FACTOR         0x100u
#define GPIO_PIN_FACTOR           0x4u
#define GPIO_PIN_CFG              0x80u
#define GPIO_FUN_SHIFT            0
#define GPIO_FUN_MASK             0x0fu
#define GPIO_DRV_SHIFT            4
#define GPIO_DRV_MASK             0x07u
#define GPIO_PULL_SHIFT           8
#define GPIO_PULL_MASK            0x03u

static inline void lvds_write(uint32_t offset, uint32_t value)
{
  putreg32(value, D13X_LVDS_BASE + offset);
}

static void d13x_lvds_pinmux(void)
{
  uint32_t pin;

  for (pin = 18; pin <= 27; pin++)
    {
      uintptr_t addr = D13X_GPIO_BASE + 3u * GPIO_GROUP_FACTOR +
                       pin * GPIO_PIN_FACTOR + GPIO_PIN_CFG;
      uint32_t reg = getreg32(addr);

      reg &= ~((GPIO_FUN_MASK << GPIO_FUN_SHIFT) |
               (GPIO_DRV_MASK << GPIO_DRV_SHIFT) |
               (GPIO_PULL_MASK << GPIO_PULL_SHIFT));
      reg |= (3u << GPIO_FUN_SHIFT) | (3u << GPIO_DRV_SHIFT);
      putreg32(reg, addr);
    }
}

static int d13x_lvds_clock_config(uint32_t pixel_clock)
{
  uint32_t reg;

  if (pixel_clock != 52000000u)
    {
      return -EINVAL;
    }

  /* 52 MHz pixel clock uses a 364 MHz LVDS serial clock.  The active
   * D13x configuration uses FRA2 in SDM mode.  These values are the direct
   * result of the vendor clock algorithm: P=1, N=120, M=3, SDM amplitude 0,
   * step 360, frequency selector 3 and triangular mode.
   */

  reg = getreg32(CMU_PLL_FRA2_GEN);
  reg &= ~PLL_OUT_MUX;
  putreg32(reg, CMU_PLL_FRA2_GEN);

  reg &= ~(0xffffu | PLL_FACTOR_M_EN | (0x1fu << 24));
  reg |= PLL_FACTOR_M_EN | (120u << 8) | (3u << 4) | 1u;
  putreg32(reg, CMU_PLL_FRA2_GEN);
  putreg32(BIT(31) | (2u << 29) | (360u << 20) | (3u << 17),
           CMU_PLL_FRA2_SDM);

  reg = getreg32(CMU_PLL_FRA2_GEN);
  reg |= PLL_ENABLE | PLL_OUT_SYS;
  putreg32(reg, CMU_PLL_FRA2_GEN);

  /* Match Luban-Lite's D13x PLL driver.  PLL_FRA2 is allowed 200 us to
   * settle; its generic register lock bit is not a valid completion signal
   * for this SDM configuration.
   */

  up_udelay(200);

  reg = getreg32(CMU_PLL_FRA2_GEN);
  reg |= PLL_OUT_MUX;
  putreg32(reg, CMU_PLL_FRA2_GEN);

  /* SCLK = FRA2 / 1, PIX = SCLK / 7. */

  reg = getreg32(CMU_CLK_DISP);
  reg &= ~((0x7u << 0) | (0x1fu << 4) |
           (0x3u << 10) | (0x3u << 12));
  reg |= 6u << 4;
  putreg32(reg, CMU_CLK_DISP);

  reg = getreg32(CMU_CLK_LVDS);
  reg |= MOD_RESET_DEASSERT | MOD_BUS_ENABLE | MOD_CLOCK_ENABLE;
  putreg32(reg, CMU_CLK_LVDS);
  return OK;
}

int d13x_lvds_initialize(uint32_t pixel_clock)
{
  int ret;

  d13x_lvds_pinmux();

  ret = d13x_lvds_clock_config(pixel_clock);
  if (ret < 0)
    {
      return ret;
    }

  d13x_lvds_disable();
  lvds_write(LVDS_0_SWAP, LVDS_LINES);
  lvds_write(LVDS_1_SWAP, LVDS_LINES);
  lvds_write(LVDS_0_POL_CTL, 0);
  lvds_write(LVDS_1_POL_CTL, 0);
  lvds_write(LVDS_0_PHY_CTL, LVDS_PHY);
  lvds_write(LVDS_1_PHY_CTL, LVDS_PHY);
  lvds_write(LVDS_CTL, LVDS_CTL_MODE(0) | LVDS_CTL_LINK(0) |
             LVDS_CTL_SYNC_MODE);
  return OK;
}

void d13x_lvds_enable(void)
{
  uint32_t reg;

  reg = getreg32(CMU_CLK_LVDS);
  reg |= MOD_RESET_DEASSERT | MOD_BUS_ENABLE | MOD_CLOCK_ENABLE;
  putreg32(reg, CMU_CLK_LVDS);

  reg = getreg32(D13X_LVDS_BASE + LVDS_CTL);
  putreg32(reg | LVDS_CTL_EN, D13X_LVDS_BASE + LVDS_CTL);
}

void d13x_lvds_disable(void)
{
  uint32_t reg = getreg32(D13X_LVDS_BASE + LVDS_CTL);
  putreg32(reg & ~LVDS_CTL_EN, D13X_LVDS_BASE + LVDS_CTL);
}
