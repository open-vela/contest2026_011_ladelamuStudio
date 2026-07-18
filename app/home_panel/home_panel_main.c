/****************************************************************************
 * D13x Hengshan-Pi 1024x600 home control panel
 ****************************************************************************/

#include <nuttx/config.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <nuttx/net/dns.h>
#include <nuttx/net/icmp.h>
#include <pthread.h>
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

LV_FONT_DECLARE(home_panel_digits_28);

#define PANEL_WIDTH       1024
#define PANEL_HEIGHT      600
#define TOPBAR_HEIGHT     72
#define NAV_WIDTH         154
#define ROOM_NAV_WIDTH    190
#define ROOM_CARD_WIDTH   292
#define ROOM_CARD_HEIGHT  216
#define ROOM_CARD_GAP     14
#define HOME_PAGE_COUNT   4
#define HOME_CARD_COUNT   3

#define COLOR_BG          0x0f1114
#define COLOR_SURFACE     0x191d22
#define COLOR_SURFACE_2   0x22272e
#define COLOR_TEXT        0xf4f6f8
#define COLOR_MUTED       0x929aa5
#define COLOR_ORANGE      0xff9f2f
#define COLOR_BLUE        0x4b8df8
#define COLOR_GREEN       0x47c486

#define NETWORK_INTERFACE       "eth0"
#define NETWORK_PROBE_INTERVAL  60
#define NETWORK_THREAD_STACK    8192
#define NETWORK_PING_POLL_US    20000
#define NETWORK_PING_POLLS      75
#define NETWORK_PING_DATA_SIZE  16
#define NETWORK_DNS_BUFFER_SIZE 512
#define UI_LOOP_MAX_DELAY_MS    5
#define TIME_VALID_EPOCH        1735689600
#define TIME_UPDATE_INTERVAL_MS 1000
#define NTP_RETRY_INTERVAL_MS   30000

enum network_state_e
{
  NETWORK_INITIALIZING = 0,
  NETWORK_DISCONNECTED,
  NETWORK_CHECKING,
  NETWORK_NO_INTERNET,
  NETWORK_ONLINE
};

static lv_obj_t *g_status_label;
static lv_obj_t *g_content;
static lv_obj_t *g_page_host;
static lv_obj_t *g_pages[HOME_PAGE_COUNT];
static lv_obj_t *g_page_status_labels[HOME_PAGE_COUNT];
static lv_obj_t *g_page_settings_network_labels[HOME_PAGE_COUNT];
static lv_obj_t *g_page_settings_probe_labels[HOME_PAGE_COUNT];
static lv_obj_t *g_page_settings_account_labels[HOME_PAGE_COUNT];
static lv_obj_t *g_page_home_summary_labels[HOME_PAGE_COUNT];
static lv_obj_t *g_nav_buttons[HOME_PAGE_COUNT];
static uint32_t g_page_model_revisions[HOME_PAGE_COUNT];
static lv_obj_t *g_network_label;
static lv_obj_t *g_settings_network_label;
static lv_obj_t *g_settings_probe_label;
static lv_obj_t *g_settings_account_label;
static lv_obj_t *g_login_top_label;
static lv_obj_t *g_login_shade;
static lv_obj_t *g_login_qr;
static lv_obj_t *g_login_message;
static lv_obj_t *g_login_action_label;
static lv_obj_t *g_home_summary_label;
static lv_obj_t *g_clock_label;
static lv_obj_t *g_date_label;
static lv_image_dsc_t g_login_qr_image;
static uint8_t *g_login_qr_data;
static uint32_t g_login_qr_revision;
static struct home_panel_family_model_s g_family_model;
static bool g_family_model_valid;
static bool g_model_refresh_pending;
static uint32_t g_family_ui_revision;
static unsigned int g_current_page;
static unsigned int g_selected_room;
static lv_obj_t *g_room_buttons[HOME_PANEL_MAX_ROOMS];
static lv_obj_t *g_room_title_label;
static lv_obj_t *g_room_summary_label;
static lv_obj_t *g_room_device_host;

struct home_panel_device_binding_s
{
  const struct home_panel_device_s *device;
  lv_obj_t *button;
  lv_obj_t *value_label;
  lv_obj_t *state_label;
};

struct home_panel_scene_binding_s
{
  const struct home_panel_scene_s *scene;
};

static struct home_panel_device_binding_s
  g_device_bindings[HOME_PAGE_COUNT][HOME_PANEL_MAX_DEVICES +
                                     HOME_CARD_COUNT];
