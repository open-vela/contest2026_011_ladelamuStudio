/****************************************************************************
 * D13x Hengshan-Pi 1024x600 home control panel
 ****************************************************************************/

#include <nuttx/config.h>

#include <arch/board/board.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <nuttx/arch.h>
#include <nuttx/net/dns.h>
#include <nuttx/net/icmp.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include <lvgl/lvgl.h>
#include <netutils/netlib.h>
#include <netutils/ntpclient.h>

#include "home_panel_mijia_client.h"
#include "home_panel_mijia_model.h"
#include "home_panel_font.h"
#include "home_panel_proactive.h"
#include "home_panel_proactive_controller.h"
#include "home_panel_theme.h"
#include "home_panel_components.h"
#include "ui/home_ui.h"

LV_FONT_DECLARE(home_panel_digits_28);

/* Layout constants (now in home_ui.h, kept for backward compat) */

#define ROOM_NAV_WIDTH    170
#define ROOM_CARD_WIDTH   260
#define ROOM_CARD_HEIGHT  108
#define ROOM_CARD_GAP     THEME_CARD_GAP

/* Network constants */

#define NETWORK_INTERFACE       "eth0"
#define NETWORK_PROBE_INTERVAL  60
#define NETWORK_THREAD_STACK    8192
#define NETWORK_THREAD_PRIORITY 50
#define NETWORK_PING_POLL_US    20000
#define NETWORK_PING_POLLS      75
#define NETWORK_PING_DATA_SIZE  16
#define NETWORK_DNS_BUFFER_SIZE 512
#define NETWORK_LINK_GRACE_SEC  12

/* Timing constants */

#define UI_LOOP_MAX_DELAY_MS    5
#define MIJIA_UI_POLL_MS        100
#define PROACTIVE_TICK_MS       50
#define PROACTIVE_UI_POLL_MS    100
#define PROACTIVE_CLOUD_POLL_MS 500
#define UI_THREAD_PRIORITY      105
#define UI_SLOW_LOG_MS          24
#define TIME_VALID_EPOCH        1735689600
#define TIME_UPDATE_INTERVAL_MS 1000
#define NTP_RETRY_INTERVAL_MS   30000

/* Color shortcuts */

#define COLOR_BG          THEME_COLOR_BG
#define COLOR_NAV         THEME_COLOR_NAV
#define COLOR_TOPBAR      THEME_COLOR_TOPBAR
#define COLOR_SURFACE     THEME_COLOR_SURFACE
#define COLOR_SURFACE_2   THEME_COLOR_SURFACE_RAISED
#define COLOR_BORDER      THEME_COLOR_DIVIDER
#define COLOR_TEXT        THEME_COLOR_TEXT_PRIMARY
#define COLOR_MUTED       THEME_COLOR_TEXT_MUTED
#define COLOR_SECONDARY   THEME_COLOR_TEXT_SECONDARY
#define COLOR_ORANGE      THEME_COLOR_SERVICE
#define COLOR_BLUE        THEME_COLOR_SENSOR
#define COLOR_GREEN       THEME_COLOR_ACTIVE
#define COLOR_WARNING     THEME_COLOR_WARNING
#define COLOR_NAV_ACTIVE  THEME_COLOR_NAV_ACTIVE_BG

/* Network state enum (mapped to home_ui enum for settings page) */

enum network_state_e
{
  NETWORK_INITIALIZING = 0,
  NETWORK_DISCONNECTED,
  NETWORK_CHECKING,
  NETWORK_NO_INTERNET,
  NETWORK_ONLINE
};

/****************************************************************************
 * Shared state: bridge macros from old g_xxx names to g_ui.xxx
 *
 * This allows the existing main.c code to work unchanged while the
 * actual storage lives in the shared g_ui struct.
 ****************************************************************************/

#define g_status_label          g_ui.status_label
#define g_content               g_ui.content
#define g_page_host             g_ui.page_host
#define g_pages                 g_ui.pages
#define g_page_status_labels    g_ui.page_status_labels
#define g_page_settings_network_labels  g_ui.page_settings_network_labels
#define g_page_settings_probe_labels    g_ui.page_settings_probe_labels
#define g_page_settings_account_labels  g_ui.page_settings_account_labels
#define g_page_settings_online_labels   g_ui.page_settings_online_labels
#define g_page_settings_channel_labels  g_ui.page_settings_channel_labels
#define g_page_home_summary_labels      g_ui.page_home_summary_labels
#define g_page_home_online_labels       g_ui.page_home_online_labels
#define g_nav_buttons           g_ui.nav_buttons
#define g_nav_icons             g_ui.nav_icons
#define g_nav_labels            g_ui.nav_labels
#define g_nav_indicators        g_ui.nav_indicators
#define g_page_model_revisions  g_ui.page_model_revisions
#define g_network_label         g_ui.network_label
#define g_settings_network_label g_ui.settings_network_label
#define g_settings_probe_label  g_ui.settings_probe_label
#define g_settings_account_label g_ui.settings_account_label
#define g_settings_online_label g_ui.settings_online_label
#define g_settings_channel_label g_ui.settings_channel_label
#define g_login_top_button      g_ui.login_top_button
#define g_login_top_label       g_ui.login_top_label
#define g_login_shade           g_ui.login_shade
#define g_login_qr              g_ui.login_qr
#define g_login_message         g_ui.login_message
#define g_login_action_label    g_ui.login_action_label
#define g_home_summary_label    g_ui.home_summary_label
#define g_home_online_label     g_ui.home_online_label
#define g_home_suggestion_title g_ui.home_suggestion_title
#define g_home_suggestion_reason g_ui.home_suggestion_reason
#define g_home_suggestion_execute g_ui.home_suggestion_execute
#define g_home_suggestion_ignore g_ui.home_suggestion_ignore
#define g_clock_label           g_ui.clock_label
#define g_date_label            g_ui.date_label
#define g_family_model_valid    g_ui.family_model_valid
#define g_model_refresh_pending g_ui.model_refresh_pending
#define g_full_refresh_pending  g_ui.full_refresh_pending
#define g_family_ui_revision    g_ui.family_ui_revision
#define g_current_page          g_ui.current_page
#define g_selected_room         g_ui.selected_room
#define g_room_buttons          g_ui.room_buttons
#define g_room_title_label      g_ui.room_title_label
#define g_room_summary_label    g_ui.room_summary_label
#define g_room_device_host      g_ui.room_device_host
#define g_room_device_hosts     g_ui.room_device_hosts
#define g_room_visible_counts   g_ui.room_visible_counts
#define g_room_highlighted      g_ui.room_highlighted
#define g_room_detail_host      g_ui.room_detail_host
#define g_selected_device       g_ui.selected_device
#define g_detail_subtitle_label g_ui.detail_subtitle_label
#define g_detail_temperature_label g_ui.detail_temperature_label
#define g_detail_humidity_label g_ui.detail_humidity_label
#define g_detail_battery_label  g_ui.detail_battery_label
#define g_proactive_mode_label  g_ui.proactive_mode_label
#define g_proactive_history_label g_ui.proactive_history_label
#define g_proactive_time_label  g_ui.proactive_time_label
#define g_proactive_confidence_label g_ui.proactive_confidence_label
#define g_proactive_progress_label g_ui.proactive_progress_label
#define g_proactive_suggestion_title g_ui.proactive_suggestion_title
#define g_proactive_reason_label g_ui.proactive_reason_label
#define g_proactive_action_label g_ui.proactive_action_label
#define g_proactive_feedback_label g_ui.proactive_feedback_label
#define g_proactive_accept_button g_ui.proactive_accept_button
#define g_proactive_automation_button g_ui.proactive_automation_button
#define g_proactive_ignore_button g_ui.proactive_ignore_button
#define g_proactive_less_button g_ui.proactive_less_button
#define g_proactive_return_button g_ui.proactive_return_button
#define g_proactive_reset_button g_ui.proactive_reset_button
#define g_proactive_replay_button g_ui.proactive_replay_button
#define g_proactive_automation_list g_ui.proactive_automation_list
#define g_proactive_confirm_shade g_ui.proactive_confirm_shade
#define g_proactive_automation_signature g_ui.proactive_automation_signature
#define g_proactive_confirm_key g_ui.proactive_confirm_key
#define g_displayed_proactive_revision g_ui.displayed_proactive_revision
#define g_displayed_agent_revision g_ui.displayed_agent_revision

/* Helper function aliases */

#define set_label_text_if_changed home_ui_set_label_text_if_changed
#define configure_fast_button     home_ui_configure_fast_button
#define style_toggle              home_ui_style_toggle
#define make_label                home_ui_make_label
#define set_chinese_font          home_ui_set_chinese_font
#define make_action_button        home_ui_make_action_button
#define style_secondary_action    home_ui_style_secondary_action
#define make_info_row             home_ui_make_info_row
#define action_clicked            home_ui_action_clicked
#define control_display_name      home_ui_control_display_name
#define format_control_value      home_ui_format_control_value
#define set_command_status        home_ui_set_command_status
#define format_device_value       home_ui_format_device_value
#define device_state_text         home_ui_device_state_text
#define device_symbol             home_ui_device_symbol
#define device_type_text          home_ui_device_type_text
#define create_room_device_container home_ui_make_room_device_container

/* Binding type aliases */

#define home_panel_device_binding_s  home_ui_device_binding_s
#define home_panel_control_binding_s home_ui_control_binding_s
#define home_panel_action_binding_s  home_ui_action_binding_s
#define home_panel_scene_binding_s   home_ui_scene_binding_s

/* Device/control/scene binding macros */

#define g_device_bindings       g_ui.device_bindings
#define g_device_binding_counts g_ui.device_binding_counts
#define g_control_bindings      g_ui.control_bindings
#define g_control_binding_count g_ui.control_binding_count
#define g_action_bindings       g_ui.action_bindings
#define g_action_binding_count  g_ui.action_binding_count
#define g_scene_bindings        g_ui.scene_bindings

/* Proactive automation UI - type alias to home_ui.h definition */

#define proactive_automation_ui_s home_ui_proactive_automation_ui_s
#define g_proactive_automation_ui g_ui.proactive_automation_ui

/* Data that stays in main.c (not in g_ui) */

static lv_image_dsc_t g_login_qr_image;
static uint8_t *g_login_qr_data;
static uint32_t g_login_qr_revision;
static lv_timer_t *g_login_success_timer;
static struct home_panel_family_model_s g_family_model;
static struct home_panel_family_model_s g_family_update_model;

/* Binding types and arrays are now in g_ui (see home_ui.h).
 * Macros above alias old names to g_ui fields.
 */

static volatile enum network_state_e g_network_state =
  NETWORK_INITIALIZING;
static volatile bool g_network_refresh_requested = true;
static uint16_t g_network_ping_id;
static uint16_t g_network_dns_id;
static bool g_ntp_started;
static bool g_time_synced;
static bool g_login_autostart_attempted;
static uint32_t g_last_time_update;
static uint32_t g_next_ntp_attempt;
static void nav_clicked(lv_event_t *event);
static void show_page(unsigned int page);
static void delete_page(unsigned int page);
static void apply_mijia_snapshot(
  const struct home_panel_mijia_snapshot_s *snapshot);

/* set_label_text_if_changed, configure_fast_button, style_toggle
 * moved to ui/home_ui_components.c (aliased via macros above).
 */

