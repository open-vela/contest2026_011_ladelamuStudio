/****************************************************************************
 * D13x home panel - Home page (family overview + device cards)
 ****************************************************************************/

#include "home_ui_internal.h"

/* Internal helpers */

static int device_card_score(const struct home_panel_device_s *device)
{
  if (!device->online || !device->power_writable)
    {
      return -1;
    }

  if (strcmp(device->type, "light") == 0)
    {
      return 100;
    }
  if (strcmp(device->type, "switch") == 0)
    {
      return 80;
    }
  if (strcmp(device->type, "outlet") == 0)
    {
      return 60;
    }
  return 20;
}

static unsigned int select_device_cards(unsigned int selected[3])
{
  bool used[HOME_PANEL_MAX_DEVICES] = {false};
  unsigned int count = 0;

  while (count < 3)
    {
      int best_score = -1;
      unsigned int best = 0;
      unsigned int index;

      for (index = 0; index < g_ui.family_model->device_count; index++)
        {
          int score;

          if (used[index])
            {
              continue;
            }
          score = device_card_score(&g_ui.family_model->devices[index]);
          if (score > best_score)
            {
              best_score = score;
              best = index;
            }
        }

      if (best_score < 0)
        {
          break;
        }
      used[best] = true;
      selected[count++] = best;
    }

  return count;
}

