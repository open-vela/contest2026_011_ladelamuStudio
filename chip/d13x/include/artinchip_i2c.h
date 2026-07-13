/****************************************************************************
 * vendor/artinchip/chips/d13x/include/artinchip_i2c.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __VENDOR_ARTINCHIP_CHIPS_D13X_INCLUDE_ARTINCHIP_I2C_H
#define __VENDOR_ARTINCHIP_CHIPS_D13X_INCLUDE_ARTINCHIP_I2C_H

#include <nuttx/config.h>

struct i2c_master_s;

struct i2c_master_s *d13x_i2cbus_initialize(int bus);

#endif
