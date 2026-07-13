/****************************************************************************
 * vendor/artinchip/chips/d13x/artinchip_i2c.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Polled DesignWare I2C master driver for D13x.
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>
#include <errno.h>

#include <debug.h>
#include <nuttx/arch.h>
#include <nuttx/i2c/i2c_master.h>
#include <nuttx/mutex.h>

#include "chip.h"
#include "include/artinchip_i2c.h"

/* D13x I2C registers. */

#define D13X_I2C_CTL                 0x000
#define D13X_I2C_TAR                 0x004
#define D13X_I2C_DATA_CMD            0x010
#define D13X_I2C_SS_SCL_HCNT         0x020
#define D13X_I2C_SS_SCL_LCNT         0x024
#define D13X_I2C_FS_SCL_HCNT         0x028
#define D13X_I2C_FS_SCL_LCNT         0x02c
#define D13X_I2C_SDA_HOLD            0x030
#define D13X_I2C_INTR_MASK           0x038
#define D13X_I2C_INTR_CLR            0x03c
#define D13X_I2C_INTR_RAW_STAT       0x040
#define D13X_I2C_ENABLE              0x048
#define D13X_I2C_ENABLE_STATUS       0x04c
#define D13X_I2C_STATUS              0x050
#define D13X_I2C_TX_ABRT_SOURCE      0x054
#define D13X_I2C_TXFLR               0x098
#define D13X_I2C_RXFLR               0x09c

#define D13X_I2C_CTL_MASTER          (1u << 0)
#define D13X_I2C_CTL_SLAVE_DISABLE   (1u << 1)
#define D13X_I2C_CTL_SPEED_SS        (1u << 4)
#define D13X_I2C_CTL_SPEED_FS        (2u << 4)
#define D13X_I2C_CTL_RESTART         (1u << 6)

#define D13X_I2C_CMD_READ            (1u << 8)
#define D13X_I2C_CMD_STOP            (1u << 9)
#define D13X_I2C_CMD_RESTART         (1u << 10)

#define D13X_I2C_INTR_TX_ABRT        (1u << 6)
#define D13X_I2C_INTR_STOP_DET       (1u << 9)

#define D13X_I2C_ENABLE_BIT          (1u << 0)
#define D13X_I2C_STATUS_ACTIVITY     (1u << 0)
#define D13X_I2C_STATUS_TFNF         (1u << 1)

#define D13X_I2C_CMU_BASE_OFFSET     0x960
#define D13X_I2C_CMU_BUS_ENABLE      (1u << 12)
#define D13X_I2C_CMU_RESET_RELEASE   (1u << 13)

#define D13X_I2C_INPUT_CLOCK         24000000u
#define D13X_I2C_MAX_FREQUENCY       400000u
#define D13X_I2C_TIMEOUT_US          100000u
#define D13X_I2C_INTR_CLEAR_ALL      0x7fffu

struct d13x_i2c_priv_s
{
  struct i2c_master_s dev;
  uintptr_t base;
  uint8_t bus;
  bool initialized;
  mutex_t lock;
};

static int d13x_i2c_transfer(struct i2c_master_s *dev,
                             struct i2c_msg_s *msgs, int count);

static const struct i2c_ops_s g_d13x_i2c_ops =
{
  .transfer = d13x_i2c_transfer,
};

static struct d13x_i2c_priv_s g_d13x_i2c[3] =
{
  {
    .dev = { .ops = &g_d13x_i2c_ops },
    .base = D13X_I2C0_BASE,
    .bus = 0,
    .lock = NXMUTEX_INITIALIZER,
  },
  {
    .dev = { .ops = &g_d13x_i2c_ops },
    .base = D13X_I2C1_BASE,
    .bus = 1,
    .lock = NXMUTEX_INITIALIZER,
  },
  {
    .dev = { .ops = &g_d13x_i2c_ops },
    .base = D13X_I2C2_BASE,
    .bus = 2,
    .lock = NXMUTEX_INITIALIZER,
  },
};

