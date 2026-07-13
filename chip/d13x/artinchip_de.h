/****************************************************************************
 * vendor/artinchip/chips/d13x/artinchip_de.h
 ****************************************************************************/

#ifndef __VENDOR_ARTINCHIP_CHIPS_D13X_ARTINCHIP_DE_H
#define __VENDOR_ARTINCHIP_CHIPS_D13X_ARTINCHIP_DE_H

#include <nuttx/config.h>
#include <stdint.h>

int d13x_de_initialize(uint32_t width, uint32_t height,
                       uint32_t hfp, uint32_t hbp, uint32_t hsync,
                       uint32_t vfp, uint32_t vbp, uint32_t vsync,
                       uint32_t stride);
void d13x_de_set_framebuffer(uintptr_t address);
void d13x_de_enable(void);
void d13x_de_disable(void);

#endif
