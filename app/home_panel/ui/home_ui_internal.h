/****************************************************************************
 * D13x home panel - Internal UI state and helpers
 *
 * Included by all ui/*.c files. NOT included by main.c.
 * Contains: common includes, color/layout macros, helper declarations.
 ****************************************************************************/

#ifndef __HOME_UI_INTERNAL_H
#define __HOME_UI_INTERNAL_H

#include <nuttx/config.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include <lvgl/lvgl.h>

#include "../home_panel_font.h"
#include "../home_panel_theme.h"
#include "../home_panel_components.h"
#include "../home_panel_mijia_model.h"
#include "../home_panel_mijia_client.h"
#include "../home_panel_proactive.h"
#include "home_ui.h"

LV_FONT_DECLARE(home_panel_digits_28);

/* Layout constants (PANEL_WIDTH/HEIGHT/NAV_WIDTH/TOPBAR_HEIGHT in home_ui.h) */

#define ROOM_CARD_GAP       THEME_CARD_GAP

/* Color shortcuts (match main.c originals) */

#define COLOR_BG            THEME_COLOR_BG
#define COLOR_NAV           THEME_COLOR_NAV
#define COLOR_TOPBAR        THEME_COLOR_TOPBAR
#define COLOR_SURFACE       THEME_COLOR_SURFACE
#define COLOR_SURFACE_2     THEME_COLOR_SURFACE_RAISED
#define COLOR_BORDER        THEME_COLOR_DIVIDER
#define COLOR_TEXT          THEME_COLOR_TEXT_PRIMARY
#define COLOR_MUTED         THEME_COLOR_TEXT_MUTED
#define COLOR_SECONDARY     THEME_COLOR_TEXT_SECONDARY
#define COLOR_ORANGE        THEME_COLOR_SERVICE
#define COLOR_BLUE          THEME_COLOR_SENSOR
#define COLOR_GREEN         THEME_COLOR_ACTIVE
#define COLOR_WARNING       THEME_COLOR_WARNING
#define COLOR_RED           THEME_COLOR_ERROR
#define COLOR_NAV_ACTIVE    THEME_COLOR_NAV_ACTIVE_BG

/****************************************************************************
 * Helper functions - defined in home_ui_components.c, used by all pages
 ****************************************************************************/

bool home_ui_set_label_text_if_changed(lv_obj_t *label, const char *text);
void home_ui_configure_fast_button(lv_obj_t *button);
void home_ui_style_toggle(lv_obj_t *toggle, bool checked);
lv_obj_t *home_ui_make_label(lv_obj_t *parent, const char *text,
                             int x, int y, lv_color_t color,
                             const lv_font_t *font);
lv_obj_t *home_ui_make_action_button(lv_obj_t *parent, const char *text,
                                     int x, int y, int width);
void home_ui_style_secondary_action(lv_obj_t *button);
lv_obj_t *home_ui_make_info_row(lv_obj_t *parent, const char *name,
                                const char *value, int y, lv_color_t color);
void home_ui_set_chinese_font(lv_obj_t *obj);
void home_ui_action_clicked(lv_event_t *event);

/* Device display helpers */

const char *home_ui_device_type_text(const struct home_panel_device_s *device);
const char *home_ui_device_symbol(const struct home_panel_device_s *device);
const char *home_ui_device_state_text(const struct home_panel_device_s *device);
void home_ui_format_device_value(const struct home_panel_device_s *device,
                                 char *value, size_t capacity);
const char *home_ui_control_display_name(const char *name);
const char *home_ui_action_display_name(
  const struct home_panel_action_s *action);
void home_ui_format_control_value(const struct home_panel_control_s *property,
                                  int value, char *text, size_t capacity);
void home_ui_set_command_status(int ret);

/* Device card helpers for rooms */

lv_obj_t *home_ui_make_room_device_container(void);

/* Login helpers */

bool home_ui_login_update_qr(
  const struct home_panel_mijia_snapshot_s *snapshot);

#endif