static inline uint32_t d13x_i2c_getreg(struct d13x_i2c_priv_s *priv,
                                       unsigned int offset)
{
  return getreg32(priv->base + offset);
}

static inline void d13x_i2c_putreg(struct d13x_i2c_priv_s *priv,
                                   unsigned int offset, uint32_t value)
{
  putreg32(value, priv->base + offset);
}

static int d13x_i2c_wait_mask(struct d13x_i2c_priv_s *priv,
                              unsigned int offset, uint32_t mask,
                              uint32_t value)
{
  unsigned int timeout = D13X_I2C_TIMEOUT_US;

  while ((d13x_i2c_getreg(priv, offset) & mask) != value)
    {
      if (timeout-- == 0)
        {
          return -ETIMEDOUT;
        }

      up_udelay(1);
    }

  return OK;
}

static int d13x_i2c_set_enable(struct d13x_i2c_priv_s *priv, bool enable)
{
  d13x_i2c_putreg(priv, D13X_I2C_ENABLE,
                  enable ? D13X_I2C_ENABLE_BIT : 0);

  return d13x_i2c_wait_mask(priv, D13X_I2C_ENABLE_STATUS,
                            D13X_I2C_ENABLE_BIT,
                            enable ? D13X_I2C_ENABLE_BIT : 0);
}

static int d13x_i2c_check_abort(struct d13x_i2c_priv_s *priv)
{
  uint32_t status = d13x_i2c_getreg(priv, D13X_I2C_INTR_RAW_STAT);

  if ((status & D13X_I2C_INTR_TX_ABRT) != 0)
    {
      uint32_t source = d13x_i2c_getreg(priv, D13X_I2C_TX_ABRT_SOURCE);

      d13x_i2c_putreg(priv, D13X_I2C_INTR_CLR,
                      D13X_I2C_INTR_TX_ABRT);
      ierr("I2C%d abort: 0x%08lx\n", priv->bus,
           (unsigned long)source);
      return -ENXIO;
    }

  return OK;
}

static int d13x_i2c_wait_tx_space(struct d13x_i2c_priv_s *priv)
{
  unsigned int timeout = D13X_I2C_TIMEOUT_US;
  int ret;

  while ((d13x_i2c_getreg(priv, D13X_I2C_STATUS) &
          D13X_I2C_STATUS_TFNF) == 0)
    {
      ret = d13x_i2c_check_abort(priv);
      if (ret < 0)
        {
          return ret;
        }

      if (timeout-- == 0)
        {
          return -ETIMEDOUT;
        }

      up_udelay(1);
    }

  return OK;
}

static int d13x_i2c_wait_rx_data(struct d13x_i2c_priv_s *priv)
{
  unsigned int timeout = D13X_I2C_TIMEOUT_US;
  int ret;

  while (d13x_i2c_getreg(priv, D13X_I2C_RXFLR) == 0)
    {
      ret = d13x_i2c_check_abort(priv);
      if (ret < 0)
        {
          return ret;
        }

      if (timeout-- == 0)
        {
          return -ETIMEDOUT;
        }

      up_udelay(1);
    }

  return OK;
}

static int d13x_i2c_set_frequency(struct d13x_i2c_priv_s *priv,
                                  uint32_t frequency)
{
  uint32_t ctl = D13X_I2C_CTL_MASTER |
                 D13X_I2C_CTL_SLAVE_DISABLE |
                 D13X_I2C_CTL_RESTART;

  if (frequency == 0)
    {
      frequency = D13X_I2C_MAX_FREQUENCY;
    }

  if (frequency <= 100000)
    {
      ctl |= D13X_I2C_CTL_SPEED_SS;

      /* Vendor HAL values for the fixed 24 MHz I2C input clock. */

      d13x_i2c_putreg(priv, D13X_I2C_SS_SCL_HCNT, 98);
      d13x_i2c_putreg(priv, D13X_I2C_SS_SCL_LCNT, 114);
    }
  else if (frequency <= D13X_I2C_MAX_FREQUENCY)
    {
      ctl |= D13X_I2C_CTL_SPEED_FS;
      d13x_i2c_putreg(priv, D13X_I2C_FS_SCL_HCNT, 16);
      d13x_i2c_putreg(priv, D13X_I2C_FS_SCL_LCNT, 33);
    }
  else
    {
      return -EINVAL;
    }

  d13x_i2c_putreg(priv, D13X_I2C_CTL, ctl);
  return OK;
}