static void update_time_ui(bool force)
{
  static const char *weekdays[] =
  {
    "星期日", "星期一", "星期二", "星期三",
    "星期四", "星期五", "星期六"
  };
  struct tm local_time;
  char clock_text[16];
  char date_text[48];
  uint32_t now_ms = lv_tick_get();
  time_t now;
  int ret;

  if (!force && lv_tick_elaps(g_last_time_update) <
      TIME_UPDATE_INTERVAL_MS)
    {
      return;
    }

  g_last_time_update = now_ms;
  if (g_network_state == NETWORK_ONLINE && !g_ntp_started &&
      (g_next_ntp_attempt == 0 ||
       (int32_t)(now_ms - g_next_ntp_attempt) >= 0))
    {
      ret = ntpc_start();
      if (ret >= 0 || ret == -EALREADY)
        {
          g_ntp_started = true;
          syslog(LOG_INFO, "[HOME][TIME] ntp client started pid=%d\n",
                 ret);
        }
      else
        {
          g_next_ntp_attempt = now_ms + NTP_RETRY_INTERVAL_MS;
          syslog(LOG_WARNING, "[HOME][TIME] ntp start failed ret=%d\n",
                 ret);
        }
    }

  now = time(NULL);
  if (now < TIME_VALID_EPOCH)
    {
      if (g_clock_label != NULL)
        {
          set_label_text_if_changed(g_clock_label, "--:--");
        }
      if (g_date_label != NULL)
        {
          set_label_text_if_changed(g_date_label, "等待网络校时");
        }
      return;
    }

  if (!g_time_synced)
    {
      g_time_synced = true;
      syslog(LOG_INFO, "[HOME][TIME] synchronized epoch=%ld timezone=UTC+8\n",
             (long)now);
    }

  now += 8 * 60 * 60;
  gmtime_r(&now, &local_time);
  snprintf(clock_text, sizeof(clock_text), "%02d:%02d",
           local_time.tm_hour, local_time.tm_min);
  snprintf(date_text, sizeof(date_text), "%d月%d日  %s",
           local_time.tm_mon + 1, local_time.tm_mday,
           weekdays[local_time.tm_wday]);
  if (g_clock_label != NULL)
    {
      if (set_label_text_if_changed(g_clock_label, clock_text))
        {
          /* Keep the top-bar slot stable so a shorter time never leaves
           * stale glyphs outside the new label bounds during a local flush.
           */
          lv_obj_invalidate(g_clock_label);
        }
    }
  if (g_date_label != NULL)
    {
      if (set_label_text_if_changed(g_date_label, date_text))
        {
          lv_obj_invalidate(g_date_label);
        }
    }
}

static const char *network_state_name(enum network_state_e state)
{
  switch (state)
    {
      case NETWORK_INITIALIZING:
        return "initializing";
      case NETWORK_CHECKING:
        return "checking";
      case NETWORK_NO_INTERNET:
        return "no-internet";
      case NETWORK_ONLINE:
        return "online";
      case NETWORK_DISCONNECTED:
        return "cable-disconnected";
      default:
        return "unknown";
    }
}

static void network_set_state(enum network_state_e state)
{
  if (g_network_state != state)
    {
      g_network_state = state;
      syslog(LOG_INFO, "[HOME][NET] state=%s\n",
             network_state_name(state));
    }
}

enum network_link_state_e
{
  NETWORK_LINK_INITIALIZING = 0,
  NETWORK_LINK_DOWN,
  NETWORK_LINK_UP
};

static enum network_link_state_e network_get_link_state(uint8_t *ifflags)
{
  uint8_t flags = 0;

  if (netlib_getifstatus(NETWORK_INTERFACE, &flags) < 0)
    {
      *ifflags = 0;
      return NETWORK_LINK_INITIALIZING;
    }

  *ifflags = flags;
  if ((flags & IFF_RUNNING) != 0)
    {
      return NETWORK_LINK_UP;
    }

  return (flags & IFF_UP) != 0 ? NETWORK_LINK_DOWN :
                                 NETWORK_LINK_INITIALIZING;
}

static bool network_has_carrier(void)
{
  uint8_t flags;

  return network_get_link_state(&flags) == NETWORK_LINK_UP;
}

static bool network_get_ipv4_address(struct in_addr *address)
{
  return netlib_get_ipv4addr(NETWORK_INTERFACE, address) >= 0 &&
         address->s_addr != INADDR_ANY;
}

struct network_ping_packet_s
{
  struct icmp_hdr_s header;
  uint8_t data[NETWORK_PING_DATA_SIZE];
};

static uint16_t network_ping_checksum(const void *buffer, size_t length)
{
  const uint16_t *word = buffer;
  uint32_t sum = 0;

  while (length > 1)
    {
      sum += *word++;
      length -= 2;
    }

  if (length != 0)
    {
      sum += *(const uint8_t *)word;
    }

  while ((sum >> 16) != 0)
    {
      sum = (sum & 0xffff) + (sum >> 16);
    }

  return (uint16_t)~sum;
}

struct network_dns_context_s
{
  struct sockaddr_in server;
  bool found;
};

static int network_dns_server_callback(void *arg, struct sockaddr *address,
                                       socklen_t address_length)
{
  struct network_dns_context_s *context = arg;

  (void)address_length;
  if (address->sa_family == AF_INET)
    {
      memcpy(&context->server, address, sizeof(context->server));
      context->found = true;
      return 1;
    }

  return 0;
}

static bool network_get_dns_server(struct sockaddr_in *server)
{
  struct network_dns_context_s context;

  memset(&context, 0, sizeof(context));
  dns_foreach_nameserver(network_dns_server_callback, &context);
  if (context.found)
    {
      *server = context.server;
    }
  else
    {
      memset(server, 0, sizeof(*server));
      server->sin_family = AF_INET;
      if (netlib_get_dripv4addr(NETWORK_INTERFACE,
                                &server->sin_addr) < 0 ||
          server->sin_addr.s_addr == INADDR_ANY)
        {
          return false;
        }
    }

  if (server->sin_port == 0)
    {
      server->sin_port = htons(DNS_DEFAULT_PORT);
    }

  return true;
}

static uint16_t network_dns_read_u16(const uint8_t *data)
{
  uint16_t value;

  memcpy(&value, data, sizeof(value));
  return ntohs(value);
}

static void network_dns_write_u16(uint8_t *data, uint16_t value)
{
  value = htons(value);
  memcpy(data, &value, sizeof(value));
}

static bool network_dns_skip_name(const uint8_t *message, size_t length,
                                  size_t *offset)
{
  while (*offset < length)
    {
      uint8_t label_length = message[(*offset)++];

      if (label_length == 0)
        {
          return true;
        }

      if ((label_length & 0xc0) == 0xc0)
        {
          if (*offset >= length)
            {
              return false;
            }

          (*offset)++;
          return true;
        }

      if (label_length > 63 || *offset + label_length > length)
        {
          return false;
        }

      *offset += label_length;
    }

  return false;
}

static bool network_resolve_host(const char *hostname,
                                 struct in_addr *resolved_address)
{
  struct sockaddr_in dns_server;
  uint8_t query[NETWORK_DNS_BUFFER_SIZE];
  uint8_t response[NETWORK_DNS_BUFFER_SIZE];
  struct dns_header_s *header = (struct dns_header_s *)query;
  const char *label = hostname;
  uint16_t query_id = ++g_network_dns_id;
  if (query_id == 0)
    {
      query_id = ++g_network_dns_id;
    }
  size_t query_length = sizeof(struct dns_header_s);
  unsigned int poll_count;
  int sockfd;
  int ret;

  if (!network_get_dns_server(&dns_server))
    {
      syslog(LOG_WARNING, "[HOME][NET] host=%s dns-server=missing\n",
             hostname);
      return false;
    }

  memset(query, 0, sizeof(query));
  header->id = htons(query_id);
  header->flags1 = DNS_FLAG1_RD;
  header->numquestions = htons(1);

  while (*label != '\0')
    {
      const char *dot = strchr(label, '.');
      size_t label_length = dot == NULL ? strlen(label) :
                                          (size_t)(dot - label);

      if (label_length == 0 || label_length > 63 ||
          query_length + label_length + 6 > sizeof(query))
        {
          return false;
        }

      query[query_length++] = (uint8_t)label_length;
      memcpy(&query[query_length], label, label_length);
      query_length += label_length;
      if (dot == NULL)
        {
          break;
        }

      label = dot + 1;
    }

  query[query_length++] = 0;
  network_dns_write_u16(&query[query_length], DNS_RECTYPE_A);
  query_length += 2;
  network_dns_write_u16(&query[query_length], DNS_CLASS_IN);
  query_length += 2;

  sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sockfd < 0)
    {
      syslog(LOG_WARNING, "[HOME][NET] host=%s dns-socket=%d\n",
             hostname, errno);
      return false;
    }

  if (fcntl(sockfd, F_SETFL, O_NONBLOCK) < 0)
    {
      syslog(LOG_WARNING, "[HOME][NET] host=%s dns-nonblock=%d\n",
             hostname, errno);
      close(sockfd);
      return false;
    }

  ret = sendto(sockfd, query, query_length, MSG_DONTWAIT,
               (const struct sockaddr *)&dns_server,
               sizeof(dns_server));
  if (ret != (int)query_length)
    {
      syslog(LOG_WARNING, "[HOME][NET] host=%s dns-send=%d errno=%d\n",
             hostname, ret, errno);
      close(sockfd);
      return false;
    }

  for (poll_count = 0; poll_count < NETWORK_PING_POLLS; poll_count++)
    {
      ssize_t received;

      received = recvfrom(sockfd, response, sizeof(response),
                          MSG_DONTWAIT, NULL, NULL);
      if (received >= (ssize_t)sizeof(struct dns_header_s))
        {
          struct dns_header_s *response_header =
            (struct dns_header_s *)response;
          size_t offset = sizeof(struct dns_header_s);
          unsigned int index;

          if (ntohs(response_header->id) != query_id ||
              (response_header->flags1 & DNS_FLAG1_RESPONSE) == 0 ||
              (response_header->flags2 & DNS_FLAG2_ERR_MASK) != 0)
            {
              continue;
            }

          for (index = 0;
               index < ntohs(response_header->numquestions); index++)
            {
              if (!network_dns_skip_name(response, received, &offset) ||
                  offset + 4 > (size_t)received)
                {
                  break;
                }

              offset += 4;
            }

          for (index = 0; index < ntohs(response_header->numanswers);
               index++)
            {
              uint16_t record_type;
              uint16_t record_class;
              uint16_t record_length;

              if (!network_dns_skip_name(response, received, &offset) ||
                  offset + 10 > (size_t)received)
                {
                  break;
                }

              record_type = network_dns_read_u16(&response[offset]);
              record_class = network_dns_read_u16(&response[offset + 2]);
              record_length = network_dns_read_u16(&response[offset + 8]);
              offset += 10;
              if (offset + record_length > (size_t)received)
                {
                  break;
                }

              if (record_type == DNS_RECTYPE_A &&
                  record_class == DNS_CLASS_IN && record_length == 4)
                {
                  memcpy(resolved_address, &response[offset], 4);
                  close(sockfd);
                  return true;
                }

              offset += record_length;
            }
        }
      else if (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
               errno != EINTR)
        {
          syslog(LOG_WARNING,
                 "[HOME][NET] host=%s dns-recv=%d\n", hostname, errno);
          break;
        }

      usleep(NETWORK_PING_POLL_US);
    }

  syslog(LOG_WARNING, "[HOME][NET] host=%s dns-timeout\n", hostname);
  close(sockfd);
  return false;
}

static int network_ping_host(const char *hostname)
{
  struct network_ping_packet_s packet;
  struct sockaddr_in destination;
  uint8_t receive_buffer[64];
  uint16_t ping_id;
  unsigned int poll_count;
  int sockfd;
  int ret;

  memset(&destination, 0, sizeof(destination));
  destination.sin_family = AF_INET;
  if (!network_resolve_host(hostname, &destination.sin_addr))
    {
      return 0;
    }

  sockfd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
  if (sockfd < 0)
    {
      syslog(LOG_WARNING, "[HOME][NET] host=%s socket=%d\n",
             hostname, errno);
      return 0;
    }

  if (fcntl(sockfd, F_SETFL, O_NONBLOCK) < 0)
    {
      syslog(LOG_WARNING, "[HOME][NET] host=%s nonblock=%d\n",
             hostname, errno);
      close(sockfd);
      return 0;
    }

  memset(&packet, 0, sizeof(packet));
  ping_id = ++g_network_ping_id;
  if (ping_id == 0)
    {
      ping_id = ++g_network_ping_id;
    }

  packet.header.type = ICMP_ECHO_REQUEST;
  packet.header.id = htons(ping_id);
  packet.header.seqno = htons(1);
  memset(packet.data, 0x5a, sizeof(packet.data));
  packet.header.icmpchksum = network_ping_checksum(&packet,
                                                   sizeof(packet));

  ret = sendto(sockfd, &packet, sizeof(packet), MSG_DONTWAIT,
               (const struct sockaddr *)&destination,
               sizeof(destination));
  if (ret != (int)sizeof(packet))
    {
      syslog(LOG_WARNING, "[HOME][NET] host=%s send=%d errno=%d\n",
             hostname, ret, errno);
      close(sockfd);
      return 0;
    }

  for (poll_count = 0; poll_count < NETWORK_PING_POLLS; poll_count++)
    {
      ssize_t received;
      size_t offset = 0;
      struct icmp_hdr_s *reply;

      received = recvfrom(sockfd, receive_buffer, sizeof(receive_buffer),
                          MSG_DONTWAIT, NULL, NULL);
      if (received > 0)
        {
          if ((receive_buffer[0] >> 4) == 4)
            {
              offset = (receive_buffer[0] & 0x0f) * 4;
            }

          if ((size_t)received >= offset + sizeof(struct icmp_hdr_s))
            {
              reply = (struct icmp_hdr_s *)(receive_buffer + offset);
              if (reply->type == ICMP_ECHO_REPLY &&
                  ntohs(reply->id) == ping_id &&
                  ntohs(reply->seqno) == 1)
                {
                  close(sockfd);
                  return 1;
                }
            }
        }
      else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
        {
          syslog(LOG_WARNING, "[HOME][NET] host=%s recv=%d\n",
                 hostname, errno);
          break;
        }

      usleep(NETWORK_PING_POLL_US);
    }

  close(sockfd);
  return 0;
}

