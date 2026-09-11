/****************************************************************************
 * D13x home panel - Rooms page
 *
 * Extracted from home_panel_main.c.
 ****************************************************************************/

#include "home_ui_internal.h"

#define UI_SLOW_LOG_MS  26

static void style_dropdown_list(lv_event_t *event)
{
  lv_obj_t *dropdown = lv_event_get_target(event);
  lv_obj_t *list = lv_dropdown_get_list(dropdown);

  if (list == NULL)
    {
      return;
    }

  lv_obj_set_style_bg_color(list, lv_color_hex(COLOR_SURFACE_2),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(list, 1, LV_PART_MAIN);
  lv_obj_set_style_border_color(list, lv_color_hex(COLOR_BORDER),
                                LV_PART_MAIN);
  lv_obj_set_style_text_color(list, lv_color_hex(COLOR_TEXT),
                              LV_PART_MAIN);
  lv_obj_set_style_bg_color(list, lv_color_hex(THEME_COLOR_NAV_ACTIVE_BG),
                            LV_PART_SELECTED);
  lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_SELECTED);
  lv_obj_set_style_text_color(list, lv_color_hex(COLOR_TEXT),
                              LV_PART_SELECTED);
}

/* Forward declarations */

static void show_room_device_detail(unsigned int device_index);
static void render_room_devices(unsigned int room_index);

/****************************************************************************
 * Detail callbacks — delegate to g_ui.cb_* function pointers
 * (implementations provided by main.c at init time)
 ****************************************************************************/

void rooms_device_toggled(lv_event_t *event)
{
  if (g_ui.cb_device_toggled != NULL)
    {
      g_ui.cb_device_toggled(event);
    }
}

/****************************************************************************
 * Rooms page functions
 ****************************************************************************/

static bool device_in_room(const struct home_panel_device_s *device,
                           const struct home_panel_room_s *room)
{
  return device->room[0] != '\0' && room->name[0] != '\0' &&
         strcmp(device->room, room->name) == 0;
}

static void update_room_button_styles(void)
{
  if (g_ui.room_highlighted < g_ui.family_model->room_count &&
      g_ui.room_highlighted != g_ui.selected_room &&
      g_ui.room_buttons[g_ui.room_highlighted] != NULL)
    {
      lv_obj_set_style_bg_color(
        g_ui.room_buttons[g_ui.room_highlighted], lv_color_hex(COLOR_NAV), 0);
    }

  if (g_ui.selected_room < g_ui.family_model->room_count &&
      g_ui.room_buttons[g_ui.selected_room] != NULL &&
      g_ui.room_highlighted != g_ui.selected_room)
    {
      lv_obj_set_style_bg_color(g_ui.room_buttons[g_ui.selected_room],
                                lv_color_hex(COLOR_NAV_ACTIVE), 0);
    }

  g_ui.room_highlighted = g_ui.selected_room;
}

/* home_ui_make_room_device_container, home_ui_device_type_text
 * moved to ui/home_ui_components.c
 */

static void room_device_clicked(lv_event_t *event)
{
  uintptr_t device = (uintptr_t)lv_event_get_user_data(event);

  if (device > 0)
    {
      show_room_device_detail((unsigned int)device - 1);
    }
}

