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

#include <debug.h>
#include <nuttx/arch.h>
#include <nuttx/i2c/i2c_master.h>
#include <nuttx/input/gt9xx.h>
#include <nuttx/irq.h>
#include <nuttx/signal.h>

#include "chip.h"
#include "include/irq.h"
#include "include/artinchip_i2c.h"
#include "artinchip_touch.h"

/* The working D13x demo88/kunlunpi88 targets use I2C2 on PA8/PA9 and
 * dedicate PA10/PA11 to GT911 reset/interrupt.
 */

#define GT911_I2C_BUS             2
#define GT911_ADDR_PRIMARY        0x5d
#define GT911_ADDR_FALLBACK       0x14
#define GT911_I2C_FREQUENCY       400000

#define GT911_GPIO_GROUP          0
#define GT911_I2C_SCL_PIN         8
#define GT911_I2C_SDA_PIN         9
#define GT911_RESET_PIN           10
#define GT911_INTERRUPT_PIN       11

#define D13X_GPIO_GROUP_STRIDE     0x100
#define D13X_GPIO_INPUT(group)     (D13X_GPIO_BASE + \
                                    (group) * D13X_GPIO_GROUP_STRIDE + 0x00)
#define D13X_GPIO_OUTPUT(group)    (D13X_GPIO_BASE + \
                                    (group) * D13X_GPIO_GROUP_STRIDE + 0x04)
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
#define D13X_PIN_INPUT             1
#define D13X_PIN_OUTPUT            2
#define D13X_PIN_IRQ_FALLING       0

#define GT911_PRODUCT_ID_REG       0x8140
#define GT911_CONFIG_REG           0x8047
#define GT911_CONFIG_DATA_SIZE     184

static xcpt_t g_gt911_isr;
static void *g_gt911_isr_arg;

/* The first 184 bytes are the GT911 configuration payload.  The final two
 * reference bytes are replaced with a freshly calculated checksum/update.
 */

static const uint8_t g_gt911_config[] =
{
  0x6b, 0x00, 0x04, 0x58, 0x02, 0x05, 0x0d, 0x00, 0x01, 0x0f, 0x28, 0x0f,
  0x50, 0x32, 0x03, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x8a, 0x2a, 0x0c, 0x45, 0x47, 0x0c, 0x08, 0x00, 0x00,
  0x00, 0x40, 0x03, 0x2c, 0x00, 0x01, 0x00, 0x00, 0x00, 0x03, 0x64, 0x32,
  0x00, 0x00, 0x00, 0x28, 0x64, 0x94, 0xd5, 0x02, 0x07, 0x00, 0x00, 0x04,
  0x95, 0x2c, 0x00, 0x8b, 0x34, 0x00, 0x82, 0x3f, 0x00, 0x7d, 0x4c, 0x00,
  0x7a, 0x5b, 0x00, 0x7a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x18, 0x16, 0x14, 0x12, 0x10, 0x0e, 0x0c, 0x0a,
  0x08, 0x06, 0x04, 0x02, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x16, 0x18,
  0x1c, 0x1d, 0x1e, 0x1f, 0x20, 0x21, 0x22, 0x24, 0x13, 0x12, 0x10, 0x0f,
  0x0a, 0x08, 0x06, 0x04, 0x02, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x79, 0x01,
};

static void d13x_gpio_config(uint8_t pin, uint8_t function,
                             uint8_t direction, uint8_t irq_mode)
{
  uintptr_t regaddr = D13X_GPIO_PIN_CONFIG(GT911_GPIO_GROUP, pin);
  uint32_t reg = getreg32(regaddr);

  reg &= ~(D13X_PIN_FUNCTION_MASK | D13X_PIN_DRIVE_MASK |
           D13X_PIN_PULL_MASK | D13X_PIN_IRQ_MODE_MASK |
           D13X_PIN_DIRECTION_MASK);
  reg |= function << D13X_PIN_FUNCTION_SHIFT;
  reg |= D13X_PIN_DRIVE_DEFAULT << D13X_PIN_DRIVE_SHIFT;
  reg |= D13X_PIN_PULL_DISABLED << D13X_PIN_PULL_SHIFT;
  reg |= irq_mode << D13X_PIN_IRQ_MODE_SHIFT;
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
                   D13X_PIN_INPUT, 0);
  d13x_gpio_config(GT911_I2C_SDA_PIN, D13X_PIN_FUNCTION_I2C2,
                   D13X_PIN_INPUT, 0);
}

