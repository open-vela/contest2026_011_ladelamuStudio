/****************************************************************************
 * vendor/artinchip/boards/d13x-hengshan-pi/src/artinchip_bringup.c
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <syslog.h>

#include <nuttx/i2c/i2c_master.h>
#include <nuttx/video/fb.h>

#include "artinchip_bringup.h"
#ifdef CONFIG_D13X_DISPLAY
#  include "artinchip_fb.h"
#endif

static void d13x_record_error(int *result, int ret)
{
  if (*result == OK && ret < 0)
    {
      *result = ret;
    }
}

int artinchip_bringup(void)
{
  int result = OK;
  int ret;

#ifdef CONFIG_D13X_I2C2
  FAR struct i2c_master_s *i2c = d13x_i2cbus_initialize(2);

  if (i2c == NULL)
    {
      syslog(LOG_ERR, "ERROR: failed to initialize I2C2\n");
      result = -ENODEV;
    }
#  ifdef CONFIG_I2C_DRIVER
  else
    {
      ret = i2c_register(i2c, 2);
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: failed to register /dev/i2c2: %d\n",
                 ret);
          d13x_record_error(&result, ret);
        }
    }
#  endif
#endif

#ifdef CONFIG_D13X_TOUCH_GT911
  ret = d13x_touch_gt911_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: failed to register GT911: %d\n", ret);
      d13x_record_error(&result, ret);
    }
  else
    {
      syslog(LOG_INFO, "GT911 registered as /dev/input0\n");
    }
#endif

#ifdef CONFIG_D13X_DISPLAY
  ret = fb_register(0, 0);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: failed to register framebuffer: %d\n", ret);
      d13x_record_error(&result, ret);
    }
  else
    {
      d13x_fb_show_colorbars();
      syslog(LOG_INFO, "LVDS framebuffer registered as /dev/fb0\n");
    }
#endif

  return result;
}