static void make_room_device_summary(lv_obj_t *parent,
                                     unsigned int column,
                                     unsigned int row,
                                     const struct home_panel_device_s *device,
                                     unsigned int device_index,
                                     lv_color_t accent)
{
  struct home_ui_device_binding_s *binding = NULL;
  lv_obj_t *card;
  lv_obj_t *label;
  lv_obj_t *value_label;
  lv_obj_t *state_label;
  unsigned int binding_index;
  char value[32];

  card = lv_button_create(parent);
  home_ui_configure_fast_button(card);
  lv_obj_set_height(card, ROOM_CARD_HEIGHT);
  lv_obj_set_grid_cell(card, LV_GRID_ALIGN_STRETCH, column, 1,
                       LV_GRID_ALIGN_START, row, 1);
  lv_obj_set_style_radius(card, THEME_RADIUS_CARD, 0);
  lv_obj_set_style_shadow_width(card, 0, 0);
  lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_SURFACE), 0);
  theme_apply_surface_gradient(card);
  lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_SURFACE_2),
                            LV_STATE_PRESSED);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_border_color(card, lv_color_hex(COLOR_BORDER), 0);
  theme_apply_card_state(card,
                         !device->online ? HOME_PANEL_CARD_OFFLINE :
                         (device->has_power && device->power) ?
                           HOME_PANEL_CARD_ACTIVE : HOME_PANEL_CARD_IDLE);
  lv_obj_set_style_pad_all(card, 0, 0);
  lv_obj_add_event_cb(card, room_device_clicked, LV_EVENT_CLICKED,
                      (void *)(uintptr_t)(device_index + 1));

  /* Device icon */

  home_ui_make_label(card, home_ui_device_symbol(device), 14, 12, accent,
             &lv_font_montserrat_16);

  /* Device name */

  label = home_ui_make_label(card, device->name, 44, 10,
                     lv_color_hex(COLOR_TEXT), home_panel_font_get());
  lv_obj_set_width(label, 128);
  lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);

  /* Arrow indicator */

  label = home_ui_make_label(card, LV_SYMBOL_RIGHT, 0, 10,
                             lv_color_hex(COLOR_MUTED),
                             &lv_font_montserrat_16);
  lv_obj_align(label, LV_ALIGN_TOP_RIGHT, -12, 0);

  /* Key value */

  home_ui_format_device_value(device, value, sizeof(value));
  value_label = home_ui_make_label(card, value, 14, 54, accent,
                           home_panel_font_get());

  /* State text */

  state_label = home_ui_make_label(card, home_ui_device_state_text(device), 14, 82,
                           lv_color_hex(device->online ?
                                        COLOR_SECONDARY : COLOR_MUTED),
                           home_panel_font_get());

  binding_index = g_ui.device_binding_counts[g_ui.current_page];
  if (binding_index < HOME_PANEL_MAX_DEVICES + HOME_CARD_COUNT)
    {
      binding = &g_ui.device_bindings[g_ui.current_page][binding_index];
      g_ui.device_binding_counts[g_ui.current_page]++;
      binding->device = device;
      binding->card = card;
      binding->button = NULL;
      binding->value_label = value_label;
      binding->state_label = state_label;
    }
}

static lv_obj_t *make_detail_metric(lv_obj_t *parent, int x, int y,
                                    const char *name, const char *value,
                                    lv_color_t accent)
{
  lv_obj_t *card = lv_obj_create(parent);

  lv_obj_set_pos(card, x, y);
  lv_obj_set_size(card, 180, 76);
  lv_obj_set_style_radius(card, THEME_RADIUS_CARD, 0);
  lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_SURFACE), 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  theme_apply_surface_gradient(card);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_border_color(card, lv_color_hex(COLOR_BORDER), 0);
  lv_obj_set_style_shadow_width(card, 0, 0);
  lv_obj_set_style_pad_all(card, 12, 0);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  home_ui_make_label(card, name, 0, 0, lv_color_hex(COLOR_MUTED),
             home_panel_font_get());
  return home_ui_make_label(card, value, 0, 30, accent, home_panel_font_get());
}

