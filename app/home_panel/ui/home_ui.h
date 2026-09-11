/****************************************************************************
 * D13x home panel - Public UI page API
 *
 * This header is the ONLY interface main.c uses to interact with UI pages.
 * Pages receive model/state through parameters, never access network
 * threads, Mijia request internals, or proactive engine state directly.
 ****************************************************************************/

#ifndef __HOME_UI_H
#define __HOME_UI_H

#include <lvgl/lvgl.h>
#include "../home_panel_mijia_model.h"
#include "../home_panel_proactive.h"
#include "../home_panel_mijia_client.h"

/* Page indices */

#define HOME_PAGE_COUNT   5
#define HOME_CARD_COUNT   3
#define PROACTIVE_PAGE    3

/* Layout constants */

#define PANEL_WIDTH       1024
#define PANEL_HEIGHT      600
#define TOPBAR_HEIGHT     64
#define NAV_WIDTH         152
#define ROOM_NAV_WIDTH    170
#define ROOM_CARD_WIDTH   260
#define ROOM_CARD_HEIGHT  108

/* Network state - shared between main.c and UI pages */

enum home_ui_network_state_e
{
  HOME_UI_NET_INITIALIZING = 0,
  HOME_UI_NET_DISCONNECTED,
  HOME_UI_NET_CHECKING,
  HOME_UI_NET_NO_INTERNET,
  HOME_UI_NET_ONLINE
};

/* Page result returned by page create functions.
 * Contains the page container and per-page status label.
 */

struct home_ui_page_s
{
  lv_obj_t *container;
  lv_obj_t *status_label;
};

/****************************************************************************
 * Page lifecycle - main.c calls these
 ****************************************************************************/

/* Create the entire home screen (topbar + nav + page host).
 * Must be called once at startup after LVGL init.
 */

void home_ui_create_screen(void);

/* Navigation */

void home_ui_show_page(unsigned int page);
void home_ui_delete_page(unsigned int page);

/* Global UI updates - main.c calls these when state changes */

void home_ui_update_network(enum home_ui_network_state_e state);
void home_ui_update_time(bool force);
void home_ui_apply_mijia_snapshot(
  const struct home_panel_mijia_snapshot_s *snapshot);

/****************************************************************************
 * Page-specific update APIs
 ****************************************************************************/

/* Home page */

void home_ui_home_update_suggestion(void);

/* Proactive page */

void home_ui_proactive_update_widgets(void);
void home_ui_proactive_update_context(void);
void home_ui_proactive_update_home_suggestion(void);

/* Settings page */

void home_ui_settings_update_network(enum home_ui_network_state_e state);

/****************************************************************************
 * Accessors for shared state that pages need
 ****************************************************************************/

const struct home_panel_family_model_s *home_ui_get_family_model(void);
bool home_ui_is_family_model_valid(void);
unsigned int home_ui_get_current_page(void);
unsigned int home_ui_get_selected_room(void);
enum home_ui_network_state_e home_ui_get_network_state(void);

/* Status label management */

lv_obj_t *home_ui_get_status_label(void);
void home_ui_set_status_label(lv_obj_t *label);

/* Login */

void home_ui_show_login(lv_event_t *event);

/****************************************************************************
 * Page-specific callbacks that main.c needs to register
 ****************************************************************************/

/* Proactive feedback callback - main.c provides the implementation */

typedef void (*home_ui_proactive_feedback_cb_t)(
  enum home_proactive_feedback_e feedback, bool confirmed);

void home_ui_set_proactive_feedback_callback(
  home_ui_proactive_feedback_cb_t cb);

/* Cloud state callbacks - main.c provides these */

void home_ui_proactive_handle_feedback(enum home_proactive_feedback_e feedback,
                                       bool confirmed);

/****************************************************************************
 * Model refresh
 ****************************************************************************/

/* Call when family model structure changes - rebuilds visible page */

void home_ui_on_model_structure_changed(void);

/* Call when model data changes (not structure) - updates bindings in place */

void home_ui_on_model_data_changed(void);

