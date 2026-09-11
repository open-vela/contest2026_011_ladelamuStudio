/****************************************************************************
 * D13x home panel - Reusable UI component builders implementation
 ****************************************************************************/

#include "home_ui_internal.h"

/* Global UI state - shared across all ui/ modules */

struct home_ui_state_s g_ui;

/* Internal helpers */

static void fast_button_configure(lv_obj_t *button)
{
  static lv_style_transition_dsc_t trans;
  static const lv_style_prop_t props[] = {0};
  static bool initialized;

  if (!initialized)
    {
      lv_style_transition_dsc_init(&trans, props, NULL, 0, 0, NULL);
      initialized = true;
    }

  lv_obj_set_style_transition(button, &trans, 0);
  lv_obj_set_style_transition(button, &trans, LV_STATE_PRESSED);
  lv_obj_set_style_anim_duration(button, 0, 0);
  lv_obj_set_style_bg_color(button, lv_color_hex(COLOR_SURFACE_2), 0);
  lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(button, lv_color_hex(COLOR_BORDER), 0);
  lv_obj_set_style_shadow_width(button, 0, 0);
  lv_obj_set_style_transform_width(button, 0, LV_STATE_PRESSED);
  lv_obj_set_style_transform_height(button, 0, LV_STATE_PRESSED);
}

static bool set_label_text_if_changed(lv_obj_t *label, const char *text)
{
  const char *current;

  if (label == NULL || text == NULL)
    {
      return false;
    }

  current = lv_label_get_text(label);
  if (current == NULL || strcmp(current, text) != 0)
    {
      lv_label_set_text(label, text);
      return true;
    }

  return false;
}

static void free_component_context(lv_event_t *event)
{
  free(lv_event_get_user_data(event));
}

/* Card builders */

lv_obj_t *component_create_card(lv_obj_t *parent, int x, int y,
                                int width, int height)
{
  lv_obj_t *card = lv_obj_create(parent);

  lv_obj_set_pos(card, x, y);
  lv_obj_set_size(card, width, height);
  lv_obj_set_style_radius(card, THEME_RADIUS_CARD, 0);
  lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_SURFACE), 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  theme_apply_surface_gradient(card);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_border_color(card, lv_color_hex(COLOR_BORDER), 0);
  lv_obj_set_style_shadow_width(card, 0, 0);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  return card;
}

lv_obj_t *component_create_card_button(lv_obj_t *parent, int x, int y,
                                       int width, int height,
                                       lv_event_cb_t callback,
                                       void *user_data)
{
  lv_obj_t *card = lv_button_create(parent);

  fast_button_configure(card);
  lv_obj_set_pos(card, x, y);
  lv_obj_set_size(card, width, height);
  lv_obj_set_style_radius(card, THEME_RADIUS_CARD, 0);
  lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_SURFACE), 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  theme_apply_surface_gradient(card);
  lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_SURFACE_2),
                            LV_STATE_PRESSED);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_border_color(card, lv_color_hex(COLOR_BORDER), 0);
  lv_obj_set_style_shadow_width(card, 0, 0);
  if (callback != NULL)
    {
      lv_obj_add_event_cb(card, callback, LV_EVENT_CLICKED, user_data);
    }
  return card;
}

/* Label helpers */

lv_obj_t *component_create_label(lv_obj_t *parent, const char *text,
                                 int x, int y, lv_color_t color)
{
  lv_obj_t *label = lv_label_create(parent);

  lv_label_set_text(label, text);
  lv_obj_set_pos(label, x, y);
  lv_obj_set_style_text_font(label, home_panel_font_get(), 0);
  lv_obj_set_style_text_color(label, color, 0);
  return label;
}

lv_obj_t *component_create_label_font(lv_obj_t *parent, const char *text,
                                      int x, int y, lv_color_t color,
                                      const lv_font_t *font)
{
  lv_obj_t *label = lv_label_create(parent);

  lv_label_set_text(label, text);
  lv_obj_set_pos(label, x, y);
  lv_obj_set_style_text_font(label, font, 0);
  lv_obj_set_style_text_color(label, color, 0);
  return label;
}

/* Button builders */

lv_obj_t *component_create_button(lv_obj_t *parent, const char *text,
                                  int x, int y, int width, int height,
                                  lv_event_cb_t callback, void *user_data)
{
  lv_obj_t *button = lv_button_create(parent);
  lv_obj_t *label;

  fast_button_configure(button);
  lv_obj_set_pos(button, x, y);
  lv_obj_set_size(button, width, height);
  lv_obj_set_style_radius(button, THEME_RADIUS_CTRL, 0);
  lv_obj_set_style_shadow_width(button, 0, 0);
  lv_obj_set_style_border_width(button, 1, 0);
  lv_obj_set_style_border_color(button, lv_color_hex(0x7bb4ff), 0);
  lv_obj_set_style_bg_color(button, lv_color_hex(COLOR_BLUE), 0);
  lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(button, lv_color_hex(0x478ee8),
                            LV_STATE_PRESSED);
  if (callback != NULL)
    {
      lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user_data);
    }

  label = lv_label_create(button);
  lv_label_set_text(label, text);
  lv_obj_set_style_text_font(label, home_panel_font_get(), 0);
  lv_obj_center(label);
  return button;
}

