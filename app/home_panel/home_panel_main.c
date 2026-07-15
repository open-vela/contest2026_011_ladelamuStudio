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
#include <unistd.h>

#include <lvgl/lvgl.h>
#include <netutils/netlib.h>

#include "home_panel_mijia_client.h"

LV_FONT_DECLARE(home_panel_misans_18);
LV_FONT_DECLARE(home_panel_digits_28);

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

#define NETWORK_INTERFACE       "eth0"
#define NETWORK_PROBE_INTERVAL  15
#define NETWORK_THREAD_STACK    8192
#define NETWORK_PING_POLL_US    20000
#define NETWORK_PING_POLLS      75
#define NETWORK_PING_DATA_SIZE  16
#define NETWORK_DNS_BUFFER_SIZE 512
#define UI_LOOP_MAX_DELAY_MS    20

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
static lv_obj_t *g_nav_buttons[4];
static lv_obj_t *g_network_label;
static lv_obj_t *g_settings_network_label;
static lv_obj_t *g_settings_probe_label;
static lv_obj_t *g_settings_account_label;
static lv_obj_t *g_login_top_label;
static lv_obj_t *g_login_shade;
static lv_obj_t *g_login_qr;
static lv_obj_t *g_login_message;
static lv_obj_t *g_login_action_label;
static lv_image_dsc_t g_login_qr_image;
static uint8_t *g_login_qr_data;
static uint32_t g_login_qr_revision;
static volatile enum network_state_e g_network_state =
  NETWORK_INITIALIZING;