static void make_detail_control(lv_obj_t *parent, int y,
                                const struct home_panel_device_s *device,
                                const struct home_panel_control_s *property)
{
  struct home_ui_control_binding_s *binding;
  lv_obj_t *row;
  lv_obj_t *control;
  lv_obj_t *value_label;
  lv_obj_t *label;
  char value[24];

  if (g_ui.control_binding_count >= HOME_PANEL_MAX_CONTROLS)
    {
      return;
    }

  binding = &g_ui.control_bindings[g_ui.control_binding_count++];
  binding->device = device;
  binding->property = property;

  /* Control row container */

  row = lv_obj_create(parent);
  lv_obj_set_pos(row, 0, y);
  lv_obj_set_size(row, 586,
                  property->type == HOME_PANEL_CONTROL_NUMBER ? 100 : 68);
  lv_obj_set_style_radius(row, THEME_RADIUS_CARD, 0);
  lv_obj_set_style_bg_color(row, lv_color_hex(COLOR_SURFACE), 0);
  lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
  theme_apply_surface_gradient(row);
  lv_obj_set_style_border_width(row, 1, 0);
  lv_obj_set_style_border_color(row, lv_color_hex(COLOR_BORDER), 0);
  lv_obj_set_style_shadow_width(row, 0, 0);
  lv_obj_set_style_pad_all(row, 16, 0);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

  /* Control name */

  label = home_ui_make_label(row, home_ui_control_display_name(property->name), 0, 0,
                     lv_color_hex(COLOR_TEXT), home_panel_font_get());
  lv_obj_set_width(label, 360);
  lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);

  if (property->type == HOME_PANEL_CONTROL_BOOLEAN)
    {
      /* Boolean: status text + toggle */

      value_label = home_ui_make_label(row,
                               property->has_value ?
                                 (property->boolean_value ? "已开启" :
                                                            "已关闭") :
                                 "状态未知",
                               0, 32, lv_color_hex(COLOR_SECONDARY),
                               home_panel_font_get());
      control = lv_button_create(row);
      home_ui_configure_fast_button(control);
      lv_obj_set_size(control, 52, 32);
      lv_obj_align(control, LV_ALIGN_TOP_RIGHT, 0, 0);
      lv_obj_set_style_radius(control, THEME_RADIUS_CTRL, 0);
      lv_obj_set_style_shadow_width(control, 0, 0);
      lv_obj_set_style_border_width(control, 0, 0);
      lv_obj_set_style_bg_color(control, lv_color_hex(COLOR_SURFACE_2), 0);
      lv_obj_set_style_bg_opa(control, LV_OPA_COVER, 0);
      lv_obj_set_style_bg_color(control, lv_color_hex(COLOR_GREEN),
                                LV_STATE_CHECKED);
      lv_obj_set_style_bg_opa(control, LV_OPA_COVER, LV_STATE_CHECKED);
      lv_obj_set_style_bg_color(control, lv_color_hex(COLOR_SURFACE),
                                LV_STATE_DISABLED);
      lv_obj_set_style_bg_opa(control, LV_OPA_60, LV_STATE_DISABLED);
      lv_obj_set_ext_click_area(control, 10);
      lv_obj_add_flag(control, LV_OBJ_FLAG_CHECKABLE);
      if (property->boolean_value)
        {
          lv_obj_add_state(control, LV_STATE_CHECKED);
        }
      label = lv_label_create(control);
      lv_label_set_text(label, LV_SYMBOL_POWER);
      lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
      lv_obj_center(label);
      lv_obj_add_event_cb(control, g_ui.cb_detail_boolean_changed,
                          LV_EVENT_VALUE_CHANGED, binding);
    }
  else if (property->type == HOME_PANEL_CONTROL_NUMBER)
    {
      /* Number: value + slider */

      home_ui_format_control_value(property, property->value,
                           value, sizeof(value));
      value_label = home_ui_make_label(row, property->has_value ? value : "--",
                               0, 32, lv_color_hex(COLOR_SENSOR),
                               home_panel_font_get());
      control = lv_slider_create(row);
      lv_obj_set_pos(control, 0, 60);
      lv_obj_set_size(control, 554, 16);
      lv_slider_set_range(control, property->minimum, property->maximum);
      lv_slider_set_value(control, property->value, LV_ANIM_OFF);
      lv_obj_set_style_bg_color(control, lv_color_hex(COLOR_SURFACE_2),
                                LV_PART_MAIN);
      lv_obj_set_style_bg_color(control, lv_color_hex(COLOR_BLUE),
                                LV_PART_INDICATOR);
      lv_obj_set_style_bg_color(control, lv_color_hex(COLOR_TEXT),
                                LV_PART_KNOB);
      lv_obj_set_style_radius(control, THEME_RADIUS_CTRL, LV_PART_MAIN);
      lv_obj_set_style_radius(control, THEME_RADIUS_CTRL,
                              LV_PART_INDICATOR);
      lv_obj_set_style_radius(control, THEME_RADIUS_CTRL, LV_PART_KNOB);
      lv_obj_add_event_cb(control, g_ui.cb_detail_number_changed,
                          LV_EVENT_VALUE_CHANGED, binding);
      lv_obj_add_event_cb(control, g_ui.cb_detail_number_released,
                          LV_EVENT_RELEASED, binding);
    }
  else
    {
      char options[HOME_PANEL_MAX_OPTIONS * 28];
      unsigned int index;
      unsigned int selected = 0;
      size_t used = 0;

      options[0] = '\0';
      for (index = 0; index < property->option_count; index++)
        {
          int written = snprintf(options + used, sizeof(options) - used,
                                 "%s%s", index == 0 ? "" : "\n",
                                 property->options[index].label);
          if (written < 0 || (size_t)written >= sizeof(options) - used)
            {
              break;
            }
          used += (size_t)written;
          if (property->options[index].value == property->value)
            {
              selected = index;
            }
        }
      value_label = home_ui_make_label(row,
                               property->option_count > 0 ?
                                 property->options[selected].label : "--",
                               0, 32, lv_color_hex(COLOR_SENSOR),
                               home_panel_font_get());
      control = lv_dropdown_create(row);
      lv_obj_set_size(control, 210, 42);
      lv_obj_align(control, LV_ALIGN_TOP_RIGHT, 0, 0);
      lv_obj_set_style_bg_color(control, lv_color_hex(COLOR_SURFACE_2), 0);
      lv_obj_set_style_bg_opa(control, LV_OPA_COVER, 0);
      lv_obj_set_style_border_width(control, 1, 0);
      lv_obj_set_style_border_color(control, lv_color_hex(COLOR_BORDER), 0);
      lv_dropdown_set_options(control, options);
      lv_dropdown_set_selected(control, selected);
      lv_obj_set_style_text_font(control, home_panel_font_get(), 0);
      lv_obj_set_style_radius(control, THEME_RADIUS_CTRL, 0);
      lv_obj_add_event_cb(control, style_dropdown_list, LV_EVENT_READY, NULL);
      lv_obj_add_event_cb(control, g_ui.cb_detail_enum_changed,
                          LV_EVENT_VALUE_CHANGED, binding);
    }

  if (!device->online)
    {
      lv_obj_add_state(control, LV_STATE_DISABLED);
    }
  binding->control = control;
  binding->value_label = value_label;
}

