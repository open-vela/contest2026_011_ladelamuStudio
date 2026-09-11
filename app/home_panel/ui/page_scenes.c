/****************************************************************************
 * D13x home panel - Scenes page
 ****************************************************************************/

#include "home_ui_internal.h"

void scenes_page_create(lv_event_cb_t scene_clicked_cb)
{
  unsigned int index;
  const int left_margin = THEME_CONTENT_PAD;
  const int content_width = PANEL_WIDTH - NAV_WIDTH - THEME_CONTENT_PAD * 2;

  /* Page title */

  theme_create_page_title(g_ui.content, "场景", left_margin, 16);
  home_ui_make_label(g_ui.content, "一次控制多个家庭设备", left_margin, 50,
                     lv_color_hex(COLOR_SECONDARY), home_panel_font_get());

  /* Scene cards - 2 column grid */

  for (index = 0; index < g_ui.family_model->scene_count && index < 4;
       index++)
    {
      unsigned int col = index % 2;
      unsigned int row = index / 2;
      int card_x = left_margin + col * (content_width / 2 + THEME_CARD_GAP);
      int card_y = 88 + row * 120;
      lv_obj_t *card;
      lv_obj_t *indicator;
      lv_obj_t *icon_label;
      lv_obj_t *name_label;
      lv_obj_t *desc_label;
      lv_obj_t *exec_button;

      card = lv_button_create(g_ui.content);
      home_ui_configure_fast_button(card);
      lv_obj_set_pos(card, card_x, card_y);
      lv_obj_set_size(card, content_width / 2 - THEME_CARD_GAP / 2, 104);
      lv_obj_set_style_radius(card, THEME_RADIUS_CARD, 0);
      lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_SURFACE), 0);
      lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
      theme_apply_surface_gradient(card);
      lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_SURFACE_2),
                                LV_STATE_PRESSED);
      lv_obj_set_style_border_width(card, 1, 0);
      lv_obj_set_style_border_color(card, lv_color_hex(COLOR_BORDER), 0);
      lv_obj_set_style_shadow_width(card, 0, 0);
      lv_obj_set_style_pad_all(card, 16, 0);
      lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
      g_ui.scene_bindings[g_ui.current_page][index].scene =
        &g_ui.family_model->scenes[index];
      if (scene_clicked_cb != NULL)
        {
          lv_obj_add_event_cb(card, scene_clicked_cb, LV_EVENT_CLICKED,
                              &g_ui.scene_bindings[g_ui.current_page][index]);
        }

      /* Green accent indicator */

      indicator = lv_obj_create(card);
      lv_obj_remove_style_all(indicator);
      lv_obj_set_pos(indicator, 0, 12);
      lv_obj_set_size(indicator, 3, 40);
      lv_obj_set_style_radius(indicator, 2, 0);
      lv_obj_set_style_bg_color(indicator, lv_color_hex(COLOR_GREEN), 0);
      lv_obj_set_style_bg_opa(indicator, LV_OPA_COVER, 0);

      icon_label = lv_label_create(card);
      lv_label_set_text(icon_label, LV_SYMBOL_PLAY);
      lv_obj_set_pos(icon_label, 16, 16);
      lv_obj_set_style_text_font(icon_label, &lv_font_montserrat_16, 0);
      lv_obj_set_style_text_color(icon_label, lv_color_hex(COLOR_GREEN), 0);

      name_label = lv_label_create(card);
      lv_label_set_text(name_label, g_ui.family_model->scenes[index].name);
      lv_obj_set_pos(name_label, 48, 14);
      lv_obj_set_style_text_font(name_label, home_panel_font_get(), 0);
      lv_obj_set_style_text_color(name_label, lv_color_hex(COLOR_TEXT), 0);

      desc_label = lv_label_create(card);
      lv_label_set_text(desc_label, "一次性控制多个家庭设备");
      lv_obj_set_pos(desc_label, 48, 44);
      lv_obj_set_style_text_font(desc_label, home_panel_font_get(), 0);
      lv_obj_set_style_text_color(desc_label,
                                  lv_color_hex(COLOR_SECONDARY), 0);

      /* Execute button */

      exec_button = lv_button_create(card);
      home_ui_configure_fast_button(exec_button);
      lv_obj_set_size(exec_button, 80, 36);
      lv_obj_set_ext_click_area(exec_button, 8);
      lv_obj_align(exec_button, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
      lv_obj_set_style_radius(exec_button, THEME_RADIUS_CTRL, 0);
      lv_obj_set_style_bg_color(exec_button, lv_color_hex(COLOR_GREEN), 0);
      lv_obj_set_style_bg_opa(exec_button, LV_OPA_COVER, 0);
      lv_obj_set_style_shadow_width(exec_button, 0, 0);
      lv_obj_set_style_border_width(exec_button, 0, 0);
      if (scene_clicked_cb != NULL)
        {
          lv_obj_add_event_cb(exec_button, scene_clicked_cb,
                              LV_EVENT_CLICKED,
                              &g_ui.scene_bindings[g_ui.current_page][index]);
        }

      name_label = lv_label_create(exec_button);
      lv_label_set_text(name_label, "执行");
      lv_obj_set_style_text_font(name_label, home_panel_font_get(), 0);
      lv_obj_set_style_text_color(name_label,
                                  lv_color_hex(THEME_COLOR_BG), 0);
      lv_obj_center(name_label);
    }

  /* Empty state */

  if (g_ui.family_model->scene_count == 0)
    {
      component_create_empty_state(g_ui.content, "暂无场景",
                                   left_margin, 150, content_width);
    }

  g_ui.status_label = home_ui_make_label(
    g_ui.content,
    g_ui.family_model->scene_count > 0 ? "点击场景即可执行" : "",
    left_margin, 400, lv_color_hex(COLOR_MUTED), home_panel_font_get());
}