static unsigned int g_device_binding_counts[HOME_PAGE_COUNT];
static struct home_panel_scene_binding_s
  g_scene_bindings[HOME_PAGE_COUNT][HOME_PANEL_MAX_SCENES];
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
          lv_label_set_text(g_clock_label, "--:--");
        }
      if (g_date_label != NULL)
        {
          lv_label_set_text(g_date_label, "等待网络校时");
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
      lv_label_set_text(g_clock_label, clock_text);
    }
  if (g_date_label != NULL)
    {
      lv_label_set_text(g_date_label, date_text);
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
  bool was_connected = false;
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
        }
      else if (!connected)
        {
          network_set_state(NETWORK_DISCONNECTED);
          was_connected = false;
          elapsed = NETWORK_PROBE_INTERVAL;
        }
      else if (!network_get_ipv4_address(&address))
        {
          /* DHCP runs independently. Keep checking without reporting a
           * false Internet failure while an address is being acquired.
           */

          network_set_state(NETWORK_CHECKING);
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
  pthread_t thread;
  int ret;

  ret = pthread_attr_init(&attr);
  if (ret != 0)
    {
      return ret;
    }

  pthread_attr_setstacksize(&attr, NETWORK_THREAD_STACK);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  ret = pthread_create(&thread, &attr, network_worker, NULL);
  pthread_attr_destroy(&attr);
  return ret;
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
      color = lv_color_hex(COLOR_GREEN);
    }
  else if (state == NETWORK_NO_INTERNET)
    {
      top_text = LV_SYMBOL_WARNING "  无互联网连接";
      settings_text = "无互联网连接";
      probe_text = "mi.com / xiaomi.cn 不可达";
      color = lv_color_hex(COLOR_ORANGE);
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
      color = lv_color_hex(COLOR_ORANGE);
    }

  lv_label_set_text(g_network_label, top_text);
  lv_obj_set_style_text_color(g_network_label, color, 0);

  if (g_settings_network_label != NULL)
    {
      lv_label_set_text(g_settings_network_label, settings_text);
      lv_obj_set_style_text_color(g_settings_network_label, color, 0);
    }

  if (g_settings_probe_label != NULL)
    {
      lv_label_set_text(g_settings_probe_label, probe_text);
      lv_obj_set_style_text_color(g_settings_probe_label, color, 0);
    }
}

static void set_chinese_font(lv_obj_t *obj)
{
  lv_obj_set_style_text_font(obj, home_panel_font_get(), 0);
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
  struct home_panel_scene_binding_s *binding =
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
  lv_label_set_text(g_status_label, message);
  lv_obj_set_style_text_color(g_status_label,
                              lv_color_hex(ret == 0 ? COLOR_BLUE :
                                                        COLOR_ORANGE), 0);
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
  struct home_panel_device_binding_s *binding =
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

  if (binding->device->power)
    {
      lv_obj_add_state(button, LV_STATE_CHECKED);
    }
  else
    {
      lv_obj_remove_state(button, LV_STATE_CHECKED);
    }

  if (ret == 0)
    {
      lv_label_set_text(binding->state_label,
                        requested ? "正在开启" : "正在关闭");
      lv_obj_set_style_text_color(binding->state_label,
                                  lv_color_hex(COLOR_BLUE), 0);
      lv_obj_add_state(button, LV_STATE_DISABLED);
      lv_label_set_text(g_status_label, "指令已发送，等待设备确认");
      lv_obj_set_style_text_color(g_status_label,
                                  lv_color_hex(COLOR_BLUE), 0);
    }
  else
    {
      lv_label_set_text(g_status_label,
                        ret == -EBUSY ? "请等待上一个设备操作完成" :
                                        "设备操作发送失败");
      lv_obj_set_style_text_color(g_status_label,
                                  lv_color_hex(COLOR_ORANGE), 0);
    }
}