/****************************************************************************
 * Helper functions (implemented in home_ui_components.c)
 * Declared here so main.c can use them via macro aliases.
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

const char *home_ui_device_type_text(const struct home_panel_device_s *device);
const char *home_ui_device_symbol(const struct home_panel_device_s *device);
const char *home_ui_device_state_text(const struct home_panel_device_s *device);
void home_ui_format_device_value(const struct home_panel_device_s *device,
                                 char *value, size_t capacity);
const char *home_ui_control_display_name(const char *name);
const char *home_ui_option_display_name(const char *property_name,
                                        const char *label);
void home_ui_format_control_value(const struct home_panel_control_s *property,
                                  int value, char *text, size_t capacity);
void home_ui_set_command_status(int ret);
lv_obj_t *home_ui_make_room_device_container(void);

/****************************************************************************
 * Page create functions - called from main.c's show_page()
 ****************************************************************************/

void settings_page_create(lv_event_cb_t network_refresh_cb);
void settings_page_update_network(enum home_ui_network_state_e state);

void scenes_page_create(lv_event_cb_t scene_clicked_cb);

void home_page_create(lv_event_cb_t device_toggle_cb,
                      lv_event_cb_t proactive_feedback_cb);
void home_page_update_suggestion(void);

void rooms_page_create(void);
void rooms_page_update_button_styles(void);
void rooms_page_render_devices(unsigned int room_index);
void rooms_device_toggled(lv_event_t *event);
void rooms_reset_detail_bindings(void);

void proactive_page_create(void);
void proactive_page_update_widgets(void);
void proactive_page_show_confirm(void);

/****************************************************************************
 * Shared UI state
 *
 * Defined in home_ui_components.c. main.c initializes and updates fields;
 * page modules read fields through this struct.
 ****************************************************************************/

/* Binding types for device/control/action/scene widget tracking */

struct home_ui_device_binding_s
{
  const struct home_panel_device_s *device;
  lv_obj_t *card;
  lv_obj_t *button;
  lv_obj_t *value_label;
  lv_obj_t *state_label;
  bool visual_initialized;
  bool visual_online;
  bool visual_power;
};

struct home_ui_control_binding_s
{
  const struct home_panel_device_s *device;
  const struct home_panel_control_s *property;
  lv_obj_t *control;
  lv_obj_t *value_label;
};

struct home_ui_action_binding_s
{
  const struct home_panel_device_s *device;
  const struct home_panel_action_s *action;
};

struct home_ui_scene_binding_s
{
  const struct home_panel_scene_s *scene;
};

struct home_ui_proactive_automation_ui_s
{
  uint32_t routine_id;
  bool cloud;
};

/* The shared state structure */

struct home_ui_state_s
{
  const struct home_panel_family_model_s *family_model;
  bool family_model_valid;
  unsigned int current_page;
  unsigned int selected_room;
  unsigned int selected_device;
  enum home_ui_network_state_e network_state;

  /* Topbar */

  lv_obj_t *clock_label;
  lv_obj_t *date_label;
  lv_obj_t *network_label;
  lv_obj_t *login_top_button;
  lv_obj_t *login_top_label;

  /* Global status */

  lv_obj_t *status_label;

  /* Navigation */

  lv_obj_t *nav_buttons[HOME_PAGE_COUNT];
  lv_obj_t *nav_icons[HOME_PAGE_COUNT];
  lv_obj_t *nav_labels[HOME_PAGE_COUNT];
  lv_obj_t *nav_indicators[HOME_PAGE_COUNT];

  /* Page host */

  lv_obj_t *page_host;
  lv_obj_t *pages[HOME_PAGE_COUNT];
  lv_obj_t *content;
  uint32_t page_model_revisions[HOME_PAGE_COUNT];
  uint32_t family_ui_revision;
  bool full_refresh_pending;
  bool model_refresh_pending;

  /* Home page widgets */

  lv_obj_t *home_summary_label;
  lv_obj_t *home_online_label;
  lv_obj_t *home_suggestion_title;
  lv_obj_t *home_suggestion_reason;
  lv_obj_t *home_suggestion_execute;
  lv_obj_t *home_suggestion_ignore;

  /* Settings page widgets */

