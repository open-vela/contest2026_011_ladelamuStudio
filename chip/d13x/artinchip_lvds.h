/****************************************************************************
 * vendor/artinchip/chips/d13x/artinchip_lvds.h
 ****************************************************************************/

#ifndef __VENDOR_ARTINCHIP_CHIPS_D13X_ARTINCHIP_LVDS_H
#define __VENDOR_ARTINCHIP_CHIPS_D13X_ARTINCHIP_LVDS_H

#include <nuttx/config.h>
#include <stdint.h>

int d13x_lvds_initialize(uint32_t pixel_clock);
void d13x_lvds_enable(void);
void d13x_lvds_disable(void);

#endif
