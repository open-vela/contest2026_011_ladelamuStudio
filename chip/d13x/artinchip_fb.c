/****************************************************************************
 * vendor/artinchip/chips/d13x/artinchip_fb.c
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/arch.h>
#include <nuttx/cache.h>
#include <nuttx/kmalloc.h>
#include <nuttx/video/fb.h>

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
#define D13X_CACHE_LINE_SIZE   32u
#define D13X_CACHE_RANGE_LIMIT (64u * 1024u)
#define THEAD_MHCR_DCACHE_EN   (1u << 1)

struct d13x_fb_state_s
{
  bool initialized;
  bool power_on;
  uint8_t *memory;
};

static struct d13x_fb_state_s g_fb;

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

static void d13x_fb_clean_cache_area(const void *address, size_t row_bytes,
                                     size_t stride, size_t row_count)
{
  uintptr_t row;
  uint32_t mhcr;
  size_t index;

  if (row_bytes == 0 || row_count == 0)
    {
      return;
    }

  /* A cache-wide clean is bounded by the cache capacity and is faster than
   * issuing tens of thousands of per-line operations for a large redraw.
   */

  if (row_bytes * row_count >= D13X_CACHE_RANGE_LIMIT)
    {
      d13x_fb_clean_cache();
      return;
    }

  __asm__ __volatile__("csrr %0, 0x7c1" : "=r"(mhcr));
  if ((mhcr & THEAD_MHCR_DCACHE_EN) == 0)
    {
      return;
    }

  __asm__ __volatile__("fence" ::: "memory");
  row = (uintptr_t)address;
  for (index = 0; index < row_count; index++)
    {
      uintptr_t current = row & ~(D13X_CACHE_LINE_SIZE - 1u);
      uintptr_t end = (row + row_bytes + D13X_CACHE_LINE_SIZE - 1u) &
                      ~(D13X_CACHE_LINE_SIZE - 1u);

      while (current < end)
        {
          register uintptr_t cache_address __asm__("a0") = current;

          /* T-Head dcache.cpa cleans one physical cache line. */

          __asm__ __volatile__(".long 0x0295000b" : "+r"(cache_address) ::
                               "memory");
          current += D13X_CACHE_LINE_SIZE;
        }

      row += stride;
    }

  __asm__ __volatile__("fence\n\t"
                       "fence.i\n\t"
                       ".long 0x01a0000b" ::: "memory");
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

  /* Both LVGL draw buffers are complete physical framebuffer pages and LVGL
   * redraws the full page before publishing it.  Clean the complete cache so
   * all framebuffer writes are visible to the DE DMA master.
   */

  d13x_fb_clean_cache();
  d13x_de_set_framebuffer(address);
  return OK;
}

#ifdef CONFIG_FB_SYNC
static int d13x_fb_waitforvsync(struct fb_vtable_s *vtable)
{
  int ret;

  ret = d13x_de_wait_for_vsync();

  /* Release the queued page after its frame boundary. */
  fb_remove_paninfo(vtable, FB_NO_OVERLAY);

  return ret;
}
#endif

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
#ifdef CONFIG_FB_SYNC
  .waitforvsync = d13x_fb_waitforvsync,
#endif
  .getpower = d13x_fb_getpower,
  .setpower = d13x_fb_setpower,
};

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
#ifdef CONFIG_FB_SYNC
  d13x_de_uninitialize_vsync();
#endif
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
#ifdef CONFIG_FB_SYNC
  d13x_de_uninitialize_vsync();
#endif
  kmm_free(g_fb.memory);
  memset(&g_fb, 0, sizeof(g_fb));
}
