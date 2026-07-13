/****************************************************************************
 * vendor/artinchip/chips/d13x/artinchip_panel.h
 ****************************************************************************/

#ifndef __VENDOR_ARTINCHIP_CHIPS_D13X_ARTINCHIP_PANEL_H
#define __VENDOR_ARTINCHIP_CHIPS_D13X_ARTINCHIP_PANEL_H

#include <nuttx/config.h>
#include <stdint.h>

int d13x_panel_initialize(void);
void d13x_panel_enable(void);
void d13x_panel_disable(void);
void d13x_panel_get_timing(uint32_t *hactive, uint32_t *vactive,
                           uint32_t *hfp, uint32_t *hbp, uint32_t *hsync,
                           uint32_t *vfp, uint32_t *vbp, uint32_t *vsync,
                           uint32_t *pixel_clock);

#endif