static void *network_worker(void *arg)
{
  unsigned int elapsed = NETWORK_PROBE_INTERVAL;
  unsigned int initial_down_seconds = 0;
  bool was_connected = false;
  bool link_seen = false;
  enum network_link_state_e last_link = NETWORK_LINK_INITIALIZING;
  uint8_t last_flags = UINT8_MAX;

  (void)arg;

  for (;;)
    {
      uint8_t flags;
      enum network_link_state_e link = network_get_link_state(&flags);
      bool connected = link == NETWORK_LINK_UP;
      bool refresh = g_network_refresh_requested;
      struct in_addr address;

      if (link != last_link || flags != last_flags)
        {
          const char *link_name = link == NETWORK_LINK_UP ? "up" :
                                  link == NETWORK_LINK_DOWN ? "down" :
                                  "initializing";

          syslog(LOG_INFO, "[HOME][NET] carrier=%s flags=%02x\n",
                 link_name, flags);
          last_link = link;
          last_flags = flags;
        }

      g_network_refresh_requested = false;
      if (link == NETWORK_LINK_INITIALIZING)
        {
          network_set_state(NETWORK_INITIALIZING);
          was_connected = false;
          elapsed = NETWORK_PROBE_INTERVAL;
          initial_down_seconds = 0;
        }
      else if (!connected)
        {
          if (!link_seen && initial_down_seconds < NETWORK_LINK_GRACE_SEC)
            {
              network_set_state(NETWORK_INITIALIZING);
              initial_down_seconds++;
            }
          else
            {
              network_set_state(NETWORK_DISCONNECTED);
            }
          was_connected = false;
          elapsed = NETWORK_PROBE_INTERVAL;
        }
      else if (!network_get_ipv4_address(&address))
        {
          /* DHCP runs independently. Keep checking without reporting a
           * false Internet failure while an address is being acquired.
           */

          network_set_state(NETWORK_CHECKING);
          link_seen = true;
          initial_down_seconds = 0;
          was_connected = false;
        }
      else if (!was_connected || refresh ||
               elapsed >= NETWORK_PROBE_INTERVAL)
        {
          int replies;
          char address_text[INET_ADDRSTRLEN];

          /* Keep the last definitive result visible during an automatic
           * background probe.  Only initial and user-requested probes show
           * the transient checking state.
           */

          if (!was_connected || refresh)
            {
              network_set_state(NETWORK_CHECKING);
            }
          link_seen = true;
          initial_down_seconds = 0;
          inet_ntop(AF_INET, &address, address_text, sizeof(address_text));
          replies = network_ping_host("mi.com");
          if (replies <= 0 && network_has_carrier())
            {
              replies = network_ping_host("xiaomi.cn");
            }

          if (!network_has_carrier())
            {
              network_set_state(NETWORK_DISCONNECTED);
            }
          else
            {
              network_set_state(replies > 0 ? NETWORK_ONLINE :
                                              NETWORK_NO_INTERNET);
              syslog(LOG_INFO,
                     "[HOME][NET] health ipv4=%s result=%s\n",
                     address_text, replies > 0 ? "online" : "unreachable");
            }

          was_connected = true;
          elapsed = 0;
        }
      else
        {
          elapsed++;
        }

      sleep(1);
    }

  return NULL;
}

static int start_network_monitor(void)
{
  pthread_attr_t attr;
  struct sched_param param;
  pthread_t thread;
  int ret;

  ret = pthread_attr_init(&attr);
  if (ret != 0)
    {
      return ret;
    }

  pthread_attr_setstacksize(&attr, NETWORK_THREAD_STACK);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  memset(&param, 0, sizeof(param));
  param.sched_priority = NETWORK_THREAD_PRIORITY;
  pthread_attr_setschedparam(&attr, &param);
  pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
  ret = pthread_create(&thread, &attr, network_worker, NULL);
  pthread_attr_destroy(&attr);
  return ret;
}

static enum home_ui_network_state_e network_to_ui_state(
  enum network_state_e state)
{
  switch (state)
    {
      case NETWORK_INITIALIZING:
        return HOME_UI_NET_INITIALIZING;
      case NETWORK_DISCONNECTED:
        return HOME_UI_NET_DISCONNECTED;
      case NETWORK_CHECKING:
        return HOME_UI_NET_CHECKING;
      case NETWORK_NO_INTERNET:
        return HOME_UI_NET_NO_INTERNET;
      case NETWORK_ONLINE:
        return HOME_UI_NET_ONLINE;
      default:
        return HOME_UI_NET_INITIALIZING;
    }
}

static void apply_network_state(enum network_state_e state)
{
  const char *top_text;
  const char *settings_text;
  const char *probe_text;
  lv_color_t color;

  if (state == NETWORK_ONLINE)
    {
      top_text = LV_SYMBOL_OK "  已连接互联网";
      settings_text = "已连接互联网";
      probe_text = "mi.com / xiaomi.cn 可达";
      color = lv_color_hex(COLOR_SECONDARY);
    }
  else if (state == NETWORK_NO_INTERNET)
    {
      top_text = LV_SYMBOL_WARNING "  无互联网连接";
      settings_text = "无互联网连接";
      probe_text = "mi.com / xiaomi.cn 不可达";
      color = lv_color_hex(COLOR_WARNING);
    }
  else if (state == NETWORK_CHECKING)
    {
      top_text = LV_SYMBOL_REFRESH "  正在检测网络";
      settings_text = "网线已连接";
      probe_text = "正在检测互联网";
      color = lv_color_hex(COLOR_BLUE);
    }
  else if (state == NETWORK_INITIALIZING)
    {
      top_text = LV_SYMBOL_REFRESH "  正在初始化网络";
      settings_text = "正在初始化有线网络";
      probe_text = "等待网络接口启动";
      color = lv_color_hex(COLOR_BLUE);
    }
  else
    {
      top_text = LV_SYMBOL_WARNING "  有线网络未连接";
      settings_text = "有线网络未连接";
      probe_text = "等待网线连接";
      color = lv_color_hex(COLOR_WARNING);
    }

  lv_label_set_text(g_network_label, top_text);
  lv_obj_set_style_text_color(g_network_label, color, 0);

  g_ui.network_state = network_to_ui_state(state);
  settings_page_update_network(g_ui.network_state);
}

/* set_chinese_font, make_label, action_clicked moved to ui/home_ui_components.c */

static void scene_clicked(lv_event_t *event)
{
  struct home_ui_scene_binding_s *binding =
    lv_event_get_user_data(event);
  char message[64];
  int ret;

  if (binding == NULL || binding->scene == NULL)
    {
      return;
    }

  ret = home_panel_mijia_request_scene(binding->scene->id,
                                       binding->scene->name);
  snprintf(message, sizeof(message), ret == 0 ? "正在执行：%s" :
                                                "场景执行失败：%s",
           binding->scene->name);
  lv_label_set_text(g_ui.status_label, message);
  lv_obj_set_style_text_color(g_ui.status_label,
                              lv_color_hex(ret == 0 ? COLOR_BLUE :
                                                       COLOR_ORANGE), 0);
}

/* control_display_name, format_control_value, set_command_status
 * moved to ui/home_ui_components.c (aliased via macros above).
 */

/* device_toggled, detail_boolean_changed, detail_number_changed,
 * detail_number_released, detail_enum_changed, detail_action_clicked
 * moved to ui/page_rooms.c
 */

static void login_close_dialog(lv_obj_t *shade)
{
  if (shade == NULL || shade != g_login_shade)
    {
      return;
    }

  if (g_login_success_timer != NULL)
    {
      lv_timer_delete(g_login_success_timer);
      g_login_success_timer = NULL;
    }

  /* Release the QR payload so a closed login dialog does not keep a ~115 KiB
   * allocation resident for the whole session.
   */

  if (g_login_qr_data != NULL)
    {
      lv_image_cache_drop(&g_login_qr_image);
      if (g_login_qr != NULL)
        {
          lv_image_set_src(g_login_qr, NULL);
        }

      free(g_login_qr_data);
      g_login_qr_data = NULL;
      g_login_qr_revision = 0;
      memset(&g_login_qr_image, 0, sizeof(g_login_qr_image));
    }

  g_login_shade = NULL;
  g_login_qr = NULL;
  g_login_message = NULL;
  g_login_action_label = NULL;
  lv_obj_delete_async(shade);
}

static void login_close(lv_event_t *event)
{
  login_close_dialog(lv_event_get_user_data(event));
}

static void login_success_timeout(lv_timer_t *timer)
{
  lv_obj_t *shade = g_login_shade;

  g_login_success_timer = NULL;
  lv_timer_delete(timer);
  login_close_dialog(shade);
  show_page(0);
}