void rooms_reset_detail_bindings(void)
{
  g_ui.selected_device = HOME_PANEL_MAX_DEVICES;
  g_ui.control_binding_count = 0;
  g_ui.action_binding_count = 0;
  memset(g_ui.control_bindings, 0, sizeof(g_ui.control_bindings));
  memset(g_ui.action_bindings, 0, sizeof(g_ui.action_bindings));
  g_ui.detail_subtitle_label = NULL;
  g_ui.detail_temperature_label = NULL;
  g_ui.detail_humidity_label = NULL;
  g_ui.detail_battery_label = NULL;
}

static void close_room_device_detail(lv_event_t *event)
{
  unsigned int room = g_ui.selected_room;

  (void)event;
  if (g_ui.room_detail_host != NULL)
    {
      lv_obj_delete(g_ui.room_detail_host);
      g_ui.room_detail_host = NULL;
    }
  rooms_reset_detail_bindings();
  g_ui.selected_room = HOME_PANEL_MAX_ROOMS;
  render_room_devices(room);
}

static void show_room_device_detail(unsigned int device_index)
{
  const struct home_panel_device_s *device;
  lv_obj_t *back;
  lv_obj_t *label;
  unsigned int metric = 0;
  unsigned int index;
  int y = 76;
  char value[32];
  char subtitle[96];

  if (device_index >= g_ui.family_model->device_count ||
      g_ui.room_device_host == NULL)
    {
      return;
    }

  if (g_ui.room_detail_host != NULL)
    {
      lv_obj_delete(g_ui.room_detail_host);
    }
  lv_obj_add_flag(g_ui.room_device_host, LV_OBJ_FLAG_HIDDEN);
  g_ui.room_detail_host = home_ui_make_room_device_container();
  rooms_reset_detail_bindings();
  g_ui.selected_device = device_index;
  device = &g_ui.family_model->devices[device_index];
  theme_create_device_ambient_mask(g_ui.room_detail_host, device->type);

  back = lv_button_create(g_ui.room_detail_host);
  home_ui_configure_fast_button(back);
  lv_obj_set_pos(back, 0, 0);
  lv_obj_set_size(back, 42, 42);
  lv_obj_set_ext_click_area(back, 5);
  lv_obj_set_style_radius(back, THEME_RADIUS_SM, 0);
  lv_obj_set_style_shadow_width(back, 0, 0);
  lv_obj_set_style_border_width(back, 1, 0);
  lv_obj_set_style_border_color(back, lv_color_hex(COLOR_BORDER), 0);
  lv_obj_set_style_bg_color(back, lv_color_hex(COLOR_SURFACE), 0);
  lv_obj_set_style_bg_opa(back, LV_OPA_COVER, 0);
  lv_obj_add_event_cb(back, close_room_device_detail, LV_EVENT_CLICKED, NULL);
  label = lv_label_create(back);
  lv_label_set_text(label, LV_SYMBOL_LEFT);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
  lv_obj_center(label);

  label = home_ui_make_label(g_ui.room_detail_host, device->name, 56, 0,
                     lv_color_hex(COLOR_TEXT), home_panel_font_get());
  lv_obj_set_width(label, 420);
  lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
  snprintf(subtitle, sizeof(subtitle), "%s · %s",
           home_ui_device_type_text(device), device->online ? "在线" : "离线");
  g_ui.detail_subtitle_label = home_ui_make_label(
    g_ui.room_detail_host, subtitle, 56, 32,
    lv_color_hex(device->online ? COLOR_SECONDARY : COLOR_MUTED),
    home_panel_font_get());

  if (device->has_temperature)
    {
      snprintf(value, sizeof(value), "%d°C", device->temperature);
      g_ui.detail_temperature_label = make_detail_metric(
        g_ui.room_detail_host, (int)metric++ * 196, y,
        "温度", value, lv_color_hex(COLOR_ORANGE));
    }
  if (device->has_humidity)
    {
      snprintf(value, sizeof(value), "%d%%", device->humidity);
      g_ui.detail_humidity_label = make_detail_metric(
        g_ui.room_detail_host, (int)metric++ * 196, y,
        "湿度", value, lv_color_hex(COLOR_BLUE));
    }
  if (device->has_battery && metric < 3)
    {
      snprintf(value, sizeof(value), "%d%%", device->battery);
      g_ui.detail_battery_label = make_detail_metric(
        g_ui.room_detail_host, (int)metric++ * 196, y,
        "电量", value, lv_color_hex(COLOR_GREEN));
    }

  for (index = 0; index < device->observable_count && metric < 9; index++)
    {
      const struct home_panel_observable_s *observable =
        &device->observables[index];
      bool standard =
        (observable->siid == device->temperature_siid &&
         observable->piid == device->temperature_piid) ||
        (observable->siid == device->humidity_siid &&
         observable->piid == device->humidity_piid) ||
        (observable->siid == device->battery_siid &&
         observable->piid == device->battery_piid) ||
        (observable->siid == device->power_siid &&
         observable->piid == device->power_piid);

      if (standard || !observable->has_value)
        {
          continue;
        }
      if (metric > 0 && metric % 3 == 0)
        {
          y += 90;
        }
      if (observable->text[0] != '\0')
        {
          snprintf(value, sizeof(value), "%s", observable->text);
        }
      else if (observable->type == HOME_PANEL_CONTROL_BOOLEAN)
        {
          snprintf(value, sizeof(value), "%s",
                   observable->boolean_value ? "是" : "否");
        }
      else
        {
          snprintf(value, sizeof(value), "%d", observable->value);
        }
      make_detail_metric(g_ui.room_detail_host, (int)(metric % 3) * 196, y,
                         home_ui_control_display_name(observable->name), value,
                         lv_color_hex(COLOR_SENSOR));
      metric++;
    }

  if (metric > 0)
    {
      y += 90;
    }

  for (index = 0; index < device->control_count; index++)
    {
      make_detail_control(g_ui.room_detail_host, y, device,
                          &device->controls[index]);
      y += device->controls[index].type == HOME_PANEL_CONTROL_NUMBER ?
           104 : 76;
    }

  for (index = 0; index < device->action_count &&
                  g_ui.action_binding_count < HOME_PANEL_MAX_ACTIONS; index++)
    {
      struct home_ui_action_binding_s *binding =
        &g_ui.action_bindings[g_ui.action_binding_count++];
      lv_obj_t *button;

      binding->device = device;
      binding->action = &device->actions[index];
      button = home_ui_make_action_button(g_ui.room_detail_host,
                                  home_ui_action_display_name(
                                    &device->actions[index]),
                                  0, y, 240);
      lv_obj_remove_event_cb(button, home_ui_action_clicked);
      lv_obj_add_event_cb(button, g_ui.cb_detail_action_clicked,
                          LV_EVENT_CLICKED, binding);
      if (!device->online)
        {
          lv_obj_add_state(button, LV_STATE_DISABLED);
        }
      y += 54;
    }

  if (metric == 0 && device->control_count == 0 &&
      device->action_count == 0)
    {
      label = home_ui_make_label(g_ui.room_detail_host, "该设备暂无可用控制项", 0, y,
                         lv_color_hex(COLOR_MUTED), home_panel_font_get());
      lv_obj_set_width(label, 586);
      lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    }

  home_ui_set_label_text_if_changed(g_ui.room_title_label, device->name);
  home_ui_set_label_text_if_changed(g_ui.room_summary_label, subtitle);
  g_ui.status_label = g_ui.room_summary_label;
  syslog(LOG_INFO, "[HOME][UI] device=%s type=%s controls=%u\n",
         device->name, device->type, device->control_count);
}