static void gt911_reset(void)
{
  d13x_gpio_write(GT911_RESET_PIN, false);
  d13x_gpio_config(GT911_RESET_PIN, D13X_PIN_FUNCTION_GPIO,
                   D13X_PIN_OUTPUT, 0);
  nxsig_usleep(10000);

  d13x_gpio_write(GT911_INTERRUPT_PIN, false);
  d13x_gpio_config(GT911_INTERRUPT_PIN, D13X_PIN_FUNCTION_GPIO,
                   D13X_PIN_OUTPUT, 0);
  nxsig_usleep(2000);

  d13x_gpio_write(GT911_RESET_PIN, true);
  nxsig_usleep(5000);
  d13x_gpio_config(GT911_RESET_PIN, D13X_PIN_FUNCTION_GPIO,
                   D13X_PIN_INPUT, 0);

  nxsig_usleep(50000);
  d13x_gpio_config(GT911_INTERRUPT_PIN, D13X_PIN_FUNCTION_GPIO,
                   D13X_PIN_INPUT, D13X_PIN_IRQ_FALLING);
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

  int ret = I2C_TRANSFER(i2c, msgs, 2);
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

static int gt911_write_config(struct i2c_master_s *i2c, uint8_t addr)
{
  uint8_t packet[2 + GT911_CONFIG_DATA_SIZE + 2];
  struct i2c_msg_s msg;
  uint8_t checksum = 0;
  unsigned int i;
  int ret;

  packet[0] = GT911_CONFIG_REG >> 8;
  packet[1] = GT911_CONFIG_REG & 0xff;
  memcpy(&packet[2], g_gt911_config, GT911_CONFIG_DATA_SIZE);

  for (i = 0; i < GT911_CONFIG_DATA_SIZE; i++)
    {
      checksum += packet[2 + i];
    }

  packet[2 + GT911_CONFIG_DATA_SIZE] = (uint8_t)(~checksum + 1);
  packet[3 + GT911_CONFIG_DATA_SIZE] = 1;

  msg.frequency = GT911_I2C_FREQUENCY;
  msg.addr = addr;
  msg.flags = 0;
  msg.buffer = packet;
  msg.length = sizeof(packet);

  ret = I2C_TRANSFER(i2c, &msg, 1);
  if (ret != 1)
    {
      return ret < 0 ? ret : -EIO;
    }

  nxsig_usleep(50000);
  return OK;
}

static int gt911_gpio_isr(int irq, void *context, void *arg)
{
  putreg32(1u << GT911_INTERRUPT_PIN,
           D13X_GPIO_IRQ_STATUS(GT911_GPIO_GROUP));

  if (g_gt911_isr != NULL)
    {
      return g_gt911_isr(irq, context, g_gt911_isr_arg);
    }

  return OK;
}

static int gt911_irq_attach(const struct gt9xx_board_s *state,
                            xcpt_t isr, void *arg)
{
  int ret;

  g_gt911_isr = isr;
  g_gt911_isr_arg = arg;
  d13x_gpio_config(GT911_INTERRUPT_PIN, D13X_PIN_FUNCTION_GPIO,
                   D13X_PIN_INPUT, D13X_PIN_IRQ_FALLING);

  ret = irq_attach(D13X_IRQ_GPIO, gt911_gpio_isr, arg);
  if (ret < 0)
    {
      g_gt911_isr = NULL;
      g_gt911_isr_arg = NULL;
    }

  return ret;
}

static void gt911_irq_enable(const struct gt9xx_board_s *state, bool enable)
{
  uint32_t reg;

  reg = getreg32(D13X_GPIO_IRQ_ENABLE(GT911_GPIO_GROUP));
  if (enable)
    {
      putreg32(1u << GT911_INTERRUPT_PIN,
               D13X_GPIO_IRQ_STATUS(GT911_GPIO_GROUP));
      reg |= 1u << GT911_INTERRUPT_PIN;
      putreg32(reg, D13X_GPIO_IRQ_ENABLE(GT911_GPIO_GROUP));
      up_enable_irq(D13X_IRQ_GPIO);
    }
  else
    {
      reg &= ~(1u << GT911_INTERRUPT_PIN);
      putreg32(reg, D13X_GPIO_IRQ_ENABLE(GT911_GPIO_GROUP));
      putreg32(1u << GT911_INTERRUPT_PIN,
               D13X_GPIO_IRQ_STATUS(GT911_GPIO_GROUP));
    }
}

static int gt911_set_power(const struct gt9xx_board_s *state, bool on)
{
  /* The panel is board-powered.  Resetting it here would change the I2C
   * address after the board-level address-selection sequence.
   */

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

  ret = gt911_write_config(i2c, addr);
  if (ret < 0)
    {
      ierr("GT9xx configuration failed: %d\n", ret);
      return ret;
    }

  ret = gt9xx_register("/dev/input0", i2c, addr, &g_gt911_board);
  if (ret < 0)
    {
      ierr("GT9xx registration failed: %d\n", ret);
    }

  return ret;
}