static int d13x_i2c_hw_initialize(struct d13x_i2c_priv_s *priv)
{
  uintptr_t cmu_reg = D13X_CMU_BASE + D13X_I2C_CMU_BASE_OFFSET +
                      priv->bus * sizeof(uint32_t);
  uint32_t reg;
  int ret;

  reg = getreg32(cmu_reg);
  reg |= D13X_I2C_CMU_BUS_ENABLE | D13X_I2C_CMU_RESET_RELEASE;
  putreg32(reg, cmu_reg);

  ret = d13x_i2c_set_enable(priv, false);
  if (ret < 0)
    {
      return ret;
    }

  ret = d13x_i2c_set_frequency(priv, D13X_I2C_MAX_FREQUENCY);
  if (ret < 0)
    {
      return ret;
    }

  d13x_i2c_putreg(priv, D13X_I2C_SDA_HOLD, 10);
  d13x_i2c_putreg(priv, D13X_I2C_INTR_MASK, 0);
  d13x_i2c_putreg(priv, D13X_I2C_INTR_CLR,
                  D13X_I2C_INTR_CLEAR_ALL);

  ret = d13x_i2c_set_enable(priv, true);
  if (ret >= 0)
    {
      priv->initialized = true;
    }

  return ret;
}

static int d13x_i2c_validate_msgs(struct i2c_msg_s *msgs, int count)
{
  uint16_t addr;
  int i;

  if (msgs == NULL || count <= 0)
    {
      return -EINVAL;
    }

  addr = msgs[0].addr;
  for (i = 0; i < count; i++)
    {
      if (msgs[i].buffer == NULL || msgs[i].length <= 0 ||
          msgs[i].addr != addr || (msgs[i].flags & I2C_M_TEN) != 0)
        {
          return -EINVAL;
        }

      if (i == 0 && (msgs[i].flags & I2C_M_NOSTART) != 0)
        {
          return -EINVAL;
        }

      if (i > 0 && (msgs[i].flags & I2C_M_NOSTART) != 0 &&
          ((msgs[i].flags ^ msgs[i - 1].flags) & I2C_M_READ) != 0)
        {
          return -EINVAL;
        }

      if (i == count - 1 && (msgs[i].flags & I2C_M_NOSTOP) != 0)
        {
          return -ENOTSUP;
        }
    }

  return OK;
}

static int d13x_i2c_transfer_locked(struct d13x_i2c_priv_s *priv,
                                    struct i2c_msg_s *msgs, int count)
{
  uint32_t cmd;
  bool restart;
  bool stop;
  bool stop_message;
  int ret;
  int i;
  ssize_t j;

  ret = d13x_i2c_validate_msgs(msgs, count);
  if (ret < 0)
    {
      return ret;
    }

  ret = d13x_i2c_wait_mask(priv, D13X_I2C_STATUS,
                            D13X_I2C_STATUS_ACTIVITY, 0);
  if (ret < 0)
    {
      return ret;
    }

  ret = d13x_i2c_set_enable(priv, false);
  if (ret < 0)
    {
      return ret;
    }

  ret = d13x_i2c_set_frequency(priv, msgs[0].frequency);
  if (ret < 0)
    {
      goto out_reenable;
    }

