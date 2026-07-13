/****************************************************************************
 * vendor/artinchip/boards/d13x-hengshan-pi/src/artinchip_bringup.h
 ****************************************************************************/

#ifndef __BOARDS_D13X_HENGSHAN_PI_SRC_ARTINCHIP_BRINGUP_H
#define __BOARDS_D13X_HENGSHAN_PI_SRC_ARTINCHIP_BRINGUP_H

#include <nuttx/config.h>

struct i2c_master_s;

int artinchip_bringup(void);

#ifdef CONFIG_D13X_I2C
struct i2c_master_s *d13x_i2cbus_initialize(int bus);
#endif

#ifdef CONFIG_D13X_TOUCH_GT911
int d13x_touch_gt911_initialize(void);
#endif

#endif
