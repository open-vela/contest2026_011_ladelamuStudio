/****************************************************************************
 * vendor/artinchip/boards/d13x-hengshan-pi/src/artinchip_touch.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Hengshan Pi GT911 board support.
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <syslog.h>

#include <debug.h>
#include <nuttx/arch.h>
#include <nuttx/i2c/i2c_master.h>
#include <nuttx/input/gt9xx.h>

#include "chip.h"
#include "include/artinchip_i2c.h"
#include "artinchip_touch.h"

/* The working D13x demo88/kunlunpi88 targets use I2C2 on PA8/PA9 and
 * dedicate PA10/PA11 to GT911 reset/interrupt.
 */

#define GT911_I2C_BUS              2
#define GT911_ADDR_PRIMARY         0x5d
#define GT911_ADDR_FALLBACK        0x14
#define GT911_I2C_FREQUENCY        400000

#define GT911_GPIO_GROUP           0
#define GT911_I2C_SCL_PIN          8
#define GT911_I2C_SDA_PIN          9
#define GT911_RESET_PIN            10
#define GT911_INTERRUPT_PIN        11

#define D13X_GPIO_GROUP_STRIDE     0x100
#define D13X_GPIO_INPUT(group)     (D13X_GPIO_BASE + \
                                    (group) * D13X_GPIO_GROUP_STRIDE + 0x00)
#define D13X_GPIO_IRQ_ENABLE(group) (D13X_GPIO_BASE + \
                                     (group) * D13X_GPIO_GROUP_STRIDE + 0x08)
#define D13X_GPIO_IRQ_STATUS(group) (D13X_GPIO_BASE + \
                                     (group) * D13X_GPIO_GROUP_STRIDE + 0x0c)
#define D13X_GPIO_OUTPUT_CLEAR(group) (D13X_GPIO_BASE + \
                                      (group) * D13X_GPIO_GROUP_STRIDE + 0x10)
#define D13X_GPIO_OUTPUT_SET(group) (D13X_GPIO_BASE + \
                                    (group) * D13X_GPIO_GROUP_STRIDE + 0x14)
#define D13X_GPIO_PIN_CONFIG(group, pin) (D13X_GPIO_BASE + \
                                         (group) * D13X_GPIO_GROUP_STRIDE + \
                                         0x80 + (pin) * 4)

#define D13X_PIN_FUNCTION_SHIFT    0
#define D13X_PIN_FUNCTION_MASK     (0x0f << D13X_PIN_FUNCTION_SHIFT)
#define D13X_PIN_DRIVE_SHIFT       4
#define D13X_PIN_DRIVE_MASK        (0x07 << D13X_PIN_DRIVE_SHIFT)
#define D13X_PIN_PULL_SHIFT        8
#define D13X_PIN_PULL_MASK         (0x03 << D13X_PIN_PULL_SHIFT)
#define D13X_PIN_IRQ_MODE_SHIFT    12
#define D13X_PIN_IRQ_MODE_MASK     (0x07 << D13X_PIN_IRQ_MODE_SHIFT)
#define D13X_PIN_DIRECTION_SHIFT   16
#define D13X_PIN_DIRECTION_MASK    (0x03 << D13X_PIN_DIRECTION_SHIFT)

#define D13X_PIN_FUNCTION_GPIO     1
#define D13X_PIN_FUNCTION_I2C2     4
#define D13X_PIN_DRIVE_DEFAULT     3
#define D13X_PIN_PULL_DISABLED     0
#define D13X_PIN_PULL_UP           3
#define D13X_PIN_INPUT             1
#define D13X_PIN_OUTPUT            2

#define GT911_PRODUCT_ID_REG       0x8140

static xcpt_t g_gt911_isr;
static void *g_gt911_isr_arg;
static volatile bool g_gt911_poll_enabled;
static bool g_gt911_int_high = true;

static void d13x_gpio_config(uint8_t pin, uint8_t function,
                             uint8_t direction, uint8_t pull)
{
  uintptr_t regaddr = D13X_GPIO_PIN_CONFIG(GT911_GPIO_GROUP, pin);
  uint32_t reg = getreg32(regaddr);

  reg &= ~(D13X_PIN_FUNCTION_MASK | D13X_PIN_DRIVE_MASK |
           D13X_PIN_PULL_MASK | D13X_PIN_IRQ_MODE_MASK |
           D13X_PIN_DIRECTION_MASK);
  reg |= function << D13X_PIN_FUNCTION_SHIFT;
  reg |= D13X_PIN_DRIVE_DEFAULT << D13X_PIN_DRIVE_SHIFT;
  reg |= pull << D13X_PIN_PULL_SHIFT;
  reg |= direction << D13X_PIN_DIRECTION_SHIFT;
  putreg32(reg, regaddr);
}

static void d13x_gpio_write(uint8_t pin, bool high)
{
  uintptr_t regaddr = high ? D13X_GPIO_OUTPUT_SET(GT911_GPIO_GROUP) :
                             D13X_GPIO_OUTPUT_CLEAR(GT911_GPIO_GROUP);

  putreg32(1u << pin, regaddr);
}

static void d13x_touch_pinmux(void)
{
  d13x_gpio_config(GT911_I2C_SCL_PIN, D13X_PIN_FUNCTION_I2C2,
                   D13X_PIN_INPUT, D13X_PIN_PULL_DISABLED);
  d13x_gpio_config(GT911_I2C_SDA_PIN, D13X_PIN_FUNCTION_I2C2,
                   D13X_PIN_INPUT, D13X_PIN_PULL_DISABLED);
}

