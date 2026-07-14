/****************************************************************************
 * D13x Hengshan-Pi 1024x600 home control panel
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <stdint.h>
#include <unistd.h>

#include <lvgl/lvgl.h>

LV_FONT_DECLARE(home_panel_misans_18);

#define PANEL_WIDTH       1024
#define PANEL_HEIGHT      600
#define TOPBAR_HEIGHT     72
#define NAV_WIDTH         154

#define COLOR_BG          0x0f1114
#define COLOR_SURFACE     0x191d22
#define COLOR_SURFACE_2   0x22272e
#define COLOR_TEXT        0xf4f6f8
#define COLOR_MUTED       0x929aa5
#define COLOR_ORANGE      0xff9f2f
#define COLOR_BLUE        0x4b8df8
#define COLOR_GREEN       0x47c486

static lv_obj_t *g_status_label;
static lv_obj_t *g_content;
static lv_obj_t *g_nav_buttons[4];
static void nav_clicked(lv_event_t *event);
static void show_page(unsigned int page);

static void set_chinese_font(lv_obj_t *obj)
{
  lv_obj_set_style_text_font(obj, &home_panel_misans_18, 0);
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text,
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

static void scene_clicked(lv_event_t *event)
{
  const char *scene = lv_event_get_user_data(event);
  char message[64];

  snprintf(message, sizeof(message), "场景已执行：%s", scene);
  lv_label_set_text(g_status_label, message);
}

static void action_clicked(lv_event_t *event)
{
  const char *action = lv_event_get_user_data(event);
  char message[72];

  snprintf(message, sizeof(message), "%s：操作已生效", action);
  if (g_status_label != NULL)
    {
      lv_label_set_text(g_status_label, message);
      lv_obj_set_style_text_color(g_status_label,
                                  lv_color_hex(COLOR_GREEN), 0);
    }
}

static void device_toggled(lv_event_t *event)
{
  lv_obj_t *button = lv_event_get_target(event);
  lv_obj_t *state = lv_event_get_user_data(event);
  bool checked = lv_obj_has_state(button, LV_STATE_CHECKED);

  lv_label_set_text(state, checked ? "已开启" : "已关闭");
  lv_obj_set_style_text_color(state,
                              lv_color_hex(checked ? COLOR_GREEN :
                                           COLOR_MUTED), 0);
  lv_obj_set_style_bg_color(button,
                            lv_color_hex(checked ? COLOR_GREEN :
                                         COLOR_SURFACE_2), 0);
  lv_label_set_text(g_status_label,
                    checked ? "设备已开启" : "设备已关闭");
}

static void login_close(lv_event_t *event)
{
  lv_obj_delete_async(lv_event_get_user_data(event));
}

static void show_login(lv_event_t *event)
{
  lv_obj_t *screen = lv_screen_active();
  lv_obj_t *shade;
  lv_obj_t *dialog;
  lv_obj_t *close;
  lv_obj_t *label;

  (void)event;

  shade = lv_obj_create(screen);
  lv_obj_remove_style_all(shade);
  lv_obj_set_size(shade, PANEL_WIDTH, PANEL_HEIGHT);
  lv_obj_set_pos(shade, 0, 0);
  lv_obj_set_style_bg_color(shade, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(shade, LV_OPA_70, 0);

  dialog = lv_obj_create(shade);
  lv_obj_set_size(dialog, 440, 300);
  lv_obj_center(dialog);
  lv_obj_set_style_radius(dialog, 8, 0);
  lv_obj_set_style_border_width(dialog, 1, 0);
  lv_obj_set_style_border_color(dialog, lv_color_hex(0x343b44), 0);
  lv_obj_set_style_bg_color(dialog, lv_color_hex(COLOR_SURFACE), 0);
  lv_obj_set_style_pad_all(dialog, 28, 0);

  label = make_label(dialog, "米家账号登录", 0, 0,
                     lv_color_hex(COLOR_TEXT), &home_panel_misans_18);
  lv_obj_set_style_text_font(label, &home_panel_misans_18, 0);

  label = make_label(dialog,
                     "请先连接有线网络。联网后将在此显示\n"
                     "米家授权二维码，账号凭据仅保存在服务器。",
                     0, 62, lv_color_hex(COLOR_MUTED),
                     &home_panel_misans_18);
  lv_obj_set_style_text_line_space(label, 12, 0);

  close = lv_button_create(dialog);
  lv_obj_set_size(close, 128, 48);
  lv_obj_align(close, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
  lv_obj_set_style_radius(close, 8, 0);
  lv_obj_set_style_bg_color(close, lv_color_hex(COLOR_BLUE), 0);
  lv_obj_set_style_bg_color(close, lv_color_hex(0x3475d6),
                            LV_STATE_PRESSED);
  lv_obj_add_event_cb(close, login_close, LV_EVENT_CLICKED, shade);
  label = lv_label_create(close);
  lv_label_set_text(label, "知道了");
  set_chinese_font(label);
  lv_obj_center(label);
}

static lv_obj_t *make_nav_button(lv_obj_t *parent, const char *symbol,
                                 const char *text, int y,
                                 unsigned int page)
{
  lv_obj_t *button = lv_button_create(parent);
  lv_obj_t *icon;
  lv_obj_t *label;

  lv_obj_set_pos(button, 12, y);
  lv_obj_set_size(button, 130, 52);
  lv_obj_set_style_radius(button, 8, 0);
  lv_obj_set_style_shadow_width(button, 0, 0);
  lv_obj_set_style_bg_color(button, lv_color_hex(COLOR_BG), 0);
  lv_obj_set_style_bg_color(button, lv_color_hex(0x303740),
                            LV_STATE_PRESSED);
  lv_obj_add_event_cb(button, nav_clicked, LV_EVENT_CLICKED,
                      (void *)(uintptr_t)page);

  icon = make_label(button, symbol, 12, 15,
                    lv_color_hex(COLOR_MUTED),
                    &lv_font_montserrat_16);
  label = make_label(button, text, 42, 13,
                     lv_color_hex(COLOR_MUTED),
                     &home_panel_misans_18);
  (void)icon;
  (void)label;
  return button;
}

static void nav_clicked(lv_event_t *event)
{
  unsigned int page = (uintptr_t)lv_event_get_user_data(event);

  show_page(page);
}

static lv_obj_t *make_scene_button(lv_obj_t *parent, const char *symbol,
                                   const char *text, int x,
                                   lv_color_t accent)
{
  lv_obj_t *button = lv_button_create(parent);
  lv_obj_t *icon;
  lv_obj_t *label;

  lv_obj_set_pos(button, x, 92);
  lv_obj_set_size(button, 180, 72);
  lv_obj_set_style_radius(button, 8, 0);
  lv_obj_set_style_shadow_width(button, 0, 0);
  lv_obj_set_style_bg_color(button, lv_color_hex(COLOR_SURFACE), 0);
  lv_obj_set_style_bg_color(button, lv_color_hex(COLOR_SURFACE_2),
                            LV_STATE_PRESSED);
  lv_obj_set_style_border_width(button, 1, 0);
  lv_obj_set_style_border_color(button, lv_color_hex(0x2c323a), 0);
  lv_obj_add_event_cb(button, scene_clicked, LV_EVENT_CLICKED, (void *)text);

  icon = make_label(button, symbol, 14, 22, accent, &lv_font_montserrat_16);
  label = make_label(button, text, 54, 20, lv_color_hex(COLOR_TEXT),
                     &home_panel_misans_18);
  (void)icon;
  return label;
}

static void make_device_card(lv_obj_t *parent, int x, const char *symbol,
                             const char *room, const char *name,
                             const char *value, lv_color_t accent,
                             bool checked)
{
  lv_obj_t *card = lv_obj_create(parent);
  lv_obj_t *toggle;
  lv_obj_t *state;
  lv_obj_t *label;

  lv_obj_set_pos(card, x, 238);
  lv_obj_set_size(card, 240, 218);
  lv_obj_set_style_radius(card, 8, 0);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_border_color(card, lv_color_hex(0x2c323a), 0);
  lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_SURFACE), 0);
  lv_obj_set_style_pad_all(card, 18, 0);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

  make_label(card, symbol, 0, 0, accent, &lv_font_montserrat_28);
  make_label(card, room, 0, 58, lv_color_hex(COLOR_MUTED),
             &home_panel_misans_18);
  make_label(card, name, 0, 88, lv_color_hex(COLOR_TEXT),
             &home_panel_misans_18);
  label = make_label(card, value, 0, 122, accent, &lv_font_montserrat_28);
  (void)label;

  state = make_label(card, checked ? "已开启" : "已关闭", 0, 169,
                     lv_color_hex(checked ? COLOR_GREEN : COLOR_MUTED),
                     &home_panel_misans_18);

  toggle = lv_button_create(card);
  lv_obj_set_size(toggle, 52, 32);
  lv_obj_align(toggle, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
  lv_obj_set_style_radius(toggle, 8, 0);
  lv_obj_set_style_shadow_width(toggle, 0, 0);
  lv_obj_set_style_bg_color(toggle,
                            lv_color_hex(checked ? COLOR_GREEN :
                                         COLOR_SURFACE_2), 0);
  lv_obj_set_style_bg_color(toggle, lv_color_hex(COLOR_BLUE),
                            LV_STATE_PRESSED);
  lv_obj_add_flag(toggle, LV_OBJ_FLAG_CHECKABLE);
  if (checked)
    {
      lv_obj_add_state(toggle, LV_STATE_CHECKED);
    }

  label = lv_label_create(toggle);
  lv_label_set_text(label, LV_SYMBOL_POWER);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
  lv_obj_center(label);
  lv_obj_add_event_cb(toggle, device_toggled, LV_EVENT_VALUE_CHANGED, state);
}

static lv_obj_t *make_action_button(lv_obj_t *parent, const char *text,
                                    int x, int y, int width)
{
  lv_obj_t *button = lv_button_create(parent);
  lv_obj_t *label;

  lv_obj_set_pos(button, x, y);
  lv_obj_set_size(button, width, 52);
  lv_obj_set_style_radius(button, 8, 0);
  lv_obj_set_style_shadow_width(button, 0, 0);
  lv_obj_set_style_bg_color(button, lv_color_hex(COLOR_BLUE), 0);
  lv_obj_set_style_bg_color(button, lv_color_hex(0x3475d6),
                            LV_STATE_PRESSED);
  lv_obj_add_event_cb(button, action_clicked, LV_EVENT_CLICKED, (void *)text);

  label = lv_label_create(button);
  lv_label_set_text(label, text);
  set_chinese_font(label);
  lv_obj_center(label);
  return button;
}

static void make_info_row(lv_obj_t *parent, const char *name,
                          const char *value, int y, lv_color_t color)
{
  make_label(parent, name, 32, y, lv_color_hex(COLOR_MUTED),
             &home_panel_misans_18);
  make_label(parent, value, 238, y, color, &home_panel_misans_18);
}

static void create_home_page(void)
{
  make_label(g_content, "晚上好，欢迎回家", 30, 22,
             lv_color_hex(COLOR_TEXT), &home_panel_misans_18);
  make_label(g_content, "客厅 24°C  ·  空气优", 30, 54,
             lv_color_hex(COLOR_MUTED), &home_panel_misans_18);

  make_scene_button(g_content, LV_SYMBOL_HOME, "回家", 30,
                    lv_color_hex(COLOR_GREEN));
  make_scene_button(g_content, LV_SYMBOL_EYE_CLOSE, "离家", 226,
                    lv_color_hex(COLOR_BLUE));
  make_scene_button(g_content, LV_SYMBOL_BELL, "晚安", 422,
                    lv_color_hex(COLOR_ORANGE));

  make_label(g_content, "常用设备", 30, 198, lv_color_hex(COLOR_TEXT),
             &home_panel_misans_18);
  g_status_label = make_label(g_content, "3 个设备在线", 666, 198,
                              lv_color_hex(COLOR_MUTED),
                              &home_panel_misans_18);

  make_device_card(g_content, 30, LV_SYMBOL_BULLET, "客厅", "主灯",
                   "72%", lv_color_hex(COLOR_ORANGE), true);
  make_device_card(g_content, 288, LV_SYMBOL_REFRESH, "客厅", "空调",
                   "24°C", lv_color_hex(COLOR_BLUE), true);
  make_device_card(g_content, 546, LV_SYMBOL_UP, "阳台", "窗帘",
                   "40%", lv_color_hex(COLOR_GREEN), false);
}

static void create_rooms_page(void)
{
  lv_obj_t *panel;

  make_label(g_content, "房间", 30, 22, lv_color_hex(COLOR_TEXT),
             &home_panel_misans_18);
  make_label(g_content, "按空间查看和控制设备", 30, 54,
             lv_color_hex(COLOR_MUTED), &home_panel_misans_18);

  panel = lv_obj_create(g_content);
  lv_obj_set_pos(panel, 30, 104);
  lv_obj_set_size(panel, 790, 254);
  lv_obj_set_style_radius(panel, 8, 0);
  lv_obj_set_style_bg_color(panel, lv_color_hex(COLOR_SURFACE), 0);
  lv_obj_set_style_border_color(panel, lv_color_hex(0x2c323a), 0);
  lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
  make_info_row(panel, "客厅", "3 个设备 · 24°C", 24,
                lv_color_hex(COLOR_TEXT));
  make_info_row(panel, "卧室", "2 个设备 · 23°C", 82,
                lv_color_hex(COLOR_TEXT));
  make_info_row(panel, "阳台", "1 个设备", 140,
                lv_color_hex(COLOR_TEXT));
  make_info_row(panel, "厨房", "暂无设备", 198,
                lv_color_hex(COLOR_MUTED));
  g_status_label = make_label(g_content, "选择房间后可查看设备",
                              30, 398, lv_color_hex(COLOR_MUTED),
                              &home_panel_misans_18);
  make_action_button(g_content, "查看客厅设备", 628, 390, 192);
}

static void create_scenes_page(void)
{
  make_label(g_content, "场景", 30, 22, lv_color_hex(COLOR_TEXT),
             &home_panel_misans_18);
  make_label(g_content, "一次控制多个家庭设备", 30, 54,
             lv_color_hex(COLOR_MUTED), &home_panel_misans_18);
  make_scene_button(g_content, LV_SYMBOL_HOME, "回家", 30,
                    lv_color_hex(COLOR_GREEN));
  make_scene_button(g_content, LV_SYMBOL_EYE_CLOSE, "离家", 226,
                    lv_color_hex(COLOR_BLUE));
  make_scene_button(g_content, LV_SYMBOL_BELL, "晚安", 422,
                    lv_color_hex(COLOR_ORANGE));
  g_status_label = make_label(g_content, "点击场景即可执行",
                              30, 208, lv_color_hex(COLOR_MUTED),
                              &home_panel_misans_18);
  make_action_button(g_content, "新建场景", 30, 278, 160);
}

static void create_settings_page(void)
{
  lv_obj_t *panel;

  make_label(g_content, "设置", 30, 22, lv_color_hex(COLOR_TEXT),
             &home_panel_misans_18);
  make_label(g_content, "网络、账号与系统状态", 30, 54,
             lv_color_hex(COLOR_MUTED), &home_panel_misans_18);

  panel = lv_obj_create(g_content);
  lv_obj_set_pos(panel, 30, 104);
  lv_obj_set_size(panel, 790, 244);
  lv_obj_set_style_radius(panel, 8, 0);
  lv_obj_set_style_bg_color(panel, lv_color_hex(COLOR_SURFACE), 0);
  lv_obj_set_style_border_color(panel, lv_color_hex(0x2c323a), 0);
  lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
  make_info_row(panel, "有线网络", "正在等待网线连接", 28,
                lv_color_hex(COLOR_ORANGE));
  make_info_row(panel, "地址获取", "DHCP 自动", 88,
                lv_color_hex(COLOR_TEXT));
  make_info_row(panel, "米家账号", "未登录", 148,
                lv_color_hex(COLOR_MUTED));
  g_status_label = make_label(g_content, "网络驱动状态会输出到串口",
                              30, 390, lv_color_hex(COLOR_MUTED),
                              &home_panel_misans_18);
  make_action_button(g_content, "重新检测网络", 628, 382, 192);
}

static void show_page(unsigned int page)
{
  unsigned int i;

  if (page > 3)
    {
      page = 0;
    }

  for (i = 0; i < 4; i++)
    {
      lv_obj_set_style_bg_color(g_nav_buttons[i],
                                lv_color_hex(i == page ? COLOR_SURFACE_2 :
                                             COLOR_BG), 0);
    }

  g_status_label = NULL;
  lv_obj_clean(g_content);
  if (page == 0)
    {
      create_home_page();
    }
  else if (page == 1)
    {
      create_rooms_page();
    }
  else if (page == 2)
    {
      create_scenes_page();
    }
  else
    {
      create_settings_page();
    }

  lv_obj_invalidate(g_content);
}

static void create_home_screen(void)
{
  lv_obj_t *screen = lv_screen_active();
  lv_obj_t *topbar;
  lv_obj_t *nav;
  lv_obj_t *login;
  lv_obj_t *label;

  lv_obj_set_style_bg_color(screen, lv_color_hex(COLOR_BG), 0);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
  lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

  topbar = lv_obj_create(screen);
  lv_obj_remove_style_all(topbar);
  lv_obj_set_pos(topbar, 0, 0);
  lv_obj_set_size(topbar, PANEL_WIDTH, TOPBAR_HEIGHT);
  lv_obj_set_style_bg_color(topbar, lv_color_hex(COLOR_SURFACE), 0);
  lv_obj_set_style_bg_opa(topbar, LV_OPA_COVER, 0);

  make_label(topbar, "08:42", 24, 17, lv_color_hex(COLOR_TEXT),
             &lv_font_montserrat_28);
  make_label(topbar, "7月14日  星期二", 132, 26,
             lv_color_hex(COLOR_MUTED), &home_panel_misans_18);
  make_label(topbar, LV_SYMBOL_WARNING "  有线网络未连接", 672, 26,
             lv_color_hex(COLOR_ORANGE), &home_panel_misans_18);

  login = lv_button_create(topbar);
  lv_obj_set_size(login, 118, 44);
  lv_obj_set_pos(login, 886, 14);
  lv_obj_set_style_radius(login, 8, 0);
  lv_obj_set_style_shadow_width(login, 0, 0);
  lv_obj_set_style_bg_color(login, lv_color_hex(COLOR_BLUE), 0);
  lv_obj_set_style_bg_color(login, lv_color_hex(0x3475d6),
                            LV_STATE_PRESSED);
  lv_obj_add_event_cb(login, show_login, LV_EVENT_CLICKED, NULL);
  label = lv_label_create(login);
  lv_label_set_text(label, "登录米家");
  set_chinese_font(label);
  lv_obj_center(label);

  nav = lv_obj_create(screen);
  lv_obj_remove_style_all(nav);
  lv_obj_set_pos(nav, 0, TOPBAR_HEIGHT);
  lv_obj_set_size(nav, NAV_WIDTH, PANEL_HEIGHT - TOPBAR_HEIGHT);
  lv_obj_set_style_bg_color(nav, lv_color_hex(0x13161a), 0);
  lv_obj_set_style_bg_opa(nav, LV_OPA_COVER, 0);

  g_nav_buttons[0] = make_nav_button(nav, LV_SYMBOL_HOME, "家庭", 20, 0);
  g_nav_buttons[1] = make_nav_button(nav, LV_SYMBOL_LIST, "房间", 84, 1);
  g_nav_buttons[2] = make_nav_button(nav, LV_SYMBOL_PLAY, "场景", 148, 2);
  g_nav_buttons[3] = make_nav_button(nav, LV_SYMBOL_SETTINGS, "设置", 432, 3);

  g_content = lv_obj_create(screen);
  lv_obj_remove_style_all(g_content);
  lv_obj_set_pos(g_content, NAV_WIDTH, TOPBAR_HEIGHT);
  lv_obj_set_size(g_content, PANEL_WIDTH - NAV_WIDTH,
                  PANEL_HEIGHT - TOPBAR_HEIGHT);
  lv_obj_set_style_bg_color(g_content, lv_color_hex(COLOR_BG), 0);
  lv_obj_set_style_bg_opa(g_content, LV_OPA_COVER, 0);
  lv_obj_clear_flag(g_content, LV_OBJ_FLAG_SCROLLABLE);
  show_page(0);
}

int main(int argc, char *argv[])
{
  lv_nuttx_dsc_t info;
  lv_nuttx_result_t result;

  (void)argc;
  (void)argv;

  if (lv_is_initialized())
    {
      fprintf(stderr, "home_panel: LVGL is already initialized\n");
      return 1;
    }

  lv_init();
  lv_nuttx_dsc_init(&info);
#ifdef CONFIG_INPUT_TOUCHSCREEN
  info.input_path = CONFIG_D13X_HOME_PANEL_INPUT_DEVPATH;
#endif
  lv_nuttx_init(&info, &result);
  if (result.disp == NULL)
    {
      fprintf(stderr, "home_panel: failed to initialize /dev/fb0\n");
      lv_deinit();
      return 1;
    }

  if (result.indev == NULL)
    {
      fprintf(stderr, "home_panel: failed to initialize %s\n",
              CONFIG_D13X_HOME_PANEL_INPUT_DEVPATH);
      lv_nuttx_deinit(&result);
      lv_deinit();
      return 1;
    }

  lv_indev_set_display(result.indev, result.disp);

  create_home_screen();
  lv_obj_invalidate(lv_screen_active());
  lv_refr_now(result.disp);

  for (;;)
    {
      uint32_t delay = lv_timer_handler();

      usleep((delay > 0 ? delay : 1) * 1000);
    }

  return 0;
}