static bool login_update_qr(
  const struct home_panel_mijia_snapshot_s *snapshot)
{
  lv_image_header_t header;
  lv_area_t area;
  uint8_t *data;
  size_t size;
  unsigned int dark_pixels = 0;
  size_t offset;
  int ret;

  if (snapshot->qr_size == 0)
    {
      return false;
    }

  if (g_login_qr_data != NULL &&
      g_login_qr_revision == snapshot->qr_revision)
    {
      lv_image_set_src(g_login_qr, &g_login_qr_image);
      if (lv_image_get_src(g_login_qr) != &g_login_qr_image)
        {
          syslog(LOG_ERR, "[HOME][UI] qr source rejected on reuse\n");
          return false;
        }

      lv_obj_remove_flag(g_login_qr, LV_OBJ_FLAG_HIDDEN);
      lv_obj_invalidate(g_login_qr);
      return true;
    }

  data = malloc(snapshot->qr_size);
  if (data == NULL)
    {
      return false;
    }

  ret = home_panel_mijia_copy_qr(snapshot->qr_revision, data,
                                 snapshot->qr_size, &size);
  if (ret < 0)
    {
      free(data);
      return false;
    }

  if (g_login_qr_data != NULL)
    {
      lv_image_cache_drop(&g_login_qr_image);
      lv_image_set_src(g_login_qr, NULL);
      free(g_login_qr_data);
    }

  memset(&g_login_qr_image, 0, sizeof(g_login_qr_image));
  g_login_qr_data = data;
  g_login_qr_revision = snapshot->qr_revision;
  g_login_qr_image.header.magic = LV_IMAGE_HEADER_MAGIC;
  g_login_qr_image.header.cf = LV_COLOR_FORMAT_RGB565;
  g_login_qr_image.header.w = 240;
  g_login_qr_image.header.h = 240;
  g_login_qr_image.header.stride = 480;
  g_login_qr_image.data_size = size;
  g_login_qr_image.data = data;

  memset(&header, 0, sizeof(header));
  if (lv_image_decoder_get_info(&g_login_qr_image, &header) !=
      LV_RESULT_OK || header.cf != LV_COLOR_FORMAT_RGB565 ||
      header.w != 240 || header.h != 240 || header.stride != 480)
    {
      syslog(LOG_ERR,
             "[HOME][UI] qr decoder rejected descriptor cf=%u %ux%u stride=%u\n",
             (unsigned int)header.cf, (unsigned int)header.w,
             (unsigned int)header.h, (unsigned int)header.stride);
      free(g_login_qr_data);
      g_login_qr_data = NULL;
      g_login_qr_revision = 0;
      memset(&g_login_qr_image, 0, sizeof(g_login_qr_image));
      return false;
    }

  for (offset = 0; offset + 1 < size; offset += 2)
    {
      if (data[offset] == 0 && data[offset + 1] == 0)
        {
          dark_pixels++;
        }
    }

  lv_image_set_src(g_login_qr, &g_login_qr_image);
  if (lv_image_get_src(g_login_qr) != &g_login_qr_image)
    {
      syslog(LOG_ERR, "[HOME][UI] qr source rejected after decode\n");
      free(g_login_qr_data);
      g_login_qr_data = NULL;
      g_login_qr_revision = 0;
      memset(&g_login_qr_image, 0, sizeof(g_login_qr_image));
      return false;
    }

  lv_obj_remove_flag(g_login_qr, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(g_login_qr);
  lv_obj_invalidate(g_login_qr);
  lv_obj_update_layout(g_login_qr);
  lv_obj_get_coords(g_login_qr, &area);
  syslog(LOG_INFO,
         "[HOME][UI] qr ready RGB565 240x240 bytes=%u dark=%u "
         "area=%d,%d-%d,%d hidden=%u revision=%u\n",
         (unsigned int)size, dark_pixels,
         (int)area.x1, (int)area.y1, (int)area.x2, (int)area.y2,
         lv_obj_has_flag(g_login_qr, LV_OBJ_FLAG_HIDDEN) ? 1 : 0,
         (unsigned int)snapshot->qr_revision);
  return true;
}

static void login_action(lv_event_t *event)
{
  struct home_panel_mijia_snapshot_s snapshot;

  home_panel_mijia_get_snapshot(&snapshot);
  if (snapshot.state == HOME_PANEL_MIJIA_AUTHENTICATED)
    {
      login_close(event);
      return;
    }

  if (g_network_state != NETWORK_ONLINE)
    {
      lv_label_set_text(g_login_message,
                        "请先连接可访问互联网的有线网络");
      return;
    }

  if (home_panel_mijia_request_login() < 0)
    {
      lv_label_set_text(g_login_message, "米家登录服务尚未就绪");
    }
}

static void show_login(lv_event_t *event)
{
  lv_obj_t *shade;
  lv_obj_t *dialog;
  lv_obj_t *close;
  lv_obj_t *action;
  lv_obj_t *label;
  struct home_panel_mijia_snapshot_s snapshot;

  (void)event;

  if (g_login_shade != NULL)
    {
      return;
    }

  /* Open-docs layers: modal overlays belong on the top layer so the
   * scrim always covers navigation, status, and the active page.
   */

  shade = lv_obj_create(lv_layer_top());
  g_login_shade = shade;
  lv_obj_remove_style_all(shade);
  lv_obj_set_size(shade, PANEL_WIDTH, PANEL_HEIGHT);
  lv_obj_set_pos(shade, 0, 0);
  theme_apply_scrim(shade);

  dialog = lv_obj_create(shade);
  lv_obj_set_size(dialog, 680, 410);
  lv_obj_center(dialog);
  lv_obj_set_style_radius(dialog, THEME_RADIUS_DIALOG, 0);
  lv_obj_set_style_border_width(dialog, 1, 0);
  lv_obj_set_style_border_color(dialog, lv_color_hex(COLOR_BORDER), 0);
  lv_obj_set_style_bg_color(dialog, lv_color_hex(COLOR_SURFACE), 0);
  lv_obj_set_style_bg_opa(dialog, LV_OPA_COVER, 0);
  theme_apply_surface_gradient(dialog);
  lv_obj_set_style_pad_all(dialog, 24, 0);
  lv_obj_clear_flag(dialog, LV_OBJ_FLAG_SCROLLABLE);

  label = make_label(dialog, "米家账号登录", 282, 8,
                     lv_color_hex(COLOR_TEXT), home_panel_font_get());
  lv_obj_set_style_text_font(label, home_panel_font_get(), 0);

  g_login_qr = lv_image_create(dialog);
  lv_obj_set_size(g_login_qr, 256, 256);
  lv_image_set_inner_align(g_login_qr, LV_IMAGE_ALIGN_CENTER);
  lv_obj_set_pos(g_login_qr, 18, 62);
  lv_obj_set_style_border_color(g_login_qr, lv_color_hex(0xffffff), 0);
  lv_obj_set_style_border_width(g_login_qr, 8, 0);
  lv_obj_add_flag(g_login_qr, LV_OBJ_FLAG_HIDDEN);

  g_login_message = make_label(dialog, "正在准备米家登录",
                               282, 78, lv_color_hex(COLOR_MUTED),
                               home_panel_font_get());
  lv_obj_set_width(g_login_message, 340);
  lv_label_set_long_mode(g_login_message, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_line_space(g_login_message, 12, 0);

  label = make_label(dialog,
                     "请使用米家 App 扫码。账号认证数据只保存在\n"
                     "本地服务端的加密保险箱中。",
                     282, 164, lv_color_hex(COLOR_MUTED),
                     home_panel_font_get());
  lv_obj_set_style_text_line_space(label, 10, 0);

  close = lv_button_create(dialog);
  configure_fast_button(close);
  lv_obj_set_size(close, 44, 44);
  lv_obj_set_ext_click_area(close, 4);
  lv_obj_align(close, LV_ALIGN_TOP_RIGHT, 0, 0);
  lv_obj_set_style_radius(close, THEME_RADIUS_SM, 0);
  lv_obj_set_style_bg_color(close, lv_color_hex(COLOR_SURFACE_2), 0);
  lv_obj_set_style_bg_color(close, lv_color_hex(0x343b44),
                            LV_STATE_PRESSED);
  lv_obj_add_event_cb(close, login_close, LV_EVENT_CLICKED, shade);
  label = lv_label_create(close);
  lv_label_set_text(label, LV_SYMBOL_CLOSE);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
  lv_obj_center(label);

  action = lv_button_create(dialog);
  configure_fast_button(action);
  lv_obj_set_size(action, 144, THEME_MIN_TOUCH);
  lv_obj_align(action, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
  lv_obj_set_style_radius(action, THEME_RADIUS_CTRL, 0);
  lv_obj_set_style_bg_color(action, lv_color_hex(COLOR_BLUE), 0);
  lv_obj_set_style_bg_color(action, lv_color_hex(0x3475d6),
                            LV_STATE_PRESSED);
  lv_obj_add_event_cb(action, login_action, LV_EVENT_CLICKED, shade);
  g_login_action_label = lv_label_create(action);
  lv_label_set_text(g_login_action_label, "重新生成");
  set_chinese_font(g_login_action_label);
  lv_obj_center(g_login_action_label);

  home_panel_mijia_get_snapshot(&snapshot);
  if (snapshot.state == HOME_PANEL_MIJIA_AUTHENTICATED)
    {
      apply_mijia_snapshot(&snapshot);
    }
  else if (g_network_state == NETWORK_ONLINE)
    {
      home_panel_mijia_request_login();
    }
  else if (g_network_state != NETWORK_ONLINE)
    {
      lv_label_set_text(g_login_message,
                        "请先连接可访问互联网的有线网络");
    }
}

static void apply_mijia_snapshot(
  const struct home_panel_mijia_snapshot_s *snapshot)
{
  char text[160];

  if (g_login_top_label != NULL)
    {
      bool authenticated =
        snapshot->state == HOME_PANEL_MIJIA_AUTHENTICATED;

      set_label_text_if_changed(
        g_login_top_label,
        authenticated ? "米家已登录" : "登录米家");
      lv_obj_set_style_text_color(
        g_login_top_label,
        lv_color_hex(authenticated ? COLOR_MUTED : COLOR_TEXT), 0);
      if (g_login_top_button != NULL)
        {
          lv_obj_set_style_bg_color(
            g_login_top_button,
            lv_color_hex(authenticated ? COLOR_TOPBAR : COLOR_BLUE), 0);
          lv_obj_set_style_bg_color(
            g_login_top_button,
            lv_color_hex(authenticated ? COLOR_SURFACE_2 : 0x478ee8),
            LV_STATE_PRESSED);
          lv_obj_set_style_border_width(g_login_top_button,
                                        authenticated ? 0 : 1, 0);
        }
    }

  if (g_settings_account_label != NULL)
    {
      if (snapshot->state == HOME_PANEL_MIJIA_AUTHENTICATED)
        {
          snprintf(text, sizeof(text), "%s",
                   snapshot->home_name[0] != '\0' ?
                   snapshot->home_name : "米家账号");
          set_label_text_if_changed(g_settings_account_label, text);
          lv_obj_set_style_text_color(g_settings_account_label,
                                      lv_color_hex(COLOR_TEXT), 0);
        }
      else
        {
          set_label_text_if_changed(g_settings_account_label, "未登录");
          lv_obj_set_style_text_color(g_settings_account_label,
                                      lv_color_hex(COLOR_MUTED), 0);
        }
    }

  if (g_settings_online_label != NULL)
    {
      if (snapshot->state == HOME_PANEL_MIJIA_AUTHENTICATED)
        {
          snprintf(text, sizeof(text), "%u / %u 台在线",
                   snapshot->online_count, snapshot->device_count);
          set_label_text_if_changed(g_settings_online_label, text);
          lv_obj_set_style_text_color(g_settings_online_label,
                                      lv_color_hex(COLOR_TEXT), 0);
        }
      else
        {
          set_label_text_if_changed(g_settings_online_label, "等待同步");
          lv_obj_set_style_text_color(g_settings_online_label,
                                      lv_color_hex(COLOR_MUTED), 0);
        }
    }

  if (g_home_summary_label != NULL && !g_family_model_valid)
    {
      if (snapshot->state == HOME_PANEL_MIJIA_IDLE)
        {
          set_label_text_if_changed(g_home_summary_label,
                                    "登录米家后同步家庭设备");
        }
      else if (snapshot->state == HOME_PANEL_MIJIA_AUTHENTICATED)
        {
          set_label_text_if_changed(g_home_summary_label,
                                    "正在同步米家设备");
        }
      else
        {
          set_label_text_if_changed(g_home_summary_label,
                                    snapshot->message);
        }
    }

  if (g_login_shade == NULL)
    {
      return;
    }

  lv_label_set_text(g_login_message, snapshot->message);
  if (snapshot->state == HOME_PANEL_MIJIA_WAITING)
    {
      if (!login_update_qr(snapshot))
        {
          lv_obj_add_flag(g_login_qr, LV_OBJ_FLAG_HIDDEN);
          lv_label_set_text(g_login_message, "登录二维码加载失败");
        }

      lv_label_set_text(g_login_action_label, "重新生成");
    }
  else if (snapshot->state == HOME_PANEL_MIJIA_AUTHENTICATED)
    {
      lv_obj_add_flag(g_login_qr, LV_OBJ_FLAG_HIDDEN);
      snprintf(text, sizeof(text),
               "已登录 %s\n共 %u 台设备，%u 台在线",
               snapshot->home_name[0] != '\0' ?
               snapshot->home_name : "米家账号",
               snapshot->device_count, snapshot->online_count);
      lv_label_set_text(g_login_message, text);
      lv_obj_set_style_text_color(g_login_message,
                                  lv_color_hex(COLOR_GREEN), 0);
      lv_label_set_text(g_login_action_label, "完成");
      if (g_login_success_timer == NULL)
        {
          g_login_success_timer =
            lv_timer_create(login_success_timeout, 3000, NULL);
        }
    }
  else
    {
      if (g_login_success_timer != NULL)
        {
          lv_timer_delete(g_login_success_timer);
          g_login_success_timer = NULL;
        }

      lv_obj_add_flag(g_login_qr, LV_OBJ_FLAG_HIDDEN);
      lv_label_set_text(g_login_action_label,
                        snapshot->state == HOME_PANEL_MIJIA_STARTING ?
                        "连接中" : "重新生成");
    }
}

static lv_obj_t *make_nav_button(lv_obj_t *parent, const char *symbol,
                                 const char *text, int y,
                                 unsigned int page)
{
  lv_obj_t *button = lv_button_create(parent);
  lv_obj_t *indicator;
  lv_obj_t *icon;
  lv_obj_t *label;

  configure_fast_button(button);
  lv_obj_set_size(button, NAV_WIDTH - 16, 52);
  lv_obj_set_style_radius(button, THEME_RADIUS_CTRL, 0);
  lv_obj_set_style_bg_color(button, lv_color_hex(COLOR_NAV), 0);
  lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(button, 0, 0);
  lv_obj_set_style_shadow_width(button, 0, 0);
  lv_obj_set_flex_flow(button, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(button, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_left(button, 14, 0);
  lv_obj_set_style_pad_column(button, 10, 0);
  lv_obj_add_event_cb(button, nav_clicked, LV_EVENT_CLICKED,
                      (void *)(uintptr_t)page);

  /* Active indicator bar */

  indicator = lv_obj_create(button);
  lv_obj_remove_style_all(indicator);
  lv_obj_set_pos(indicator, 0, 12);
  lv_obj_set_size(indicator, 3, 28);
  lv_obj_set_style_radius(indicator, 2, 0);
  lv_obj_set_style_bg_color(indicator, lv_color_hex(COLOR_GREEN), 0);
  lv_obj_set_style_bg_opa(indicator, LV_OPA_COVER, 0);
  lv_obj_add_flag(indicator, LV_OBJ_FLAG_FLOATING);

  icon = lv_label_create(button);
  lv_label_set_text(icon, symbol);
  lv_obj_set_style_text_font(icon, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(icon, lv_color_hex(COLOR_MUTED), 0);

  label = lv_label_create(button);
  lv_label_set_text(label, text);
  lv_obj_set_style_text_font(label, home_panel_font_get(), 0);
  lv_obj_set_style_text_color(label, lv_color_hex(COLOR_MUTED), 0);

  g_nav_icons[page] = icon;
  g_nav_labels[page] = label;
  g_nav_indicators[page] = indicator;
  return button;
}

static void set_nav_selected(unsigned int page, bool selected)
{
  if (page >= HOME_PAGE_COUNT || g_nav_buttons[page] == NULL)
    {
      return;
    }

  /* Background: active gets subtle highlight */

  lv_obj_set_style_bg_color(
    g_nav_buttons[page],
    lv_color_hex(selected ? THEME_COLOR_NAV_ACTIVE_BG : COLOR_NAV), 0);
  lv_obj_set_style_bg_grad_color(
    g_nav_buttons[page],
    lv_color_hex(selected ? 0x17483f : COLOR_NAV), 0);
  lv_obj_set_style_bg_grad_dir(g_nav_buttons[page],
                               selected ? LV_GRAD_DIR_HOR :
                                          LV_GRAD_DIR_NONE, 0);
  lv_obj_set_style_bg_opa(g_nav_buttons[page], LV_OPA_COVER, 0);

  /* Icon and label: active uses primary text color */

  lv_obj_set_style_text_color(
    g_nav_icons[page],
    lv_color_hex(selected ? COLOR_GREEN : COLOR_MUTED), 0);
  lv_obj_set_style_text_color(
    g_nav_labels[page],
    lv_color_hex(selected ? COLOR_TEXT : COLOR_MUTED), 0);

  /* Indicator bar: green for active */

  if (selected)
    {
      lv_obj_remove_flag(g_nav_indicators[page], LV_OBJ_FLAG_HIDDEN);
    }
  else
    {
      lv_obj_add_flag(g_nav_indicators[page], LV_OBJ_FLAG_HIDDEN);
    }
}

static void nav_clicked(lv_event_t *event)
{
  unsigned int page = (uintptr_t)lv_event_get_user_data(event);

  show_page(page);
}

/* find_device_control, format_device_value, device_state_text
 * moved to ui/home_ui_components.c (aliased via macros above).
 */

/* make_device_card() moved to ui/page_home.c */

/* make_action_button, style_secondary_action, make_info_row
 * moved to ui/home_ui_components.c (aliased via macros above).
 */

static void network_refresh_clicked(lv_event_t *event)
{
  (void)event;

  g_network_refresh_requested = true;
  if (g_status_label != NULL)
    {
      lv_label_set_text(g_status_label, "已请求重新检测网络");
      lv_obj_set_style_text_color(g_status_label,
                                  lv_color_hex(COLOR_BLUE), 0);
    }
}

/* device_card_score(), select_device_cards() moved to ui/page_home.c */

/* create_home_page() moved to ui/page_home.c → home_page_create() */

/* All rooms page functions moved to ui/page_rooms.c → rooms_page_create() */

/* create_scenes_page() moved to ui/page_scenes.c → scenes_page_create() */

static void proactive_feedback_clicked(lv_event_t *event)
{
  enum home_proactive_feedback_e feedback =
    (enum home_proactive_feedback_e)(uintptr_t)lv_event_get_user_data(event);
  struct proactive_viewmodel_s vm;

  proactive_ctrl_handle_feedback(feedback, false, &vm);
  proactive_page_update_widgets();
}

/* create_settings_page() moved to ui/page_settings.c → settings_page_create() */

static uint32_t select_visible_page(unsigned int page)
{
  uint32_t visible_mask = 0;
  unsigned int index;

  for (index = 0; index < HOME_PAGE_COUNT; index++)
    {
      if (g_pages[index] == NULL)
        {
          continue;
        }

      if (index == page)
        {
          lv_obj_remove_flag(g_pages[index], LV_OBJ_FLAG_HIDDEN);
          lv_obj_move_foreground(g_pages[index]);
          visible_mask |= 1u << index;
        }
      else
        {
          lv_obj_add_flag(g_pages[index], LV_OBJ_FLAG_HIDDEN);
        }
    }

  return visible_mask;
}

static void show_page(unsigned int page)
{
  struct home_panel_mijia_snapshot_s snapshot;
  bool creating;
  unsigned int previous_page;
  uint32_t visible_mask;
  uint32_t started = lv_tick_get();

  if (page >= HOME_PAGE_COUNT)
    {
      page = 0;
    }

  if (g_pages[page] != NULL &&
      g_page_model_revisions[page] != g_family_ui_revision)
    {
      delete_page(page);
    }

  previous_page = g_current_page;
  g_current_page = page;
  creating = g_pages[page] == NULL;

  if (previous_page != page)
    {
      set_nav_selected(previous_page, false);
      set_nav_selected(page, true);
    }
  else
    {
      set_nav_selected(page, true);
    }

  visible_mask = select_visible_page(page);

  if (!creating)
    {
      g_content = g_pages[page];
      g_status_label = g_page_status_labels[page];
      g_settings_network_label = g_page_settings_network_labels[page];
      g_settings_probe_label = g_page_settings_probe_labels[page];
      g_settings_account_label = g_page_settings_account_labels[page];
      g_settings_online_label = g_page_settings_online_labels[page];
      g_settings_channel_label = g_page_settings_channel_labels[page];
      g_home_summary_label = g_page_home_summary_labels[page];
      g_home_online_label = g_page_home_online_labels[page];
      if (page == PROACTIVE_PAGE)
        {
          proactive_page_update_widgets();
        }
      home_panel_mijia_get_snapshot(&snapshot);
      apply_mijia_snapshot(&snapshot);
      syslog(LOG_INFO,
             "[HOME][UI] page-switch page=%u model=%u visible=%02lx\n",
             page, (unsigned int)g_family_model.revision,
             (unsigned long)visible_mask);
      if (lv_tick_elaps(started) >= UI_SLOW_LOG_MS)
        {
          syslog(LOG_WARNING,
                 "[HOME][PERF] page-switch page=%u elapsed=%ums\n",
                 page, (unsigned int)lv_tick_elaps(started));
        }
      return;
    }

  g_pages[page] = lv_obj_create(g_page_host);
  lv_obj_remove_style_all(g_pages[page]);
  lv_obj_set_pos(g_pages[page], 0, 0);
  lv_obj_set_size(g_pages[page], PANEL_WIDTH - NAV_WIDTH,
                  PANEL_HEIGHT - TOPBAR_HEIGHT);
  theme_apply_page_atmosphere(g_pages[page], page);
  lv_obj_clear_flag(g_pages[page], LV_OBJ_FLAG_SCROLLABLE);
  theme_create_ambient_mask(g_pages[page], page);
  visible_mask = select_visible_page(page);
  g_content = g_pages[page];
  g_device_binding_counts[page] = 0;
  memset(g_device_bindings[page], 0, sizeof(g_device_bindings[page]));
  memset(g_scene_bindings[page], 0, sizeof(g_scene_bindings[page]));

  if (page == 0)
    {
      g_home_suggestion_title = NULL;
      g_home_suggestion_reason = NULL;
      g_home_suggestion_execute = NULL;
      g_home_suggestion_ignore = NULL;
    }
  g_status_label = NULL;
  g_settings_network_label = NULL;
  g_settings_probe_label = NULL;
  g_settings_account_label = NULL;
  g_settings_online_label = NULL;
  g_settings_channel_label = NULL;
  g_home_summary_label = NULL;
  g_home_online_label = NULL;
  syslog(LOG_INFO, "[HOME][UI] page-render begin page=%u model=%u\n",
         page, (unsigned int)g_family_model.revision);

  if (page == 0)
    {
      home_page_create(rooms_device_toggled, proactive_feedback_clicked);
    }
  else if (page == 1)
    {
      rooms_page_create();
    }
  else if (page == 2)
    {
      scenes_page_create(scene_clicked);
    }
  else if (page == PROACTIVE_PAGE)
    {
      proactive_page_create();
    }
  else
    {
      settings_page_create(network_refresh_clicked);
    }

  g_page_status_labels[page] = g_status_label;
  g_page_settings_network_labels[page] = g_settings_network_label;
  g_page_settings_probe_labels[page] = g_settings_probe_label;
  g_page_settings_account_labels[page] = g_settings_account_label;
  g_page_settings_online_labels[page] = g_settings_online_label;
  g_page_settings_channel_labels[page] = g_settings_channel_label;
  g_page_home_summary_labels[page] = g_home_summary_label;
  g_page_home_online_labels[page] = g_home_online_label;
  g_page_model_revisions[page] = g_family_ui_revision;
  home_panel_mijia_get_snapshot(&snapshot);
  apply_mijia_snapshot(&snapshot);
  syslog(LOG_INFO,
         "[HOME][UI] page-render end page=%u model=%u visible=%02lx\n",
         page, (unsigned int)g_family_model.revision,
         (unsigned long)visible_mask);
  if (lv_tick_elaps(started) >= UI_SLOW_LOG_MS)
    {
      syslog(LOG_WARNING,
             "[HOME][PERF] page-render page=%u elapsed=%ums\n",
             page, (unsigned int)lv_tick_elaps(started));
    }
}

static void delete_page(unsigned int page)
{
  if (page >= HOME_PAGE_COUNT)
    {
      return;
    }

  if (g_pages[page] != NULL)
    {
      lv_obj_delete(g_pages[page]);
      g_pages[page] = NULL;
    }

  g_page_status_labels[page] = NULL;
  g_page_settings_network_labels[page] = NULL;
  g_page_settings_probe_labels[page] = NULL;
  g_page_settings_account_labels[page] = NULL;
  g_page_settings_online_labels[page] = NULL;
  g_page_settings_channel_labels[page] = NULL;
  g_page_home_summary_labels[page] = NULL;
  g_page_home_online_labels[page] = NULL;
  g_page_model_revisions[page] = UINT32_MAX;
  g_device_binding_counts[page] = 0;
  memset(g_device_bindings[page], 0, sizeof(g_device_bindings[page]));
  memset(g_scene_bindings[page], 0, sizeof(g_scene_bindings[page]));

  if (page == 1)
    {
      memset(g_room_buttons, 0, sizeof(g_room_buttons));
      memset(g_room_device_hosts, 0, sizeof(g_room_device_hosts));
      memset(g_room_visible_counts, 0, sizeof(g_room_visible_counts));
      g_room_highlighted = HOME_PANEL_MAX_ROOMS;
      g_room_title_label = NULL;
      g_room_summary_label = NULL;
      g_room_device_host = NULL;
      g_room_detail_host = NULL;
      rooms_reset_detail_bindings();
    }

  if (page == PROACTIVE_PAGE)
    {
      g_proactive_mode_label = NULL;
      g_proactive_history_label = NULL;
      g_proactive_time_label = NULL;
      g_proactive_confidence_label = NULL;
      g_proactive_progress_label = NULL;
      g_proactive_suggestion_title = NULL;
      g_proactive_reason_label = NULL;
      g_proactive_action_label = NULL;
      g_proactive_feedback_label = NULL;
      g_proactive_accept_button = NULL;
      g_proactive_automation_button = NULL;
      g_proactive_ignore_button = NULL;
      g_proactive_less_button = NULL;
      g_proactive_return_button = NULL;
      g_proactive_reset_button = NULL;
      g_proactive_replay_button = NULL;
      g_proactive_automation_list = NULL;
      g_proactive_automation_signature = 0;
      if (g_proactive_confirm_shade != NULL)
        {
          component_close_dialog(g_proactive_confirm_shade);
          g_proactive_confirm_shade = NULL;
          g_proactive_confirm_key = 0;
        }
    }

  if (g_current_page == page)
    {
      g_content = NULL;
      g_status_label = NULL;
      g_settings_network_label = NULL;
      g_settings_probe_label = NULL;
      g_settings_account_label = NULL;
      g_settings_online_label = NULL;
      g_settings_channel_label = NULL;
      g_home_summary_label = NULL;
      g_home_online_label = NULL;
    }
}

static bool device_structure_changed(
  const struct home_panel_device_s *before,
  const struct home_panel_device_s *after)
{
  unsigned int index;

  if (strcmp(before->did, after->did) != 0 ||
      strcmp(before->name, after->name) != 0 ||
      strcmp(before->room, after->room) != 0 ||
      strcmp(before->model, after->model) != 0 ||
      strcmp(before->type, after->type) != 0 ||
      before->has_power != after->has_power ||
      before->power_writable != after->power_writable ||
      before->power_siid != after->power_siid ||
      before->power_piid != after->power_piid ||
      before->has_brightness != after->has_brightness ||
      before->has_temperature != after->has_temperature ||
      before->has_humidity != after->has_humidity ||
      before->has_battery != after->has_battery ||
      before->control_count != after->control_count)
    {
      return true;
    }

  for (index = 0; index < after->control_count; index++)
    {
      const struct home_panel_control_s *left = &before->controls[index];
      const struct home_panel_control_s *right = &after->controls[index];

      if (strcmp(left->name, right->name) != 0 ||
          left->siid != right->siid || left->piid != right->piid ||
          left->type != right->type ||
          left->has_range != right->has_range ||
          left->minimum != right->minimum ||
          left->maximum != right->maximum || left->step != right->step)
        {
          return true;
        }
    }

  return false;
}

static bool family_structure_changed(
  const struct home_panel_family_model_s *before,
  const struct home_panel_family_model_s *after)
{
  unsigned int index;

  if (before->device_count != after->device_count ||
      before->room_count != after->room_count ||
      before->scene_count != after->scene_count)
    {
      return true;
    }

  for (index = 0; index < after->device_count; index++)
    {
      if (device_structure_changed(&before->devices[index],
                                   &after->devices[index]))
        {
          return true;
        }
    }

  for (index = 0; index < after->room_count; index++)
    {
      if (strcmp(before->rooms[index].name, after->rooms[index].name) != 0 ||
          before->rooms[index].device_count !=
          after->rooms[index].device_count)
        {
          return true;
        }
    }

  for (index = 0; index < after->scene_count; index++)
    {
      if (strcmp(before->scenes[index].id, after->scenes[index].id) != 0 ||
          strcmp(before->scenes[index].name,
                 after->scenes[index].name) != 0)
        {
          return true;
        }
    }

  return false;
}

static void update_device_binding(
  struct home_panel_device_binding_s *binding)
{
  const struct home_panel_device_s *device = binding->device;
  bool checked;
  bool current_checked;
  bool disabled;
  bool state_text_changed;
  char value[32];

  if (device == NULL || binding->value_label == NULL ||
      binding->state_label == NULL)
    {
      return;
    }

  checked = device->has_power && device->power;
  format_device_value(device, value, sizeof(value));
  set_label_text_if_changed(binding->value_label, value);
  state_text_changed = set_label_text_if_changed(
    binding->state_label, device_state_text(device));

  if (binding->card != NULL)
    {
      theme_apply_card_state(
        binding->card,
        !device->online ? HOME_PANEL_CARD_OFFLINE :
        checked ? HOME_PANEL_CARD_ACTIVE : HOME_PANEL_CARD_IDLE);
    }

  if (binding->button == NULL)
    {
      if (state_text_changed)
        {
          lv_obj_set_style_text_color(
            binding->state_label,
            lv_color_hex(device->online ? COLOR_SECONDARY : COLOR_MUTED),
            0);
        }
      return;
    }

  current_checked = lv_obj_has_state(binding->button, LV_STATE_CHECKED);
  if (state_text_changed || checked != current_checked)
    {
      lv_obj_set_style_text_color(
        binding->state_label,
        lv_color_hex(checked ? COLOR_GREEN : COLOR_MUTED), 0);
    }

  if (checked != current_checked)
    {
      style_toggle(binding->button, checked);
      if (checked)
        {
          lv_obj_add_state(binding->button, LV_STATE_CHECKED);
        }
      else
        {
          lv_obj_remove_state(binding->button, LV_STATE_CHECKED);
        }
    }

  disabled = lv_obj_has_state(binding->button, LV_STATE_DISABLED);
  if (device->online && disabled)
    {
      lv_obj_remove_state(binding->button, LV_STATE_DISABLED);
    }
  else if (!device->online && !disabled)
    {
      lv_obj_add_state(binding->button, LV_STATE_DISABLED);
    }
}

static void update_control_binding(
  struct home_panel_control_binding_s *binding)
{
  const struct home_panel_control_s *property = binding->property;
  bool disabled;
  char value[24];

  if (binding->device == NULL || property == NULL ||
      binding->control == NULL || binding->value_label == NULL)
    {
      return;
    }

  if (property->type == HOME_PANEL_CONTROL_BOOLEAN)
    {
      bool checked = lv_obj_has_state(binding->control, LV_STATE_CHECKED);

      set_label_text_if_changed(binding->value_label,
                                property->has_value ?
                                  (property->boolean_value ? "已开启" :
                                                             "已关闭") :
                                  "状态未知");
      if (checked != property->boolean_value)
        {
          style_toggle(binding->control, property->boolean_value);
          if (property->boolean_value)
            {
              lv_obj_add_state(binding->control, LV_STATE_CHECKED);
            }
          else
            {
              lv_obj_remove_state(binding->control, LV_STATE_CHECKED);
            }
        }
    }
  else if (property->type == HOME_PANEL_CONTROL_NUMBER)
    {
      if (!lv_obj_has_state(binding->control, LV_STATE_PRESSED))
        {
          lv_slider_set_value(binding->control, property->value,
                              LV_ANIM_OFF);
          format_control_value(property, property->value,
                               value, sizeof(value));
          set_label_text_if_changed(binding->value_label,
                                    property->has_value ? value : "--");
        }
    }
  else
    {
      unsigned int index;
      unsigned int selected = 0;

      for (index = 0; index < property->option_count; index++)
        {
          if (property->options[index].value == property->value)
            {
              selected = index;
              break;
            }
        }
      if (!lv_obj_has_state(binding->control, LV_STATE_PRESSED))
        {
          lv_dropdown_set_selected(binding->control, selected);
          set_label_text_if_changed(
            binding->value_label,
            property->option_count > 0 ?
              property->options[selected].label : "--");
        }
    }

  disabled = lv_obj_has_state(binding->control, LV_STATE_DISABLED);
  if (binding->device->online && disabled)
    {
      lv_obj_remove_state(binding->control, LV_STATE_DISABLED);
    }
  else if (!binding->device->online && !disabled)
    {
      lv_obj_add_state(binding->control, LV_STATE_DISABLED);
    }
}

static void update_model_widgets(void)
{
  const struct home_panel_room_s *environment = NULL;
  const struct home_panel_device_s *detail = NULL;
  unsigned int page;
  unsigned int index;
  char text[96];

  for (page = 0; page < HOME_PAGE_COUNT; page++)
    {
      for (index = 0; index < g_device_binding_counts[page]; index++)
        {
          update_device_binding(&g_device_bindings[page][index]);
        }
    }

  for (index = 0; index < g_control_binding_count; index++)
    {
      update_control_binding(&g_control_bindings[index]);
    }

  if (g_settings_online_label != NULL)
    {
      snprintf(text, sizeof(text), "%u / %u 台在线",
               g_family_model.online_count, g_family_model.device_count);
      set_label_text_if_changed(g_settings_online_label, text);
      lv_obj_set_style_text_color(g_settings_online_label,
                                  lv_color_hex(COLOR_TEXT), 0);
    }

  if (g_settings_channel_label != NULL)
    {
      const char *channel = g_family_model.mqtt_connected ?
                            "MQTT 实时推送" :
                            (g_family_model.event_source[0] != '\0' ?
                             "云端定向回读" : "等待米家同步");

      set_label_text_if_changed(g_settings_channel_label, channel);
      lv_obj_set_style_text_color(
        g_settings_channel_label,
        lv_color_hex(g_family_model.mqtt_connected ? COLOR_TEXT :
                                                       COLOR_SENSOR), 0);
    }

  if (g_room_detail_host != NULL &&
      g_selected_device < g_family_model.device_count)
    {
      detail = &g_family_model.devices[g_selected_device];
      snprintf(text, sizeof(text), "%s · %s",
               device_type_text(detail), detail->online ? "在线" : "离线");
      if (set_label_text_if_changed(g_detail_subtitle_label, text))
        {
          lv_obj_set_style_text_color(
            g_detail_subtitle_label,
            lv_color_hex(detail->online ? COLOR_SECONDARY : COLOR_MUTED), 0);
        }
      if (g_detail_temperature_label != NULL)
        {
          snprintf(text, sizeof(text), "%d°C", detail->temperature);
          set_label_text_if_changed(g_detail_temperature_label, text);
        }
      if (g_detail_humidity_label != NULL)
        {
          snprintf(text, sizeof(text), "%d%%", detail->humidity);
          set_label_text_if_changed(g_detail_humidity_label, text);
        }
      if (g_detail_battery_label != NULL)
        {
          snprintf(text, sizeof(text), "%d%%", detail->battery);
          set_label_text_if_changed(g_detail_battery_label, text);
        }
    }

  for (index = 0; index < g_family_model.room_count; index++)
    {
      if (g_family_model.rooms[index].has_temperature)
        {
          environment = &g_family_model.rooms[index];
          if (strcmp(environment->name, "客厅") == 0)
            {
              break;
            }
        }
    }

  if (g_page_home_summary_labels[0] != NULL)
    {
      if (environment != NULL)
        {
          snprintf(text, sizeof(text), "%s %d°C  ·  湿度 %d%%",
                   environment->name, environment->temperature,
                   environment->humidity);
        }
      else
        {
          snprintf(text, sizeof(text), "环境传感器暂无数据");
        }
      set_label_text_if_changed(g_page_home_summary_labels[0], text);
    }

  if (g_page_home_online_labels[0] != NULL)
    {
      snprintf(text, sizeof(text), "%u/%u 台在线",
               g_family_model.online_count, g_family_model.device_count);
      set_label_text_if_changed(g_page_home_online_labels[0], text);
    }

  if (g_page_status_labels[0] != NULL)
    {
      if (g_family_model.online_count == g_family_model.device_count)
        {
          snprintf(text, sizeof(text), "一切正常");
        }
      else
        {
          snprintf(text, sizeof(text), "%u 台离线",
                   g_family_model.device_count -
                     g_family_model.online_count);
        }
      set_label_text_if_changed(g_page_status_labels[0], text);
      lv_obj_set_style_text_color(
        g_page_status_labels[0],
        lv_color_hex(g_family_model.online_count ==
                     g_family_model.device_count ?
                     COLOR_SECONDARY : COLOR_ORANGE), 0);
    }

  if (g_room_detail_host == NULL && g_room_summary_label != NULL &&
      g_selected_room < g_family_model.room_count)
    {
      const struct home_panel_room_s *room =
        &g_family_model.rooms[g_selected_room];

      if (room->has_temperature && room->has_humidity)
        {
          snprintf(text, sizeof(text), "%u 台设备 · %d°C · 湿度 %d%%",
                   room->device_count, room->temperature, room->humidity);
        }
      else
        {
          snprintf(text, sizeof(text), "%u 台设备", room->device_count);
        }
      set_label_text_if_changed(g_room_summary_label, text);
    }
}

static int refresh_family_model(
  const struct home_panel_mijia_family_snapshot_s *snapshot,
  const struct home_panel_family_model_s *model)
{
  bool changed;
  bool structure_changed;

  if (snapshot == NULL || model == NULL || snapshot->revision == 0 ||
      snapshot->json_size == 0 || model->revision != snapshot->revision)
    {
      return -EAGAIN;
    }

  /* The client only hands out a new model when its revision changed, so a
   * full 118 KiB memcmp here would always be true.  Compare the revision
   * field instead and skip the expensive scan.
   */

  changed = !g_family_model_valid ||
            model->revision != g_family_model.revision;
  if (changed)
    {
      structure_changed = !g_family_model_valid ||
        family_structure_changed(&g_family_model, model);
      if (g_family_model_valid)
        {
          proactive_ctrl_observe_model_changes(&g_family_model, model);
        }
      memcpy(&g_family_model, model, sizeof(g_family_model));
      g_family_model_valid = true;
      if (structure_changed)
        {
          g_family_ui_revision++;
          if (g_family_ui_revision == 0)
            {
              g_family_ui_revision = 1;
            }
        }
      else
        {
          update_model_widgets();
        }
      syslog(LOG_INFO,
             "[HOME][MODEL] revision=%u devices=%u online=%u rooms=%u "
             "scenes=%u event=%s mqtt=%u update=%s\n",
             (unsigned int)model->revision, model->device_count,
             model->online_count, model->room_count, model->scene_count,
             model->event_source, model->mqtt_connected ? 1 : 0,
             structure_changed ? "rebuild" : "in-place");
      return structure_changed ? 0 : 2;
    }

  g_family_model.revision = model->revision;
  syslog(LOG_INFO,
         "[HOME][MODEL] revision=%u display-unchanged\n",
         (unsigned int)model->revision);
  return 1;
}

/****************************************************************************
 * Device control callbacks (MIoT API calls, kept in main.c)
 ****************************************************************************/

static void cb_device_toggled(lv_event_t *event)
{
  lv_obj_t *button = lv_event_get_target(event);
  struct home_ui_device_binding_s *binding =
    lv_event_get_user_data(event);
  bool requested;
  int ret;

  if (binding == NULL || binding->device == NULL ||
      !binding->device->power_writable || !binding->device->online)
    {
      return;
    }

  requested = lv_obj_has_state(button, LV_STATE_CHECKED);
  ret = home_panel_mijia_request_bool_property(binding->device->did,
                                                binding->device->name,
                                                "on",
                                                binding->device->power_siid,
                                                binding->device->power_piid,
                                                requested);

  if (ret == 0)
    {
      /* Optimistic update: keep the user's intended state while the cloud
       * confirm is in flight.  Rolling back to the stale model value makes
       * the switch bounce and fights the user's gesture.
       */

      lv_obj_add_state(button, LV_STATE_DISABLED);
      lv_label_set_text(binding->state_label,
                        requested ? "正在开启" : "正在关闭");
      lv_obj_set_style_text_color(binding->state_label,
                                  lv_color_hex(COLOR_BLUE), 0);
      set_command_status(ret);
    }
  else
    {
      /* Revert to the authoritative model value only on failure. */

      if (binding->device->power)
        {
          lv_obj_add_state(button, LV_STATE_CHECKED);
        }
      else
        {
          lv_obj_remove_state(button, LV_STATE_CHECKED);
        }

      set_command_status(ret);
    }
}

static void cb_detail_boolean_changed(lv_event_t *event)
{
  lv_obj_t *button = lv_event_get_target(event);
  struct home_ui_control_binding_s *binding =
    lv_event_get_user_data(event);
  bool requested;
  int ret;

  if (binding == NULL || binding->device == NULL ||
      binding->property == NULL || !binding->device->online)
    {
      return;
    }

  requested = lv_obj_has_state(button, LV_STATE_CHECKED);
  ret = home_panel_mijia_request_bool_property(
    binding->device->did, binding->device->name,
    binding->property->name, binding->property->siid,
    binding->property->piid, requested);

  if (ret == 0)
    {
      /* Optimistic update; keep the user's intended state while the command
       * is in flight instead of bouncing back to the stale model value.
       */

      lv_obj_add_state(button, LV_STATE_DISABLED);
    }
  else
    {
      /* Only revert to the authoritative model value on failure. */

      if (binding->property->boolean_value)
        {
          lv_obj_add_state(button, LV_STATE_CHECKED);
        }
      else
        {
          lv_obj_remove_state(button, LV_STATE_CHECKED);
        }
    }
  set_command_status(ret);
}

static void cb_detail_number_changed(lv_event_t *event)
{
  struct home_ui_control_binding_s *binding =
    lv_event_get_user_data(event);
  char value[24];

  if (binding == NULL || binding->property == NULL ||
      binding->value_label == NULL)
    {
      return;
    }

  format_control_value(binding->property,
                       lv_slider_get_value(lv_event_get_target(event)),
                       value, sizeof(value));
  set_label_text_if_changed(binding->value_label, value);
}

static void cb_detail_number_released(lv_event_t *event)
{
  struct home_ui_control_binding_s *binding =
    lv_event_get_user_data(event);
  int value;
  int ret;

  if (binding == NULL || binding->device == NULL ||
      binding->property == NULL || !binding->device->online)
    {
      return;
    }

  value = lv_slider_get_value(lv_event_get_target(event));
  if (binding->property->step > 1)
    {
      int offset = value - binding->property->minimum;

      value = binding->property->minimum +
              ((offset + binding->property->step / 2) /
               binding->property->step) * binding->property->step;
      if (value > binding->property->maximum)
        {
          value = binding->property->maximum;
        }
      lv_slider_set_value(lv_event_get_target(event), value, LV_ANIM_OFF);
    }
  ret = home_panel_mijia_request_number_property(
    binding->device->did, binding->device->name,
    binding->property->name, binding->property->siid,
    binding->property->piid, value);
  set_command_status(ret);
}

static void cb_detail_enum_changed(lv_event_t *event)
{
  struct home_ui_control_binding_s *binding =
    lv_event_get_user_data(event);
  uint32_t selected;
  int ret;

  if (binding == NULL || binding->device == NULL ||
      binding->property == NULL || !binding->device->online)
    {
      return;
    }

  selected = lv_dropdown_get_selected(lv_event_get_target(event));
  if (selected >= binding->property->option_count)
    {
      return;
    }
  ret = home_panel_mijia_request_number_property(
    binding->device->did, binding->device->name,
    binding->property->name, binding->property->siid,
    binding->property->piid,
    binding->property->options[selected].value);
  set_command_status(ret);
}

static void cb_detail_action_clicked(lv_event_t *event)
{
  struct home_ui_action_binding_s *binding =
    lv_event_get_user_data(event);
  int ret;

  if (binding == NULL || binding->device == NULL ||
      binding->action == NULL || !binding->device->online)
    {
      return;
    }
  ret = home_panel_mijia_request_action(
    binding->device->did, binding->device->name,
    binding->action->name, binding->action->siid,
    binding->action->aiid);
  set_command_status(ret);
}

static void create_home_screen(void)
{
  lv_obj_t *screen = lv_screen_active();
  lv_obj_t *body;
  lv_obj_t *topbar;
  lv_obj_t *nav;
  lv_obj_t *login;

  /* Initialize shared UI state */

  memset(&g_ui, 0, sizeof(g_ui));
  g_ui.family_model = &g_family_model;
  g_ui.room_highlighted = HOME_PANEL_MAX_ROOMS;
  g_ui.selected_device = HOME_PANEL_MAX_DEVICES;

  /* Set device control callbacks (MIoT API stays in main.c) */

  g_ui.cb_device_toggled = cb_device_toggled;
  g_ui.cb_detail_boolean_changed = cb_detail_boolean_changed;
  g_ui.cb_detail_number_changed = cb_detail_number_changed;
  g_ui.cb_detail_number_released = cb_detail_number_released;
  g_ui.cb_detail_enum_changed = cb_detail_enum_changed;
  g_ui.cb_detail_action_clicked = cb_detail_action_clicked;

  home_panel_theme_init();

  /* Screen: Flex column - topbar on top, body below */

  lv_obj_set_style_bg_color(screen, lv_color_hex(COLOR_BG), 0);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(screen, 0, 0);
  lv_obj_set_style_pad_row(screen, 0, 0);
  lv_obj_set_style_pad_column(screen, 0, 0);
  lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_layout(screen, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(screen, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

  /* Top bar - Flex row: time | date | spacer | network | login */

  topbar = lv_obj_create(screen);
  lv_obj_remove_style_all(topbar);
  lv_obj_set_size(topbar, PANEL_WIDTH, TOPBAR_HEIGHT);
  lv_obj_set_style_bg_color(topbar, lv_color_hex(COLOR_TOPBAR), 0);
  lv_obj_set_style_bg_opa(topbar, LV_OPA_COVER, 0);
  theme_apply_topbar_gradient(topbar);
  lv_obj_set_style_border_width(topbar, 0, 0);
  lv_obj_set_style_border_side(topbar, LV_BORDER_SIDE_BOTTOM, 0);
  lv_obj_set_flex_flow(topbar, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(topbar, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_left(topbar, 20, 0);
  lv_obj_set_style_pad_right(topbar, 20, 0);
  lv_obj_set_flex_grow(topbar, 0);

  /* Time display */

  g_clock_label = lv_label_create(topbar);
  lv_label_set_text(g_clock_label, "--:--");
  lv_obj_set_width(g_clock_label, 78);
  lv_obj_set_height(g_clock_label, 40);
  lv_label_set_long_mode(g_clock_label, LV_LABEL_LONG_CLIP);
  lv_obj_set_style_text_font(g_clock_label, &home_panel_digits_28, 0);
  lv_obj_set_style_text_color(g_clock_label, lv_color_hex(COLOR_TEXT), 0);

  /* Date */

  g_date_label = lv_label_create(topbar);
  lv_label_set_text(g_date_label, "等待网络校时");
  lv_obj_set_width(g_date_label, 190);
  lv_obj_set_height(g_date_label, 40);
  lv_label_set_long_mode(g_date_label, LV_LABEL_LONG_CLIP);
  lv_obj_set_style_text_font(g_date_label, home_panel_font_get(), 0);
  lv_obj_set_style_text_color(g_date_label, lv_color_hex(COLOR_MUTED), 0);
  lv_obj_set_style_pad_left(g_date_label, 12, 0);

  /* Spacer pushes network and login to the right */

  {
    lv_obj_t *spacer = lv_obj_create(topbar);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_set_width(spacer, 1);
    lv_obj_set_height(spacer, 1);
  }

  /* Network status */

  g_network_label = lv_label_create(topbar);
  lv_label_set_text(g_network_label, LV_SYMBOL_WARNING "  有线网络未连接");
  lv_obj_set_style_text_font(g_network_label, home_panel_font_get(), 0);
  lv_obj_set_style_text_color(g_network_label,
                              lv_color_hex(COLOR_WARNING), 0);

  /* Login button */

  login = lv_button_create(topbar);
  g_login_top_button = login;
  configure_fast_button(login);
  lv_obj_set_size(login, 108, 40);
  lv_obj_set_ext_click_area(login, 6);
  lv_obj_set_style_radius(login, THEME_RADIUS_CTRL, 0);
  lv_obj_set_style_shadow_width(login, 0, 0);
  lv_obj_set_style_border_width(login, 1, 0);
  lv_obj_set_style_border_color(login, lv_color_hex(0x7bb4ff), 0);
  lv_obj_set_style_bg_color(login, lv_color_hex(COLOR_BLUE), 0);
  lv_obj_set_style_bg_color(login, lv_color_hex(0x478ee8),
                            LV_STATE_PRESSED);
  lv_obj_set_style_pad_left(login, 0, 0);
  lv_obj_set_style_pad_right(login, 0, 0);
  lv_obj_add_event_cb(login, show_login, LV_EVENT_CLICKED, NULL);
  g_login_top_label = lv_label_create(login);
  lv_label_set_text(g_login_top_label, "登录米家");
  set_chinese_font(g_login_top_label);
  lv_obj_center(g_login_top_label);

  /* Body: Flex row - nav | content */

  body = lv_obj_create(screen);
  lv_obj_remove_style_all(body);
  lv_obj_set_size(body, PANEL_WIDTH, PANEL_HEIGHT - TOPBAR_HEIGHT);
  lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_set_style_border_width(body, 0, 0);
  lv_obj_set_flex_grow(body, 1);

  /* Left navigation - Flex column */

  nav = lv_obj_create(body);
  lv_obj_remove_style_all(nav);
  lv_obj_set_size(nav, NAV_WIDTH, PANEL_HEIGHT - TOPBAR_HEIGHT);
  lv_obj_set_style_bg_color(nav, lv_color_hex(COLOR_NAV), 0);
  lv_obj_set_style_bg_opa(nav, LV_OPA_COVER, 0);
  theme_apply_nav_gradient(nav);
  lv_obj_set_style_border_width(nav, 0, 0);
  lv_obj_set_flex_flow(nav, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_top(nav, 12, 0);
  lv_obj_set_style_pad_bottom(nav, 12, 0);
  lv_obj_set_style_pad_row(nav, 4, 0);
  lv_obj_set_flex_grow(nav, 0);

  g_nav_buttons[0] = make_nav_button(nav, LV_SYMBOL_HOME, "家庭", 0, 0);
  g_nav_buttons[1] = make_nav_button(nav, LV_SYMBOL_LIST, "房间", 0, 1);
  g_nav_buttons[2] = make_nav_button(nav, LV_SYMBOL_PLAY, "场景", 0, 2);
  g_nav_buttons[3] = make_nav_button(nav, LV_SYMBOL_REFRESH, "智能", 0, 3);

  /* Spacer pushes settings to bottom */

  {
    lv_obj_t *spacer = lv_obj_create(nav);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_set_width(spacer, 1);
    lv_obj_set_height(spacer, 1);
  }

  g_nav_buttons[4] = make_nav_button(nav, LV_SYMBOL_SETTINGS, "设置", 0, 4);

  /* Content host - fills remaining space */

  g_page_host = lv_obj_create(body);
  lv_obj_remove_style_all(g_page_host);
  lv_obj_set_size(g_page_host, 1, PANEL_HEIGHT - TOPBAR_HEIGHT);
  lv_obj_set_style_bg_color(g_page_host, lv_color_hex(COLOR_BG), 0);
  lv_obj_set_style_bg_opa(g_page_host, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(g_page_host, 0, 0);
  lv_obj_set_flex_grow(g_page_host, 1);
  lv_obj_clear_flag(g_page_host, LV_OBJ_FLAG_SCROLLABLE);

  show_page(0);
  update_time_ui(true);
}

static void log_fpu_state(void)
{
#ifdef CONFIG_ARCH_FPU
  uint32_t mstatus;
  uint32_t mexstatus;
  uint32_t fcsr;

  __asm__ __volatile__("csrr %0, mstatus" : "=r"(mstatus));
  __asm__ __volatile__("csrr %0, 0x7e1" : "=r"(mexstatus));
  __asm__ __volatile__("csrr %0, fcsr" : "=r"(fcsr));
  syslog(LOG_INFO,
         "[HOME][FPU] mstatus=%08lx fs=%lu fcsr=%08lx mexstatus=%08lx "
         "spush=%lu spswap=%lu\n",
         (unsigned long)mstatus, (unsigned long)((mstatus >> 13) & 3u),
         (unsigned long)fcsr, (unsigned long)mexstatus,
         (unsigned long)((mexstatus >> 16) & 1u),
         (unsigned long)((mexstatus >> 17) & 1u));
#endif
}

int main(int argc, char *argv[])
{
  lv_nuttx_dsc_t info;
  lv_nuttx_result_t result;
  enum network_state_e displayed_state = (enum network_state_e)-1;
  struct home_panel_mijia_snapshot_s mijia_snapshot;
  struct home_panel_mijia_family_snapshot_s family_snapshot;
  struct home_panel_mijia_command_snapshot_s command_snapshot;
  struct home_panel_agent_snapshot_s agent_snapshot;
  uint32_t displayed_mijia_revision = UINT32_MAX;
  uint32_t displayed_family_revision = 0;
  uint32_t displayed_command_revision = 0;
  uint32_t displayed_cloud_proposal_revision = 0;
  uint32_t last_slow_refresh_log = 0;
  uint32_t last_mijia_poll = 0;
  uint32_t last_proactive_tick = 0;
  uint32_t last_proactive_ui_poll = 0;
  uint32_t last_proactive_cloud_poll = 0;
  uint32_t last_proactive_context = 0;
  struct sched_param ui_param;
  int ui_policy;
  int ret;

  (void)argc;
  (void)argv;
  memset(&mijia_snapshot, 0, sizeof(mijia_snapshot));

  if (lv_is_initialized())
    {
      fprintf(stderr, "home_panel: LVGL is already initialized\n");
      return 1;
    }

  if (pthread_getschedparam(pthread_self(), &ui_policy, &ui_param) == 0)
    {
      ui_param.sched_priority = UI_THREAD_PRIORITY;
      ret = pthread_setschedparam(pthread_self(), ui_policy, &ui_param);
      if (ret != 0)
        {
          syslog(LOG_WARNING,
                 "[HOME][PERF] UI priority unchanged ret=%d\n", ret);
        }
    }

  log_fpu_state();
  lv_init();
  ret = home_panel_font_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR,
             "[HOME][FONT] using built-in subset fallback ret=%d\n", ret);
    }
  proactive_ctrl_initialize();
  proactive_ctrl_load_profile();
  proactive_ctrl_set_family_model(&g_family_model);
  proactive_ctrl_update_context();

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

  /* Poll touch input faster than the display refresh timer so short taps and
   * scroll gestures do not wait for the default 10 ms input sample. */
  lv_timer_set_period(lv_indev_get_read_timer(result.indev), 5);
  lv_indev_set_display(result.indev, result.disp);

  create_home_screen();
  ret = start_network_monitor();
  if (ret != 0)
    {
      fprintf(stderr, "home_panel: network monitor failed: %d\n", ret);
    }

  ret = home_panel_mijia_initialize();
  if (ret != 0)
    {
      fprintf(stderr, "home_panel: Mijia client failed: %d\n", ret);
    }

  lv_obj_invalidate(lv_screen_active());
  lv_refr_now(result.disp);
  g_full_refresh_pending = false;

  for (;;)
    {
      uint32_t delay;

      /* Service input and pending invalidation before background polling.  A
       * cloud or proactive request may take longer than one UI frame; doing
       * this at the end of the loop makes a tap wait behind that work. */
      {
        uint32_t refresh_started = lv_tick_get();
        uint32_t refresh_elapsed;

        delay = lv_timer_handler();
        refresh_elapsed = lv_tick_elaps(refresh_started);
        if (refresh_elapsed >= UI_SLOW_LOG_MS &&
            (last_slow_refresh_log == 0 ||
             lv_tick_elaps(last_slow_refresh_log) >= 1000))
          {
            last_slow_refresh_log = lv_tick_get();
            syslog(LOG_WARNING,
                   "[HOME][PERF] lv-refresh page=%u elapsed=%ums\n",
                   g_current_page, (unsigned int)refresh_elapsed);
          }
      }

      if (displayed_state != g_network_state)
        {
          displayed_state = g_network_state;
          apply_network_state(displayed_state);
          proactive_ctrl_set_network_online(
            displayed_state == NETWORK_ONLINE);
          syslog(LOG_INFO, "[HOME][UI] network=%s\n",
                 network_state_name(displayed_state));
        }

      update_time_ui(false);
      if (last_proactive_tick == 0 ||
          lv_tick_elaps(last_proactive_tick) >= PROACTIVE_TICK_MS)
        {
          last_proactive_tick = lv_tick_get();
          home_proactive_tick(last_proactive_tick);
          proactive_ctrl_process_automation();
          proactive_ctrl_process_tick();
        }
      if (proactive_ctrl_persist_retry_ready())
        {
          proactive_ctrl_schedule_persist();
        }
      if (last_proactive_context == 0 ||
          lv_tick_elaps(last_proactive_context) >= 1000)
        {
          last_proactive_context = lv_tick_get();
          proactive_ctrl_update_context();
        }
      if ((g_proactive_mode_label != NULL ||
           g_home_suggestion_title != NULL) &&
          (last_proactive_ui_poll == 0 ||
           lv_tick_elaps(last_proactive_ui_poll) >= PROACTIVE_UI_POLL_MS))
        {
          struct home_proactive_snapshot_s proactive_snapshot;

          last_proactive_ui_poll = lv_tick_get();
          home_proactive_get_snapshot(&proactive_snapshot);
          home_panel_mijia_get_agent_snapshot(&agent_snapshot);
          {
            struct home_panel_cloud_proposal_s cloud_proposal;

            home_panel_mijia_get_board_proposal(&cloud_proposal);
            if (cloud_proposal.update_revision !=
                displayed_cloud_proposal_revision)
              {
                proactive_page_update_widgets();
              }
          }
          if (g_displayed_proactive_revision !=
                proactive_snapshot.revision ||
              g_displayed_agent_revision != agent_snapshot.revision)
            {
              proactive_page_update_widgets();
            }
        }

      if (last_mijia_poll == 0 ||
          lv_tick_elaps(last_mijia_poll) >= MIJIA_UI_POLL_MS)
        {
          last_mijia_poll = lv_tick_get();
          home_panel_mijia_get_snapshot(&mijia_snapshot);
          if (displayed_mijia_revision != mijia_snapshot.revision)
            {
              displayed_mijia_revision = mijia_snapshot.revision;
              apply_mijia_snapshot(&mijia_snapshot);
              syslog(LOG_INFO, "[HOME][UI] mijia=%s\n",
                     home_panel_mijia_state_name(mijia_snapshot.state));
            }

          if (!g_login_autostart_attempted &&
              g_network_state == NETWORK_ONLINE &&
              mijia_snapshot.state == HOME_PANEL_MIJIA_IDLE)
            {
              g_login_autostart_attempted = true;
              syslog(LOG_INFO,
                     "[HOME][UI] no persisted Mijia session; opening login\n");
              show_login(NULL);
            }

          ret = home_panel_mijia_get_family_update(
            displayed_family_revision, &family_snapshot,
            &g_family_update_model);
          if (ret == 0)
            {
              ret = refresh_family_model(&family_snapshot,
                                         &g_family_update_model);
              if (ret >= 0)
                {
                  displayed_family_revision = family_snapshot.revision;
                  if (ret == 0)
                    {
                      g_model_refresh_pending = true;
                    }
                }
            }

          home_panel_mijia_get_command_snapshot(&command_snapshot);
          if (displayed_command_revision != command_snapshot.revision)
            {
              displayed_command_revision = command_snapshot.revision;
              if (g_status_label != NULL && command_snapshot.revision != 0)
                {
                  lv_label_set_text(g_status_label,
                                    command_snapshot.message);
                  lv_obj_set_style_text_color(
                    g_status_label,
                    lv_color_hex(command_snapshot.state ==
                                 HOME_PANEL_MIJIA_COMMAND_CONFIRMED ?
                                 COLOR_GREEN :
                                 command_snapshot.state ==
                                 HOME_PANEL_MIJIA_COMMAND_ERROR ?
                                 COLOR_ORANGE : COLOR_BLUE), 0);
                }
              syslog(LOG_INFO, "[HOME][UI] command=%u code=%d\n",
                     (unsigned int)command_snapshot.state,
                     command_snapshot.code);
            }
        }

      if (last_proactive_cloud_poll == 0 ||
          lv_tick_elaps(last_proactive_cloud_poll) >=
            PROACTIVE_CLOUD_POLL_MS)
        {
          last_proactive_cloud_poll = lv_tick_get();
          proactive_ctrl_request_cloud_learning(mijia_snapshot.state);
          proactive_ctrl_request_cloud_analysis(mijia_snapshot.state);
          proactive_ctrl_request_board_sync(mijia_snapshot.state);
        }

      if (g_model_refresh_pending &&
          lv_indev_get_state(result.indev) == LV_INDEV_STATE_RELEASED)
        {
          g_model_refresh_pending = false;
          show_page(g_current_page);
        }

      if (delay == LV_NO_TIMER_READY || delay > UI_LOOP_MAX_DELAY_MS)
        {
          delay = UI_LOOP_MAX_DELAY_MS;
        }

      usleep((delay > 0 ? delay : 1) * 1000);
    }

  return 0;
}