static volatile bool g_network_refresh_requested = true;
static uint16_t g_network_ping_id;
static uint16_t g_network_dns_id;
static void nav_clicked(lv_event_t *event);
static void show_page(unsigned int page);
static void apply_mijia_snapshot(
  const struct home_panel_mijia_snapshot_s *snapshot);

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
  char server_text[INET_ADDRSTRLEN];
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

  inet_ntop(AF_INET, &dns_server.sin_addr, server_text,
            sizeof(server_text));
  syslog(LOG_INFO, "[HOME][NET] host=%s dns=%s\n",
         hostname, server_text);

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

  syslog(LOG_INFO, "[HOME][NET] host=%s dns-sent=%d\n", hostname, ret);

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

      if (poll_count == NETWORK_PING_POLLS / 3 ||
          poll_count == (NETWORK_PING_POLLS * 2) / 3)
        {
          syslog(LOG_INFO, "[HOME][NET] host=%s dns-wait=%u/%u\n",
                 hostname, poll_count, NETWORK_PING_POLLS);
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
  char address_text[INET_ADDRSTRLEN];
  int sockfd;
  int ret;

  memset(&destination, 0, sizeof(destination));
  destination.sin_family = AF_INET;
  if (!network_resolve_host(hostname, &destination.sin_addr))
    {
      return 0;
    }

  inet_ntop(AF_INET, &destination.sin_addr, address_text,
            sizeof(address_text));
  syslog(LOG_INFO, "[HOME][NET] host=%s address=%s\n",
         hostname, address_text);

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
          syslog(LOG_INFO, "[HOME][NET] ipv4=%s probe=mi.com\n",
                 address_text);
          replies = network_ping_host("mi.com");
          syslog(LOG_INFO, "[HOME][NET] host=mi.com replies=%d\n", replies);
          if (replies <= 0 && network_has_carrier())
            {
              replies = network_ping_host("xiaomi.cn");
              syslog(LOG_INFO,
                     "[HOME][NET] host=xiaomi.cn replies=%d\n", replies);
            }

          if (!network_has_carrier())
            {
              network_set_state(NETWORK_DISCONNECTED);
            }
          else
            {
              network_set_state(replies > 0 ? NETWORK_ONLINE :
                                              NETWORK_NO_INTERNET);
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
                     lv_color_hex(COLOR_TEXT), &home_panel_misans_18);
  lv_obj_set_style_text_font(label, &home_panel_misans_18, 0);

  g_login_qr = lv_image_create(dialog);
  lv_obj_set_size(g_login_qr, 256, 256);
  lv_image_set_inner_align(g_login_qr, LV_IMAGE_ALIGN_CENTER);
  lv_obj_set_pos(g_login_qr, 18, 62);
  lv_obj_set_style_border_color(g_login_qr, lv_color_hex(0xffffff), 0);
  lv_obj_set_style_border_width(g_login_qr, 8, 0);
  lv_obj_add_flag(g_login_qr, LV_OBJ_FLAG_HIDDEN);

  g_login_message = make_label(dialog, "正在准备米家登录",
                               282, 78, lv_color_hex(COLOR_MUTED),
                               &home_panel_misans_18);
  lv_obj_set_width(g_login_message, 340);
  lv_label_set_long_mode(g_login_message, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_line_space(g_login_message, 12, 0);

  label = make_label(dialog,
                     "请使用米家 App 扫码。账号认证数据只保存在\n"
                     "本地服务端的加密保险箱中。",
                     282, 164, lv_color_hex(COLOR_MUTED),
                     &home_panel_misans_18);
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

  make_label(card, symbol, 0, 0, accent, &lv_font_montserrat_16);
  make_label(card, room, 0, 58, lv_color_hex(COLOR_MUTED),
             &home_panel_misans_18);
  make_label(card, name, 0, 88, lv_color_hex(COLOR_TEXT),
             &home_panel_misans_18);
  label = make_label(card, value, 0, 122, accent, &home_panel_digits_28);
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

static lv_obj_t *make_info_row(lv_obj_t *parent, const char *name,
                               const char *value, int y, lv_color_t color)
{
  make_label(parent, name, 32, y, lv_color_hex(COLOR_MUTED),
             &home_panel_misans_18);
  return make_label(parent, value, 238, y, color,
                    &home_panel_misans_18);
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
  lv_obj_set_size(panel, 790, 270);
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
  g_status_label = make_label(g_content, "系统每 15 秒自动检测网络",
                              30, 416, lv_color_hex(COLOR_MUTED),
                              &home_panel_misans_18);
  panel = make_action_button(g_content, "重新检测网络", 628, 408, 192);
  lv_obj_remove_event_cb(panel, action_clicked);
  lv_obj_add_event_cb(panel, network_refresh_clicked, LV_EVENT_CLICKED, NULL);
  apply_network_state(g_network_state);
}

static void show_page(unsigned int page)
{
  struct home_panel_mijia_snapshot_s snapshot;
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
  g_settings_network_label = NULL;
  g_settings_probe_label = NULL;
  g_settings_account_label = NULL;
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
  home_panel_mijia_get_snapshot(&snapshot);
  apply_mijia_snapshot(&snapshot);
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

  make_label(topbar, "08:42", 24, 17, lv_color_hex(COLOR_TEXT),
             &home_panel_digits_28);
  make_label(topbar, "7月14日  星期二", 132, 26,
             lv_color_hex(COLOR_MUTED), &home_panel_misans_18);
  g_network_label = make_label(topbar,
                               LV_SYMBOL_WARNING "  有线网络未连接",
                               650, 26, lv_color_hex(COLOR_ORANGE),
                               &home_panel_misans_18);

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
  enum network_state_e displayed_state = (enum network_state_e)-1;
  struct home_panel_mijia_snapshot_s mijia_snapshot;
  uint32_t displayed_mijia_revision = UINT32_MAX;
  int ret;

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
          lv_refr_now(result.disp);
          syslog(LOG_INFO, "[HOME][UI] network=%s\n",
                 network_state_name(displayed_state));
        }

      home_panel_mijia_get_snapshot(&mijia_snapshot);
      if (displayed_mijia_revision != mijia_snapshot.revision)
        {
          displayed_mijia_revision = mijia_snapshot.revision;
          apply_mijia_snapshot(&mijia_snapshot);
          lv_refr_now(result.disp);
          syslog(LOG_INFO, "[HOME][UI] mijia=%s\n",
                 home_panel_mijia_state_name(mijia_snapshot.state));
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