static lv_obj_t *create_room_device_view(unsigned int room_index)
{
  static const int32_t grid_columns[] =
  {
    LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST
  };
  static const int32_t grid_rows[] =
  {
    ROOM_CARD_HEIGHT, ROOM_CARD_HEIGHT, ROOM_CARD_HEIGHT, ROOM_CARD_HEIGHT,
    ROOM_CARD_HEIGHT, ROOM_CARD_HEIGHT, LV_GRID_TEMPLATE_LAST
  };
  static const uint32_t colors[] =
  {
    COLOR_ORANGE, COLOR_BLUE, COLOR_GREEN
  };
  const struct home_panel_room_s *room;
  lv_obj_t *host;
  unsigned int device_index;
  unsigned int visible = 0;

  if (room_index >= g_ui.family_model->room_count)
    {
      return NULL;
    }

  host = home_ui_make_room_device_container();
  if (host == NULL)
    {
      return NULL;
    }

  lv_obj_set_layout(host, LV_LAYOUT_GRID);
  lv_obj_set_grid_dsc_array(host, grid_columns, grid_rows);
  lv_obj_set_style_pad_column(host, THEME_CARD_GAP, 0);
  lv_obj_set_style_pad_row(host, 12, 0);

  room = &g_ui.family_model->rooms[room_index];
  for (device_index = 0;
       device_index < g_ui.family_model->device_count;
       device_index++)
    {
      const struct home_panel_device_s *device =
        &g_ui.family_model->devices[device_index];
      unsigned int column;
      unsigned int row;

      if (!device_in_room(device, room))
        {
          continue;
        }

      column = visible % 3;
      row = visible / 3;
      make_room_device_summary(
        host, column, row, device, device_index,
        lv_color_hex(colors[visible % 3]));
      visible++;
    }

  if (visible == 0)
    {
      lv_obj_t *empty = home_ui_make_label(host,
                                   "该房间暂无设备", 0, 24,
                                   lv_color_hex(COLOR_MUTED),
                                   home_panel_font_get());
      lv_obj_set_width(empty, 580);
      lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
    }

  g_ui.room_visible_counts[room_index] = visible;
  lv_obj_add_flag(host, LV_OBJ_FLAG_HIDDEN);
  return host;
}