static void login_close(lv_event_t *event)
{
  lv_obj_t *shade = lv_event_get_user_data(event);

  g_login_shade = NULL;
  g_login_qr = NULL;
  g_login_message = NULL;
  g_login_action_label = NULL;
  lv_obj_delete_async(shade);
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
  lv_obj_t *screen = lv_screen_active();
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

  shade = lv_obj_create(screen);
  g_login_shade = shade;
  lv_obj_remove_style_all(shade);
  lv_obj_set_size(shade, PANEL_WIDTH, PANEL_HEIGHT);
  lv_obj_set_pos(shade, 0, 0);
  lv_obj_set_style_bg_color(shade, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(shade, LV_OPA_70, 0);

  dialog = lv_obj_create(shade);
  lv_obj_set_size(dialog, 680, 410);
  lv_obj_center(dialog);
  lv_obj_set_style_radius(dialog, 8, 0);
  lv_obj_set_style_border_width(dialog, 1, 0);
  lv_obj_set_style_border_color(dialog, lv_color_hex(0x343b44), 0);
  lv_obj_set_style_bg_color(dialog, lv_color_hex(COLOR_SURFACE), 0);
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
  lv_obj_set_size(close, 44, 44);
  lv_obj_align(close, LV_ALIGN_TOP_RIGHT, 0, 0);
  lv_obj_set_style_radius(close, 8, 0);
  lv_obj_set_style_bg_color(close, lv_color_hex(COLOR_SURFACE_2), 0);
  lv_obj_set_style_bg_color(close, lv_color_hex(0x343b44),
                            LV_STATE_PRESSED);
  lv_obj_add_event_cb(close, login_close, LV_EVENT_CLICKED, shade);
  label = lv_label_create(close);
  lv_label_set_text(label, LV_SYMBOL_CLOSE);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
  lv_obj_center(label);

  action = lv_button_create(dialog);
  lv_obj_set_size(action, 144, 50);
  lv_obj_align(action, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
  lv_obj_set_style_radius(action, 8, 0);
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
      lv_label_set_text(g_login_top_label,
                        snapshot->state == HOME_PANEL_MIJIA_AUTHENTICATED ?
                        "米家已登录" : "登录米家");
    }

  if (g_settings_account_label != NULL)
    {
      if (snapshot->state == HOME_PANEL_MIJIA_AUTHENTICATED)
        {
          snprintf(text, sizeof(text), "%s · %u/%u 台在线",
                   snapshot->home_name[0] != '\0' ?
                   snapshot->home_name : "米家账号",
                   snapshot->online_count, snapshot->device_count);
          lv_label_set_text(g_settings_account_label, text);
          lv_obj_set_style_text_color(g_settings_account_label,
                                      lv_color_hex(COLOR_GREEN), 0);
        }
      else
        {
          lv_label_set_text(g_settings_account_label, "未登录");
          lv_obj_set_style_text_color(g_settings_account_label,
                                      lv_color_hex(COLOR_MUTED), 0);
        }
    }

  if (g_home_summary_label != NULL && !g_family_model_valid)
    {
      if (snapshot->state == HOME_PANEL_MIJIA_IDLE)
        {
          lv_label_set_text(g_home_summary_label,
                            "登录米家后同步家庭设备");
        }
      else if (snapshot->state == HOME_PANEL_MIJIA_AUTHENTICATED)
        {
          lv_label_set_text(g_home_summary_label, "正在同步米家设备");
        }
      else
        {
          lv_label_set_text(g_home_summary_label, snapshot->message);
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
    }
  else
    {
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
                     home_panel_font_get());
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
                                   const struct home_panel_scene_s *scene,
                                   unsigned int binding_index, int x,
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
  g_scene_bindings[g_current_page][binding_index].scene = scene;
  lv_obj_add_event_cb(button, scene_clicked, LV_EVENT_CLICKED,
                      &g_scene_bindings[g_current_page][binding_index]);

  icon = make_label(button, symbol, 14, 22, accent, &lv_font_montserrat_16);
  label = make_label(button, scene->name, 54, 20,
                     lv_color_hex(COLOR_TEXT),
                     home_panel_font_get());
  (void)icon;
  return label;
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
  else if (device->has_battery)
    {
      snprintf(value, capacity, "%d%%", device->battery);
    }
  else
    {
      snprintf(value, capacity, "%s", device->online ? "在线" : "离线");
    }
}

static const char *device_state_text(
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

static void make_device_card(lv_obj_t *parent, int x, int y,
                             int width, int height, const char *symbol,
                             const struct home_panel_device_s *device,
                             lv_color_t accent)
{
  lv_obj_t *card = lv_obj_create(parent);
  lv_obj_t *toggle;
  lv_obj_t *state;
  lv_obj_t *label;
  lv_obj_t *value_label;
  struct home_panel_device_binding_s *binding;
  unsigned int binding_index;
  const lv_font_t *value_font;
  char value[32];
  const char *state_text;
  bool checked = device->has_power && device->power;

  format_device_value(device, value, sizeof(value));
  value_font = device->has_brightness || device->has_temperature ||
               device->has_battery ? &home_panel_digits_28 :
                                     home_panel_font_get();

  lv_obj_set_pos(card, x, y);
  lv_obj_set_size(card, width, height);
  lv_obj_set_style_radius(card, 8, 0);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_border_color(card, lv_color_hex(0x2c323a), 0);
  lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_SURFACE), 0);
  lv_obj_set_style_pad_all(card, 18, 0);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

  make_label(card, symbol, 0, 0, accent, &lv_font_montserrat_16);
  make_label(card, device->room[0] != '\0' ? device->room : "未分房间",
             0, 58, lv_color_hex(COLOR_MUTED),
             home_panel_font_get());
  label = make_label(card, device->name, 0, 88,
                     lv_color_hex(COLOR_TEXT), home_panel_font_get());
  lv_obj_set_width(label, width - 36);
  lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
  value_label = make_label(card, value, 0, 122, accent, value_font);

  state_text = device_state_text(device);
  state = make_label(card, state_text, 0, 169,
                     lv_color_hex(checked ? COLOR_GREEN : COLOR_MUTED),
                     home_panel_font_get());

  if (!device->power_writable)
    {
      return;
    }

  binding_index = g_device_binding_counts[g_current_page];
  if (binding_index >= HOME_PANEL_MAX_DEVICES + HOME_CARD_COUNT)
    {
      return;
    }
  binding = &g_device_bindings[g_current_page][binding_index];
  g_device_binding_counts[g_current_page]++;

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
  if (!device->online)
    {
      lv_obj_add_state(toggle, LV_STATE_DISABLED);
    }

  label = lv_label_create(toggle);
  lv_label_set_text(label, LV_SYMBOL_POWER);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
  lv_obj_center(label);
  binding->device = device;
  binding->button = toggle;
  binding->value_label = value_label;
  binding->state_label = state;
  lv_obj_add_event_cb(toggle, device_toggled, LV_EVENT_VALUE_CHANGED,
                      binding);
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

static lv_obj_t *make_info_row(lv_obj_t *parent, const char *name,
                               const char *value, int y, lv_color_t color)
{
  make_label(parent, name, 32, y, lv_color_hex(COLOR_MUTED),
             home_panel_font_get());
  return make_label(parent, value, 238, y, color,
                    home_panel_font_get());
}

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

static unsigned int select_device_cards(unsigned int selected[3])
{
  bool used[HOME_PANEL_MAX_DEVICES] = {false};
  unsigned int count = 0;

  while (count < 3)
    {
      int best_score = -1;
      unsigned int best = 0;
      unsigned int index;

      for (index = 0; index < g_family_model.device_count; index++)
        {
          int score;

          if (used[index])
            {
              continue;
            }
          score = device_card_score(&g_family_model.devices[index]);
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

static void create_home_page(void)
{
  const struct home_panel_room_s *environment = NULL;
  unsigned int selected[3];
  unsigned int selected_count;
  unsigned int index;
  char subtitle[96];
  char status[64];

  make_label(g_content, "晚上好，欢迎回家", 30, 22,
             lv_color_hex(COLOR_TEXT), home_panel_font_get());

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
  if (environment != NULL)
    {
      snprintf(subtitle, sizeof(subtitle), "%s %d°C  ·  湿度 %d%%",
               environment->name, environment->temperature,
               environment->humidity);
    }
  else
    {
      snprintf(subtitle, sizeof(subtitle), "%s",
               g_family_model_valid ? "环境传感器暂无数据" :
                                      "正在同步米家设备");
    }
  g_home_summary_label = make_label(g_content, subtitle, 30, 54,
                                    lv_color_hex(COLOR_MUTED),
                                    home_panel_font_get());

  for (index = 0; index < g_family_model.scene_count && index < 3; index++)
    {
      static const int positions[3] = {30, 226, 422};
      static const uint32_t colors[3] =
      {
        COLOR_GREEN, COLOR_BLUE, COLOR_ORANGE
      };
      make_scene_button(g_content, LV_SYMBOL_PLAY,
                        &g_family_model.scenes[index], index,
                        positions[index], lv_color_hex(colors[index]));
    }

  make_label(g_content, "常用设备", 30, 198, lv_color_hex(COLOR_TEXT),
             home_panel_font_get());
  snprintf(status, sizeof(status), "%u/%u 台在线",
           g_family_model.online_count, g_family_model.device_count);
  g_status_label = make_label(g_content, status, 666, 198,
                              lv_color_hex(COLOR_MUTED),
                              home_panel_font_get());

  selected_count = select_device_cards(selected);
  for (index = 0; index < selected_count; index++)
    {
      static const int positions[3] = {30, 288, 546};
      static const uint32_t colors[3] =
      {
        COLOR_ORANGE, COLOR_BLUE, COLOR_GREEN
      };
      const struct home_panel_device_s *device =
        &g_family_model.devices[selected[index]];
      make_device_card(g_content, positions[index], 238, 240, 218,
                       device_symbol(device), device,
                       lv_color_hex(colors[index]));
    }
}

static bool device_in_room(const struct home_panel_device_s *device,
                           const struct home_panel_room_s *room)
{
  return device->room[0] != '\0' && room->name[0] != '\0' &&
         strcmp(device->room, room->name) == 0;
}

static void update_room_button_styles(void)
{
  unsigned int index;

  for (index = 0; index < g_family_model.room_count; index++)
    {
      if (g_room_buttons[index] == NULL)
        {
          continue;
        }

      lv_obj_set_style_bg_color(
        g_room_buttons[index],
        lv_color_hex(index == g_selected_room ? COLOR_SURFACE_2 :
                                                0x13161a), 0);
      lv_obj_set_style_text_color(
        g_room_buttons[index],
        lv_color_hex(index == g_selected_room ? COLOR_TEXT : COLOR_MUTED),
        0);
    }
}

static void render_room_devices(unsigned int room_index)
{
  static const uint32_t colors[] =
  {
    COLOR_ORANGE, COLOR_BLUE, COLOR_GREEN
  };
  const struct home_panel_room_s *room;
  unsigned int device_index;
  unsigned int visible = 0;
  char summary[96];

  if (g_room_device_host == NULL || g_family_model.room_count == 0)
    {
      return;
    }

  if (room_index >= g_family_model.room_count)
    {
      room_index = 0;
    }

  g_selected_room = room_index;
  room = &g_family_model.rooms[room_index];
  lv_label_set_text(g_room_title_label, room->name);
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
  lv_label_set_text(g_room_summary_label, summary);

  lv_obj_clean(g_room_device_host);
  memset(g_device_bindings[1], 0, sizeof(g_device_bindings[1]));
  g_device_binding_counts[1] = 0;
  for (device_index = 0;
       device_index < g_family_model.device_count;
       device_index++)
    {
      const struct home_panel_device_s *device =
        &g_family_model.devices[device_index];
      unsigned int column;
      unsigned int row;

      if (!device_in_room(device, room))
        {
          continue;
        }

      column = visible % 2;
      row = visible / 2;
      make_device_card(g_room_device_host,
                       (int)column * (ROOM_CARD_WIDTH + ROOM_CARD_GAP),
                       (int)row * (ROOM_CARD_HEIGHT + ROOM_CARD_GAP),
                       ROOM_CARD_WIDTH, ROOM_CARD_HEIGHT,
                       device_symbol(device), device,
                       lv_color_hex(colors[visible % 3]));
      visible++;
    }

  if (visible == 0)
    {
      lv_obj_t *empty = make_label(g_room_device_host,
                                   "该房间暂无设备", 0, 24,
                                   lv_color_hex(COLOR_MUTED),
                                   home_panel_font_get());
      lv_obj_set_width(empty, 580);
      lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
    }

  lv_obj_scroll_to_y(g_room_device_host, 0, LV_ANIM_OFF);
  update_room_button_styles();
  syslog(LOG_INFO, "[HOME][UI] room=%s devices=%u\n",
         room->name, visible);
}

static void room_clicked(lv_event_t *event)
{
  uintptr_t room = (uintptr_t)lv_event_get_user_data(event);

  if (room == 0)
    {
      return;
    }

  render_room_devices((unsigned int)room - 1);
}

static void create_rooms_page(void)
{
  lv_obj_t *sidebar;
  lv_obj_t *button;
  lv_obj_t *label;
  unsigned int index;
  char count[24];

  memset(g_room_buttons, 0, sizeof(g_room_buttons));
  g_room_title_label = NULL;
  g_room_summary_label = NULL;
  g_room_device_host = NULL;

  sidebar = lv_obj_create(g_content);
  lv_obj_remove_style_all(sidebar);
  lv_obj_set_pos(sidebar, 0, 0);
  lv_obj_set_size(sidebar, ROOM_NAV_WIDTH,
                  PANEL_HEIGHT - TOPBAR_HEIGHT);
  lv_obj_set_style_bg_color(sidebar, lv_color_hex(0x13161a), 0);
  lv_obj_set_style_bg_opa(sidebar, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_left(sidebar, 10, 0);
  lv_obj_set_style_pad_right(sidebar, 10, 0);
  lv_obj_set_style_pad_top(sidebar, 12, 0);
  lv_obj_set_style_pad_bottom(sidebar, 12, 0);
  lv_obj_set_scroll_dir(sidebar, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(sidebar, LV_SCROLLBAR_MODE_ACTIVE);

  make_label(sidebar, "房间", 10, 4, lv_color_hex(COLOR_TEXT),
             home_panel_font_get());
  for (index = 0; index < g_family_model.room_count; index++)
    {
      const struct home_panel_room_s *room = &g_family_model.rooms[index];

      button = lv_button_create(sidebar);
      g_room_buttons[index] = button;
      lv_obj_set_pos(button, 0, 48 + (int)index * 58);
      lv_obj_set_size(button, ROOM_NAV_WIDTH - 20, 50);
      lv_obj_set_style_radius(button, 8, 0);
      lv_obj_set_style_shadow_width(button, 0, 0);
      lv_obj_set_style_bg_color(button, lv_color_hex(0x13161a), 0);
      lv_obj_set_style_bg_color(button, lv_color_hex(0x303740),
                                LV_STATE_PRESSED);
      lv_obj_add_event_cb(button, room_clicked, LV_EVENT_CLICKED,
                          (void *)(uintptr_t)(index + 1));

      label = make_label(button, room->name, 8, 4,
                         lv_color_hex(COLOR_MUTED),
                         home_panel_font_get());
      lv_obj_set_width(label, 112);
      lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
      snprintf(count, sizeof(count), "%u", room->device_count);
      label = make_label(button, count, 132, 4,
                         lv_color_hex(COLOR_MUTED),
                         home_panel_font_get());
      lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, 0);
    }

  g_room_title_label = make_label(g_content, "房间", 216, 20,
                                  lv_color_hex(COLOR_TEXT),
                                  home_panel_font_get());
  g_room_summary_label = make_label(g_content,
                                    "按空间查看和控制设备", 216, 52,
                                    lv_color_hex(COLOR_MUTED),
                                    home_panel_font_get());

  g_room_device_host = lv_obj_create(g_content);
  lv_obj_set_pos(g_room_device_host, 210, 92);
  lv_obj_set_size(g_room_device_host, 642, 418);
  lv_obj_set_style_radius(g_room_device_host, 0, 0);
  lv_obj_set_style_border_width(g_room_device_host, 0, 0);
  lv_obj_set_style_bg_color(g_room_device_host, lv_color_hex(COLOR_BG), 0);
  lv_obj_set_style_bg_opa(g_room_device_host, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(g_room_device_host, 10, 0);
  lv_obj_set_scroll_dir(g_room_device_host, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(g_room_device_host, LV_SCROLLBAR_MODE_ACTIVE);
  lv_obj_set_scroll_snap_y(g_room_device_host, LV_SCROLL_SNAP_NONE);

  if (g_family_model.room_count > 0)
    {
      if (g_selected_room >= g_family_model.room_count)
        {
          g_selected_room = 0;
        }
      render_room_devices(g_selected_room);
    }
  else
    {
      lv_label_set_text(g_room_summary_label, "正在同步米家房间");
      label = make_label(g_room_device_host, "暂无房间数据", 0, 24,
                         lv_color_hex(COLOR_MUTED),
                         home_panel_font_get());
      lv_obj_set_width(label, 580);
      lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    }

  g_status_label = g_room_summary_label;
}

static void create_scenes_page(void)
{
  unsigned int index;

  make_label(g_content, "场景", 30, 22, lv_color_hex(COLOR_TEXT),
             home_panel_font_get());
  make_label(g_content, "一次控制多个家庭设备", 30, 54,
             lv_color_hex(COLOR_MUTED), home_panel_font_get());
  for (index = 0; index < g_family_model.scene_count && index < 3; index++)
    {
      static const int positions[3] = {30, 226, 422};
      static const uint32_t colors[3] =
      {
        COLOR_GREEN, COLOR_BLUE, COLOR_ORANGE
      };
      make_scene_button(g_content, LV_SYMBOL_PLAY,
                        &g_family_model.scenes[index], index,
                        positions[index], lv_color_hex(colors[index]));
    }
  g_status_label = make_label(g_content,
                              g_family_model.scene_count > 0 ?
                              "点击场景即可执行" : "暂无可执行场景",
                              30, 208, lv_color_hex(COLOR_MUTED),
                              home_panel_font_get());
}

static void create_settings_page(void)
{
  lv_obj_t *panel;

  make_label(g_content, "设置", 30, 22, lv_color_hex(COLOR_TEXT),
             home_panel_font_get());
  make_label(g_content, "网络、账号与系统状态", 30, 54,
             lv_color_hex(COLOR_MUTED), home_panel_font_get());

  panel = lv_obj_create(g_content);
  lv_obj_set_pos(panel, 30, 104);
  lv_obj_set_size(panel, 790, 318);
  lv_obj_set_style_radius(panel, 8, 0);
  lv_obj_set_style_bg_color(panel, lv_color_hex(COLOR_SURFACE), 0);
  lv_obj_set_style_border_color(panel, lv_color_hex(0x2c323a), 0);
  lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
  g_settings_network_label = make_info_row(panel, "有线网络",
                                            "有线网络未连接", 24,
                                            lv_color_hex(COLOR_ORANGE));
  g_settings_probe_label = make_info_row(panel, "互联网检测",
                                          "等待网线连接", 82,
                                          lv_color_hex(COLOR_ORANGE));
  make_info_row(panel, "地址获取", "DHCP 自动", 140,
                lv_color_hex(COLOR_TEXT));
  g_settings_account_label = make_info_row(panel, "米家账号", "未登录",
                                            198,
                                            lv_color_hex(COLOR_MUTED));
  make_info_row(panel, "状态通道",
                g_family_model.mqtt_connected ? "MQTT 实时推送" :
                (g_family_model.event_source[0] != '\0' ?
                 "云端定向回读" : "等待米家同步"),
                246, g_family_model.mqtt_connected ?
                     lv_color_hex(COLOR_GREEN) : lv_color_hex(COLOR_BLUE));
  g_status_label = make_label(g_content, "系统每分钟自动检测网络",
                              30, 458, lv_color_hex(COLOR_MUTED),
                              home_panel_font_get());
  panel = make_action_button(g_content, "重新检测网络", 628, 450, 192);
  lv_obj_remove_event_cb(panel, action_clicked);
  lv_obj_add_event_cb(panel, network_refresh_clicked, LV_EVENT_CLICKED, NULL);
  apply_network_state(g_network_state);
}

static void show_page(unsigned int page)
{
  struct home_panel_mijia_snapshot_s snapshot;
  bool creating;
  unsigned int i;

  if (page >= HOME_PAGE_COUNT)
    {
      page = 0;
    }

  if (g_pages[page] != NULL &&
      g_page_model_revisions[page] != g_family_ui_revision)
    {
      delete_page(page);
    }

  g_current_page = page;
  creating = g_pages[page] == NULL;

  for (i = 0; i < HOME_PAGE_COUNT; i++)
    {
      lv_obj_set_style_bg_color(g_nav_buttons[i],
                                lv_color_hex(i == page ? COLOR_SURFACE_2 :
                                             COLOR_BG), 0);
      if (g_pages[i] != NULL)
        {
          if (i == page)
            {
              lv_obj_remove_flag(g_pages[i], LV_OBJ_FLAG_HIDDEN);
            }
          else
            {
              lv_obj_add_flag(g_pages[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

  if (!creating)
    {
      g_content = g_pages[page];
      g_status_label = g_page_status_labels[page];
      g_settings_network_label = g_page_settings_network_labels[page];
      g_settings_probe_label = g_page_settings_probe_labels[page];
      g_settings_account_label = g_page_settings_account_labels[page];
      g_home_summary_label = g_page_home_summary_labels[page];
      home_panel_mijia_get_snapshot(&snapshot);
      apply_mijia_snapshot(&snapshot);
      syslog(LOG_INFO, "[HOME][UI] page-switch page=%u model=%u\n",
             page, (unsigned int)g_family_model.revision);
      return;
    }

  g_pages[page] = lv_obj_create(g_page_host);
  lv_obj_remove_style_all(g_pages[page]);
  lv_obj_set_pos(g_pages[page], 0, 0);
  lv_obj_set_size(g_pages[page], PANEL_WIDTH - NAV_WIDTH,
                  PANEL_HEIGHT - TOPBAR_HEIGHT);
  lv_obj_set_style_bg_color(g_pages[page], lv_color_hex(COLOR_BG), 0);
  lv_obj_set_style_bg_opa(g_pages[page], LV_OPA_COVER, 0);
  lv_obj_clear_flag(g_pages[page], LV_OBJ_FLAG_SCROLLABLE);
  g_content = g_pages[page];
  g_device_binding_counts[page] = 0;
  memset(g_device_bindings[page], 0, sizeof(g_device_bindings[page]));
  memset(g_scene_bindings[page], 0, sizeof(g_scene_bindings[page]));
  g_status_label = NULL;
  g_settings_network_label = NULL;
  g_settings_probe_label = NULL;
  g_settings_account_label = NULL;
  g_home_summary_label = NULL;
  syslog(LOG_INFO, "[HOME][UI] page-render begin page=%u model=%u\n",
         page, (unsigned int)g_family_model.revision);

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

  g_page_status_labels[page] = g_status_label;
  g_page_settings_network_labels[page] = g_settings_network_label;
  g_page_settings_probe_labels[page] = g_settings_probe_label;
  g_page_settings_account_labels[page] = g_settings_account_label;
  g_page_home_summary_labels[page] = g_home_summary_label;
  g_page_model_revisions[page] = g_family_ui_revision;
  lv_obj_invalidate(g_content);
  home_panel_mijia_get_snapshot(&snapshot);
  apply_mijia_snapshot(&snapshot);
  syslog(LOG_INFO, "[HOME][UI] page-render end page=%u model=%u\n",
         page, (unsigned int)g_family_model.revision);
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
  g_page_home_summary_labels[page] = NULL;
  g_page_model_revisions[page] = UINT32_MAX;
  g_device_binding_counts[page] = 0;
  memset(g_device_bindings[page], 0, sizeof(g_device_bindings[page]));
  memset(g_scene_bindings[page], 0, sizeof(g_scene_bindings[page]));

  if (page == 1)
    {
      memset(g_room_buttons, 0, sizeof(g_room_buttons));
      g_room_title_label = NULL;
      g_room_summary_label = NULL;
      g_room_device_host = NULL;
    }

  if (g_current_page == page)
    {
      g_content = NULL;
      g_status_label = NULL;
      g_settings_network_label = NULL;
      g_settings_probe_label = NULL;
      g_settings_account_label = NULL;
      g_home_summary_label = NULL;
    }
}

static bool device_structure_changed(
  const struct home_panel_device_s *before,
  const struct home_panel_device_s *after)
{
  return strcmp(before->did, after->did) != 0 ||
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
         before->has_battery != after->has_battery;
}

static bool family_structure_changed(
  const struct home_panel_family_model_s *before,
  const struct home_panel_family_model_s *after)
{
  unsigned int index;

  if (before->device_count != after->device_count ||
      before->room_count != after->room_count ||
      before->scene_count != after->scene_count ||
      before->mqtt_connected != after->mqtt_connected ||
      strcmp(before->event_source, after->event_source) != 0)
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
  char value[32];

  if (device == NULL || binding->button == NULL ||
      binding->value_label == NULL || binding->state_label == NULL)
    {
      return;
    }

  checked = device->has_power && device->power;
  format_device_value(device, value, sizeof(value));
  lv_label_set_text(binding->value_label, value);
  lv_label_set_text(binding->state_label, device_state_text(device));
  lv_obj_set_style_text_color(
    binding->state_label,
    lv_color_hex(checked ? COLOR_GREEN : COLOR_MUTED), 0);
  if (checked)
    {
      lv_obj_add_state(binding->button, LV_STATE_CHECKED);
    }
  else
    {
      lv_obj_remove_state(binding->button, LV_STATE_CHECKED);
    }

  lv_obj_set_style_bg_color(
    binding->button,
    lv_color_hex(checked ? COLOR_GREEN : COLOR_SURFACE_2), 0);
  if (device->online)
    {
      lv_obj_remove_state(binding->button, LV_STATE_DISABLED);
    }
  else
    {
      lv_obj_add_state(binding->button, LV_STATE_DISABLED);
    }
}

static void update_model_widgets(void)
{
  const struct home_panel_room_s *environment = NULL;
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
      lv_label_set_text(g_page_home_summary_labels[0], text);
    }

  if (g_page_status_labels[0] != NULL)
    {
      snprintf(text, sizeof(text), "%u/%u 台在线",
               g_family_model.online_count, g_family_model.device_count);
      lv_label_set_text(g_page_status_labels[0], text);
      lv_obj_set_style_text_color(g_page_status_labels[0],
                                  lv_color_hex(COLOR_MUTED), 0);
    }

  if (g_room_summary_label != NULL &&
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
      lv_label_set_text(g_room_summary_label, text);
    }
}

static int refresh_family_model(
  const struct home_panel_mijia_family_snapshot_s *snapshot)
{
  struct home_panel_family_model_s model;
  bool changed;
  bool structure_changed;
  int ret;

  if (snapshot->revision == 0 || snapshot->json_size == 0)
    {
      return -EAGAIN;
    }

  ret = home_panel_mijia_get_family_model(snapshot->revision, &model);

  if (ret == 0)
    {
      changed = !g_family_model_valid ||
                memcmp((const char *)&g_family_model +
                       sizeof(g_family_model.revision),
                       (const char *)&model + sizeof(model.revision),
                       sizeof(model) - sizeof(model.revision)) != 0;
      if (changed)
        {
          structure_changed = !g_family_model_valid ||
            family_structure_changed(&g_family_model, &model);
          memcpy(&g_family_model, &model, sizeof(g_family_model));
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
              ret = 2;
            }
          syslog(LOG_INFO,
                 "[HOME][MODEL] revision=%u devices=%u online=%u rooms=%u "
                 "scenes=%u event=%s mqtt=%u update=%s\n",
                 (unsigned int)model.revision, model.device_count,
                 model.online_count, model.room_count, model.scene_count,
                 model.event_source, model.mqtt_connected ? 1 : 0,
                 structure_changed ? "rebuild" : "in-place");
        }
      else
        {
          g_family_model.revision = model.revision;
          syslog(LOG_INFO,
                 "[HOME][MODEL] revision=%u display-unchanged\n",
                 (unsigned int)model.revision);
          ret = 1;
        }
    }
  else
    {
      syslog(LOG_WARNING,
             "[HOME][MODEL] parse failed revision=%u ret=%d\n",
             (unsigned int)snapshot->revision, ret);
    }

  return ret;
}

static void create_home_screen(void)
{
  lv_obj_t *screen = lv_screen_active();
  lv_obj_t *topbar;
  lv_obj_t *nav;
  lv_obj_t *login;

  lv_obj_set_style_bg_color(screen, lv_color_hex(COLOR_BG), 0);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
  lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

  topbar = lv_obj_create(screen);
  lv_obj_remove_style_all(topbar);
  lv_obj_set_pos(topbar, 0, 0);
  lv_obj_set_size(topbar, PANEL_WIDTH, TOPBAR_HEIGHT);
  lv_obj_set_style_bg_color(topbar, lv_color_hex(COLOR_SURFACE), 0);
  lv_obj_set_style_bg_opa(topbar, LV_OPA_COVER, 0);

  g_clock_label = make_label(topbar, "--:--", 24, 17,
                             lv_color_hex(COLOR_TEXT),
                             &home_panel_digits_28);
  g_date_label = make_label(topbar, "等待网络校时", 132, 26,
                            lv_color_hex(COLOR_MUTED),
                            home_panel_font_get());
  g_network_label = make_label(topbar,
                               LV_SYMBOL_WARNING "  有线网络未连接",
                               650, 26, lv_color_hex(COLOR_ORANGE),
                               home_panel_font_get());

  login = lv_button_create(topbar);
  lv_obj_set_size(login, 118, 44);
  lv_obj_set_pos(login, 886, 14);
  lv_obj_set_style_radius(login, 8, 0);
  lv_obj_set_style_shadow_width(login, 0, 0);
  lv_obj_set_style_bg_color(login, lv_color_hex(COLOR_BLUE), 0);
  lv_obj_set_style_bg_color(login, lv_color_hex(0x3475d6),
                            LV_STATE_PRESSED);
  lv_obj_add_event_cb(login, show_login, LV_EVENT_CLICKED, NULL);
  g_login_top_label = lv_label_create(login);
  lv_label_set_text(g_login_top_label, "登录米家");
  set_chinese_font(g_login_top_label);
  lv_obj_center(g_login_top_label);

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

  g_page_host = lv_obj_create(screen);
  lv_obj_remove_style_all(g_page_host);
  lv_obj_set_pos(g_page_host, NAV_WIDTH, TOPBAR_HEIGHT);
  lv_obj_set_size(g_page_host, PANEL_WIDTH - NAV_WIDTH,
                  PANEL_HEIGHT - TOPBAR_HEIGHT);
  lv_obj_set_style_bg_color(g_page_host, lv_color_hex(COLOR_BG), 0);
  lv_obj_set_style_bg_opa(g_page_host, LV_OPA_COVER, 0);
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
  uint32_t displayed_mijia_revision = UINT32_MAX;
  uint32_t displayed_family_revision = 0;
  uint32_t displayed_command_revision = 0;
  int ret;

  (void)argc;
  (void)argv;

  if (lv_is_initialized())
    {
      fprintf(stderr, "home_panel: LVGL is already initialized\n");
      return 1;
    }

  log_fpu_state();
  lv_init();
  ret = home_panel_font_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR,
             "[HOME][FONT] using built-in subset fallback ret=%d\n", ret);
    }

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

  for (;;)
    {
      uint32_t delay;

      if (displayed_state != g_network_state)
        {
          displayed_state = g_network_state;
          apply_network_state(displayed_state);
          syslog(LOG_INFO, "[HOME][UI] network=%s\n",
                 network_state_name(displayed_state));
        }

      update_time_ui(false);

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

      home_panel_mijia_get_family_snapshot(&family_snapshot);
      if (family_snapshot.revision != 0 &&
          displayed_family_revision != family_snapshot.revision)
        {
          ret = refresh_family_model(&family_snapshot);
          if (ret >= 0)
            {
              displayed_family_revision = family_snapshot.revision;
              if (ret == 0)
                {
                  g_model_refresh_pending = true;
                }
            }
        }

      if (g_model_refresh_pending &&
          lv_indev_get_state(result.indev) == LV_INDEV_STATE_RELEASED)
        {
          g_model_refresh_pending = false;
          show_page(g_current_page);
        }

      home_panel_mijia_get_command_snapshot(&command_snapshot);
      if (displayed_command_revision != command_snapshot.revision)
        {
          displayed_command_revision = command_snapshot.revision;
          if (g_status_label != NULL && command_snapshot.revision != 0)
            {
              lv_label_set_text(g_status_label, command_snapshot.message);
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

      delay = lv_timer_handler();
      if (delay == LV_NO_TIMER_READY || delay > UI_LOOP_MAX_DELAY_MS)
        {
          delay = UI_LOOP_MAX_DELAY_MS;
        }

      usleep((delay > 0 ? delay : 1) * 1000);
    }

  return 0;
}