  lv_obj_t *settings_network_label;
  lv_obj_t *settings_probe_label;
  lv_obj_t *settings_account_label;
  lv_obj_t *settings_online_label;
  lv_obj_t *settings_channel_label;
  lv_obj_t *page_settings_network_labels[HOME_PAGE_COUNT];
  lv_obj_t *page_settings_probe_labels[HOME_PAGE_COUNT];
  lv_obj_t *page_settings_account_labels[HOME_PAGE_COUNT];
  lv_obj_t *page_settings_online_labels[HOME_PAGE_COUNT];
  lv_obj_t *page_settings_channel_labels[HOME_PAGE_COUNT];

  /* Per-page status and home labels */

  lv_obj_t *page_status_labels[HOME_PAGE_COUNT];
  lv_obj_t *page_home_summary_labels[HOME_PAGE_COUNT];
  lv_obj_t *page_home_online_labels[HOME_PAGE_COUNT];

  /* Rooms page widgets */

  lv_obj_t *room_buttons[HOME_PANEL_MAX_ROOMS];
  lv_obj_t *room_title_label;
  lv_obj_t *room_summary_label;
  lv_obj_t *room_device_host;
  lv_obj_t *room_device_hosts[HOME_PANEL_MAX_ROOMS];
  unsigned int room_visible_counts[HOME_PANEL_MAX_ROOMS];
  unsigned int room_highlighted;
  lv_obj_t *room_detail_host;

  /* Device detail widgets */

  lv_obj_t *detail_subtitle_label;
  lv_obj_t *detail_temperature_label;
  lv_obj_t *detail_humidity_label;
  lv_obj_t *detail_battery_label;

  /* Proactive page widgets */

  lv_obj_t *proactive_mode_label;
  lv_obj_t *proactive_history_label;
  lv_obj_t *proactive_time_label;
  lv_obj_t *proactive_confidence_label;
  lv_obj_t *proactive_progress_label;
  lv_obj_t *proactive_suggestion_title;
  lv_obj_t *proactive_reason_label;
  lv_obj_t *proactive_action_label;
  lv_obj_t *proactive_feedback_label;
  lv_obj_t *proactive_accept_button;
  lv_obj_t *proactive_automation_button;
  lv_obj_t *proactive_ignore_button;
  lv_obj_t *proactive_less_button;
  lv_obj_t *proactive_return_button;
  lv_obj_t *proactive_reset_button;
  lv_obj_t *proactive_replay_button;
  lv_obj_t *proactive_automation_list;
  lv_obj_t *proactive_confirm_shade;
  uint32_t proactive_automation_signature;
  uint32_t proactive_confirm_key;
  uint32_t displayed_proactive_revision;
  uint32_t displayed_agent_revision;

  /* Login dialog widgets */

  lv_obj_t *login_shade;
  lv_obj_t *login_qr;
  lv_obj_t *login_message;
  lv_obj_t *login_action_label;

  /* Device/control/action/scene bindings */

  struct home_ui_device_binding_s
    device_bindings[HOME_PAGE_COUNT][HOME_PANEL_MAX_DEVICES + HOME_CARD_COUNT];
  unsigned int device_binding_counts[HOME_PAGE_COUNT];

  struct home_ui_control_binding_s
    control_bindings[HOME_PANEL_MAX_CONTROLS];
  unsigned int control_binding_count;

  struct home_ui_action_binding_s
    action_bindings[HOME_PANEL_MAX_ACTIONS];
  unsigned int action_binding_count;

  struct home_ui_scene_binding_s
    scene_bindings[HOME_PAGE_COUNT][HOME_PANEL_MAX_SCENES];

  /* Proactive automation UI list */

  struct home_ui_proactive_automation_ui_s
    proactive_automation_ui[HOME_PROACTIVE_MAX_AUTOMATIONS + 4];

  /* Callbacks from main.c for MIoT device control (set once at init) */

  lv_event_cb_t cb_device_toggled;
  lv_event_cb_t cb_detail_boolean_changed;
  lv_event_cb_t cb_detail_number_changed;
  lv_event_cb_t cb_detail_number_released;
  lv_event_cb_t cb_detail_enum_changed;
  lv_event_cb_t cb_detail_action_clicked;
};

extern struct home_ui_state_s g_ui;

#endif
