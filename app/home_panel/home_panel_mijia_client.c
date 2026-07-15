/****************************************************************************
 * D13x home panel Mijia API client
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include <netutils/cJSON.h>
#include <netutils/webclient.h>

#include "home_panel_mijia_client.h"

#define MIJIA_HTTP_BUFFER_SIZE       1024
#define MIJIA_JSON_BUFFER_SIZE       2048
#define MIJIA_LOGIN_POLL_SECONDS     2
#define MIJIA_LOGIN_MAX_POLLS        90
#define MIJIA_CLIENT_THREAD_STACK    16384
#define MIJIA_QR_WIRE_HEADER_SIZE    12
#define MIJIA_QR_WIDTH               240
#define MIJIA_QR_HEIGHT              240
#define MIJIA_QR_STRIDE              (MIJIA_QR_WIDTH * 2)
#define MIJIA_QR_DATA_SIZE           (MIJIA_QR_STRIDE * MIJIA_QR_HEIGHT)
#define MIJIA_QR_WIRE_SIZE           (MIJIA_QR_WIRE_HEADER_SIZE + \
                                      MIJIA_QR_DATA_SIZE)

struct mijia_response_s
{
  char *data;
  size_t capacity;
  size_t length;
};

struct mijia_client_s
{
  pthread_mutex_t lock;
  pthread_cond_t condition;
  pthread_t thread;
  bool initialized;
  bool request_pending;
  uint32_t request_generation;
  uint32_t next_revision;
  uint32_t next_qr_revision;
  size_t qr_size;
  struct home_panel_mijia_snapshot_s snapshot;
};

static struct mijia_client_s g_mijia =
{
  .lock = PTHREAD_MUTEX_INITIALIZER,
  .condition = PTHREAD_COND_INITIALIZER,
};
static uint8_t g_mijia_qr_data[MIJIA_QR_DATA_SIZE];

static int mijia_response_sink(char **buffer, int offset, int datend,
                               int *buflen, void *arg)
{
  struct mijia_response_s *response = arg;
  size_t length;

  (void)buflen;

  if (offset < 0 || datend < offset)
    {
      return -EINVAL;
    }

  length = (size_t)(datend - offset);
  if (response->length + length >= response->capacity)
    {
      return -EFBIG;
    }

  memcpy(response->data + response->length, *buffer + offset, length);
  response->length += length;
  response->data[response->length] = '\0';
  return 0;
}

static int mijia_binary_sink(char **buffer, int offset, int datend,
                             int *buflen, void *arg)
{
  struct mijia_response_s *response = arg;
  size_t length;

  (void)buflen;
  if (offset < 0 || datend < offset)
    {
      return -EINVAL;
    }

  length = (size_t)(datend - offset);
  if (response->length + length > response->capacity)
    {
      return -EFBIG;
    }

  memcpy(response->data + response->length, *buffer + offset, length);
  response->length += length;
  return 0;
}

static int mijia_http_request(const char *method, const char *path,
                              const char *body, char *response_data,
                              size_t response_capacity,
                              unsigned int *http_status)
{
  static const char *json_headers[] =
  {
    "Accept: application/json",
    "Content-Type: application/json",
    "Connection: close"
  };
  static const char *get_headers[] =
  {
    "Accept: application/json",
    "Connection: close"
  };
  struct webclient_context context;
  struct mijia_response_s response;
  char url[512];
  char buffer[MIJIA_HTTP_BUFFER_SIZE];
  int length;
  int ret;

  length = snprintf(url, sizeof(url), "%s%s",
                    CONFIG_D13X_HOME_PANEL_MIJIA_SERVER_URL, path);
  if (length < 0 || (size_t)length >= sizeof(url))
    {
      return -ENAMETOOLONG;
    }

  memset(&response, 0, sizeof(response));
  response.data = response_data;
  response.capacity = response_capacity;
  response_data[0] = '\0';

  webclient_set_defaults(&context);
  context.method = method;
  context.url = url;
  context.buffer = buffer;
  context.buflen = sizeof(buffer);
  context.sink_callback = mijia_response_sink;
  context.sink_callback_arg = &response;
  context.timeout_sec = CONFIG_D13X_HOME_PANEL_MIJIA_TIMEOUT;

  if (body != NULL)
    {
      context.headers = json_headers;
      context.nheaders = sizeof(json_headers) / sizeof(json_headers[0]);
      webclient_set_static_body(&context, body, strlen(body));
    }
  else
    {
      context.headers = get_headers;
      context.nheaders = sizeof(get_headers) / sizeof(get_headers[0]);
    }

  ret = webclient_perform(&context);
  *http_status = context.http_status;
  if (ret < 0)
    {
      syslog(LOG_WARNING,
             "[HOME][MIJIA] %s %s transport=%d status=%u\n",
             method, path, ret, context.http_status);
      return ret;
    }

  if (context.http_status < 200 || context.http_status >= 300)
    {
      syslog(LOG_WARNING,
             "[HOME][MIJIA] %s %s status=%u body=%s\n",
             method, path, context.http_status, response_data);
      return -EPROTO;
    }

  return 0;
}

static const char *mijia_json_string(cJSON *root, const char *name)
{
  cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);

  return cJSON_IsString(item) && item->valuestring != NULL ?
         item->valuestring : NULL;
}

static void mijia_copy_string(char *destination, size_t capacity,
                              const char *source)
{
  if (source == NULL)
    {
      destination[0] = '\0';
      return;
    }

  snprintf(destination, capacity, "%s", source);
}

static bool mijia_generation_active(uint32_t generation)
{
  bool active;

  pthread_mutex_lock(&g_mijia.lock);
  active = generation == g_mijia.request_generation;
  pthread_mutex_unlock(&g_mijia.lock);
  return active;
}

static void mijia_publish(uint32_t generation,
                          enum home_panel_mijia_state_e state,
                          const char *message, const char *login_url,
                          const char *home_name, unsigned int device_count,
                          unsigned int online_count)
{
  pthread_mutex_lock(&g_mijia.lock);
  if (generation == g_mijia.request_generation)
    {
      g_mijia.snapshot.state = state;
      g_mijia.snapshot.revision = ++g_mijia.next_revision;
      g_mijia.snapshot.device_count = device_count;
      g_mijia.snapshot.online_count = online_count;
      mijia_copy_string(g_mijia.snapshot.message,
                        sizeof(g_mijia.snapshot.message), message);
      mijia_copy_string(g_mijia.snapshot.home_name,
                        sizeof(g_mijia.snapshot.home_name), home_name);
    }

  pthread_mutex_unlock(&g_mijia.lock);
  (void)login_url;
}

static int mijia_download_login_qr(const char *session_id,
                                   uint32_t generation)
{
  static const char *headers[] =
  {
    "Accept: application/octet-stream",
    "Connection: close"
  };
  struct webclient_context context;
  struct mijia_response_s sink;
  char path[96];
  char url[512];
  char http_buffer[MIJIA_HTTP_BUFFER_SIZE];
  uint8_t *wire_data;
  int length;
  int ret;

  wire_data = malloc(MIJIA_QR_WIRE_SIZE);
  if (wire_data == NULL)
    {
      return -ENOMEM;
    }

  length = snprintf(path, sizeof(path),
                    "/api/login/qr.rgb565?session_id=%s", session_id);
  if (length < 0 || (size_t)length >= sizeof(path))
    {
      free(wire_data);
      return -ENAMETOOLONG;
    }

  length = snprintf(url, sizeof(url), "%s%s",
                    CONFIG_D13X_HOME_PANEL_MIJIA_SERVER_URL, path);
  if (length < 0 || (size_t)length >= sizeof(url))
    {
      free(wire_data);
      return -ENAMETOOLONG;
    }

  memset(&sink, 0, sizeof(sink));
  sink.data = (char *)wire_data;
  sink.capacity = MIJIA_QR_WIRE_SIZE;
  webclient_set_defaults(&context);
  context.method = "GET";
  context.url = url;
  context.buffer = http_buffer;
  context.buflen = sizeof(http_buffer);
  context.headers = headers;
  context.nheaders = sizeof(headers) / sizeof(headers[0]);
  context.sink_callback = mijia_binary_sink;
  context.sink_callback_arg = &sink;
  context.timeout_sec = CONFIG_D13X_HOME_PANEL_MIJIA_TIMEOUT;
  ret = webclient_perform(&context);
  if (ret >= 0 && (context.http_status < 200 || context.http_status >= 300))
    {
      ret = -EPROTO;
    }

  if (ret >= 0 &&
      (sink.length != MIJIA_QR_WIRE_SIZE ||
       memcmp(wire_data, "HQR2", 4) != 0 ||
       wire_data[4] != 0 || wire_data[5] != MIJIA_QR_WIDTH ||
       wire_data[6] != 0 || wire_data[7] != MIJIA_QR_HEIGHT ||
       wire_data[8] != (MIJIA_QR_STRIDE >> 8) ||
       wire_data[9] != (MIJIA_QR_STRIDE & 0xff) ||
       wire_data[10] != 2 || wire_data[11] != 0))
    {
      ret = -EBADMSG;
    }

  if (ret >= 0)
    {
      pthread_mutex_lock(&g_mijia.lock);
      if (generation != g_mijia.request_generation)
        {
          ret = -ECANCELED;
        }
      else
        {
          memcpy(g_mijia_qr_data,
                 wire_data + MIJIA_QR_WIRE_HEADER_SIZE,
                 MIJIA_QR_DATA_SIZE);
          g_mijia.qr_size = MIJIA_QR_DATA_SIZE;
          g_mijia.snapshot.qr_size = MIJIA_QR_DATA_SIZE;
          g_mijia.snapshot.qr_revision = ++g_mijia.next_qr_revision;
          syslog(LOG_INFO,
                 "[HOME][MIJIA] qr downloaded format=RGB565 %ux%u bytes=%u\n",
                 MIJIA_QR_WIDTH, MIJIA_QR_HEIGHT,
                 (unsigned int)MIJIA_QR_DATA_SIZE);
        }
      pthread_mutex_unlock(&g_mijia.lock);
    }
  else
    {
      syslog(LOG_WARNING,
             "[HOME][MIJIA] qr download failed ret=%d status=%u bytes=%u\n",
             ret, context.http_status, (unsigned int)sink.length);
    }

  free(wire_data);
  return ret;
}

static int mijia_start_login(char *session_id, size_t session_capacity,
                             char *claim_secret, size_t claim_capacity,
                             char *login_url, size_t login_capacity)
{
  char response[MIJIA_JSON_BUFFER_SIZE];
  const char *value;
  unsigned int status;
  cJSON *root;
  int ret;

  ret = mijia_http_request("POST", "/api/login/start", "", response,
                           sizeof(response), &status);
  if (ret < 0)
    {
      return ret;
    }

  root = cJSON_Parse(response);
  if (root == NULL)
    {
      return -EBADMSG;
    }

  value = mijia_json_string(root, "session_id");
  if (value == NULL || strlen(value) != 32)
    {
      ret = -EBADMSG;
      goto out;
    }

  mijia_copy_string(session_id, session_capacity, value);
  value = mijia_json_string(root, "claim_secret");
  if (value == NULL)
    {
      ret = -EBADMSG;
      goto out;
    }

  mijia_copy_string(claim_secret, claim_capacity, value);
  value = mijia_json_string(root, "login_url");
  if (value == NULL)
    {
      ret = -EBADMSG;
      goto out;
    }

  mijia_copy_string(login_url, login_capacity, value);
  ret = 0;

out:
  cJSON_Delete(root);
  return ret;
}

static int mijia_get_login_status(const char *session_id,
                                  char *state, size_t state_capacity,
                                  char *message, size_t message_capacity)
{
  char response[MIJIA_JSON_BUFFER_SIZE];
  char path[96];
  const char *value;
  unsigned int status;
  cJSON *root;
  int ret;

  snprintf(path, sizeof(path), "/api/login/status?session_id=%s",
           session_id);
  ret = mijia_http_request("GET", path, NULL, response, sizeof(response),
                           &status);
  if (ret < 0)
    {
      return ret;
    }

  root = cJSON_Parse(response);
  if (root == NULL)
    {
      return -EBADMSG;
    }

  value = mijia_json_string(root, "status");
  if (value == NULL)
    {
      ret = -EBADMSG;
      goto out;
    }

  mijia_copy_string(state, state_capacity, value);
  value = mijia_json_string(root, "message");
  mijia_copy_string(message, message_capacity, value);
  ret = 0;

out:
  cJSON_Delete(root);
  return ret;
}

static int mijia_claim_login(const char *session_id,
                             const char *claim_secret,
                             char *token, size_t token_capacity)
{
  char response[MIJIA_JSON_BUFFER_SIZE];
  const char *value;
  char *body = NULL;
  unsigned int status;
  cJSON *root = NULL;
  cJSON *payload;
  int ret = -ENOMEM;

  payload = cJSON_CreateObject();
  if (payload == NULL ||
      !cJSON_AddStringToObject(payload, "session_id", session_id) ||
      !cJSON_AddStringToObject(payload, "claim_secret", claim_secret))
    {
      goto out;
    }

  body = cJSON_PrintUnformatted(payload);
  if (body == NULL)
    {
      goto out;
    }

  ret = mijia_http_request("POST", "/api/login/claim", body, response,
                           sizeof(response), &status);
  if (ret < 0)
    {
      goto out;
    }

  root = cJSON_Parse(response);
  if (root == NULL)
    {
      ret = -EBADMSG;
      goto out;
    }

  value = mijia_json_string(root, "token");
  if (value == NULL)
    {
      ret = -EBADMSG;
      goto out;
    }

  mijia_copy_string(token, token_capacity, value);
  ret = 0;

out:
  cJSON_Delete(root);
  cJSON_free(body);
  cJSON_Delete(payload);
  return ret;
}

static int mijia_set_vault_password(const char *token,
                                    const char *claim_secret)
{
  char response[MIJIA_JSON_BUFFER_SIZE];
  char authorization[192];
  const char *headers[3];
  struct webclient_context context;
  struct mijia_response_s sink;
  cJSON *payload;
  char *body = NULL;
  char url[512];
  char buffer[MIJIA_HTTP_BUFFER_SIZE];
  int ret = -ENOMEM;

  payload = cJSON_CreateObject();
  if (payload == NULL ||
      !cJSON_AddStringToObject(payload, "password", claim_secret))
    {
      goto out;
    }

  body = cJSON_PrintUnformatted(payload);
  if (body == NULL)
    {
      goto out;
    }

  snprintf(url, sizeof(url), "%s/api/session/set_vault_password",
           CONFIG_D13X_HOME_PANEL_MIJIA_SERVER_URL);
  snprintf(authorization, sizeof(authorization),
           "Authorization: Bearer %s", token);
  headers[0] = authorization;
  headers[1] = "Content-Type: application/json";
  headers[2] = "Connection: close";

  memset(&sink, 0, sizeof(sink));
  sink.data = response;
  sink.capacity = sizeof(response);
  response[0] = '\0';
  webclient_set_defaults(&context);
  context.method = "POST";
  context.url = url;
  context.buffer = buffer;
  context.buflen = sizeof(buffer);
  context.headers = headers;
  context.nheaders = 3;
  context.sink_callback = mijia_response_sink;
  context.sink_callback_arg = &sink;
  context.timeout_sec = CONFIG_D13X_HOME_PANEL_MIJIA_TIMEOUT;
  webclient_set_static_body(&context, body, strlen(body));
  ret = webclient_perform(&context);
  if (ret >= 0 && (context.http_status < 200 || context.http_status >= 300))
    {
      ret = -EPROTO;
    }

  if (ret < 0)
    {
      syslog(LOG_WARNING,
             "[HOME][MIJIA] vault setup failed ret=%d status=%u\n",
             ret, context.http_status);
    }

out:
  cJSON_free(body);
  cJSON_Delete(payload);
  return ret;
}

static int mijia_fetch_devices(const char *token, char *home_name,
                               size_t home_capacity,
                               unsigned int *device_count,
                               unsigned int *online_count)
{
  char authorization[192];
  const char *headers[2];
  struct webclient_context context;
  struct mijia_response_s sink;
  cJSON *devices;
  cJSON *item;
  cJSON *root = NULL;
  char *response;
  char url[512];
  char buffer[MIJIA_HTTP_BUFFER_SIZE];
  int ret = -ENOMEM;

  response = malloc(CONFIG_D13X_HOME_PANEL_MIJIA_MAX_RESPONSE);
  if (response == NULL)
    {
      return -ENOMEM;
    }

  snprintf(url, sizeof(url), "%s/api/devices",
           CONFIG_D13X_HOME_PANEL_MIJIA_SERVER_URL);
  snprintf(authorization, sizeof(authorization),
           "Authorization: Bearer %s", token);
  headers[0] = authorization;
  headers[1] = "Connection: close";
  memset(&sink, 0, sizeof(sink));
  sink.data = response;
  sink.capacity = CONFIG_D13X_HOME_PANEL_MIJIA_MAX_RESPONSE;
  response[0] = '\0';

  webclient_set_defaults(&context);
  context.method = "GET";
  context.url = url;
  context.buffer = buffer;
  context.buflen = sizeof(buffer);
  context.headers = headers;
  context.nheaders = 2;
  context.sink_callback = mijia_response_sink;
  context.sink_callback_arg = &sink;
  context.timeout_sec = CONFIG_D13X_HOME_PANEL_MIJIA_TIMEOUT;
  ret = webclient_perform(&context);
  if (ret < 0 || context.http_status < 200 || context.http_status >= 300)
    {
      ret = ret < 0 ? ret : -EPROTO;
      goto out;
    }

  root = cJSON_Parse(response);
  devices = root == NULL ? NULL :
            cJSON_GetObjectItemCaseSensitive(root, "devices");
  if (!cJSON_IsArray(devices))
    {
      ret = -EBADMSG;
      goto out;
    }

  *device_count = 0;
  *online_count = 0;
  home_name[0] = '\0';
  cJSON_ArrayForEach(item, devices)
    {
      cJSON *online = cJSON_GetObjectItemCaseSensitive(item, "isOnline");
      const char *name = mijia_json_string(item, "home_name");

      (*device_count)++;
      if (cJSON_IsTrue(online))
        {
          (*online_count)++;
        }

      if (home_name[0] == '\0' && name != NULL)
        {
          mijia_copy_string(home_name, home_capacity, name);
        }
    }

  ret = 0;

out:
  if (ret < 0)
    {
      syslog(LOG_WARNING,
             "[HOME][MIJIA] devices failed ret=%d status=%u\n",
             ret, context.http_status);
    }

  cJSON_Delete(root);
  free(response);
  return ret;
}

static void mijia_process_login(uint32_t generation)
{
  char session_id[40];
  char claim_secret[64];
  char login_url[384];
  char state[24];
  char message[96];
  char token[128];
  char home_name[64];
  unsigned int device_count;
  unsigned int online_count;
  unsigned int poll;
  int ret;

  mijia_publish(generation, HOME_PANEL_MIJIA_STARTING,
                "正在连接米家服务", NULL, NULL, 0, 0);
  ret = mijia_start_login(session_id, sizeof(session_id),
                          claim_secret, sizeof(claim_secret),
                          login_url, sizeof(login_url));
  if (ret < 0)
    {
      mijia_publish(generation, HOME_PANEL_MIJIA_ERROR,
                    "无法连接米家服务", NULL, NULL, 0, 0);
      return;
    }

  syslog(LOG_INFO, "[HOME][MIJIA] login session started\n");
  ret = mijia_download_login_qr(session_id, generation);
  if (ret < 0)
    {
      mijia_publish(generation, HOME_PANEL_MIJIA_ERROR,
                    "登录二维码下载失败", NULL, NULL, 0, 0);
      return;
    }

  mijia_publish(generation, HOME_PANEL_MIJIA_WAITING,
                "请使用米家 App 扫描并确认", NULL, NULL, 0, 0);

  for (poll = 0; poll < MIJIA_LOGIN_MAX_POLLS; poll++)
    {
      if (!mijia_generation_active(generation))
        {
          return;
        }

      sleep(MIJIA_LOGIN_POLL_SECONDS);
      ret = mijia_get_login_status(session_id, state, sizeof(state),
                                   message, sizeof(message));
      if (ret < 0)
        {
          mijia_publish(generation, HOME_PANEL_MIJIA_ERROR,
                        "登录状态查询失败", NULL, NULL, 0, 0);
          return;
        }

      if (strcmp(state, "waiting") == 0)
        {
          continue;
        }

      if (strcmp(state, "success") != 0)
        {
          mijia_publish(generation,
                        strcmp(state, "expired") == 0 ||
                        strcmp(state, "timeout") == 0 ?
                        HOME_PANEL_MIJIA_EXPIRED : HOME_PANEL_MIJIA_ERROR,
                        message[0] != '\0' ? message : "米家登录失败",
                        NULL, NULL, 0, 0);
          return;
        }

      ret = mijia_claim_login(session_id, claim_secret, token,
                              sizeof(token));
      if (ret < 0)
        {
          mijia_publish(generation, HOME_PANEL_MIJIA_ERROR,
                        "登录凭据领取失败", NULL, NULL, 0, 0);
          return;
        }

      ret = mijia_set_vault_password(token, claim_secret);
      if (ret < 0)
        {
          mijia_publish(generation, HOME_PANEL_MIJIA_ERROR,
                        "账号保险箱初始化失败", NULL, NULL, 0, 0);
          return;
        }

      ret = mijia_fetch_devices(token, home_name, sizeof(home_name),
                                &device_count, &online_count);
      if (ret < 0)
        {
          mijia_publish(generation, HOME_PANEL_MIJIA_ERROR,
                        "已登录，但设备同步失败", NULL, NULL, 0, 0);
          return;
        }

      syslog(LOG_INFO,
             "[HOME][MIJIA] authenticated devices=%u online=%u\n",
             device_count, online_count);
      mijia_publish(generation, HOME_PANEL_MIJIA_AUTHENTICATED,
                    "米家账号登录成功", NULL, home_name,
                    device_count, online_count);
      return;
    }

  mijia_publish(generation, HOME_PANEL_MIJIA_EXPIRED,
                "二维码已过期，请重新生成", NULL, NULL, 0, 0);
}

static void *mijia_worker(void *arg)
{
  uint32_t generation;

  (void)arg;

  for (;;)
    {
      pthread_mutex_lock(&g_mijia.lock);
      while (!g_mijia.request_pending)
        {
          pthread_cond_wait(&g_mijia.condition, &g_mijia.lock);
        }

      g_mijia.request_pending = false;
      generation = g_mijia.request_generation;
      pthread_mutex_unlock(&g_mijia.lock);
      mijia_process_login(generation);
    }

  return NULL;
}

int home_panel_mijia_initialize(void)
{
  pthread_attr_t attr;
  int ret;

  pthread_mutex_lock(&g_mijia.lock);
  if (g_mijia.initialized)
    {
      pthread_mutex_unlock(&g_mijia.lock);
      return 0;
    }

  g_mijia.snapshot.state = HOME_PANEL_MIJIA_IDLE;
  g_mijia.initialized = true;
  pthread_mutex_unlock(&g_mijia.lock);

  ret = pthread_attr_init(&attr);
  if (ret != 0)
    {
      return ret;
    }

  pthread_attr_setstacksize(&attr, MIJIA_CLIENT_THREAD_STACK);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  ret = pthread_create(&g_mijia.thread, &attr, mijia_worker, NULL);
  pthread_attr_destroy(&attr);
  if (ret != 0)
    {
      pthread_mutex_lock(&g_mijia.lock);
      g_mijia.initialized = false;
      pthread_mutex_unlock(&g_mijia.lock);
    }

  return ret;
}

int home_panel_mijia_request_login(void)
{
  pthread_mutex_lock(&g_mijia.lock);
  if (!g_mijia.initialized)
    {
      pthread_mutex_unlock(&g_mijia.lock);
      return -ENODEV;
    }

  g_mijia.request_generation++;
  g_mijia.request_pending = true;
  g_mijia.snapshot.state = HOME_PANEL_MIJIA_STARTING;
  g_mijia.snapshot.revision = ++g_mijia.next_revision;
  mijia_copy_string(g_mijia.snapshot.message,
                    sizeof(g_mijia.snapshot.message),
                    "正在连接米家服务");
  g_mijia.qr_size = 0;
  g_mijia.snapshot.qr_size = 0;
  g_mijia.snapshot.qr_revision = ++g_mijia.next_qr_revision;
  pthread_cond_signal(&g_mijia.condition);
  pthread_mutex_unlock(&g_mijia.lock);
  return 0;
}

void home_panel_mijia_get_snapshot(
  struct home_panel_mijia_snapshot_s *snapshot)
{
  pthread_mutex_lock(&g_mijia.lock);
  memcpy(snapshot, &g_mijia.snapshot, sizeof(*snapshot));
  pthread_mutex_unlock(&g_mijia.lock);
}

int home_panel_mijia_copy_qr(uint32_t revision, void *buffer,
                             size_t capacity, size_t *size)
{
  int ret = 0;

  if (buffer == NULL || size == NULL)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&g_mijia.lock);
  if (revision != g_mijia.snapshot.qr_revision ||
      g_mijia.qr_size == 0)
    {
      ret = -EAGAIN;
    }
  else if (capacity < g_mijia.qr_size)
    {
      ret = -ENOSPC;
    }
  else
    {
      memcpy(buffer, g_mijia_qr_data, g_mijia.qr_size);
      *size = g_mijia.qr_size;
    }
  pthread_mutex_unlock(&g_mijia.lock);
  return ret;
}

const char *home_panel_mijia_state_name(
  enum home_panel_mijia_state_e state)
{
  switch (state)
    {
      case HOME_PANEL_MIJIA_IDLE:
        return "idle";
      case HOME_PANEL_MIJIA_STARTING:
        return "starting";
      case HOME_PANEL_MIJIA_WAITING:
        return "waiting";
      case HOME_PANEL_MIJIA_AUTHENTICATED:
        return "authenticated";
      case HOME_PANEL_MIJIA_EXPIRED:
        return "expired";
      case HOME_PANEL_MIJIA_ERROR:
        return "error";
      default:
        return "unknown";
    }
}