static void render_room_devices(unsigned int room_index)
{
  const struct home_panel_room_s *room;
  bool reused;
  char summary[96];

  if (g_ui.family_model->room_count == 0)
    {
      return;
    }

  if (room_index >= g_ui.family_model->room_count)
    {
      room_index = 0;
    }

  if (g_ui.room_detail_host != NULL)
    {
      lv_obj_delete(g_ui.room_detail_host);
      g_ui.room_detail_host = NULL;
      rooms_reset_detail_bindings();
    }

  if (room_index == g_ui.selected_room &&
      g_ui.room_device_host != NULL &&
      g_ui.room_device_host == g_ui.room_device_hosts[room_index])
    {
      return;
    }

  if (g_ui.room_device_host != NULL)
    {
      lv_obj_add_flag(g_ui.room_device_host, LV_OBJ_FLAG_HIDDEN);
    }

  g_ui.selected_room = room_index;
  room = &g_ui.family_model->rooms[room_index];
  g_ui.room_device_host = g_ui.room_device_hosts[room_index];
  reused = g_ui.room_device_host != NULL;
  if (g_ui.room_device_host == NULL)
    {
      g_ui.room_device_host = create_room_device_view(room_index);
      g_ui.room_device_hosts[room_index] = g_ui.room_device_host;
    }

  if (g_ui.room_device_host == NULL)
    {
      return;
    }

  lv_obj_remove_flag(g_ui.room_device_host, LV_OBJ_FLAG_HIDDEN);
  home_ui_set_label_text_if_changed(g_ui.room_title_label, room->name);
  if (room->has_temperature && room->has_humidity)
    {
      snprintf(summary, sizeof(summary), "%u 台设备 · %d°C · 湿度 %d%%",
               room->device_count, room->temperature, room->humidity);
    }
  else
    {
      snprintf(summary, sizeof(summary), "%u 台设备",
               room->device_count);
    }
  home_ui_set_label_text_if_changed(g_ui.room_summary_label, summary);
  update_room_button_styles();
  syslog(LOG_INFO, "[HOME][UI] room=%s devices=%u cached=%u\n",
         room->name, g_ui.room_visible_counts[room_index], reused ? 1 : 0);
}

