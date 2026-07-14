/****************************************************************************
 * vendor/artinchip/chips/d13x/artinchip_fb.c
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/arch.h>
#include <nuttx/cache.h>
#include <nuttx/kmalloc.h>
#include <nuttx/video/fb.h>
#include <nuttx/wdog.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "artinchip_de.h"
#include "artinchip_lvds.h"
#include "artinchip_panel.h"

#define D13X_FB_WIDTH          1024u
#define D13X_FB_HEIGHT         600u
#define D13X_FB_BPP            16u
#define D13X_FB_STRIDE         (D13X_FB_WIDTH * 2u)
#define D13X_FB_FRAME_SIZE     (D13X_FB_STRIDE * D13X_FB_HEIGHT)
#define D13X_FB_BUFFERS        2u
#define D13X_FB_TOTAL_SIZE     (D13X_FB_FRAME_SIZE * D13X_FB_BUFFERS)
#define THEAD_MHCR_DCACHE_EN   (1u << 1)

struct d13x_fb_state_s
{
  bool initialized;
  bool power_on;
  uint8_t *memory;
  struct wdog_s pan_wdog;
};

static struct d13x_fb_state_s g_fb;
static void d13x_fb_pan_complete(wdparm_t arg);

static void d13x_fb_clean_cache(void)
{
  uint32_t mhcr;

  /* The custom D13x port does not expose NuttX ARCH_DCACHE hooks, while the
   * E907 may inherit an enabled write-back cache from the bootloader.  Use
   * the vendor's numeric CSR and raw dcache.call encoding so ordinary GCC
   * toolchains still make framebuffer writes visible to the DE DMA master.
   */

  __asm__ __volatile__("csrr %0, 0x7c1" : "=r"(mhcr));
  if ((mhcr & THEAD_MHCR_DCACHE_EN) != 0)
    {
      __asm__ __volatile__("fence rw, rw" ::: "memory");
      __asm__ __volatile__(".long 0x0010000b" ::: "memory");
      __asm__ __volatile__("fence rw, rw" ::: "memory");
    }
}

static int d13x_fb_getvideoinfo(struct fb_vtable_s *vtable,
                                struct fb_videoinfo_s *vinfo)
{
  (void)vtable;

  if (vinfo == NULL)
    {
      return -EINVAL;
    }

  vinfo->fmt = FB_FMT_RGB16_565;
  vinfo->xres = D13X_FB_WIDTH;
  vinfo->yres = D13X_FB_HEIGHT;
  vinfo->nplanes = 1;
  return OK;
}

static int d13x_fb_getplaneinfo(struct fb_vtable_s *vtable, int planeno,
                                struct fb_planeinfo_s *pinfo)
{
  (void)vtable;

  if (planeno != 0 || pinfo == NULL || !g_fb.initialized)
    {
      return -EINVAL;
    }

  pinfo->fbmem = g_fb.memory;
  pinfo->fblen = D13X_FB_TOTAL_SIZE;
  pinfo->stride = D13X_FB_STRIDE;
  pinfo->display = 0;
  pinfo->bpp = D13X_FB_BPP;
  pinfo->xres_virtual = D13X_FB_WIDTH;
  pinfo->yres_virtual = D13X_FB_HEIGHT * D13X_FB_BUFFERS;
  pinfo->xoffset = 0;
  pinfo->yoffset = 0;
  return OK;
}

static int d13x_fb_pandisplay(struct fb_vtable_s *vtable,
                              struct fb_planeinfo_s *pinfo)
{
  uintptr_t address;

  (void)vtable;

  if (pinfo == NULL || !g_fb.initialized ||
      (pinfo->yoffset != 0 && pinfo->yoffset != D13X_FB_HEIGHT))
    {
      return -EINVAL;
    }

  address = (uintptr_t)g_fb.memory + pinfo->yoffset * D13X_FB_STRIDE;
  d13x_fb_clean_cache();
  d13x_de_set_framebuffer(address);