  d13x_i2c_putreg(priv, D13X_I2C_TAR, msgs[0].addr);
  d13x_i2c_putreg(priv, D13X_I2C_INTR_CLR,
                  D13X_I2C_INTR_CLEAR_ALL);

  ret = d13x_i2c_set_enable(priv, true);
  if (ret < 0)
    {
      return ret;
    }

  for (i = 0; i < count; i++)
    {
      restart = i > 0 && (msgs[i].flags & I2C_M_NOSTART) == 0 &&
                (msgs[i - 1].flags & I2C_M_NOSTOP) != 0;
      stop_message = (msgs[i].flags & I2C_M_NOSTOP) == 0 &&
                     (i == count - 1 ||
                      (msgs[i + 1].flags & I2C_M_NOSTART) == 0);

      for (j = 0; j < msgs[i].length; j++)
        {
          stop = stop_message && j == msgs[i].length - 1;
          cmd = 0;

          if ((msgs[i].flags & I2C_M_READ) != 0)
            {
              cmd |= D13X_I2C_CMD_READ;
            }
          else
            {
              cmd |= msgs[i].buffer[j];
            }

          if (restart && j == 0)
            {
              cmd |= D13X_I2C_CMD_RESTART;
            }

          if (stop)
            {
              cmd |= D13X_I2C_CMD_STOP;
            }

          ret = d13x_i2c_wait_tx_space(priv);
          if (ret < 0)
            {
              goto out_abort;
            }

          d13x_i2c_putreg(priv, D13X_I2C_DATA_CMD, cmd);

          if ((msgs[i].flags & I2C_M_READ) != 0)
            {
              ret = d13x_i2c_wait_rx_data(priv);
              if (ret < 0)
                {
                  goto out_abort;
                }

              msgs[i].buffer[j] =
                d13x_i2c_getreg(priv, D13X_I2C_DATA_CMD) & 0xff;
            }
        }

      if (stop_message)
        {
          ret = d13x_i2c_wait_mask(priv, D13X_I2C_INTR_RAW_STAT,
                                   D13X_I2C_INTR_STOP_DET,
                                   D13X_I2C_INTR_STOP_DET);
          if (ret < 0)
            {
              goto out_abort;
            }

          ret = d13x_i2c_check_abort(priv);
          d13x_i2c_putreg(priv, D13X_I2C_INTR_CLR,
                          D13X_I2C_INTR_STOP_DET);
          if (ret < 0)
            {
              goto out_abort;
            }
        }
    }

  d13x_i2c_putreg(priv, D13X_I2C_INTR_CLR,
                  D13X_I2C_INTR_CLEAR_ALL);
  return count;

out_abort:
  d13x_i2c_set_enable(priv, false);
  d13x_i2c_putreg(priv, D13X_I2C_INTR_CLR,
                  D13X_I2C_INTR_CLEAR_ALL);

out_reenable:
  d13x_i2c_set_enable(priv, true);
  return ret;
}

static int d13x_i2c_transfer(struct i2c_master_s *dev,
                             struct i2c_msg_s *msgs, int count)
{
  struct d13x_i2c_priv_s *priv = (struct d13x_i2c_priv_s *)dev;
  int ret;

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!priv->initialized)
    {
      ret = d13x_i2c_hw_initialize(priv);
    }

  if (ret >= 0)
    {
      ret = d13x_i2c_transfer_locked(priv, msgs, count);
    }

  nxmutex_unlock(&priv->lock);
  return ret;
}

struct i2c_master_s *d13x_i2cbus_initialize(int bus)
{
  struct d13x_i2c_priv_s *priv;
  int ret;

  if (bus < 0 || bus >= 3)
    {
      return NULL;
    }

  priv = &g_d13x_i2c[bus];
  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return NULL;
    }

  if (!priv->initialized)
    {
      ret = d13x_i2c_hw_initialize(priv);
    }

  nxmutex_unlock(&priv->lock);
  return ret < 0 ? NULL : &priv->dev;
}
