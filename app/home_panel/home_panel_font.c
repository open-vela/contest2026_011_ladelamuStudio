#include <nuttx/config.h>

#include <arch/board/board.h>

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include <lvgl/lvgl.h>

#include "home_panel_font.h"

LV_FONT_DECLARE(home_panel_misans_18);

#define HOME_FONT_DRIVE_LETTER  'F'
#define HOME_FONT_PATH          "F:MiSans-Regular-18-full.bin"
#define HOME_FONT_FILE_NAME     "MiSans-Regular-18-full.bin"
#define HOME_FONT_CACHE_BYTES   (16 * 1024)

struct home_font_file_s
{
  uint32_t position;
};

static lv_fs_drv_t g_font_fs;
static lv_font_t *g_runtime_font;
static const lv_font_t *g_home_font = &home_panel_misans_18;

static void *home_font_open(lv_fs_drv_t *drv, const char *path,
                            lv_fs_mode_t mode)
{
  struct home_font_file_s *file;

  (void)drv;
  if (mode != LV_FS_MODE_RD || strcmp(path, HOME_FONT_FILE_NAME) != 0)
    {
      return NULL;
    }

  file = malloc(sizeof(*file));
  if (file != NULL)
    {
      file->position = 0;
    }

  return file;
}

static lv_fs_res_t home_font_close(lv_fs_drv_t *drv, void *file_p)
{
  (void)drv;
  free(file_p);
  return LV_FS_RES_OK;
}

static lv_fs_res_t home_font_read(lv_fs_drv_t *drv, void *file_p,
                                  void *buffer, uint32_t bytes_to_read,
                                  uint32_t *bytes_read)
{
  struct home_font_file_s *file = file_p;
  uint32_t remaining;
  int ret;

  (void)drv;
  if (file == NULL || buffer == NULL || bytes_read == NULL)
    {
      return LV_FS_RES_INV_PARAM;
    }

  remaining = BOARD_FONT_FLASH_SIZE - file->position;
  if (bytes_to_read > remaining)
    {
      bytes_to_read = remaining;
    }

  if (bytes_to_read == 0)
    {
      *bytes_read = 0;
      return LV_FS_RES_OK;
    }

  ret = board_flash_read(BOARD_FONT_FLASH_OFFSET + file->position, buffer,
                         bytes_to_read);
  if (ret < 0)
    {
      *bytes_read = 0;
      return LV_FS_RES_HW_ERR;
    }

  file->position += bytes_to_read;
  *bytes_read = bytes_to_read;
  return LV_FS_RES_OK;
}

static lv_fs_res_t home_font_seek(lv_fs_drv_t *drv, void *file_p,
                                  uint32_t position,
                                  lv_fs_whence_t whence)
{
  struct home_font_file_s *file = file_p;
  uint32_t target;

  (void)drv;
  if (file == NULL)
    {
      return LV_FS_RES_INV_PARAM;
    }

  switch (whence)
    {
      case LV_FS_SEEK_SET:
        target = position;
        break;
      case LV_FS_SEEK_CUR:
        if (position > BOARD_FONT_FLASH_SIZE - file->position)
          {
            return LV_FS_RES_INV_PARAM;
          }

        target = file->position + position;
        break;
      case LV_FS_SEEK_END:
        if (position > BOARD_FONT_FLASH_SIZE)
          {
            return LV_FS_RES_INV_PARAM;
          }

        target = BOARD_FONT_FLASH_SIZE - position;
        break;
      default:
        return LV_FS_RES_INV_PARAM;
    }

  if (target > BOARD_FONT_FLASH_SIZE)
    {
      return LV_FS_RES_INV_PARAM;
    }

  file->position = target;
  return LV_FS_RES_OK;
}

static lv_fs_res_t home_font_tell(lv_fs_drv_t *drv, void *file_p,
                                  uint32_t *position)
{
  struct home_font_file_s *file = file_p;

  (void)drv;
  if (file == NULL || position == NULL)
    {
      return LV_FS_RES_INV_PARAM;
    }

  *position = file->position;
  return LV_FS_RES_OK;
}

int home_panel_font_initialize(void)
{
  uint8_t signature[4] = {0};
  int ret;

  ret = board_flash_read(BOARD_FONT_FLASH_OFFSET, signature,
                         sizeof(signature));
  if (ret < 0 || signature[0] != 0x30 || signature[1] != 0x00 ||
      signature[2] != 0x00 || signature[3] != 0x00)
    {
      syslog(LOG_ERR,
             "[HOME][FONT] invalid flash font ret=%d sig=%02x%02x%02x%02x\n",
             ret, signature[0], signature[1], signature[2], signature[3]);
      return ret < 0 ? ret : -EBADMSG;
    }

  lv_fs_drv_init(&g_font_fs);
  g_font_fs.letter = HOME_FONT_DRIVE_LETTER;
  g_font_fs.cache_size = HOME_FONT_CACHE_BYTES;
  g_font_fs.open_cb = home_font_open;
  g_font_fs.close_cb = home_font_close;
  g_font_fs.read_cb = home_font_read;
  g_font_fs.seek_cb = home_font_seek;
  g_font_fs.tell_cb = home_font_tell;
  lv_fs_drv_register(&g_font_fs);

  g_runtime_font = lv_binfont_create(HOME_FONT_PATH);
  if (g_runtime_font == NULL)
    {
      syslog(LOG_ERR, "[HOME][FONT] binary font initialization failed\n");
      return -EINVAL;
    }

  g_runtime_font->fallback = &home_panel_misans_18;

  g_home_font = g_runtime_font;
  syslog(LOG_INFO,
         "[HOME][FONT] full MiSans bitmap ready bytes=%u bpp=1\n",
         (unsigned int)BOARD_FONT_FLASH_SIZE);
  return 0;
}

const lv_font_t *home_panel_font_get(void)
{
  return g_home_font;
}