  /* The DE switches buffers immediately and this port has no VSYNC IRQ.
   * Release the queued frame after FBIOPAN_DISPLAY has added it, otherwise
   * LVGL stops refreshing when both framebuffer slots are queued.
   */

  wd_start(&g_fb.pan_wdog, 1, d13x_fb_pan_complete, 0);
  return OK;
}

static int d13x_fb_getpower(struct fb_vtable_s *vtable)
{
  (void)vtable;
  return g_fb.power_on ? 1 : 0;
}

static int d13x_fb_setpower(struct fb_vtable_s *vtable, int power)
{
  (void)vtable;

  if (!g_fb.initialized)
    {
      return -ENODEV;
    }

  if (power > 0 && !g_fb.power_on)
    {
      d13x_lvds_enable();
      d13x_de_enable();
      d13x_panel_enable();
      g_fb.power_on = true;
    }
  else if (power <= 0 && g_fb.power_on)
    {
      d13x_panel_disable();
      d13x_de_disable();
      d13x_lvds_disable();
      g_fb.power_on = false;
    }

  return OK;
}

static struct fb_vtable_s g_fb_vtable =
{
  .getvideoinfo = d13x_fb_getvideoinfo,
  .getplaneinfo = d13x_fb_getplaneinfo,
  .pandisplay = d13x_fb_pandisplay,
  .getpower = d13x_fb_getpower,
  .setpower = d13x_fb_setpower,
};

static void d13x_fb_pan_complete(wdparm_t arg)
{
  (void)arg;

  while (fb_remove_paninfo(&g_fb_vtable, FB_NO_OVERLAY) == OK)
    {
    }
}

int up_fbinitialize(int display)
{
  uint32_t hactive;
  uint32_t vactive;
  uint32_t hfp;
  uint32_t hbp;
  uint32_t hsync;
  uint32_t vfp;
  uint32_t vbp;
  uint32_t vsync;
  uint32_t pixel_clock;
  int ret;

  if (display != 0)
    {
      return -ENODEV;
    }

  if (g_fb.initialized)
    {
      return OK;
    }

  g_fb.memory = kmm_memalign(64, D13X_FB_TOTAL_SIZE);
  if (g_fb.memory == NULL)
    {
      return -ENOMEM;
    }

  memset(g_fb.memory, 0, D13X_FB_TOTAL_SIZE);
  d13x_fb_clean_cache();

  d13x_panel_get_timing(&hactive, &vactive, &hfp, &hbp, &hsync,
                         &vfp, &vbp, &vsync, &pixel_clock);

  ret = d13x_panel_initialize();
  if (ret < 0)
    {
      goto errout;
    }

  ret = d13x_lvds_initialize(pixel_clock);
  if (ret < 0)
    {
      goto errout;
    }

  ret = d13x_de_initialize(hactive, vactive, hfp, hbp, hsync,
                           vfp, vbp, vsync, D13X_FB_STRIDE);
  if (ret < 0)
    {
      goto errout;
    }

  d13x_de_set_framebuffer((uintptr_t)g_fb.memory);
  d13x_lvds_enable();
  d13x_de_enable();
  up_mdelay(20);
  d13x_panel_enable();

  g_fb.power_on = true;
  g_fb.initialized = true;
  return OK;

errout:
  d13x_panel_disable();
  d13x_de_disable();
  d13x_lvds_disable();
  wd_cancel(&g_fb.pan_wdog);
  kmm_free(g_fb.memory);
  memset(&g_fb, 0, sizeof(g_fb));
  return ret;
}

struct fb_vtable_s *up_fbgetvplane(int display, int vplane)
{
  if (display != 0 || vplane != 0 || !g_fb.initialized)
    {
      return NULL;
    }

  return &g_fb_vtable;
}

void up_fbuninitialize(int display)
{
  if (display != 0 || !g_fb.initialized)
    {
      return;
    }

  d13x_panel_disable();
  d13x_de_disable();
  d13x_lvds_disable();
  wd_cancel(&g_fb.pan_wdog);
  kmm_free(g_fb.memory);
  memset(&g_fb, 0, sizeof(g_fb));
}