static void room_clicked(lv_event_t *event)
{
  uintptr_t room = (uintptr_t)lv_event_get_user_data(event);
  uint32_t started;
  uint32_t elapsed;

  if (room == 0)
    {
      return;
    }

  started = lv_tick_get();
  render_room_devices((unsigned int)room - 1);
  elapsed = lv_tick_elaps(started);
  if (elapsed >= UI_SLOW_LOG_MS)
    {
      syslog(LOG_WARNING,
             "[HOME][PERF] room-build index=%u elapsed=%ums\n",
             (unsigned int)room - 1, (unsigned int)elapsed);
    }
}

static void create_rooms_page(void)
{
  lv_obj_t *sidebar;
  lv_obj_t *button;
  lv_obj_t *label;
  unsigned int index;
  char count[24];

  memset(g_ui.room_buttons, 0, sizeof(g_ui.room_buttons));
  memset(g_ui.room_device_hosts, 0, sizeof(g_ui.room_device_hosts));
  memset(g_ui.room_visible_counts, 0, sizeof(g_ui.room_visible_counts));
  g_ui.room_highlighted = HOME_PANEL_MAX_ROOMS;
  g_ui.room_title_label = NULL;
  g_ui.room_summary_label = NULL;
  g_ui.room_device_host = NULL;
  g_ui.room_detail_host = NULL;
  rooms_reset_detail_bindings();

  sidebar = lv_obj_create(g_ui.content);
  lv_obj_remove_style_all(sidebar);
  lv_obj_set_pos(sidebar, 0, 0);
  lv_obj_set_size(sidebar, ROOM_NAV_WIDTH,
                  PANEL_HEIGHT - TOPBAR_HEIGHT);
  lv_obj_set_style_bg_color(sidebar, lv_color_hex(COLOR_NAV), 0);
  lv_obj_set_style_bg_opa(sidebar, LV_OPA_COVER, 0);
  theme_apply_nav_gradient(sidebar);
  lv_obj_set_style_pad_left(sidebar, 10, 0);
  lv_obj_set_style_pad_right(sidebar, 10, 0);
  lv_obj_set_style_pad_top(sidebar, 12, 0);
  lv_obj_set_style_pad_bottom(sidebar, 12, 0);
  lv_obj_set_scroll_dir(sidebar, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(sidebar, LV_SCROLLBAR_MODE_OFF);
  lv_obj_remove_flag(sidebar, LV_OBJ_FLAG_SCROLL_MOMENTUM |
                              LV_OBJ_FLAG_SCROLL_ELASTIC);

  home_ui_make_label(sidebar, "房间", 10, 4, lv_color_hex(COLOR_TEXT),
             home_panel_font_get());
  for (index = 0; index < g_ui.family_model->room_count; index++)
    {
      const struct home_panel_room_s *room = &g_ui.family_model->rooms[index];

      button = lv_button_create(sidebar);
      home_ui_configure_fast_button(button);
      g_ui.room_buttons[index] = button;
      lv_obj_set_pos(button, 0, 48 + (int)index * 58);
      lv_obj_set_size(button, ROOM_NAV_WIDTH - 20, 50);
      lv_obj_set_style_radius(button, THEME_RADIUS_CTRL, 0);
      lv_obj_set_style_shadow_width(button, 0, 0);
      lv_obj_set_style_border_width(button, 0, 0);
      lv_obj_set_style_bg_color(button, lv_color_hex(COLOR_NAV), 0);
      lv_obj_set_style_bg_color(button, lv_color_hex(COLOR_SURFACE_2),
                                LV_STATE_PRESSED);
      lv_obj_add_event_cb(button, room_clicked, LV_EVENT_CLICKED,
                          (void *)(uintptr_t)(index + 1));

      label = home_ui_make_label(button, room->name, 8, 4,
                         lv_color_hex(COLOR_MUTED),
                         home_panel_font_get());
      lv_obj_set_width(label, 112);
      lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
      snprintf(count, sizeof(count), "%u", room->device_count);
      label = home_ui_make_label(button, count, 132, 4,
                         lv_color_hex(COLOR_MUTED),
                         home_panel_font_get());
      lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, 0);
    }

  g_ui.room_title_label = theme_create_page_title(
    g_ui.content, "房间", ROOM_NAV_WIDTH + 16, 16);
  g_ui.room_summary_label = home_ui_make_label(g_ui.content,
                                    "按空间查看和控制设备",
                                    ROOM_NAV_WIDTH + 16, 50,
                                    lv_color_hex(COLOR_SECONDARY),
                                    home_panel_font_get());

  if (g_ui.family_model->room_count > 0)
    {
      if (g_ui.selected_room >= g_ui.family_model->room_count)
        {
          g_ui.selected_room = 0;
        }
      render_room_devices(g_ui.selected_room);
    }
  else
    {
      lv_label_set_text(g_ui.room_summary_label, "正在同步米家房间");
      g_ui.room_device_host = home_ui_make_room_device_container();
      label = home_ui_make_label(g_ui.room_device_host, "暂无房间数据", 0, 24,
                         lv_color_hex(COLOR_MUTED),
                         home_panel_font_get());
      lv_obj_set_width(label, 580);
      lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    }

  g_ui.status_label = g_ui.room_summary_label;
}

/****************************************************************************
 * Public API
 ****************************************************************************/

void rooms_page_create(void)
{
  create_rooms_page();
}

void rooms_page_update_button_styles(void)
{
  update_room_button_styles();
}

void rooms_page_render_devices(unsigned int room_index)
{
  render_room_devices(room_index);
}