lv_obj_t *component_create_icon_button(lv_obj_t *parent, const char *icon,
                                       int x, int y, int size,
                                       lv_event_cb_t callback,
                                       void *user_data)
{
  lv_obj_t *button = lv_button_create(parent);
  lv_obj_t *label;

  fast_button_configure(button);
  lv_obj_set_pos(button, x, y);
  lv_obj_set_size(button, size, size);
  lv_obj_set_style_radius(button, THEME_RADIUS_SM, 0);
  lv_obj_set_style_bg_color(button, lv_color_hex(COLOR_SURFACE_2), 0);
  lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(button, lv_color_hex(COLOR_BORDER),
                            LV_STATE_PRESSED);
  lv_obj_set_style_shadow_width(button, 0, 0);
  lv_obj_set_style_border_width(button, 0, 0);
  if (callback != NULL)
    {
      lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user_data);
    }

  label = lv_label_create(button);
  lv_label_set_text(label, icon);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
  lv_obj_center(label);
  return button;
}

/* Toggle/Switch */

lv_obj_t *component_create_toggle(lv_obj_t *parent, bool checked,
                                  int x, int y,
                                  lv_event_cb_t callback, void *user_data)
{
  lv_obj_t *toggle = lv_button_create(parent);
  lv_obj_t *label;

  fast_button_configure(toggle);
  lv_obj_set_pos(toggle, x, y);
  lv_obj_set_size(toggle, 52, 32);
  lv_obj_set_style_radius(toggle, THEME_RADIUS_CTRL, 0);
  lv_obj_set_style_shadow_width(toggle, 0, 0);
  lv_obj_set_style_border_width(toggle, 0, 0);
  lv_obj_set_style_bg_color(toggle, lv_color_hex(COLOR_SURFACE_2), 0);
  lv_obj_set_style_bg_opa(toggle, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(toggle, lv_color_hex(COLOR_GREEN),
                            LV_STATE_CHECKED);
  lv_obj_set_style_bg_opa(toggle, LV_OPA_COVER, LV_STATE_CHECKED);
  lv_obj_set_style_bg_color(toggle, lv_color_hex(COLOR_SURFACE),
                            LV_STATE_DISABLED);
  lv_obj_set_style_bg_opa(toggle, LV_OPA_60, LV_STATE_DISABLED);
  lv_obj_set_ext_click_area(toggle, 10);
  lv_obj_add_flag(toggle, LV_OBJ_FLAG_CHECKABLE);
  if (checked)
    {
      lv_obj_add_state(toggle, LV_STATE_CHECKED);
    }
  if (callback != NULL)
    {
      lv_obj_add_event_cb(toggle, callback, LV_EVENT_VALUE_CHANGED,
                          user_data);
    }

  label = lv_label_create(toggle);
  lv_label_set_text(label, LV_SYMBOL_POWER);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
  lv_obj_center(label);
  return toggle;
}

void component_update_toggle(lv_obj_t *toggle, bool checked)
{
  if (toggle == NULL)
    {
      return;
    }

  if (checked)
    {
      lv_obj_add_state(toggle, LV_STATE_CHECKED);
    }
  else
    {
      lv_obj_clear_state(toggle, LV_STATE_CHECKED);
    }
}

/* Slider */

lv_obj_t *component_create_slider(lv_obj_t *parent, int min, int max,
                                  int value, int x, int y, int width,
                                  lv_event_cb_t callback, void *user_data)
{
  lv_obj_t *slider = lv_slider_create(parent);

  lv_obj_set_pos(slider, x, y);
  lv_obj_set_size(slider, width, 16);
  lv_slider_set_range(slider, min, max);
  lv_slider_set_value(slider, value, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(slider, lv_color_hex(COLOR_SURFACE_2),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_color(slider, lv_color_hex(COLOR_BLUE),
                            LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(slider, lv_color_hex(COLOR_TEXT),
                            LV_PART_KNOB);
  lv_obj_set_style_radius(slider, THEME_RADIUS_CTRL, LV_PART_MAIN);
  lv_obj_set_style_radius(slider, THEME_RADIUS_CTRL, LV_PART_INDICATOR);
  lv_obj_set_style_radius(slider, THEME_RADIUS_CTRL, LV_PART_KNOB);
  if (callback != NULL)
    {
      lv_obj_add_event_cb(slider, callback, LV_EVENT_VALUE_CHANGED,
                          user_data);
    }
  return slider;
}

/* Navigation item */

lv_obj_t *component_create_nav_item(lv_obj_t *parent, const char *icon,
                                    const char *text, int x, int y,
                                    int width, int height,
                                    lv_event_cb_t callback, void *user_data)
{
  lv_obj_t *button = lv_button_create(parent);
  lv_obj_t *icon_label;
  lv_obj_t *text_label;
  lv_obj_t *indicator;

  fast_button_configure(button);
  lv_obj_set_pos(button, x, y);
  lv_obj_set_size(button, width, height);
  lv_obj_set_style_radius(button, THEME_RADIUS_CTRL, 0);
  lv_obj_set_style_bg_color(button, lv_color_hex(COLOR_NAV), 0);
  lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(button, 0, 0);
  lv_obj_set_style_shadow_width(button, 0, 0);
  if (callback != NULL)
    {
      lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user_data);
    }

  /* Active indicator */

  indicator = lv_obj_create(button);
  lv_obj_remove_style_all(indicator);
  lv_obj_set_pos(indicator, 0, 12);
  lv_obj_set_size(indicator, 3, 28);
  lv_obj_set_style_radius(indicator, 2, 0);
  lv_obj_set_style_bg_color(indicator, lv_color_hex(COLOR_GREEN), 0);
  lv_obj_set_style_bg_opa(indicator, LV_OPA_COVER, 0);
  lv_obj_add_flag(indicator, LV_OBJ_FLAG_HIDDEN);

  icon_label = lv_label_create(button);
  lv_label_set_text(icon_label, icon);
  lv_obj_set_pos(icon_label, 14, 16);
  lv_obj_set_style_text_font(icon_label, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(icon_label, lv_color_hex(COLOR_MUTED), 0);

  text_label = lv_label_create(button);
  lv_label_set_text(text_label, text);
  lv_obj_set_pos(text_label, 44, 15);
  lv_obj_set_style_text_font(text_label, home_panel_font_get(), 0);
  lv_obj_set_style_text_color(text_label, lv_color_hex(COLOR_MUTED), 0);

  /* Store indicator pointer in button's user data for later use */

  lv_obj_set_user_data(button, indicator);
  return button;
}

void component_set_nav_active(lv_obj_t *item, bool active)
{
  lv_obj_t *indicator;
  lv_obj_t *icon_label;
  lv_obj_t *text_label;

  if (item == NULL)
    {
      return;
    }

  indicator = lv_obj_get_user_data(item);
  lv_obj_set_style_bg_color(item,
                            lv_color_hex(active ?
                                         THEME_COLOR_NAV_ACTIVE_BG :
                                         COLOR_NAV), 0);

  /* Find icon and text labels (skip indicator which is first child) */

  icon_label = lv_obj_get_child(item, 1);
  text_label = lv_obj_get_child(item, 2);
  if (icon_label != NULL)
    {
      lv_obj_set_style_text_color(icon_label,
                                  lv_color_hex(active ?
                                               COLOR_GREEN :
                                               COLOR_MUTED), 0);
    }
  if (text_label != NULL)
    {
      lv_obj_set_style_text_color(text_label,
                                  lv_color_hex(active ?
                                               COLOR_TEXT :
                                               COLOR_MUTED), 0);
    }
  if (indicator != NULL)
    {
      if (active)
        {
          lv_obj_remove_flag(indicator, LV_OBJ_FLAG_HIDDEN);
        }
      else
        {
          lv_obj_add_flag(indicator, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* Device card */

static const char *device_state_text(const struct home_panel_device_s *device)
{
  if (!device->online)
    {
      return "离线";
    }
  if (device->has_power)
    {
      return device->power ? "已开启" : "已关闭";
    }
  return "状态已同步";
}

static const char *device_symbol(const struct home_panel_device_s *device)
{
  if (strcmp(device->type, "light") == 0)
    {
      return LV_SYMBOL_BULLET;
    }
  if (strcmp(device->type, "outlet") == 0)
    {
      return LV_SYMBOL_CHARGE;
    }
  return LV_SYMBOL_POWER;
}

static void format_device_value(const struct home_panel_device_s *device,
                                char *value, size_t capacity)
{
  if (device->has_brightness)
    {
      int span = device->brightness_max - device->brightness_min;
      int percent = span > 0 ?
        (device->brightness - device->brightness_min) * 100 / span : 0;
      snprintf(value, capacity, "%d%%", percent);
    }
  else if (device->has_temperature)
    {
      snprintf(value, capacity, "%d°C", device->temperature);
    }
  else if (device->has_humidity)
    {
      snprintf(value, capacity, "湿度 %d%%", device->humidity);
    }
  else if (device->has_battery)
    {
      snprintf(value, capacity, "%d%%", device->battery);
    }
  else
    {
      snprintf(value, capacity, "%s", device->online ? "在线" : "离线");
    }
}

component_device_card_t *component_create_device_card(
  lv_obj_t *parent, int x, int y, int width, int height,
  const struct home_panel_device_s *device,
  lv_event_cb_t toggle_callback, void *toggle_user_data)
{
  component_device_card_t *ctx;
  lv_obj_t *card;
  lv_obj_t *icon_bg;
  lv_color_t accent;
  char value[32];
  bool checked;

  ctx = malloc(sizeof(*ctx));
  if (ctx == NULL)
    {
      return NULL;
    }
  memset(ctx, 0, sizeof(*ctx));
  ctx->device = device;

  checked = device->has_power && device->power;
  accent = lv_color_hex(checked ? COLOR_GREEN : COLOR_SENSOR);

  format_device_value(device, value, sizeof(value));

  card = component_create_card(parent, x, y, width, height);
  ctx->card = card;
  theme_apply_card_state(card,
                         !device->online ? HOME_PANEL_CARD_OFFLINE :
                         checked ? HOME_PANEL_CARD_ACTIVE :
                                   HOME_PANEL_CARD_IDLE);
  lv_obj_add_event_cb(card, free_component_context, LV_EVENT_DELETE, ctx);

  /* Icon background */

  icon_bg = lv_obj_create(card);
  lv_obj_remove_style_all(icon_bg);
  lv_obj_set_pos(icon_bg, 0, 0);
  lv_obj_set_size(icon_bg, 36, 36);
  lv_obj_set_style_radius(icon_bg, THEME_RADIUS_SM, 0);
  lv_obj_set_style_bg_color(icon_bg, lv_color_hex(COLOR_SURFACE_2), 0);
  lv_obj_set_style_bg_opa(icon_bg, LV_OPA_COVER, 0);
  ctx->icon_label = component_create_label_font(
    icon_bg, device_symbol(device), 10, 10, accent,
    &lv_font_montserrat_16);

  /* Device name */

  ctx->name_label = component_create_label(card, device->name, 0, 44,
                                           lv_color_hex(COLOR_TEXT));
  lv_obj_set_width(ctx->name_label, width - 32);
  lv_label_set_long_mode(ctx->name_label, LV_LABEL_LONG_DOT);

  /* Key value */

  ctx->value_label = component_create_label_font(
    card, value, 0, 74, accent,
    device->has_brightness || device->has_temperature ||
    device->has_battery ? &home_panel_digits_28 :
                          home_panel_font_get());

  /* State text */

  ctx->state_label = component_create_label(
    card, device_state_text(device), 0, 120,
    lv_color_hex(device->online ? COLOR_SECONDARY : COLOR_MUTED));

  /* Toggle button */

  if (device->power_writable)
    {
      ctx->toggle = component_create_toggle(
        card, checked, width - 60, height - 44,
        toggle_callback, toggle_user_data);
      if (!device->online)
        {
          lv_obj_add_state(ctx->toggle, LV_STATE_DISABLED);
        }
    }

  return ctx;
}

void component_update_device_card(component_device_card_t *card,
                                  const struct home_panel_device_s *device)
{
  char value[32];
  lv_color_t accent;
  bool checked;

  if (card == NULL || device == NULL)
    {
      return;
    }

  card->device = device;
  checked = device->has_power && device->power;
  accent = lv_color_hex(checked ? COLOR_GREEN : COLOR_SENSOR);

  format_device_value(device, value, sizeof(value));

  set_label_text_if_changed(card->name_label, device->name);
  set_label_text_if_changed(card->value_label, value);
  set_label_text_if_changed(card->state_label, device_state_text(device));

  lv_obj_set_style_text_color(card->value_label, accent, 0);
  lv_obj_set_style_text_color(card->state_label,
                              lv_color_hex(device->online ?
                                           COLOR_SECONDARY :
                                           COLOR_MUTED), 0);
  theme_apply_card_state(card->card,
                         !device->online ? HOME_PANEL_CARD_OFFLINE :
                         checked ? HOME_PANEL_CARD_ACTIVE :
                                   HOME_PANEL_CARD_IDLE);

  if (card->toggle != NULL)
    {
      component_update_toggle(card->toggle, checked);
      if (!device->online)
        {
          lv_obj_add_state(card->toggle, LV_STATE_DISABLED);
        }
      else
        {
          lv_obj_clear_state(card->toggle, LV_STATE_DISABLED);
        }
    }
}

/* Scene card */

lv_obj_t *component_create_scene_card(lv_obj_t *parent, int x, int y,
                                      int width, int height,
                                      const struct home_panel_scene_s *scene,
                                      lv_color_t accent,
                                      lv_event_cb_t callback,
                                      void *user_data)
{
  lv_obj_t *card;
  lv_obj_t *indicator;
  lv_obj_t *icon_label;
  lv_obj_t *name_label;

  card = lv_button_create(parent);
  fast_button_configure(card);
  lv_obj_set_pos(card, x, y);
  lv_obj_set_size(card, width, height);
  lv_obj_set_style_radius(card, THEME_RADIUS_CARD, 0);
  lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_SURFACE), 0);
  theme_apply_surface_gradient(card);
  lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_SURFACE_2),
                            LV_STATE_PRESSED);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_border_color(card, lv_color_hex(COLOR_BORDER), 0);
  lv_obj_set_style_shadow_width(card, 0, 0);
  if (callback != NULL)
    {
      lv_obj_add_event_cb(card, callback, LV_EVENT_CLICKED, user_data);
    }

  /* Accent indicator */

  indicator = lv_obj_create(card);
  lv_obj_remove_style_all(indicator);
  lv_obj_set_pos(indicator, 0, 14);
  lv_obj_set_size(indicator, 3, height - 28);
  lv_obj_set_style_radius(indicator, 2, 0);
  lv_obj_set_style_bg_color(indicator, accent, 0);
  lv_obj_set_style_bg_opa(indicator, LV_OPA_COVER, 0);

  icon_label = lv_label_create(card);
  lv_label_set_text(icon_label, LV_SYMBOL_PLAY);
  lv_obj_set_pos(icon_label, 16, 24);
  lv_obj_set_style_text_font(icon_label, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(icon_label, accent, 0);

  name_label = lv_label_create(card);
  lv_label_set_text(name_label, scene->name);
  lv_obj_set_pos(name_label, 48, 22);
  lv_obj_set_style_text_font(name_label, home_panel_font_get(), 0);
  lv_obj_set_style_text_color(name_label, lv_color_hex(COLOR_TEXT), 0);

  return card;
}

/* Metric card */

lv_obj_t *component_create_metric_card(lv_obj_t *parent, int x, int y,
                                       int width, int height,
                                       const char *name, const char *value,
                                       lv_color_t accent)
{
  lv_obj_t *card;

  card = component_create_card(parent, x, y, width, height);
  component_create_label(card, name, 12, 10,
                         lv_color_hex(COLOR_MUTED));
  component_create_label_font(card, value, 12, 40, accent,
                              home_panel_font_get());
  return card;
}

/* Section title */

lv_obj_t *component_create_section_title(lv_obj_t *parent, const char *text,
                                         int x, int y)
{
  return component_create_label(parent, text, x, y,
                                lv_color_hex(COLOR_TEXT));
}

/* Status badge */

lv_obj_t *component_create_status_badge(lv_obj_t *parent, const char *text,
                                        int x, int y, lv_color_t color)
{
  return component_create_label(parent, text, x, y, color);
}

/* Empty state */

lv_obj_t *component_create_empty_state(lv_obj_t *parent, const char *text,
                                       int x, int y, int width)
{
  lv_obj_t *label;

  label = component_create_label(parent, text, x, y,
                                 lv_color_hex(COLOR_MUTED));
  lv_obj_set_width(label, width);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
  return label;
}

/* Dialog/Modal */

lv_obj_t *component_create_dialog(lv_obj_t *parent, int width, int height,
                                  const char *title)
{
  lv_obj_t *shade;
  lv_obj_t *dialog;
  lv_obj_t *title_label;

  shade = lv_obj_create(parent != NULL ? parent : lv_layer_top());
  lv_obj_remove_style_all(shade);
  lv_obj_set_size(shade, PANEL_WIDTH, PANEL_HEIGHT);
  lv_obj_set_pos(shade, 0, 0);
  theme_apply_scrim(shade);

  dialog = lv_obj_create(shade);
  lv_obj_set_size(dialog, width, height);
  lv_obj_center(dialog);
  lv_obj_set_style_radius(dialog, THEME_RADIUS_DIALOG, 0);
  lv_obj_set_style_border_width(dialog, 1, 0);
  lv_obj_set_style_border_color(dialog, lv_color_hex(COLOR_BORDER), 0);
  lv_obj_set_style_bg_color(dialog, lv_color_hex(COLOR_SURFACE), 0);
  lv_obj_set_style_bg_opa(dialog, LV_OPA_COVER, 0);
  theme_apply_surface_gradient(dialog);
  lv_obj_set_style_pad_all(dialog, 24, 0);
  lv_obj_clear_flag(dialog, LV_OBJ_FLAG_SCROLLABLE);

  title_label = component_create_label(dialog, title, 0, 8,
                                       lv_color_hex(COLOR_TEXT));
  lv_obj_set_width(title_label, width - 48);
  lv_obj_set_style_text_align(title_label, LV_TEXT_ALIGN_CENTER, 0);

  return shade;
}

void component_close_dialog(lv_obj_t *shade)
{
  if (shade != NULL)
    {
      lv_obj_delete(shade);
    }
}

/****************************************************************************
 * Shared helper functions (migrated from home_panel_main.c)
 ****************************************************************************/

bool home_ui_set_label_text_if_changed(lv_obj_t *label, const char *text)
{
  const char *current;

  if (label == NULL || text == NULL)
    {
      return false;
    }

  current = lv_label_get_text(label);
  if (current == NULL || strcmp(current, text) != 0)
    {
      lv_label_set_text(label, text);
      return true;
    }

  return false;
}

void home_ui_configure_fast_button(lv_obj_t *button)
{
  static lv_style_transition_dsc_t trans;
  static const lv_style_prop_t props[] = {0};
  static bool initialized;

  if (!initialized)
    {
      lv_style_transition_dsc_init(&trans, props, NULL, 0, 0, NULL);
      initialized = true;
    }

  lv_obj_set_style_transition(button, &trans, 0);
  lv_obj_set_style_transition(button, &trans, LV_STATE_PRESSED);
  lv_obj_set_style_anim_duration(button, 0, 0);
  /* Override the LVGL default theme, whose button surface is black on this
   * target.  Individual buttons can still replace the base color below. */
  lv_obj_set_style_bg_color(button, lv_color_hex(COLOR_SURFACE_2), 0);
  lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(button, lv_color_hex(COLOR_BORDER),
                            LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_STATE_PRESSED);
  lv_obj_set_style_text_color(button, lv_color_hex(COLOR_TEXT), 0);
  lv_obj_set_style_shadow_width(button, 0, 0);
  lv_obj_set_style_transform_width(button, 0, LV_STATE_PRESSED);
  lv_obj_set_style_transform_height(button, 0, LV_STATE_PRESSED);
}

void home_ui_style_toggle(lv_obj_t *toggle, bool checked)
{
  lv_obj_set_style_radius(toggle, THEME_RADIUS_CTRL, 0);
  lv_obj_set_style_shadow_width(toggle, 0, 0);
  lv_obj_set_style_border_width(toggle, 0, 0);
  lv_obj_set_style_bg_color(toggle, lv_color_hex(COLOR_SURFACE_2), 0);
  lv_obj_set_style_bg_opa(toggle, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(toggle, lv_color_hex(COLOR_GREEN),
                            LV_STATE_CHECKED);
  lv_obj_set_style_bg_opa(toggle, LV_OPA_COVER, LV_STATE_CHECKED);
  lv_obj_set_style_bg_color(toggle, lv_color_hex(COLOR_SURFACE),
                            LV_STATE_DISABLED);
  lv_obj_set_style_bg_opa(toggle, LV_OPA_60, LV_STATE_DISABLED);
  lv_obj_set_ext_click_area(toggle, 12);
  if (checked)
    {
      lv_obj_add_state(toggle, LV_STATE_CHECKED);
    }
  else
    {
      lv_obj_clear_state(toggle, LV_STATE_CHECKED);
    }
}

lv_obj_t *home_ui_make_label(lv_obj_t *parent, const char *text,
                             int x, int y, lv_color_t color,
                             const lv_font_t *font)
{
  lv_obj_t *label = lv_label_create(parent);

  lv_label_set_text(label, text);
  lv_obj_set_pos(label, x, y);
  lv_obj_set_style_text_color(label, color, 0);
  lv_obj_set_style_text_font(label, font, 0);
  return label;
}

void home_ui_set_chinese_font(lv_obj_t *obj)
{
  lv_obj_set_style_text_font(obj, home_panel_font_get(), 0);
}

void home_ui_action_clicked(lv_event_t *event)
{
  const char *action = lv_event_get_user_data(event);
  char message[72];

  snprintf(message, sizeof(message), "%s：操作已生效", action);
  if (g_ui.status_label != NULL)
    {
      lv_label_set_text(g_ui.status_label, message);
      lv_obj_set_style_text_color(g_ui.status_label,
                                  lv_color_hex(COLOR_GREEN), 0);
    }
}

lv_obj_t *home_ui_make_action_button(lv_obj_t *parent, const char *text,
                                     int x, int y, int width)
{
  lv_obj_t *button = lv_button_create(parent);
  lv_obj_t *label;

  home_ui_configure_fast_button(button);
  lv_obj_set_pos(button, x, y);
  lv_obj_set_size(button, width, THEME_MIN_TOUCH);
  lv_obj_set_style_radius(button, THEME_RADIUS_CTRL, 0);
  lv_obj_set_style_shadow_width(button, 0, 0);
  lv_obj_set_style_border_width(button, 1, 0);
  lv_obj_set_style_border_color(button, lv_color_hex(0x7bb4ff), 0);
  lv_obj_set_style_bg_color(button, lv_color_hex(COLOR_BLUE), 0);
  lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(button, lv_color_hex(0x478ee8),
                            LV_STATE_PRESSED);
  lv_obj_add_event_cb(button, home_ui_action_clicked, LV_EVENT_CLICKED, (void *)text);

  label = lv_label_create(button);
  lv_label_set_text(label, text);
  home_ui_set_chinese_font(label);
  lv_obj_center(label);
  return button;
}

void home_ui_style_secondary_action(lv_obj_t *button)
{
  lv_obj_set_style_bg_color(button, lv_color_hex(COLOR_SURFACE_2), 0);
  lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(button, lv_color_hex(COLOR_BORDER),
                            LV_STATE_PRESSED);
  lv_obj_set_style_border_color(button, lv_color_hex(COLOR_BORDER), 0);
  lv_obj_set_style_radius(button, THEME_RADIUS_CTRL, 0);
}

lv_obj_t *home_ui_make_info_row(lv_obj_t *parent, const char *name,
                                const char *value, int y, lv_color_t color)
{
  home_ui_make_label(parent, name, 32, y, lv_color_hex(COLOR_MUTED),
                     home_panel_font_get());
  return home_ui_make_label(parent, value, 238, y, color,
                            home_panel_font_get());
}

/****************************************************************************
 * Device display helpers
 ****************************************************************************/

static const struct home_panel_control_s *find_device_control(
  const struct home_panel_device_s *device, const char *name)
{
  unsigned int index;

  for (index = 0; index < device->control_count; index++)
    {
      if (strcmp(device->controls[index].name, name) == 0)
        {
          return &device->controls[index];
        }
    }

  return NULL;
}

const char *home_ui_control_display_name(const char *name)
{
  if (strcmp(name, "on") == 0)
    {
      return "电源";
    }
  if (strcmp(name, "brightness") == 0)
    {
      return "亮度";
    }
  if (strcmp(name, "volume") == 0)
    {
      return "音量";
    }
  if (strcmp(name, "mute") == 0)
    {
      return "静音";
    }
  if (strcmp(name, "microphone-mute") == 0)
    {
      return "麦克风静音";
    }
  if (strcmp(name, "target-temperature") == 0)
    {
      return "目标温度";
    }
  if (strcmp(name, "color-temperature") == 0)
    {
      return "色温";
    }
  if (strcmp(name, "physical-controls-locked") == 0)
    {
      return "童锁";
    }
  if (strcmp(name, "indicator-light-on") == 0)
    {
      return "指示灯";
    }
  if (strcmp(name, "alarm") == 0)
    {
      return "提示音";
    }
  if (strcmp(name, "sleep-mode") == 0)
    {
      return "睡眠模式";
    }
  if (strcmp(name, "no-disturb") == 0)
    {
      return "勿扰模式";
    }
  if (strcmp(name, "countdown-time") == 0)
    {
      return "定时关闭";
    }
  if (strcmp(name, "charging-protection-on") == 0)
    {
      return "充电保护";
    }
  if (strcmp(name, "power") == 0 || strcmp(name, "switch-status") == 0)
    {
      return "电源状态";
    }
  if (strcmp(name, "mode") == 0 || strcmp(name, "work-mode") == 0)
    {
      return "工作模式";
    }
  if (strcmp(name, "fan-level") == 0 || strcmp(name, "fan-speed") == 0)
    {
      return "风速";
    }
  if (strcmp(name, "oscillate") == 0 || strcmp(name, "swing-mode") == 0)
    {
      return "摆动";
    }
  if (strcmp(name, "door") == 0 || strcmp(name, "window") == 0)
    {
      return "门窗状态";
    }
  if (strcmp(name, "temperature") == 0)
    {
      return "温度";
    }
  if (strcmp(name, "humidity") == 0 ||
      strcmp(name, "relative-humidity") == 0)
    {
      return "湿度";
    }
  if (strcmp(name, "battery") == 0 ||
      strcmp(name, "battery-level") == 0)
    {
      return "电量";
    }

  return name;
}

const char *home_ui_option_display_name(const char *property_name,
                                        const char *label)
{
  (void)property_name;

  if (label == NULL || label[0] == '\0')
    {
      return "未设置";
    }
  if (strcmp(label, "Auto") == 0 || strcmp(label, "auto") == 0 ||
      strcmp(label, "Automatic") == 0)
    {
      return "自动";
    }
  if (strcmp(label, "Manual") == 0 || strcmp(label, "manual") == 0)
    {
      return "手动";
    }
  if (strcmp(label, "Low") == 0 || strcmp(label, "low") == 0)
    {
      return "低";
    }
  if (strcmp(label, "Medium") == 0 || strcmp(label, "medium") == 0 ||
      strcmp(label, "Mid") == 0)
    {
      return "中";
    }
  if (strcmp(label, "High") == 0 || strcmp(label, "high") == 0)
    {
      return "高";
    }
  if (strcmp(label, "Silent") == 0 || strcmp(label, "silent") == 0)
    {
      return "静音";
    }
  if (strcmp(label, "Strong") == 0 || strcmp(label, "strong") == 0)
    {
      return "强力";
    }
  if (strcmp(label, "On") == 0 || strcmp(label, "on") == 0)
    {
      return "开启";
    }
  if (strcmp(label, "Off") == 0 || strcmp(label, "off") == 0)
    {
      return "关闭";
    }
  if (strcmp(label, "Open") == 0 || strcmp(label, "open") == 0)
    {
      return "打开";
    }
  if (strcmp(label, "Close") == 0 || strcmp(label, "close") == 0)
    {
      return "关闭";
    }
  if (strcmp(label, "Cool") == 0 || strcmp(label, "cool") == 0)
    {
      return "制冷";
    }
  if (strcmp(label, "Heat") == 0 || strcmp(label, "heat") == 0)
    {
      return "制热";
    }
  if (strcmp(label, "Dry") == 0 || strcmp(label, "dry") == 0)
    {
      return "除湿";
    }
  if (strcmp(label, "Fan") == 0 || strcmp(label, "fan") == 0)
    {
      return "送风";
    }
  if (strcmp(label, "Sleep") == 0 || strcmp(label, "sleep") == 0)
    {
      return "睡眠";
    }
  if (strcmp(label, "Standard") == 0 || strcmp(label, "standard") == 0)
    {
      return "标准";
    }

  return label;
}

const char *home_ui_action_display_name(
  const struct home_panel_action_s *action)
{
  const char *name;

  if (action == NULL)
    {
      return "执行";
    }

  name = action->name;
  if (strcmp(name, "on") == 0 || strcmp(name, "turn-on") == 0 ||
      strcmp(name, "turn_on") == 0 || strcmp(action->display_name, "Turn On") == 0)
    {
      return "开启";
    }
  if (strcmp(name, "off") == 0 || strcmp(name, "turn-off") == 0 ||
      strcmp(name, "turn_off") == 0 || strcmp(action->display_name, "Turn Off") == 0)
    {
      return "关闭";
    }
  if (strcmp(name, "toggle") == 0 || strcmp(action->display_name, "Toggle") == 0)
    {
      return "切换";
    }
  if (strcmp(name, "open") == 0 || strcmp(action->display_name, "Open") == 0)
    {
      return "打开";
    }
  if (strcmp(name, "close") == 0 || strcmp(action->display_name, "Close") == 0)
    {
      return "关闭";
    }
  if (strcmp(name, "pause") == 0 || strcmp(action->display_name, "Pause") == 0)
    {
      return "暂停";
    }
  if (strcmp(name, "play") == 0 || strcmp(action->display_name, "Play") == 0)
    {
      return "播放";
    }
  if (strcmp(name, "stop") == 0 || strcmp(action->display_name, "Stop") == 0)
    {
      return "停止";
    }

  return action->display_name[0] != '\0' ? action->display_name : "执行";
}

void home_ui_format_control_value(const struct home_panel_control_s *property,
                                  int value, char *text, size_t capacity)
{
  if (strcmp(property->name, "brightness") == 0)
    {
      int span = property->maximum - property->minimum;
      int percent = span > 0 ?
        (value - property->minimum) * 100 / span : value;
      snprintf(text, capacity, "%d%%", percent);
    }
  else if (strcmp(property->name, "volume") == 0)
    {
      snprintf(text, capacity, "%d%%", value);
    }
  else if (strcmp(property->name, "target-temperature") == 0)
    {
      snprintf(text, capacity, "%d°C", value);
    }
  else if (strcmp(property->name, "color-temperature") == 0)
    {
      snprintf(text, capacity, "%d K", value);
    }
  else
    {
      snprintf(text, capacity, "%d", value);
    }
}

void home_ui_set_command_status(int ret)
{
  if (g_ui.status_label == NULL)
    {
      return;
    }

  lv_label_set_text(g_ui.status_label,
                    ret == 0 ? "指令已发送，等待设备确认" :
                    ret == -EBUSY ? "请等待上一个设备操作完成" :
                                    "设备操作发送失败");
  lv_obj_set_style_text_color(g_ui.status_label,
                              lv_color_hex(ret == 0 ? COLOR_BLUE :
                                                      COLOR_ORANGE), 0);
}

void home_ui_format_device_value(const struct home_panel_device_s *device,
                                 char *value, size_t capacity)
{
  const struct home_panel_control_s *volume;

  if (device->has_brightness)
    {
      int span = device->brightness_max - device->brightness_min;
      int percent = span > 0 ?
        (device->brightness - device->brightness_min) * 100 / span : 0;

      snprintf(value, capacity, "%d%%", percent);
    }
  else if (device->has_temperature)
    {
      snprintf(value, capacity, "%d°C", device->temperature);
    }
  else if (device->has_humidity)
    {
      snprintf(value, capacity, "湿度 %d%%", device->humidity);
    }
  else if (device->has_battery)
    {
      snprintf(value, capacity, "%d%%", device->battery);
    }
  else
    {
      volume = find_device_control(device, "volume");
      if (volume != NULL && volume->has_value)
        {
          snprintf(value, capacity, "音量 %d%%", volume->value);
        }
      else
        {
          snprintf(value, capacity, "%s",
                   device->online ? "在线" : "离线");
        }
    }
}

const char *home_ui_device_state_text(
  const struct home_panel_device_s *device)
{
  if (!device->online)
    {
      return "离线";
    }

  if (device->has_power)
    {
      return device->power ? "已开启" : "已关闭";
    }

  return "状态已同步";
}

const char *home_ui_device_symbol(const struct home_panel_device_s *device)
{
  if (strcmp(device->type, "light") == 0)
    {
      return LV_SYMBOL_BULLET;
    }
  if (strcmp(device->type, "outlet") == 0)
    {
      return LV_SYMBOL_CHARGE;
    }
  return LV_SYMBOL_POWER;
}

const char *home_ui_device_type_text(const struct home_panel_device_s *device)
{
  if (strcmp(device->type, "light") == 0)
    {
      return "灯光设备";
    }
  if (strcmp(device->type, "speaker") == 0)
    {
      return "智能音响";
    }
  if (strcmp(device->type, "environment-sensor") == 0)
    {
      return "温湿度传感器";
    }
  if (strcmp(device->type, "contact-sensor") == 0)
    {
      return "门窗传感器";
    }
  if (strcmp(device->type, "outlet") == 0)
    {
      return "智能插座";
    }
  if (strcmp(device->type, "heater") == 0)
    {
      return "取暖设备";
    }
  return "米家设备";
}

lv_obj_t *home_ui_make_room_device_container(void)
{
  lv_obj_t *host = lv_obj_create(g_ui.content);

  lv_obj_set_pos(host, 210, 92);
  lv_obj_set_size(host, 642, 418);
  lv_obj_set_style_radius(host, 0, 0);
  lv_obj_set_style_border_width(host, 0, 0);
  lv_obj_set_style_bg_color(host, lv_color_hex(COLOR_BG), 0);
  lv_obj_set_style_bg_opa(host, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(host, 10, 0);
  lv_obj_set_scroll_dir(host, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(host, LV_SCROLLBAR_MODE_OFF);
  lv_obj_remove_flag(host, LV_OBJ_FLAG_SCROLL_MOMENTUM |
                           LV_OBJ_FLAG_SCROLL_ELASTIC);
  lv_obj_set_scroll_snap_y(host, LV_SCROLL_SNAP_NONE);
  return host;
}

/****************************************************************************
 * Accessor functions (home_ui.h API)
 ****************************************************************************/

const struct home_panel_family_model_s *home_ui_get_family_model(void)
{
  return g_ui.family_model;
}

bool home_ui_is_family_model_valid(void)
{
  return g_ui.family_model_valid;
}

unsigned int home_ui_get_current_page(void)
{
  return g_ui.current_page;
}

unsigned int home_ui_get_selected_room(void)
{
  return g_ui.selected_room;
}

enum home_ui_network_state_e home_ui_get_network_state(void)
{
  return g_ui.network_state;
}

lv_obj_t *home_ui_get_status_label(void)
{
  return g_ui.status_label;
}

void home_ui_set_status_label(lv_obj_t *label)
{
  g_ui.status_label = label;
}