static void make_device_card(lv_obj_t *parent, int x, int y,
                             int width, int height, const char *symbol,
                             const struct home_panel_device_s *device,
                             lv_color_t accent,
                             lv_event_cb_t device_toggle_cb)
{
  lv_obj_t *card = lv_obj_create(parent);
  lv_obj_t *toggle;
  lv_obj_t *state;
  lv_obj_t *label;
  lv_obj_t *value_label;
  struct home_ui_device_binding_s *binding;
  unsigned int binding_index;
  const lv_font_t *value_font;
  char value[32];
  const char *state_text;
  bool checked = device->has_power && device->power;

  home_ui_format_device_value(device, value, sizeof(value));
  value_font = device->has_brightness || device->has_temperature ||
               device->has_battery ? &home_panel_digits_28 :
                                     home_panel_font_get();

  lv_obj_set_pos(card, x, y);
  lv_obj_set_size(card, width, height);
  lv_obj_set_style_radius(card, THEME_RADIUS_CARD, 0);
  lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_SURFACE), 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  theme_apply_surface_gradient(card);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_border_color(card, lv_color_hex(COLOR_BORDER), 0);
  theme_apply_card_state(card,
                         !device->online ? HOME_PANEL_CARD_OFFLINE :
                         checked ? HOME_PANEL_CARD_ACTIVE :
                                   HOME_PANEL_CARD_IDLE);
  lv_obj_set_style_shadow_width(card, 0, 0);
  lv_obj_set_style_pad_all(card, 16, 0);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

  label = lv_obj_create(card);
  lv_obj_remove_style_all(label);
  lv_obj_set_pos(label, 0, 0);
  lv_obj_set_size(label, 36, 36);
  lv_obj_set_style_radius(label, THEME_RADIUS_SM, 0);
  lv_obj_set_style_bg_color(label, lv_color_hex(COLOR_SURFACE_2), 0);
  lv_obj_set_style_bg_opa(label, LV_OPA_COVER, 0);
  home_ui_make_label(label, symbol, 10, 10, accent, &lv_font_montserrat_16);

  label = home_ui_make_label(card, device->name, 0, 44,
                             lv_color_hex(COLOR_TEXT), home_panel_font_get());
  lv_obj_set_width(label, width - 32);
  lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);

  value_label = home_ui_make_label(card, value, 0, 74, accent, value_font);

  state_text = home_ui_device_state_text(device);
  state = home_ui_make_label(card, state_text, 0, 120,
                             lv_color_hex(device->online ?
                                          COLOR_SECONDARY : COLOR_MUTED),
                             home_panel_font_get());

  binding_index = g_ui.device_binding_counts[g_ui.current_page];
  if (binding_index >= HOME_PANEL_MAX_DEVICES + HOME_CARD_COUNT)
    {
      return;
    }
  binding = &g_ui.device_bindings[g_ui.current_page][binding_index];
  g_ui.device_binding_counts[g_ui.current_page]++;
  binding->device = device;
  binding->card = card;
  binding->button = NULL;
  binding->value_label = value_label;
  binding->state_label = state;

  if (!device->power_writable)
    {
      return;
    }

  toggle = lv_button_create(card);
  home_ui_configure_fast_button(toggle);
  lv_obj_set_size(toggle, 48, 28);
  lv_obj_set_ext_click_area(toggle, 12);
  lv_obj_align(toggle, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
  lv_obj_set_style_radius(toggle, THEME_RADIUS_CTRL, 0);
  lv_obj_set_style_shadow_width(toggle, 0, 0);
  lv_obj_set_style_border_width(toggle, 0, 0);
  lv_obj_set_style_bg_color(toggle,
                            lv_color_hex(checked ?
                                         COLOR_GREEN :
                                         COLOR_SURFACE_2), 0);
  lv_obj_set_style_bg_opa(toggle, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(toggle, lv_color_hex(COLOR_GREEN),
                            LV_STATE_CHECKED);
  lv_obj_set_style_bg_opa(toggle, LV_OPA_COVER, LV_STATE_CHECKED);
  lv_obj_set_style_bg_color(toggle, lv_color_hex(COLOR_SURFACE),
                            LV_STATE_DISABLED);
  lv_obj_set_style_bg_opa(toggle, LV_OPA_60, LV_STATE_DISABLED);
  lv_obj_add_flag(toggle, LV_OBJ_FLAG_CHECKABLE);
  if (checked)
    {
      lv_obj_add_state(toggle, LV_STATE_CHECKED);
    }
  if (!device->online)
    {
      lv_obj_add_state(toggle, LV_STATE_DISABLED);
    }

  label = lv_label_create(toggle);
  lv_label_set_text(label, LV_SYMBOL_POWER);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
  lv_obj_center(label);
  binding->button = toggle;
  if (device_toggle_cb != NULL)
    {
      lv_obj_add_event_cb(toggle, device_toggle_cb, LV_EVENT_VALUE_CHANGED,
                          binding);
    }
}

/* Public API */

void home_page_create(lv_event_cb_t device_toggle_cb,
                      lv_event_cb_t proactive_feedback_cb)
{
  const struct home_panel_room_s *environment = NULL;
  unsigned int selected[HOME_CARD_COUNT];
  unsigned int selected_count;
  unsigned int index;
  char subtitle[96];
  char status[64];
  char online_text[32];
  lv_obj_t *top_row;
  lv_obj_t *right_col;
  lv_obj_t *bottom_section;
  lv_obj_t *device_row;

  lv_obj_set_layout(g_ui.content, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(g_ui.content, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(g_ui.content, THEME_CONTENT_PAD, 0);
  lv_obj_set_style_pad_row(g_ui.content, THEME_CARD_GAP, 0);
  lv_obj_clear_flag(g_ui.content, LV_OBJ_FLAG_SCROLLABLE);

  theme_create_page_title(g_ui.content, "晚上好，欢迎回家", 0, 0);

  for (index = 0; index < g_ui.family_model->room_count; index++)
    {
      if (g_ui.family_model->rooms[index].has_temperature)
        {
          environment = &g_ui.family_model->rooms[index];
          if (strcmp(environment->name, "客厅") == 0)
            {
              break;
            }
        }
    }
  if (environment != NULL)
    {
      snprintf(subtitle, sizeof(subtitle), "%s %d°C  ·  湿度 %d%%",
               environment->name, environment->temperature,
               environment->humidity);
    }
  else
    {
      snprintf(subtitle, sizeof(subtitle), "%s",
               g_ui.family_model_valid ? "环境传感器暂无数据" :
                                         "正在同步米家设备");
    }
  {
    lv_obj_t *sub = lv_label_create(g_ui.content);

    lv_label_set_text(sub, subtitle);
    lv_obj_set_style_text_font(sub, home_panel_font_get(), 0);
    lv_obj_set_style_text_color(sub, lv_color_hex(COLOR_SECONDARY), 0);
    g_ui.home_summary_label = sub;
  }

  /* Top row */

  top_row = lv_obj_create(g_ui.content);
  lv_obj_remove_style_all(top_row);
  lv_obj_set_size(top_row, LV_SIZE_CONTENT, 140);
  lv_obj_set_flex_flow(top_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(top_row, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_column(top_row, THEME_CARD_GAP, 0);
  lv_obj_set_style_border_width(top_row, 0, 0);

  /* Proactive suggestion card */

  {
    lv_obj_t *suggestion_card = lv_obj_create(top_row);

    lv_obj_set_size(suggestion_card, 560, 120);
    lv_obj_set_style_radius(suggestion_card, THEME_RADIUS_CARD, 0);
    theme_apply_surface_gradient(suggestion_card);
    lv_obj_set_style_border_width(suggestion_card, 1, 0);
    lv_obj_set_style_border_color(suggestion_card, lv_color_hex(0x2f6759), 0);
    lv_obj_set_style_shadow_width(suggestion_card, 0, 0);
    lv_obj_set_style_pad_all(suggestion_card, 0, 0);
    lv_obj_clear_flag(suggestion_card, LV_OBJ_FLAG_SCROLLABLE);

    home_ui_make_label(suggestion_card, "米家建议", 16, 10,
                       lv_color_hex(COLOR_GREEN), home_panel_font_get());
    g_ui.home_suggestion_title = home_ui_make_label(
      suggestion_card, "正在学习家庭习惯", 16, 38,
      lv_color_hex(COLOR_TEXT), home_panel_font_get());
    g_ui.home_suggestion_reason = home_ui_make_label(
      suggestion_card, "设备状态与反馈会在本地持续形成建议", 16, 70,
      lv_color_hex(COLOR_SECONDARY), home_panel_font_get());
    lv_obj_set_width(g_ui.home_suggestion_reason, 326);
    lv_label_set_long_mode(g_ui.home_suggestion_reason, LV_LABEL_LONG_DOT);

    g_ui.home_suggestion_execute = home_ui_make_action_button(
      suggestion_card, "执行", 356, 54, 84);
    lv_obj_remove_event_cb(g_ui.home_suggestion_execute,
                           home_ui_action_clicked);
    if (proactive_feedback_cb != NULL)
      {
        lv_obj_add_event_cb(g_ui.home_suggestion_execute,
                            proactive_feedback_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)HOME_PROACTIVE_ACCEPT);
      }

    g_ui.home_suggestion_ignore = home_ui_make_action_button(
      suggestion_card, "忽略", 452, 54, 84);
    home_ui_style_secondary_action(g_ui.home_suggestion_ignore);
    lv_obj_remove_event_cb(g_ui.home_suggestion_ignore,
                           home_ui_action_clicked);
    if (proactive_feedback_cb != NULL)
      {
        lv_obj_add_event_cb(g_ui.home_suggestion_ignore,
                            proactive_feedback_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)HOME_PROACTIVE_IGNORE_TODAY);
      }
    home_page_update_suggestion();
  }

  /* Family status card */

  right_col = lv_obj_create(top_row);
  lv_obj_remove_style_all(right_col);
  lv_obj_set_size(right_col, 220, 120);
  lv_obj_set_style_radius(right_col, THEME_RADIUS_CARD, 0);
  lv_obj_set_style_bg_color(right_col, lv_color_hex(COLOR_SURFACE), 0);
  lv_obj_set_style_bg_opa(right_col, LV_OPA_COVER, 0);
  theme_apply_surface_gradient(right_col);
  lv_obj_set_style_border_width(right_col, 1, 0);
  lv_obj_set_style_border_color(right_col, lv_color_hex(COLOR_BORDER), 0);
  lv_obj_set_style_shadow_width(right_col, 0, 0);
  lv_obj_set_style_pad_all(right_col, 16, 0);
  lv_obj_set_flex_flow(right_col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_column(right_col, 4, 0);

  {
    lv_obj_t *title = lv_label_create(right_col);

    lv_label_set_text(title, "家庭状态");
    lv_obj_set_style_text_font(title, home_panel_font_get(), 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_SECONDARY), 0);
  }
  {
    lv_obj_t *count = lv_label_create(right_col);

    snprintf(online_text, sizeof(online_text), "%u / %u",
             g_ui.family_model->online_count,
             g_ui.family_model->device_count);
    lv_label_set_text(count, online_text);
    lv_obj_set_style_text_font(count, home_panel_font_get(), 0);
    lv_obj_set_style_text_color(count, lv_color_hex(COLOR_TEXT), 0);
    g_ui.home_online_label = count;
  }
  {
    lv_obj_t *label = lv_label_create(right_col);

    lv_label_set_text(label, "台设备在线");
    lv_obj_set_style_text_font(label, home_panel_font_get(), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(COLOR_MUTED), 0);
  }
  if (!g_ui.family_model_valid)
    {
      snprintf(status, sizeof(status), "正在同步");
    }
  else if (g_ui.family_model->online_count ==
           g_ui.family_model->device_count)
    {
      snprintf(status, sizeof(status), "一切正常");
    }
  else
    {
      snprintf(status, sizeof(status), "%u 台离线",
               g_ui.family_model->device_count -
                 g_ui.family_model->online_count);
    }
  {
    lv_obj_t *label = lv_label_create(right_col);

    lv_label_set_text(label, status);
    lv_obj_set_style_text_font(label, home_panel_font_get(), 0);
    lv_obj_set_style_text_color(label,
                                lv_color_hex(
                                  g_ui.family_model->online_count ==
                                    g_ui.family_model->device_count ?
                                    COLOR_SECONDARY : COLOR_ORANGE), 0);
    g_ui.status_label = label;
  }

  /* Bottom section: device cards */

  bottom_section = lv_obj_create(g_ui.content);
  lv_obj_remove_style_all(bottom_section);
  lv_obj_set_size(bottom_section, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(bottom_section, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(bottom_section, THEME_CARD_GAP, 0);
  lv_obj_set_style_border_width(bottom_section, 0, 0);

  {
    lv_obj_t *title = lv_label_create(bottom_section);

    lv_label_set_text(title, "常用设备");
    lv_obj_set_style_text_font(title, home_panel_font_get(), 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT), 0);
  }

  device_row = lv_obj_create(bottom_section);
  lv_obj_remove_style_all(device_row);
  lv_obj_set_size(device_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(device_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(device_row, THEME_CARD_GAP, 0);
  lv_obj_set_style_border_width(device_row, 0, 0);

  selected_count = select_device_cards(selected);
  for (index = 0; index < selected_count; index++)
    {
      const struct home_panel_device_s *device =
        &g_ui.family_model->devices[selected[index]];
      lv_color_t accent = lv_color_hex(
        device->has_brightness ? THEME_COLOR_LIGHTING :
        (device->has_temperature || device->has_humidity ?
         COLOR_BLUE : (device->has_power && device->power ?
                       COLOR_GREEN : COLOR_SECONDARY)));

      make_device_card(device_row, 0, 0, 180, 200,
                       home_ui_device_symbol(device), device, accent,
                       device_toggle_cb);
    }
}

void home_page_update_suggestion(void)
{
  struct home_proactive_snapshot_s snapshot;
  const char *title;
  char reason[160];

  if (g_ui.home_suggestion_title == NULL ||
      g_ui.home_suggestion_reason == NULL)
    {
      return;
    }

  home_proactive_get_snapshot(&snapshot);
  if (snapshot.suggestion_available)
    {
      title = snapshot.candidate_kind ==
                HOME_PROACTIVE_CANDIDATE_EVENT_ROUTINE ?
                "发现设备联动建议" :
              snapshot.candidate_kind ==
                HOME_PROACTIVE_CANDIDATE_TIME_ROUTINE ?
                "发现时间习惯建议" : "你可能准备休息了";
      snprintf(reason, sizeof(reason),
               "依据 %u 次观察 · 置信度 %u%%%s%s",
               snapshot.candidate_observations, snapshot.confidence,
               snapshot.target_name[0] != '\0' ? " · 目标 " : "",
               snapshot.target_name);
    }
  else
    {
      title = snapshot.clock_valid ? "正在学习家庭习惯" :
                                     "等待时间与设备状态";
      snprintf(reason, sizeof(reason),
               snapshot.profile_learned || snapshot.routine_count > 0 ?
                 "已学习 %u 条习惯，达到阈值后会先征求你的确认" :
                 "设备状态与反馈会在本地持续形成建议",
               snapshot.routine_count);
    }

  home_ui_set_label_text_if_changed(g_ui.home_suggestion_title, title);
  home_ui_set_label_text_if_changed(g_ui.home_suggestion_reason, reason);
  if (g_ui.home_suggestion_execute != NULL)
    {
      if (snapshot.suggestion_available)
        {
          lv_obj_remove_flag(g_ui.home_suggestion_execute,
                             LV_OBJ_FLAG_HIDDEN);
          lv_obj_remove_flag(g_ui.home_suggestion_ignore,
                             LV_OBJ_FLAG_HIDDEN);
        }
      else
        {
          lv_obj_add_flag(g_ui.home_suggestion_execute, LV_OBJ_FLAG_HIDDEN);
          lv_obj_add_flag(g_ui.home_suggestion_ignore, LV_OBJ_FLAG_HIDDEN);
        }
    }
}