static void gt911_reset(void)
{
  d13x_gpio_write(GT911_RESET_PIN, false);
  d13x_gpio_config(GT911_RESET_PIN, D13X_PIN_FUNCTION_GPIO,
                   D13X_PIN_OUTPUT, D13X_PIN_PULL_DISABLED);
  up_mdelay(10);

  d13x_gpio_write(GT911_INTERRUPT_PIN, false);
  d13x_gpio_config(GT911_INTERRUPT_PIN, D13X_PIN_FUNCTION_GPIO,
                   D13X_PIN_OUTPUT, D13X_PIN_PULL_DISABLED);
  up_mdelay(2);

  d13x_gpio_write(GT911_RESET_PIN, true);
  up_mdelay(5);
  d13x_gpio_config(GT911_RESET_PIN, D13X_PIN_FUNCTION_GPIO,
                   D13X_PIN_INPUT, D13X_PIN_PULL_DISABLED);

  up_mdelay(50);
  d13x_gpio_config(GT911_INTERRUPT_PIN, D13X_PIN_FUNCTION_GPIO,
                   D13X_PIN_INPUT, D13X_PIN_PULL_UP);
  up_mdelay(100);
}

static int gt911_read_reg(struct i2c_master_s *i2c, uint8_t addr,
                          uint16_t reg, uint8_t *buffer, size_t length)
{
  uint8_t regbuf[2] = { reg >> 8, reg & 0xff };
  struct i2c_msg_s msgs[2] =
  {
    {
      .frequency = GT911_I2C_FREQUENCY,
      .addr = addr,
      .flags = I2C_M_NOSTOP,
      .buffer = regbuf,
      .length = sizeof(regbuf),
    },
    {
      .frequency = GT911_I2C_FREQUENCY,
      .addr = addr,
      .flags = I2C_M_READ,
      .buffer = buffer,
      .length = length,
    },
  };
  int ret;

  ret = I2C_TRANSFER(i2c, msgs, 2);
  return ret == 2 ? OK : (ret < 0 ? ret : -EIO);
}

static int gt911_probe(struct i2c_master_s *i2c, uint8_t *addr)
{
  static const uint8_t addresses[] =
  {
    GT911_ADDR_PRIMARY,
    GT911_ADDR_FALLBACK,
  };
  uint8_t product_id[4];
  unsigned int i;
  int ret = -ENODEV;

  for (i = 0; i < sizeof(addresses); i++)
    {
      ret = gt911_read_reg(i2c, addresses[i], GT911_PRODUCT_ID_REG,
                           product_id, sizeof(product_id));
      if (ret >= 0 && memcmp(product_id, "\0\0\0\0", 4) != 0)
        {
          *addr = addresses[i];
          iinfo("GT9xx address=0x%02x id=%c%c%c%c\n", *addr,
                product_id[0], product_id[1], product_id[2], product_id[3]);
          return OK;
        }
    }

  return ret < 0 ? ret : -ENODEV;
}

static int gt911_irq_attach(const struct gt9xx_board_s *state,
                            xcpt_t isr, void *arg)
{
  (void)state;
  g_gt911_isr = isr;
  g_gt911_isr_arg = arg;
  return OK;
}

static void gt911_irq_enable(const struct gt9xx_board_s *state, bool enable)
{
  (void)state;
  g_gt911_int_high = true;
  g_gt911_poll_enabled = enable;
}

void d13x_touch_poll(void)
{
  bool high;

  high = (getreg32(D13X_GPIO_INPUT(GT911_GPIO_GROUP)) &
          (1u << GT911_INTERRUPT_PIN)) != 0;

  if (g_gt911_poll_enabled && g_gt911_int_high && !high &&
      g_gt911_isr != NULL)
    {
      g_gt911_isr(0, NULL, g_gt911_isr_arg);
    }

  g_gt911_int_high = high;
}

static int gt911_set_power(const struct gt9xx_board_s *state, bool on)
{
  /* The panel is board-powered.  Resetting it here would change the I2C
   * address after the board-level address-selection sequence.
   */

  (void)state;
  (void)on;
  return OK;
}

static const struct gt9xx_board_s g_gt911_board =
{
  .irq_attach = gt911_irq_attach,
  .irq_enable = gt911_irq_enable,
  .set_power = gt911_set_power,
};

int d13x_touch_gt911_initialize(void)
{
  struct i2c_master_s *i2c;
  uint8_t addr;
  int ret;

  d13x_touch_pinmux();

  /* GPIO/CLIC task return is not stable yet.  Keep the hardware GPIO IRQ
   * masked and detect the GT911 falling edge from the idle task instead.
   */

  putreg32(getreg32(D13X_GPIO_IRQ_ENABLE(GT911_GPIO_GROUP)) &
           ~(1u << GT911_INTERRUPT_PIN),
           D13X_GPIO_IRQ_ENABLE(GT911_GPIO_GROUP));
  putreg32(1u << GT911_INTERRUPT_PIN,
           D13X_GPIO_IRQ_STATUS(GT911_GPIO_GROUP));

  i2c = d13x_i2cbus_initialize(GT911_I2C_BUS);
  if (i2c == NULL)
    {
      return -ENODEV;
    }

  gt911_reset();
  ret = gt911_probe(i2c, &addr);
  if (ret < 0)
    {
      ierr("GT9xx probe failed: %d\n", ret);
      return ret;
    }

  /* Retain the panel vendor configuration.  Loading a generic table at
   * every boot can change resolution, axis mapping, or sensor tuning.
   */

  ret = gt9xx_register("/dev/input0", i2c, addr, &g_gt911_board);
  if (ret < 0)
    {
      ierr("GT9xx registration failed: %d\n", ret);
    }
  else
    {
      syslog(LOG_INFO, "[D13TOUCH] ready addr=0x%02x dev=/dev/input0\n",
             addr);
    }

  return ret;
}
