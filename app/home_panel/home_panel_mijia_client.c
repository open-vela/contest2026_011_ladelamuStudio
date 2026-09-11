/****************************************************************************
 * D13x home panel Mijia API client
 ****************************************************************************/

#include <nuttx/config.h>

#include <arch/board/board.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include <netutils/cJSON.h>
#include <netutils/webclient.h>

#include "home_panel_mijia_client.h"
#include "home_panel_mijia_model.h"
#include "home_panel_https.h"

#define MIJIA_HTTP_BUFFER_SIZE       1024
#define MIJIA_JSON_BUFFER_SIZE       2048
#define MIJIA_DELTA_RESPONSE_SIZE    16384
#define MIJIA_LOGIN_POLL_SECONDS     2
#define MIJIA_LOGIN_MAX_POLLS        90
#define MIJIA_CLIENT_THREAD_STACK    16384
#define MIJIA_COMMAND_THREAD_STACK   16384
#define MIJIA_AGENT_THREAD_STACK     12288
#define MIJIA_BOARD_AGENT_THREAD_STACK 16384
#define MIJIA_BOARD_AGENT_RESPONSE_SIZE 8192
#define MIJIA_BOARD_AGENT_MAX_PROPERTIES 32
#define MIJIA_AGENT_PENDING_SECONDS  20
#define MIJIA_BACKGROUND_PRIORITY    60
#define MIJIA_QR_WIRE_HEADER_SIZE    12
#define MIJIA_QR_WIDTH               240
#define MIJIA_QR_HEIGHT              240
#define MIJIA_QR_STRIDE              (MIJIA_QR_WIDTH * 2)
#define MIJIA_QR_DATA_SIZE           (MIJIA_QR_STRIDE * MIJIA_QR_HEIGHT)
#define MIJIA_QR_WIRE_SIZE           (MIJIA_QR_WIRE_HEADER_SIZE + \
                                      MIJIA_QR_DATA_SIZE)
#define MIJIA_CREDENTIAL_MAGIC        0x4d4a5331u /* MJS1 */
#define MIJIA_CREDENTIAL_VERSION      1u
#define MIJIA_RESTORE_RETRY_SECONDS   3

#ifndef CONFIG_D13X_HOME_PANEL_MIJIA_SYNC_INTERVAL
#  define CONFIG_D13X_HOME_PANEL_MIJIA_SYNC_INTERVAL 10
#endif

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
  bool restore_pending;
  uint32_t request_generation;
  uint32_t next_revision;
  uint32_t next_qr_revision;
  uint32_t next_family_revision;
  uint32_t next_command_revision;
  uint32_t next_agent_revision;
  uint32_t next_board_update_revision;
  size_t qr_size;
  bool command_busy;
  bool agent_busy;
  bool agent_learning_busy;
  bool board_agent_busy;
  unsigned int quiet_delta_count;
  uint32_t agent_learning_completed_revision;
  uint32_t agent_learning_completed_routine;
  char token[128];
  bool family_model_valid;
  struct home_panel_family_model_s family_model;
  struct home_panel_mijia_snapshot_s snapshot;
  struct home_panel_mijia_family_snapshot_s family_snapshot;
  struct home_panel_mijia_command_snapshot_s command_snapshot;
  struct home_panel_agent_snapshot_s agent_snapshot;
  struct home_panel_cloud_proposal_s board_proposal;
};

struct mijia_credentials_s
{
  uint32_t magic;
  uint32_t version;
  uint32_t endpoint_hash;
  char token[128];
  char vault_password[64];
};

static void mijia_configure_background_thread(pthread_attr_t *attr)
{
  struct sched_param param;

  memset(&param, 0, sizeof(param));
  param.sched_priority = MIJIA_BACKGROUND_PRIORITY;
  pthread_attr_setschedparam(attr, &param);
  pthread_attr_setinheritsched(attr, PTHREAD_EXPLICIT_SCHED);
}

enum mijia_command_kind_e
{
  MIJIA_COMMAND_PROPERTY = 0,
  MIJIA_COMMAND_SCENE,
  MIJIA_COMMAND_ACTION
};

struct mijia_command_request_s
{
  enum mijia_command_kind_e kind;
  uint32_t generation;
  bool value_is_boolean;
  bool target;
  int number_value;
  uint16_t siid;
  uint16_t piid;
  char token[128];
  char identifier[80];
  char property_name[48];
  char display_name[48];
};

struct mijia_agent_request_s
{
  uint32_t generation;
  struct home_panel_agent_request_s context;
  char token[128];
};

struct mijia_agent_learning_request_s
{
  uint32_t generation;
  struct home_panel_agent_learning_request_s context;
  char token[128];
};

enum mijia_board_agent_request_kind_e
{
  MIJIA_BOARD_AGENT_STATE = 0,
  MIJIA_BOARD_AGENT_POLL,
  MIJIA_BOARD_AGENT_CONFIRM,
  MIJIA_BOARD_AGENT_FEEDBACK
};

struct mijia_board_agent_request_s
{
  enum mijia_board_agent_request_kind_e kind;
  uint32_t generation;
  uint32_t board_revision;
  uint32_t proposal_after;
  uint64_t generated_at;
  unsigned int minute_of_day;
  char token[128];
  char proposal_id[65];
  char feedback[24];
  struct home_panel_family_model_s *model;
};

static struct mijia_client_s g_mijia =
{
  .lock = PTHREAD_MUTEX_INITIALIZER,
  .condition = PTHREAD_COND_INITIALIZER,
};

static void mijia_webclient_set_defaults(struct webclient_context *context)
{
  webclient_set_defaults(context);
  home_panel_https_configure(context);
}
static uint8_t g_mijia_qr_data[MIJIA_QR_DATA_SIZE];

static bool mijia_board_proposal_id_valid(const char *value);

static uint64_t mijia_monotonic_ms(void)
{
  struct timespec value;

  if (clock_gettime(CLOCK_MONOTONIC, &value) < 0)
    {
      return 0;
    }
  return (uint64_t)value.tv_sec * 1000u +
         (uint64_t)value.tv_nsec / 1000000u;
}

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

  mijia_webclient_set_defaults(&context);
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
             "[HOME][MIJIA] %s %s status=%u bytes=%u\n",
             method, path, context.http_status,
             (unsigned int)response.length);
      return -EPROTO;
    }

  return 0;
}

static int mijia_authorized_post(const char *token, const char *path,
                                 const char *body, char *response_data,
                                 size_t response_capacity,
                                 unsigned int *http_status)
{
  struct webclient_context context;
  struct mijia_response_s response;
  const char *headers[4];
  char authorization[192];
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

  snprintf(authorization, sizeof(authorization),
           "Authorization: Bearer %s", token);
  headers[0] = "Accept: application/json";
  headers[1] = "Content-Type: application/json";
  headers[2] = authorization;
  headers[3] = "Connection: close";

  memset(&response, 0, sizeof(response));
  response.data = response_data;
  response.capacity = response_capacity;
  response_data[0] = '\0';

  mijia_webclient_set_defaults(&context);
  context.method = "POST";
  context.url = url;
  context.buffer = buffer;
  context.buflen = sizeof(buffer);
  context.headers = headers;
  context.nheaders = 4;
  context.sink_callback = mijia_response_sink;
  context.sink_callback_arg = &response;
  context.timeout_sec = CONFIG_D13X_HOME_PANEL_MIJIA_TIMEOUT;
  webclient_set_static_body(&context, body, strlen(body));

  ret = webclient_perform(&context);
  *http_status = context.http_status;
  if (ret < 0)
    {
      return ret;
    }

  return context.http_status >= 200 && context.http_status < 300 ?
         0 : -EPROTO;
}

static int mijia_authorized_get(const char *token, const char *path,
                                char *response_data,
                                size_t response_capacity,
                                unsigned int *http_status)
{
  struct webclient_context context;
  struct mijia_response_s response;
  const char *headers[3];
  char authorization[192];
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
  snprintf(authorization, sizeof(authorization),
           "Authorization: Bearer %s", token);
  headers[0] = "Accept: application/json";
  headers[1] = authorization;
  headers[2] = "Connection: close";
  memset(&response, 0, sizeof(response));
  response.data = response_data;
  response.capacity = response_capacity;
  response_data[0] = '\0';
  mijia_webclient_set_defaults(&context);
  context.method = "GET";
  context.url = url;
  context.buffer = buffer;
  context.buflen = sizeof(buffer);
  context.headers = headers;
  context.nheaders = 3;
  context.sink_callback = mijia_response_sink;
  context.sink_callback_arg = &response;
  context.timeout_sec = CONFIG_D13X_HOME_PANEL_MIJIA_TIMEOUT;
  ret = webclient_perform(&context);
  *http_status = context.http_status;
  if (ret < 0)
    {
      return ret;
    }
  return context.http_status >= 200 && context.http_status < 300 ?
         0 : -EPROTO;
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

static uint32_t mijia_endpoint_hash(void)
{
  const uint8_t *value =
    (const uint8_t *)CONFIG_D13X_HOME_PANEL_MIJIA_SERVER_URL;
  uint32_t hash = 2166136261u;

  while (*value != '\0')
    {
      hash = (hash ^ *value++) * 16777619u;
    }

  return hash;
}

static int mijia_load_credentials(char *token, size_t token_capacity,
                                  char *password,
                                  size_t password_capacity)
{
  struct mijia_credentials_s credentials;
  size_t length = 0;
  int ret;

  memset(&credentials, 0, sizeof(credentials));
  ret = board_persist_read(&credentials, sizeof(credentials), &length);
  if (ret < 0)
    {
      return ret;
    }

  if (length != sizeof(credentials) ||
      credentials.magic != MIJIA_CREDENTIAL_MAGIC ||
      credentials.version != MIJIA_CREDENTIAL_VERSION ||
      credentials.endpoint_hash != mijia_endpoint_hash() ||
      memchr(credentials.token, '\0', sizeof(credentials.token)) == NULL ||
      memchr(credentials.vault_password, '\0',
             sizeof(credentials.vault_password)) == NULL ||
      credentials.token[0] == '\0' || credentials.vault_password[0] == '\0')
    {
      memset(&credentials, 0, sizeof(credentials));
      return -EBADMSG;
    }

  mijia_copy_string(token, token_capacity, credentials.token);
  mijia_copy_string(password, password_capacity,
                    credentials.vault_password);
  memset(&credentials, 0, sizeof(credentials));
  return 0;
}

static int mijia_save_credentials(const char *token, const char *password)
{
  struct mijia_credentials_s credentials;
  int ret;

  if (token == NULL || password == NULL || token[0] == '\0' ||
      password[0] == '\0' || strlen(token) >= sizeof(credentials.token) ||
      strlen(password) >= sizeof(credentials.vault_password))
    {
      return -EINVAL;
    }

  memset(&credentials, 0, sizeof(credentials));
  credentials.magic = MIJIA_CREDENTIAL_MAGIC;
  credentials.version = MIJIA_CREDENTIAL_VERSION;
  credentials.endpoint_hash = mijia_endpoint_hash();
  mijia_copy_string(credentials.token, sizeof(credentials.token), token);
  mijia_copy_string(credentials.vault_password,
                    sizeof(credentials.vault_password), password);
  ret = board_persist_write(&credentials, sizeof(credentials));
  memset(&credentials, 0, sizeof(credentials));
  return ret;
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
  const char *next_message = message != NULL ? message : "";
  const char *next_home_name = home_name != NULL ? home_name : "";

  pthread_mutex_lock(&g_mijia.lock);
  if (generation == g_mijia.request_generation)
    {
      bool changed = g_mijia.snapshot.state != state ||
                     g_mijia.snapshot.device_count != device_count ||
                     g_mijia.snapshot.online_count != online_count ||
                     strcmp(g_mijia.snapshot.message, next_message) != 0 ||
                     strcmp(g_mijia.snapshot.home_name,
                            next_home_name) != 0;

      g_mijia.snapshot.state = state;
      g_mijia.snapshot.device_count = device_count;
      g_mijia.snapshot.online_count = online_count;
      mijia_copy_string(g_mijia.snapshot.message,
                        sizeof(g_mijia.snapshot.message), next_message);
      mijia_copy_string(g_mijia.snapshot.home_name,
                        sizeof(g_mijia.snapshot.home_name), next_home_name);
      if (changed)
        {
          g_mijia.snapshot.revision = ++g_mijia.next_revision;
        }
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
  mijia_webclient_set_defaults(&context);
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
  mijia_webclient_set_defaults(&context);
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

static int mijia_unlock_session(const char *token, const char *password,
                                unsigned int *http_status)
{
  char response[MIJIA_JSON_BUFFER_SIZE];
  cJSON *payload;
  char *body = NULL;
  int ret = -ENOMEM;

  payload = cJSON_CreateObject();
  if (payload == NULL ||
      !cJSON_AddStringToObject(payload, "password", password))
    {
      goto out;
    }

  body = cJSON_PrintUnformatted(payload);
  if (body == NULL)
    {
      goto out;
    }

  ret = mijia_authorized_post(token, "/api/session/unlock", body,
                              response, sizeof(response), http_status);

out:
  cJSON_free(body);
  cJSON_Delete(payload);
  return ret;
}

static int mijia_reunlock_persisted_session(const char *active_token)
{
  char stored_token[128];
  char password[64];
  unsigned int http_status = 0;
  int ret;

  ret = mijia_load_credentials(stored_token, sizeof(stored_token),
                               password, sizeof(password));
  if (ret < 0)
    {
      goto out;
    }

  if (strcmp(stored_token, active_token) != 0)
    {
      ret = -EACCES;
      goto out;
    }

  ret = mijia_unlock_session(active_token, password, &http_status);
  if (ret == 0)
    {
      syslog(LOG_INFO,
             "[HOME][MIJIA] server vault unlocked after restart\n");
    }
  else
    {
      syslog(LOG_WARNING,
             "[HOME][MIJIA] server re-unlock failed ret=%d status=%u\n",
             ret, http_status);
    }

out:
  memset(stored_token, 0, sizeof(stored_token));
  memset(password, 0, sizeof(password));
  return ret;
}

static bool mijia_json_uint(cJSON *object, const char *name,
                            unsigned int *value)
{
  cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);

  if (!cJSON_IsNumber(item) || item->valueint < 0)
    {
      return false;
    }

  *value = (unsigned int)item->valueint;
  return true;
}

static void mijia_mark_sync_failure(uint32_t generation, int error)
{
  uint32_t revision = 0;
  unsigned int failures = 0;

  pthread_mutex_lock(&g_mijia.lock);
  if (generation == g_mijia.request_generation)
    {
      g_mijia.family_snapshot.stale = true;
      g_mijia.family_snapshot.consecutive_failures++;
      revision = g_mijia.family_snapshot.revision;
      failures = g_mijia.family_snapshot.consecutive_failures;
    }
  pthread_mutex_unlock(&g_mijia.lock);

  syslog(LOG_WARNING,
         "[HOME][SYNC] refresh failed error=%d retained=%u failures=%u\n",
         error, (unsigned int)revision, failures);
}

static int mijia_fetch_family(const char *token, uint32_t generation,
                              char *home_name, size_t home_capacity,
                              unsigned int *device_count,
                              unsigned int *online_count)
{
  char authorization[192];
  const char *headers[2];
  struct webclient_context context;
  struct mijia_response_s sink;
  cJSON *devices;
  cJSON *homes;
  cJSON *scenes;
  cJSON *summary;
  cJSON *root = NULL;
  char *response;
  char url[512];
  char buffer[MIJIA_HTTP_BUFFER_SIZE];
  const char *primary_home;
  unsigned int detail_error_count;
  unsigned int generated_at;
  unsigned int home_count;
  unsigned int room_count;
  unsigned int scene_count;
  unsigned int server_revision;
  struct home_panel_family_model_s *parsed_model = NULL;
  int ret = -ENOMEM;

  /* The model is ~118 KiB.  Both full sync and delta merge run inside the
   * single mijia_worker thread, so a reused scratch buffer avoids a repeated
   * large malloc/free cycle on every long poll change and keeps the heap
   * free of fragmentation.
   */

  static struct home_panel_family_model_s g_sync_model;

  response = malloc(CONFIG_D13X_HOME_PANEL_MIJIA_MAX_RESPONSE);
  if (response == NULL)
    {
      return -ENOMEM;
    }

  memset(&context, 0, sizeof(context));
  memset(&sink, 0, sizeof(sink));

  snprintf(url, sizeof(url), "%s/api/sync",
           CONFIG_D13X_HOME_PANEL_MIJIA_SERVER_URL);
  snprintf(authorization, sizeof(authorization),
           "Authorization: Bearer %s", token);
  headers[0] = authorization;
  headers[1] = "Connection: close";
  sink.data = response;
  sink.capacity = CONFIG_D13X_HOME_PANEL_MIJIA_MAX_RESPONSE;
  response[0] = '\0';

  mijia_webclient_set_defaults(&context);
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
      syslog(LOG_WARNING,
             "[HOME][SYNC] webclient ret=%d errno=%d status=%u\n",
             ret, errno, context.http_status);
      ret = ret < 0 ? ret :
            context.http_status == 423 ? -EACCES : -EPROTO;
      goto out;
    }

  root = cJSON_Parse(response);
  summary = root == NULL ? NULL :
            cJSON_GetObjectItemCaseSensitive(root, "summary");
  homes = root == NULL ? NULL :
          cJSON_GetObjectItemCaseSensitive(root, "homes");
  devices = root == NULL ? NULL :
            cJSON_GetObjectItemCaseSensitive(root, "devices");
  scenes = root == NULL ? NULL :
           cJSON_GetObjectItemCaseSensitive(root, "scenes");
  if (!cJSON_IsObject(summary) || !cJSON_IsArray(homes) ||
      !cJSON_IsArray(devices) || !cJSON_IsArray(scenes) ||
      !mijia_json_uint(summary, "home_count", &home_count) ||
      !mijia_json_uint(summary, "room_count", &room_count) ||
      !mijia_json_uint(summary, "device_count", device_count) ||
      !mijia_json_uint(summary, "online_count", online_count) ||
      !mijia_json_uint(summary, "scene_count", &scene_count) ||
      !mijia_json_uint(summary, "detail_error_count",
                       &detail_error_count) ||
      !mijia_json_uint(root, "generated_at", &generated_at) ||
      !mijia_json_uint(root, "sync_revision", &server_revision) ||
      *device_count != (unsigned int)cJSON_GetArraySize(devices) ||
      home_count != (unsigned int)cJSON_GetArraySize(homes) ||
      scene_count != (unsigned int)cJSON_GetArraySize(scenes) ||
      *online_count > *device_count)
    {
      ret = -EBADMSG;
      goto out;
    }

  primary_home = mijia_json_string(summary, "primary_home_name");
  mijia_copy_string(home_name, home_capacity, primary_home);

  /* Parsing the full family response is intentionally done in this
   * low-priority worker.  The LVGL thread only copies the compact model,
   * so a cloud update cannot stall input while cJSON walks the payload.
   */

  parsed_model = &g_sync_model;
  memset(parsed_model, 0, sizeof(*parsed_model));

  ret = home_panel_mijia_model_parse(response, 0, parsed_model);
  if (ret < 0)
    {
      goto out;
    }

  pthread_mutex_lock(&g_mijia.lock);
  if (generation != g_mijia.request_generation)
    {
      pthread_mutex_unlock(&g_mijia.lock);
      ret = -ECANCELED;
      goto out;
    }

  g_mijia.family_snapshot.revision = ++g_mijia.next_family_revision;
  parsed_model->revision = g_mijia.family_snapshot.revision;
  memcpy(&g_mijia.family_model, parsed_model, sizeof(*parsed_model));
  g_mijia.family_model_valid = true;
  g_mijia.family_snapshot.server_revision = server_revision;
  g_mijia.family_snapshot.generated_at = generated_at;
  g_mijia.family_snapshot.json_size = sink.length;
  g_mijia.family_snapshot.home_count = home_count;
  g_mijia.family_snapshot.room_count = parsed_model->room_count;
  g_mijia.family_snapshot.device_count = parsed_model->device_count;
  g_mijia.family_snapshot.online_count = parsed_model->online_count;
  g_mijia.family_snapshot.scene_count = parsed_model->scene_count;
  g_mijia.family_snapshot.detail_error_count = detail_error_count;
  g_mijia.family_snapshot.consecutive_failures = 0;
  g_mijia.family_snapshot.stale = false;
  syslog(LOG_INFO,
         "[HOME][SYNC] revision=%u server=%u bytes=%u homes=%u rooms=%u "
         "devices=%u online=%u scenes=%u detail_errors=%u\n",
         (unsigned int)g_mijia.family_snapshot.revision,
         server_revision, (unsigned int)sink.length, home_count, room_count,
         parsed_model->device_count, parsed_model->online_count,
         parsed_model->scene_count, detail_error_count);
  pthread_mutex_unlock(&g_mijia.lock);
  ret = 0;

out:
  if (ret < 0)
    {
      syslog(LOG_WARNING,
             "[HOME][SYNC] request failed ret=%d status=%u bytes=%u\n",
             ret, context.http_status, (unsigned int)sink.length);
    }

  cJSON_Delete(root);
  free(response);
  return ret;
}

static int mijia_wait_for_changes(const char *token, uint32_t generation,
                                  uint32_t after, bool *changed,
                                  bool *full_resync)
{
  char authorization[192];
  const char *headers[2];
  struct webclient_context context;
  struct mijia_response_s sink;
  cJSON *changes;
  cJSON *item;
  cJSON *params;
  cJSON *root = NULL;
  char *response;
  char url[512];
  char buffer[MIJIA_HTTP_BUFFER_SIZE];
  const char *method;
  const char *did;
  unsigned int base_revision;
  unsigned int generated_at;
  unsigned int online_changes = 0;
  unsigned int property_changes = 0;
  unsigned int revision;
  unsigned int working_device_count;
  unsigned int working_online_count;
  uint32_t working_model_revision;
  int online_count_delta = 0;
  bool model_changed = false;
  bool resync_required;
  bool server_stale;
  struct home_panel_family_model_s *working_model = NULL;
  int length;
  int ret = -ENOMEM;

  /* Reused by the worker thread for the delta merge; the scratch model is
   * 118 KiB and would otherwise be malloc'd and freed on every poll change.
   */

  static struct home_panel_family_model_s g_sync_working_model;

  *changed = false;
  *full_resync = false;
  response = malloc(MIJIA_DELTA_RESPONSE_SIZE);
  if (response == NULL)
    {
      return -ENOMEM;
    }

  memset(&context, 0, sizeof(context));
  memset(&sink, 0, sizeof(sink));
  length = snprintf(url, sizeof(url),
                    "%s/api/sync/changes?after=%u&timeout=8",
                    CONFIG_D13X_HOME_PANEL_MIJIA_SERVER_URL,
                    (unsigned int)after);
  if (length < 0 || (size_t)length >= sizeof(url))
    {
      ret = -ENAMETOOLONG;
      goto out;
    }

  snprintf(authorization, sizeof(authorization),
           "Authorization: Bearer %s", token);
  headers[0] = authorization;
  headers[1] = "Connection: close";
  sink.data = response;
  sink.capacity = MIJIA_DELTA_RESPONSE_SIZE;
  response[0] = '\0';

  mijia_webclient_set_defaults(&context);
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
      ret = ret < 0 ? ret :
            context.http_status == 423 ? -EACCES : -EPROTO;
      goto out;
    }

  if (!mijia_generation_active(generation))
    {
      ret = -ECANCELED;
      goto out;
    }

  root = cJSON_Parse(response);
  changes = root == NULL ? NULL :
            cJSON_GetObjectItemCaseSensitive(root, "changes");
  if (!cJSON_IsArray(changes) ||
      !mijia_json_uint(root, "base_revision", &base_revision) ||
      !mijia_json_uint(root, "revision", &revision) ||
      !mijia_json_uint(root, "generated_at", &generated_at))
    {
      ret = -EBADMSG;
      goto out;
    }

  item = cJSON_GetObjectItemCaseSensitive(root, "resync_required");
  if (!cJSON_IsBool(item))
    {
      ret = -EBADMSG;
      goto out;
    }
  resync_required = cJSON_IsTrue(item);
  item = cJSON_GetObjectItemCaseSensitive(root, "stale");
  if (!cJSON_IsBool(item))
    {
      ret = -EBADMSG;
      goto out;
    }
  server_stale = cJSON_IsTrue(item);

  if (resync_required)
    {
      *changed = true;
      *full_resync = true;
      syslog(LOG_INFO,
             "[HOME][SYNC] delta gap local=%u server=%u; full resync\n",
             (unsigned int)after, revision);
      ret = 0;
      goto out;
    }

  if (revision < after || base_revision != after)
    {
      /* The server reset its revision counter (service restart) or the local
       * base no longer matches.  A permanent error would stall family
       * synchronization forever because the worker retries with the same
       * base.  Fall back to a full resync instead.
       */

      *changed = true;
      *full_resync = true;
      syslog(LOG_INFO,
             "[HOME][SYNC] delta base mismatch local=%u server=%u; "
             "full resync\n",
             (unsigned int)after, (unsigned int)revision);
      ret = 0;
      goto out;
    }

  cJSON_ArrayForEach(item, changes)
    {
      method = mijia_json_string(item, "method");
      params = cJSON_GetObjectItemCaseSensitive(item, "params");
      if (method == NULL || !cJSON_IsArray(params))
        {
          ret = -EBADMSG;
          goto out;
        }
      if (strcmp(method, "properties_changed") == 0)
        {
          property_changes += cJSON_GetArraySize(params);
        }
      else if (strcmp(method, "device_online_changed") == 0)
        {
          online_changes += cJSON_GetArraySize(params);
        }
      else
        {
          *changed = true;
          *full_resync = true;
          ret = 0;
          goto out;
        }
    }

  if (server_stale && cJSON_GetArraySize(changes) == 0)
    {
      ret = -EAGAIN;
      goto out;
    }

  if (cJSON_GetArraySize(changes) == 0)
    {
      pthread_mutex_lock(&g_mijia.lock);
      if (generation != g_mijia.request_generation ||
          g_mijia.family_snapshot.server_revision != after)
        {
          pthread_mutex_unlock(&g_mijia.lock);
          ret = -ECANCELED;
          goto out;
        }

      g_mijia.family_snapshot.server_revision = revision;
      g_mijia.family_snapshot.generated_at = generated_at;
      g_mijia.family_snapshot.json_size = sink.length;
      g_mijia.family_snapshot.consecutive_failures = 0;
      g_mijia.family_snapshot.stale = false;
      pthread_mutex_unlock(&g_mijia.lock);
      ret = 0;
      goto out;
    }

  working_model = &g_sync_working_model;
  memset(working_model, 0, sizeof(*working_model));

  pthread_mutex_lock(&g_mijia.lock);
  if (generation != g_mijia.request_generation ||
      !g_mijia.family_model_valid)
    {
      pthread_mutex_unlock(&g_mijia.lock);
      ret = -ECANCELED;
      goto out;
    }

  if (g_mijia.family_snapshot.server_revision != after)
    {
      pthread_mutex_unlock(&g_mijia.lock);
      *changed = true;
      *full_resync = true;
      ret = 0;
      goto out;
    }

  memcpy(working_model, &g_mijia.family_model, sizeof(*working_model));
  working_model_revision = g_mijia.family_model.revision;
  working_device_count = g_mijia.family_snapshot.device_count;
  working_online_count = g_mijia.family_snapshot.online_count;
  pthread_mutex_unlock(&g_mijia.lock);

  cJSON_ArrayForEach(item, changes)
    {
      method = mijia_json_string(item, "method");
      params = cJSON_GetObjectItemCaseSensitive(item, "params");
      if (strcmp(method, "device_online_changed") == 0)
        {
          cJSON *param;

          cJSON_ArrayForEach(param, params)
            {
              cJSON *previous = cJSON_GetObjectItemCaseSensitive(
                param, "previous_value");
              cJSON *value = cJSON_GetObjectItemCaseSensitive(param,
                                                               "value");
              int apply_ret;

              did = mijia_json_string(param, "did");
              if (did == NULL || !cJSON_IsBool(previous) ||
                  !cJSON_IsBool(value))
                {
                  ret = -EBADMSG;
                  goto out;
                }

              apply_ret = home_panel_mijia_model_apply_online(
                working_model, did, cJSON_IsTrue(value));
              if (apply_ret == -ENOENT)
                {
                  continue;
                }
              if (apply_ret < 0)
                {
                  ret = apply_ret;
                  goto out;
                }

              if (apply_ret > 0)
                {
                  if (cJSON_IsTrue(previous) != cJSON_IsTrue(value))
                    {
                      online_count_delta += cJSON_IsTrue(value) ? 1 : -1;
                    }
                  model_changed = true;
                }
            }
        }
      else
        {
          cJSON *param;

          cJSON_ArrayForEach(param, params)
            {
              cJSON *code_item;
              cJSON *value;
              unsigned int siid;
              unsigned int piid;
              int apply_ret;

              did = mijia_json_string(param, "did");
              code_item = cJSON_GetObjectItemCaseSensitive(param, "code");
              value = cJSON_GetObjectItemCaseSensitive(param, "value");
              if (did == NULL || !mijia_json_uint(param, "siid", &siid) ||
                  !mijia_json_uint(param, "piid", &piid) ||
                  siid == 0 || siid > UINT16_MAX ||
                  piid == 0 || piid > UINT16_MAX ||
                  !cJSON_IsNumber(code_item))
                {
                  ret = -EBADMSG;
                  goto out;
                }

              if (code_item->valueint != 0)
                {
                  continue;
                }

              apply_ret = home_panel_mijia_model_apply_property(
                working_model, did, (uint16_t)siid,
                (uint16_t)piid, cJSON_IsBool(value), cJSON_IsTrue(value),
                cJSON_IsNumber(value),
                cJSON_IsNumber(value) ? value->valueint : 0);
              if (apply_ret == -ENOENT)
                {
                  continue;
                }
              if (apply_ret < 0)
                {
                  ret = apply_ret;
                  goto out;
                }

              model_changed |= apply_ret > 0;
            }
        }
    }

  if (online_count_delta > 0)
    {
      unsigned int delta = (unsigned int)online_count_delta;

      if (working_online_count <= working_device_count &&
          delta <= working_device_count - working_online_count)
        {
          working_online_count += delta;
        }
    }
  else if (online_count_delta < 0)
    {
      unsigned int delta = (unsigned int)(-online_count_delta);

      if (delta <= working_online_count)
        {
          working_online_count -= delta;
        }
    }
  if (model_changed)
    {
      home_panel_mijia_model_refresh_rooms(working_model);
    }

  pthread_mutex_lock(&g_mijia.lock);
  if (generation != g_mijia.request_generation ||
      !g_mijia.family_model_valid ||
      g_mijia.family_snapshot.server_revision != after ||
      g_mijia.family_model.revision != working_model_revision)
    {
      pthread_mutex_unlock(&g_mijia.lock);
      *changed = true;
      *full_resync = true;
      ret = 0;
      goto out;
    }

  g_mijia.family_snapshot.server_revision = revision;
  g_mijia.family_snapshot.generated_at = generated_at;
  g_mijia.family_snapshot.json_size = sink.length;
  g_mijia.family_snapshot.consecutive_failures = 0;
  g_mijia.family_snapshot.stale = false;
  g_mijia.family_snapshot.online_count = working_online_count;
  if (model_changed)
    {
      g_mijia.family_snapshot.revision = ++g_mijia.next_family_revision;
      working_model->revision = g_mijia.family_snapshot.revision;
      memcpy(&g_mijia.family_model, working_model,
             sizeof(g_mijia.family_model));
    }
  pthread_mutex_unlock(&g_mijia.lock);

  *changed = model_changed;
  if (model_changed)
    {
      g_mijia.quiet_delta_count = 0;
      syslog(LOG_INFO,
             "[HOME][SYNC] delta server=%u bytes=%u methods=%u "
             "properties=%u online=%u\n",
             revision, (unsigned int)sink.length,
             (unsigned int)cJSON_GetArraySize(changes),
             property_changes, online_changes);
    }
  else
    {
      g_mijia.quiet_delta_count++;
      if (g_mijia.quiet_delta_count % 30 == 0)
        {
          syslog(LOG_INFO,
                 "[HOME][SYNC] quiet deltas=%u server=%u bytes=%u\n",
                 g_mijia.quiet_delta_count, revision,
                 (unsigned int)sink.length);
        }
    }
  ret = 0;

out:
  if (ret < 0 && ret != -ECANCELED)
    {
      syslog(LOG_WARNING,
             "[HOME][SYNC] changes failed ret=%d status=%u bytes=%u after=%u\n",
             ret, context.http_status, (unsigned int)sink.length,
             (unsigned int)after);
    }
  cJSON_Delete(root);
  free(response);
  return ret;
}

static bool mijia_process_login(uint32_t generation, char *token,
                                size_t token_capacity)
{
  char session_id[40];
  char claim_secret[64];
  char login_url[384];
  char state[24];
  char message[96];
  char home_name[64];
  unsigned int device_count;
  unsigned int online_count;
  unsigned int poll;
  int ret;

  token[0] = '\0';
  mijia_publish(generation, HOME_PANEL_MIJIA_STARTING,
                "正在连接米家服务", NULL, NULL, 0, 0);
  ret = mijia_start_login(session_id, sizeof(session_id),
                          claim_secret, sizeof(claim_secret),
                          login_url, sizeof(login_url));
  if (ret < 0)
    {
      mijia_publish(generation, HOME_PANEL_MIJIA_ERROR,
                    "无法连接米家服务", NULL, NULL, 0, 0);
      return false;
    }

  syslog(LOG_INFO, "[HOME][MIJIA] login session started\n");
  ret = mijia_download_login_qr(session_id, generation);
  if (ret < 0)
    {
      mijia_publish(generation, HOME_PANEL_MIJIA_ERROR,
                    "登录二维码下载失败", NULL, NULL, 0, 0);
      return false;
    }

  mijia_publish(generation, HOME_PANEL_MIJIA_WAITING,
                "请使用米家 App 扫描并确认", NULL, NULL, 0, 0);

  for (poll = 0; poll < MIJIA_LOGIN_MAX_POLLS; poll++)
    {
      if (!mijia_generation_active(generation))
        {
          return false;
        }

      sleep(MIJIA_LOGIN_POLL_SECONDS);
      ret = mijia_get_login_status(session_id, state, sizeof(state),
                                   message, sizeof(message));
      if (ret < 0)
        {
          mijia_publish(generation, HOME_PANEL_MIJIA_ERROR,
                        "登录状态查询失败", NULL, NULL, 0, 0);
          return false;
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
          return false;
        }

      ret = mijia_claim_login(session_id, claim_secret, token,
                              token_capacity);
      if (ret < 0)
        {
          mijia_publish(generation, HOME_PANEL_MIJIA_ERROR,
                        "登录凭据领取失败", NULL, NULL, 0, 0);
          return false;
        }

      ret = mijia_set_vault_password(token, claim_secret);
      if (ret < 0)
        {
          mijia_publish(generation, HOME_PANEL_MIJIA_ERROR,
                        "账号保险箱初始化失败", NULL, NULL, 0, 0);
          return false;
        }

      ret = mijia_fetch_family(token, generation, home_name,
                               sizeof(home_name), &device_count,
                               &online_count);
      if (ret < 0)
        {
          mijia_mark_sync_failure(generation, ret);
          mijia_publish(generation, HOME_PANEL_MIJIA_ERROR,
                        "已登录，但设备同步失败", NULL, NULL, 0, 0);
          return false;
        }

      ret = mijia_save_credentials(token, claim_secret);
      if (ret < 0)
        {
          syslog(LOG_ERR,
                 "[HOME][MIJIA] credential persistence failed ret=%d\n",
                 ret);
          mijia_publish(generation, HOME_PANEL_MIJIA_ERROR,
                        "已登录，但登录状态保存失败", NULL,
                        home_name, device_count, online_count);
          memset(claim_secret, 0, sizeof(claim_secret));
          memset(token, 0, token_capacity);
          return false;
        }

      syslog(LOG_INFO, "[HOME][MIJIA] credentials persisted\n");

      syslog(LOG_INFO,
             "[HOME][MIJIA] authenticated devices=%u online=%u\n",
             device_count, online_count);
      pthread_mutex_lock(&g_mijia.lock);
      if (generation == g_mijia.request_generation)
        {
          mijia_copy_string(g_mijia.token, sizeof(g_mijia.token), token);
        }
      pthread_mutex_unlock(&g_mijia.lock);
      mijia_publish(generation, HOME_PANEL_MIJIA_AUTHENTICATED,
                    "米家账号登录成功", NULL, home_name,
                    device_count, online_count);
      memset(claim_secret, 0, sizeof(claim_secret));
      return true;
    }

  mijia_publish(generation, HOME_PANEL_MIJIA_EXPIRED,
                "二维码已过期，请重新生成", NULL, NULL, 0, 0);
  return false;
}

static int mijia_restore_session(uint32_t generation, char *token,
                                 size_t token_capacity)
{
  char password[64];
  char home_name[64];
  unsigned int device_count;
  unsigned int online_count;
  unsigned int http_status = 0;
  int ret;

  token[0] = '\0';
  password[0] = '\0';
  ret = mijia_load_credentials(token, token_capacity, password,
                               sizeof(password));
  if (ret < 0)
    {
      goto out;
    }

  mijia_publish(generation, HOME_PANEL_MIJIA_STARTING,
                "正在恢复米家登录", NULL, NULL, 0, 0);
  ret = mijia_unlock_session(token, password, &http_status);
  if (ret < 0)
    {
      if (http_status == 401 || http_status == 403)
        {
          ret = -EACCES;
        }
      goto out;
    }

  ret = mijia_fetch_family(token, generation, home_name,
                           sizeof(home_name), &device_count,
                           &online_count);
  if (ret < 0)
    {
      goto out;
    }

  pthread_mutex_lock(&g_mijia.lock);
  if (generation == g_mijia.request_generation)
    {
      mijia_copy_string(g_mijia.token, sizeof(g_mijia.token), token);
    }
  else
    {
      ret = -ECANCELED;
    }
  pthread_mutex_unlock(&g_mijia.lock);
  if (ret == 0)
    {
      syslog(LOG_INFO,
             "[HOME][MIJIA] persisted session restored devices=%u "
             "online=%u\n", device_count, online_count);
      mijia_publish(generation, HOME_PANEL_MIJIA_AUTHENTICATED,
                    "米家账号已自动登录", NULL, home_name,
                    device_count, online_count);
    }

out:
  memset(password, 0, sizeof(password));
  if (ret < 0)
    {
      memset(token, 0, token_capacity);
    }
  return ret;
}

static bool mijia_wait_for_refresh(uint32_t generation)
{
  struct timespec deadline;
  bool refresh;
  int ret;

  if (clock_gettime(CLOCK_REALTIME, &deadline) < 0)
    {
      sleep(CONFIG_D13X_HOME_PANEL_MIJIA_SYNC_INTERVAL);
      return mijia_generation_active(generation);
    }

  deadline.tv_sec += CONFIG_D13X_HOME_PANEL_MIJIA_SYNC_INTERVAL;
  pthread_mutex_lock(&g_mijia.lock);
  while (generation == g_mijia.request_generation &&
         !g_mijia.request_pending)
    {
      ret = pthread_cond_timedwait(&g_mijia.condition, &g_mijia.lock,
                                   &deadline);
      if (ret == ETIMEDOUT)
        {
          break;
        }
    }

  refresh = generation == g_mijia.request_generation &&
            !g_mijia.request_pending;
  pthread_mutex_unlock(&g_mijia.lock);
  return refresh;
}

static void mijia_refresh_summary(uint32_t generation,
                                  const char *home_name,
                                  unsigned int device_count,
                                  unsigned int online_count)
{
  bool changed;

  pthread_mutex_lock(&g_mijia.lock);
  if (generation == g_mijia.request_generation)
    {
      changed = g_mijia.snapshot.state != HOME_PANEL_MIJIA_AUTHENTICATED ||
                g_mijia.snapshot.device_count != device_count ||
                g_mijia.snapshot.online_count != online_count ||
                strcmp(g_mijia.snapshot.home_name, home_name) != 0;
      g_mijia.snapshot.state = HOME_PANEL_MIJIA_AUTHENTICATED;
      g_mijia.snapshot.device_count = device_count;
      g_mijia.snapshot.online_count = online_count;
      mijia_copy_string(g_mijia.snapshot.home_name,
                        sizeof(g_mijia.snapshot.home_name), home_name);
      if (changed)
        {
          g_mijia.snapshot.revision = ++g_mijia.next_revision;
        }
    }
  pthread_mutex_unlock(&g_mijia.lock);
}

static void mijia_publish_command(
  const struct mijia_command_request_s *request,
  enum home_panel_mijia_command_state_e state, int code,
  const char *message)
{
  pthread_mutex_lock(&g_mijia.lock);

  /* A worker started under an old login generation must still release the
   * busy flag; otherwise every later device command is rejected with -EBUSY
   * until the next login.  The snapshot fields are only updated for the
   * current generation so stale results do not overwrite a newer login.
   */

  if (state != HOME_PANEL_MIJIA_COMMAND_PENDING)
    {
      g_mijia.command_busy = false;
    }

  if (request->generation == g_mijia.request_generation)
    {
      g_mijia.command_snapshot.state = state;
      g_mijia.command_snapshot.revision = ++g_mijia.next_command_revision;
      g_mijia.command_snapshot.code = code;
      g_mijia.command_snapshot.target = request->target;
      mijia_copy_string(g_mijia.command_snapshot.device_name,
                        sizeof(g_mijia.command_snapshot.device_name),
                        request->display_name);
      mijia_copy_string(g_mijia.command_snapshot.message,
                        sizeof(g_mijia.command_snapshot.message), message);
    }
  pthread_mutex_unlock(&g_mijia.lock);
}

static void *mijia_command_worker(void *arg)
{
  struct mijia_command_request_s *request = arg;
  char response[MIJIA_JSON_BUFFER_SIZE];
  unsigned int http_status = 0;
  cJSON *accepted_item;
  cJSON *confirmed_item;
  cJSON *payload = NULL;
  cJSON *root = NULL;
  char *body = NULL;
  const char *path;
  bool accepted = false;
  bool confirmed = false;
  int code = -1;
  int ret = -ENOMEM;

  response[0] = '\0';
  payload = cJSON_CreateObject();
  if (payload == NULL)
    {
      goto out;
    }

  if (request->kind == MIJIA_COMMAND_PROPERTY)
    {
      cJSON_AddStringToObject(payload, "did", request->identifier);
      cJSON_AddStringToObject(payload, "prop_name",
                             request->property_name);
      cJSON_AddNumberToObject(payload, "siid", request->siid);
      cJSON_AddNumberToObject(payload, "piid", request->piid);
      if (request->value_is_boolean)
        {
          cJSON_AddBoolToObject(payload, "value", request->target);
        }
      else
        {
          cJSON_AddNumberToObject(payload, "value", request->number_value);
        }
      path = "/api/device/property";
    }
  else if (request->kind == MIJIA_COMMAND_ACTION)
    {
      cJSON *arguments;

      cJSON_AddStringToObject(payload, "did", request->identifier);
      cJSON_AddStringToObject(payload, "action_name",
                             request->property_name);
      cJSON_AddNumberToObject(payload, "siid", request->siid);
      cJSON_AddNumberToObject(payload, "aiid", request->piid);
      arguments = cJSON_AddArrayToObject(payload, "arguments");
      if (arguments == NULL)
        {
          goto out;
        }
      path = "/api/device/action";
    }
  else
    {
      cJSON_AddStringToObject(payload, "scene_id", request->identifier);
      path = "/api/scenes/run";
    }

  body = cJSON_PrintUnformatted(payload);
  if (body == NULL)
    {
      goto out;
    }

  ret = mijia_authorized_post(request->token, path, body, response,
                              sizeof(response), &http_status);
  if (ret < 0)
    {
      goto out;
    }

  root = cJSON_Parse(response);
  accepted_item = root == NULL ? NULL :
                  cJSON_GetObjectItemCaseSensitive(root, "accepted");
  if (!cJSON_IsBool(accepted_item))
    {
      ret = -EBADMSG;
      goto out;
    }

  accepted = cJSON_IsTrue(accepted_item);
  confirmed_item = cJSON_GetObjectItemCaseSensitive(root, "confirmed");
  confirmed = cJSON_IsTrue(confirmed_item);
  if (cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(root, "code")))
    {
      code = cJSON_GetObjectItemCaseSensitive(root, "code")->valueint;
    }
  else if (request->kind == MIJIA_COMMAND_SCENE)
    {
      code = accepted ? 0 : -1;
      confirmed = accepted;
    }

  if (!accepted)
    {
      ret = -EIO;
      goto out;
    }

  mijia_publish_command(request,
                        confirmed ? HOME_PANEL_MIJIA_COMMAND_CONFIRMED :
                                    HOME_PANEL_MIJIA_COMMAND_ACCEPTED,
                        code,
                        confirmed ? "设备状态已确认" :
                                    "指令已接受，等待状态确认");

  ret = 0;

out:
  if (ret < 0)
    {
      syslog(LOG_WARNING,
             "[HOME][MIJIA] command failed ret=%d status=%u kind=%u "
             "bytes=%u\n",
             ret, http_status, (unsigned int)request->kind,
             (unsigned int)strlen(response));
      mijia_publish_command(request, HOME_PANEL_MIJIA_COMMAND_ERROR,
                            code, "设备操作失败");
    }
  cJSON_free(body);
  cJSON_Delete(root);
  cJSON_Delete(payload);
  memset(request->token, 0, sizeof(request->token));
  free(request);
  return NULL;
}

static void mijia_publish_agent(
  const struct mijia_agent_request_s *request,
  enum home_panel_agent_state_e state,
  enum home_panel_agent_decision_e decision,
  enum home_panel_agent_policy_e policy,
  unsigned int adjusted_confidence,
  unsigned int valid_for_seconds,
  bool requires_confirmation,
  const char *summary,
  const char *reasons,
  const char *caution)
{
  pthread_mutex_lock(&g_mijia.lock);

  /* Release the busy flag regardless of generation so a worker started
   * before a login re-generation cannot leave the agent permanently busy.
   */

  if (state != HOME_PANEL_AGENT_PENDING)
    {
      g_mijia.agent_busy = false;
    }

  if (request->generation == g_mijia.request_generation)
    {
      g_mijia.agent_snapshot.state = state;
      g_mijia.agent_snapshot.decision = decision;
      g_mijia.agent_snapshot.policy = policy;
      g_mijia.agent_snapshot.revision = ++g_mijia.next_agent_revision;
      g_mijia.agent_snapshot.context_revision =
        request->context.context_revision;
      g_mijia.agent_snapshot.adjusted_confidence = adjusted_confidence;
      g_mijia.agent_snapshot.valid_for_seconds = valid_for_seconds;
      g_mijia.agent_snapshot.valid_until_monotonic_ms =
        mijia_monotonic_ms() + (uint64_t)valid_for_seconds * 1000u;
      g_mijia.agent_snapshot.requires_confirmation =
        requires_confirmation;
      mijia_copy_string(g_mijia.agent_snapshot.summary,
                        sizeof(g_mijia.agent_snapshot.summary), summary);
      mijia_copy_string(g_mijia.agent_snapshot.reasons,
                        sizeof(g_mijia.agent_snapshot.reasons), reasons);
      mijia_copy_string(g_mijia.agent_snapshot.caution,
                        sizeof(g_mijia.agent_snapshot.caution), caution);
    }
  pthread_mutex_unlock(&g_mijia.lock);
}

static int mijia_parse_agent_decision(
  const char *value, enum home_panel_agent_decision_e *decision)
{
  if (strcmp(value, "propose") == 0)
    {
      *decision = HOME_PANEL_AGENT_PROPOSE;
    }
  else if (strcmp(value, "suppress") == 0)
    {
      *decision = HOME_PANEL_AGENT_SUPPRESS;
    }
  else if (strcmp(value, "defer") == 0)
    {
      *decision = HOME_PANEL_AGENT_DEFER;
    }
  else
    {
      return -EBADMSG;
    }
  return 0;
}

static int mijia_parse_agent_policy(
  const char *value, enum home_panel_agent_policy_e *policy)
{
  if (strcmp(value, "none") == 0)
    {
      *policy = HOME_PANEL_AGENT_POLICY_NONE;
    }
  else if (strcmp(value, "turn_off_selected_light_keep_ac") == 0)
    {
      *policy = HOME_PANEL_AGENT_POLICY_TURN_OFF_SELECTED_LIGHT_KEEP_AC;
    }
  else if (strcmp(value, "turn_off_selected_light") == 0)
    {
      *policy = HOME_PANEL_AGENT_POLICY_TURN_OFF_SELECTED_LIGHT;
    }
  else if (strcmp(value, "notify_only") == 0)
    {
      *policy = HOME_PANEL_AGENT_POLICY_NOTIFY_ONLY;
    }
  else if (strcmp(value, "execute_local_candidate") == 0)
    {
      *policy = HOME_PANEL_AGENT_POLICY_EXECUTE_LOCAL_CANDIDATE;
    }
  else
    {
      return -EBADMSG;
    }
  return 0;
}

static bool mijia_agent_text_valid(const char *value, size_t max_length)
{
  const unsigned char *cursor = (const unsigned char *)value;
  const unsigned char *end;
  size_t length;
  unsigned int continuation;
  uint32_t codepoint;
  uint32_t minimum;

  if (value == NULL)
    {
      return false;
    }

  length = strlen(value);
  if (length > max_length)
    {
      return false;
    }

  end = cursor + length;
  while (cursor < end)
    {
      if (*cursor < 0x20 || *cursor == 0x7f)
        {
          return false;
        }

      if (*cursor < 0x80)
        {
          cursor++;
          continue;
        }

      if (*cursor >= 0xc2 && *cursor <= 0xdf)
        {
          continuation = 1;
          codepoint = *cursor & 0x1fu;
          minimum = 0x80u;
        }
      else if (*cursor >= 0xe0 && *cursor <= 0xef)
        {
          continuation = 2;
          codepoint = *cursor & 0x0fu;
          minimum = 0x800u;
        }
      else if (*cursor >= 0xf0 && *cursor <= 0xf4)
        {
          continuation = 3;
          codepoint = *cursor & 0x07u;
          minimum = 0x10000u;
        }
      else
        {
          return false;
        }

      cursor++;
      if ((size_t)(end - cursor) < continuation)
        {
          return false;
        }

      while (continuation-- > 0)
        {
          if ((*cursor & 0xc0u) != 0x80u)
            {
              return false;
            }
          codepoint = (codepoint << 6) | (*cursor & 0x3fu);
          cursor++;
        }

      if (codepoint < minimum ||
          (codepoint >= 0xd800u && codepoint <= 0xdfffu) ||
          codepoint > 0x10ffffu)
        {
          return false;
        }
    }
  return true;
}

static void *mijia_agent_worker(void *arg)
{
  struct mijia_agent_request_s *request = arg;
  enum home_panel_agent_decision_e decision = HOME_PANEL_AGENT_DEFER;
  enum home_panel_agent_policy_e policy =
    HOME_PANEL_AGENT_POLICY_NOTIFY_ONLY;
  enum home_panel_agent_state_e state;
  char response[MIJIA_JSON_BUFFER_SIZE];
  char reasons[192];
  const char *mode;
  const char *decision_text;
  const char *policy_text;
  const char *summary;
  const char *caution;
  const char *source;
  const char *scenario;
  cJSON *payload = NULL;
  cJSON *root = NULL;
  cJSON *item;
  cJSON *reason;
  char *body = NULL;
  unsigned int http_status = 0;
  unsigned int valid_for_seconds;
  unsigned int adjusted_confidence;
  unsigned int reason_count = 0;
  int adjustment;
  int ret = -ENOMEM;

  response[0] = '\0';
  reasons[0] = '\0';
  source = request->context.demo ? "demo" :
           request->context.source == 1 ? "learned" : "rule";
  scenario =
    request->context.candidate_kind == 1 ? "time_routine" :
    request->context.candidate_kind == 2 ? "event_routine" :
                                           "sleep_prepare";
  payload = cJSON_CreateObject();
  if (payload == NULL)
    {
      goto out;
    }

  cJSON_AddStringToObject(payload, "scenario", scenario);
  cJSON_AddStringToObject(payload, "source", source);
  cJSON_AddStringToObject(payload, "analysis_reason",
                         "periodic_review");
  cJSON_AddNumberToObject(payload, "routine_id",
                         request->context.routine_id);
  cJSON_AddNumberToObject(payload, "routine_observations",
                         request->context.routine_observations);
  cJSON_AddNumberToObject(payload, "routine_accepted",
                         request->context.routine_accepted);
  cJSON_AddNumberToObject(payload, "routine_rejected",
                         request->context.routine_rejected);
  cJSON_AddBoolToObject(payload, "automation_enabled",
                       request->context.automation_enabled);
  cJSON_AddBoolToObject(payload, "local_eligible",
                       request->context.local_eligible);
  cJSON_AddNumberToObject(payload, "minute_of_day",
                         request->context.minute_of_day);
  cJSON_AddNumberToObject(payload, "local_confidence",
                         request->context.local_confidence);
  cJSON_AddNumberToObject(payload, "history_days",
                         request->context.history_days);
  cJSON_AddNumberToObject(payload, "feedback_count",
                         request->context.feedback_count);
  cJSON_AddNumberToObject(payload, "accepted_count",
                         request->context.accepted_count);
  cJSON_AddNumberToObject(payload, "ignored_count",
                         request->context.ignored_count);
  cJSON_AddNumberToObject(payload, "lights_on",
                         request->context.lights_on);
  cJSON_AddBoolToObject(payload, "air_conditioner_on",
                       request->context.air_conditioner_on);
  cJSON_AddBoolToObject(payload, "network_online",
                       request->context.network_online);
  body = cJSON_PrintUnformatted(payload);
  if (body == NULL)
    {
      goto out;
    }

  ret = mijia_authorized_post(request->token, "/api/agent/analyze",
                              body, response, sizeof(response),
                              &http_status);
  if (ret < 0)
    {
      goto out;
    }

  root = cJSON_Parse(response);
  mode = root == NULL ? NULL : mijia_json_string(root, "mode");
  decision_text = root == NULL ? NULL :
                  mijia_json_string(root, "decision");
  policy_text = root == NULL ? NULL :
                mijia_json_string(root, "action_policy");
  summary = root == NULL ? NULL : mijia_json_string(root, "summary");
  caution = root == NULL ? NULL : mijia_json_string(root, "caution");
  item = root == NULL ? NULL :
         cJSON_GetObjectItemCaseSensitive(root, "requires_confirmation");
  if (mode == NULL || decision_text == NULL || policy_text == NULL ||
      !mijia_agent_text_valid(summary, 80) ||
      (caution != NULL && !mijia_agent_text_valid(caution, 60)) ||
      !cJSON_IsBool(item) || !cJSON_IsTrue(item) ||
      mijia_parse_agent_decision(decision_text, &decision) < 0 ||
      mijia_parse_agent_policy(policy_text, &policy) < 0)
    {
      ret = -EBADMSG;
      goto out;
    }

  item = cJSON_GetObjectItemCaseSensitive(root, "confidence_adjustment");
  if (!cJSON_IsNumber(item) || item->valueint < -10 ||
      item->valueint > 10)
    {
      ret = -EBADMSG;
      goto out;
    }
  adjustment = item->valueint;
  item = cJSON_GetObjectItemCaseSensitive(root, "valid_for_seconds");
  if (!cJSON_IsNumber(item) || item->valueint < 60 ||
      item->valueint > 1800)
    {
      ret = -EBADMSG;
      goto out;
    }
  valid_for_seconds = (unsigned int)item->valueint;
  adjusted_confidence = request->context.local_confidence;
  if (adjustment < 0 &&
      (unsigned int)(-adjustment) > adjusted_confidence)
    {
      adjusted_confidence = 0;
    }
  else if (adjustment < 0)
    {
      adjusted_confidence -= (unsigned int)(-adjustment);
    }
  else
    {
      adjusted_confidence += (unsigned int)adjustment;
      if (adjusted_confidence > 100)
        {
          adjusted_confidence = 100;
        }
    }

  item = cJSON_GetObjectItemCaseSensitive(root, "reasons");
  if (!cJSON_IsArray(item))
    {
      ret = -EBADMSG;
      goto out;
    }
  cJSON_ArrayForEach(reason, item)
    {
      size_t used = strlen(reasons);

      if (++reason_count > 3 || !cJSON_IsString(reason) ||
          !mijia_agent_text_valid(reason->valuestring, 60))
        {
          ret = -EBADMSG;
          goto out;
        }
      snprintf(reasons + used, sizeof(reasons) - used, "%s%s",
               used == 0 ? "" : "；", reason->valuestring);
    }

  if ((decision == HOME_PANEL_AGENT_SUPPRESS &&
       policy != HOME_PANEL_AGENT_POLICY_NONE) ||
      (decision == HOME_PANEL_AGENT_DEFER &&
       policy != HOME_PANEL_AGENT_POLICY_NONE &&
       policy != HOME_PANEL_AGENT_POLICY_NOTIFY_ONLY) ||
      (decision == HOME_PANEL_AGENT_PROPOSE &&
       ((request->context.candidate_kind == 0 &&
         policy !=
           HOME_PANEL_AGENT_POLICY_TURN_OFF_SELECTED_LIGHT_KEEP_AC &&
         policy != HOME_PANEL_AGENT_POLICY_TURN_OFF_SELECTED_LIGHT) ||
        (request->context.candidate_kind != 0 &&
         policy != HOME_PANEL_AGENT_POLICY_EXECUTE_LOCAL_CANDIDATE))))
    {
      ret = -EPERM;
      goto out;
    }

  state = strcmp(mode, "cloud") == 0 ?
          HOME_PANEL_AGENT_CLOUD_READY :
          strcmp(mode, "local-fallback") == 0 ?
          HOME_PANEL_AGENT_LOCAL_FALLBACK : HOME_PANEL_AGENT_ERROR;
  if (state == HOME_PANEL_AGENT_ERROR)
    {
      ret = -EBADMSG;
      goto out;
    }
  if (!request->context.local_eligible &&
      (decision != HOME_PANEL_AGENT_SUPPRESS ||
       policy != HOME_PANEL_AGENT_POLICY_NONE))
    {
      ret = -EPERM;
      goto out;
    }

  mijia_publish_agent(request, state, decision, policy,
                      adjusted_confidence, valid_for_seconds, true,
                      summary, reasons, caution);
  ret = 0;

out:
  if (ret < 0)
    {
      syslog(LOG_WARNING,
             "[HOME][AGENT] cloud analysis failed ret=%d status=%u "
             "bytes=%u; local fallback\n",
             ret, http_status, (unsigned int)strlen(response));
      mijia_publish_agent(request, HOME_PANEL_AGENT_ERROR,
                          request->context.local_eligible ?
                            HOME_PANEL_AGENT_PROPOSE :
                            HOME_PANEL_AGENT_SUPPRESS,
                          !request->context.local_eligible ?
                            HOME_PANEL_AGENT_POLICY_NONE :
                          request->context.candidate_kind == 0 ?
                            (request->context.air_conditioner_on ?
                              HOME_PANEL_AGENT_POLICY_TURN_OFF_SELECTED_LIGHT_KEEP_AC :
                              HOME_PANEL_AGENT_POLICY_TURN_OFF_SELECTED_LIGHT) :
                            HOME_PANEL_AGENT_POLICY_EXECUTE_LOCAL_CANDIDATE,
                          request->context.local_confidence, 300, true,
                          "云端分析不可用，已切换板端算法",
                          "板端安全规则与用户画像继续生效", "");
    }
  cJSON_free(body);
  cJSON_Delete(root);
  cJSON_Delete(payload);
  memset(request->token, 0, sizeof(request->token));
  free(request);
  return NULL;
}

static const char *mijia_learning_routine_kind(unsigned int kind)
{
  return kind == 2 ? "event" : "time";
}

static const char *mijia_learning_update_kind(unsigned int kind)
{
  static const char *const names[] =
  {
    "observation",
    "accepted",
    "rejected",
    "automation_enabled",
    "automation_disabled",
    "execution_succeeded",
    "execution_failed"
  };

  return kind < sizeof(names) / sizeof(names[0]) ?
           names[kind] : NULL;
}

static void *mijia_agent_learning_worker(void *arg)
{
  struct mijia_agent_learning_request_s *request = arg;
  const char *routine_kind;
  const char *update_kind;
  char response[1024];
  cJSON *payload = NULL;
  cJSON *root = NULL;
  cJSON *item;
  char *body = NULL;
  unsigned int http_status = 0;
  bool analysis_recommended = false;
  const char *reason = "";
  int ret = -ENOMEM;

  response[0] = '\0';
  routine_kind =
    mijia_learning_routine_kind(request->context.routine_kind);
  update_kind =
    mijia_learning_update_kind(request->context.update_kind);
  if (update_kind == NULL)
    {
      ret = -EINVAL;
      goto out;
    }

  payload = cJSON_CreateObject();
  if (payload == NULL)
    {
      goto out;
    }

  cJSON_AddNumberToObject(payload, "profile_revision",
                         request->context.profile_revision);
  cJSON_AddNumberToObject(payload, "routine_id",
                         request->context.routine_id);
  cJSON_AddStringToObject(payload, "routine_kind", routine_kind);
  cJSON_AddStringToObject(payload, "update_kind", update_kind);
  cJSON_AddNumberToObject(payload, "observation_count",
                         request->context.observation_count);
  cJSON_AddNumberToObject(payload, "accepted_count",
                         request->context.accepted_count);
  cJSON_AddNumberToObject(payload, "rejected_count",
                         request->context.rejected_count);
  cJSON_AddNumberToObject(payload, "confidence",
                         request->context.confidence);
  cJSON_AddNumberToObject(payload, "mean_minute_of_day",
                         request->context.mean_minute_of_day);
  cJSON_AddNumberToObject(payload, "mean_deviation_minutes",
                         request->context.mean_deviation_minutes);
  cJSON_AddNumberToObject(payload, "mean_delay_seconds",
                         request->context.mean_delay_seconds);
  cJSON_AddNumberToObject(payload, "execution_success_count",
                         request->context.execution_success_count);
  cJSON_AddNumberToObject(payload, "execution_failure_count",
                         request->context.execution_failure_count);
  cJSON_AddNumberToObject(payload, "consecutive_rejections",
                         request->context.consecutive_rejections);
  cJSON_AddBoolToObject(payload, "automation_enabled",
                       request->context.automation_enabled);
  body = cJSON_PrintUnformatted(payload);
  if (body == NULL)
    {
      goto out;
    }

  ret = mijia_authorized_post(request->token, "/api/agent/learn",
                              body, response, sizeof(response),
                              &http_status);
  if (ret < 0)
    {
      goto out;
    }

  root = cJSON_Parse(response);
  item = root == NULL ? NULL :
         cJSON_GetObjectItemCaseSensitive(root, "ok");
  if (!cJSON_IsTrue(item))
    {
      ret = -EBADMSG;
      goto out;
    }
  item = cJSON_GetObjectItemCaseSensitive(root,
                                          "analysis_recommended");
  analysis_recommended = cJSON_IsTrue(item);
  reason = mijia_json_string(root, "analysis_reason");
  ret = 0;

out:
  pthread_mutex_lock(&g_mijia.lock);
  /* The worker always releases the busy flag, even when it belongs to a
   * superseded login generation, so learning cannot wedge permanently.
   */
  g_mijia.agent_learning_busy = false;
  if (request->generation == g_mijia.request_generation)
    {
      if (ret == 0)
        {
          g_mijia.agent_learning_completed_revision =
            request->context.profile_revision;
          g_mijia.agent_learning_completed_routine =
            request->context.routine_id;
        }
    }
  pthread_mutex_unlock(&g_mijia.lock);

  if (ret == 0)
    {
      syslog(LOG_INFO,
             "[HOME][AGENT] learning synced revision=%u routine=%u "
             "cloud-review=%u reason=%s\n",
             (unsigned int)request->context.profile_revision,
             (unsigned int)request->context.routine_id,
             analysis_recommended ? 1u : 0u,
             reason == NULL ? "periodic_review" : reason);
    }
  else
    {
      syslog(LOG_WARNING,
             "[HOME][AGENT] learning sync failed revision=%u ret=%d "
             "status=%u\n",
             (unsigned int)request->context.profile_revision, ret,
             http_status);
    }
  cJSON_free(body);
  cJSON_Delete(root);
  cJSON_Delete(payload);
  memset(request->token, 0, sizeof(request->token));
  free(request);
  return NULL;
}

static uint32_t mijia_board_hash(const char *text)
{
  uint32_t hash = 2166136261u;
  const unsigned char *cursor = (const unsigned char *)text;

  while (cursor != NULL && *cursor != '\0')
    {
      hash = (hash ^ *cursor++) * 16777619u;
    }
  return hash == 0 ? 1 : hash;
}

static bool mijia_board_semantic_valid(const char *value)
{
  size_t index;
  size_t length;

  if (value == NULL)
    {
      return false;
    }
  length = strlen(value);
  if (length == 0 || length > 32)
    {
      return false;
    }
  for (index = 0; index < length; index++)
    {
      char ch = value[index];
      if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
            ch == '.' || ch == '_' || ch == '-'))
        {
          return false;
        }
    }
  return true;
}

static cJSON *mijia_board_property_json(const char *semantic,
                                        uint16_t siid, uint16_t piid,
                                        enum home_panel_control_type_e type,
                                        bool boolean_value, int value,
                                        bool writable, bool has_range,
                                        int minimum, int maximum, int step)
{
  cJSON *item;

  if (!mijia_board_semantic_valid(semantic) || siid == 0 || piid == 0 ||
      (type != HOME_PANEL_CONTROL_BOOLEAN &&
       type != HOME_PANEL_CONTROL_NUMBER &&
       type != HOME_PANEL_CONTROL_ENUM))
    {
      return NULL;
    }
  item = cJSON_CreateObject();
  if (item == NULL)
    {
      return NULL;
    }
  cJSON_AddStringToObject(item, "semantic", semantic);
  cJSON_AddNumberToObject(item, "siid", siid);
  cJSON_AddNumberToObject(item, "piid", piid);
  cJSON_AddStringToObject(item, "value_type",
                         type == HOME_PANEL_CONTROL_BOOLEAN ?
                           "boolean" : "number");
  if (type == HOME_PANEL_CONTROL_BOOLEAN)
    {
      cJSON_AddBoolToObject(item, "value", boolean_value);
    }
  else
    {
      cJSON_AddNumberToObject(item, "value", value);
    }
  cJSON_AddBoolToObject(item, "writable", writable);
  if (writable && type != HOME_PANEL_CONTROL_BOOLEAN && has_range)
    {
      cJSON_AddNumberToObject(item, "minimum", minimum);
      cJSON_AddNumberToObject(item, "maximum", maximum);
      cJSON_AddNumberToObject(item, "step", step > 0 ? step : 1);
    }
  cJSON_AddStringToObject(item, "purpose",
                         writable ? "同步当前值和可写能力供规律分析" :
                                    "同步只读状态供规律分析");
  return item;
}

static bool mijia_board_property_duplicate(
  const struct home_panel_device_s *device,
  const struct home_panel_observable_s *observable)
{
  unsigned int index;

  for (index = 0; index < device->control_count; index++)
    {
      if (device->controls[index].siid == observable->siid &&
          device->controls[index].piid == observable->piid)
        {
          return true;
        }
    }
  return false;
}

static char *mijia_build_board_state_body(
  const struct mijia_board_agent_request_s *request)
{
  cJSON *root = NULL;
  cJSON *devices = NULL;
  char *body = NULL;
  unsigned int device_index;

  root = cJSON_CreateObject();
  if (root == NULL)
    {
      return NULL;
    }
  cJSON_AddStringToObject(
    root, "purpose", "定时同步匿名设备状态，供服务端发现主动智能规律");
  cJSON_AddNumberToObject(root, "board_revision", request->board_revision);
  cJSON_AddNumberToObject(root, "proposal_after", request->proposal_after);
  cJSON_AddNumberToObject(root, "generated_at",
                         (double)request->generated_at);
  cJSON_AddNumberToObject(root, "minute_of_day", request->minute_of_day);
  devices = cJSON_AddArrayToObject(root, "devices");
  if (devices == NULL)
    {
      goto out;
    }

  for (device_index = 0;
       device_index < request->model->device_count; device_index++)
    {
      const struct home_panel_device_s *device =
        &request->model->devices[device_index];
      cJSON *item = cJSON_CreateObject();
      cJSON *properties;
      char key[9];
      const char *category;
      unsigned int index;
      unsigned int property_count = 0;

      if (item == NULL)
        {
          goto out;
        }
      snprintf(key, sizeof(key), "%08x",
               (unsigned int)mijia_board_hash(device->did));
      cJSON_AddStringToObject(item, "device_key", key);
      snprintf(key, sizeof(key), "%08x",
               (unsigned int)mijia_board_hash(device->room));
      cJSON_AddStringToObject(item, "room_key", key);
      category = mijia_board_semantic_valid(device->type) ?
                   device->type : "unknown";
      cJSON_AddStringToObject(item, "category", category);
      cJSON_AddBoolToObject(item, "online", device->online);
      cJSON_AddStringToObject(item, "purpose",
                             "同步匿名设备状态和可写能力");
      properties = cJSON_AddArrayToObject(item, "properties");
      if (properties == NULL)
        {
          cJSON_Delete(item);
          goto out;
        }
      for (index = 0; index < device->control_count; index++)
        {
          const struct home_panel_control_s *control =
            &device->controls[index];
          cJSON *property;

          if (!control->has_value ||
              property_count >= MIJIA_BOARD_AGENT_MAX_PROPERTIES)
            {
              continue;
            }
          property = mijia_board_property_json(
            control->name, control->siid, control->piid, control->type,
            control->boolean_value, control->value, true,
            control->has_range, control->minimum, control->maximum,
            control->step);
          if (property != NULL)
            {
              cJSON_AddItemToArray(properties, property);
              property_count++;
            }
        }
      for (index = 0; index < device->observable_count; index++)
        {
          const struct home_panel_observable_s *observable =
            &device->observables[index];
          cJSON *property;

          if (!observable->has_value ||
              observable->type == HOME_PANEL_CONTROL_TEXT ||
              mijia_board_property_duplicate(device, observable) ||
              property_count >= MIJIA_BOARD_AGENT_MAX_PROPERTIES)
            {
              continue;
            }
          property = mijia_board_property_json(
            observable->name, observable->siid, observable->piid,
            observable->type, observable->boolean_value,
            observable->value, false, false, 0, 0, 1);
          if (property != NULL)
            {
              cJSON_AddItemToArray(properties, property);
              property_count++;
            }
        }
      cJSON_AddItemToArray(devices, item);
    }
  body = cJSON_PrintUnformatted(root);

out:
  cJSON_Delete(root);
  return body;
}

static int mijia_board_status_parse(
  const char *value, enum home_panel_cloud_proposal_status_e *status_value)
{
  static const struct
  {
    const char *name;
    enum home_panel_cloud_proposal_status_e value;
  } values[] =
  {
    {"awaiting_confirmation", HOME_PANEL_CLOUD_PROPOSAL_AWAITING_CONFIRMATION},
    {"generating_plan", HOME_PANEL_CLOUD_PROPOSAL_GENERATING_PLAN},
    {"plan_ready", HOME_PANEL_CLOUD_PROPOSAL_PLAN_READY},
    {"accepted", HOME_PANEL_CLOUD_PROPOSAL_ACCEPTED},
    {"completed", HOME_PANEL_CLOUD_PROPOSAL_COMPLETED},
    {"failed", HOME_PANEL_CLOUD_PROPOSAL_FAILED},
    {"installed", HOME_PANEL_CLOUD_PROPOSAL_INSTALLED},
    {"dismissed", HOME_PANEL_CLOUD_PROPOSAL_DISMISSED},
  };
  unsigned int index;

  for (index = 0; index < sizeof(values) / sizeof(values[0]); index++)
    {
      if (value != NULL && strcmp(value, values[index].name) == 0)
        {
          *status_value = values[index].value;
          return 0;
        }
    }
  return -EBADMSG;
}

static int mijia_board_parse_trigger(
  cJSON *item, struct home_panel_cloud_trigger_s *trigger)
{
  const char *kind;
  const char *semantic;
  const char *device_key;
  cJSON *value;
  char *end;
  unsigned long parsed;

  memset(trigger, 0, sizeof(*trigger));
  if (item == NULL || cJSON_IsNull(item))
    {
      return 0;
    }
  if (!cJSON_IsObject(item))
    {
      return -EBADMSG;
    }
  kind = mijia_json_string(item, "kind");
  if (kind != NULL && strcmp(kind, "time") == 0)
    {
      cJSON *minute = cJSON_GetObjectItemCaseSensitive(item,
                                                       "minute_of_day");
      cJSON *window = cJSON_GetObjectItemCaseSensitive(item,
                                                       "window_minutes");
      cJSON *days = cJSON_GetObjectItemCaseSensitive(item, "days_mask");
      if (!cJSON_IsNumber(minute) || minute->valueint < 0 ||
          minute->valueint > 1439 || !cJSON_IsNumber(window) ||
          window->valueint < 1 || window->valueint > 120 ||
          !cJSON_IsNumber(days) || days->valueint < 1 ||
          days->valueint > 0x7f)
        {
          return -EBADMSG;
        }
      trigger->kind = HOME_PANEL_CLOUD_TRIGGER_TIME;
      trigger->minute_of_day = minute->valueint;
      trigger->window_minutes = window->valueint;
      trigger->days_mask = days->valueint;
      return 0;
    }
  if (kind == NULL || strcmp(kind, "property") != 0)
    {
      return -EBADMSG;
    }
  device_key = mijia_json_string(item, "device_key");
  semantic = mijia_json_string(item, "semantic");
  if (device_key == NULL || strlen(device_key) != 8 ||
      !mijia_board_semantic_valid(semantic))
    {
      return -EBADMSG;
    }
  parsed = strtoul(device_key, &end, 16);
  if (*end != '\0' || parsed > UINT32_MAX)
    {
      return -EBADMSG;
    }
  trigger->device_key = (uint32_t)parsed;
  mijia_copy_string(trigger->semantic, sizeof(trigger->semantic), semantic);
  value = cJSON_GetObjectItemCaseSensitive(item, "value");
  if (cJSON_IsBool(value))
    {
      trigger->value_is_boolean = true;
      trigger->value = cJSON_IsTrue(value) ? 1 : 0;
    }
  else if (cJSON_IsNumber(value))
    {
      trigger->value = value->valueint;
    }
  else
    {
      return -EBADMSG;
    }
  value = cJSON_GetObjectItemCaseSensitive(item, "delay_seconds");
  if (!cJSON_IsNumber(value) || value->valueint < 0 ||
      value->valueint > 3600)
    {
      return -EBADMSG;
    }
  trigger->delay_seconds = value->valueint;
  trigger->kind = HOME_PANEL_CLOUD_TRIGGER_PROPERTY;
  return 0;
}

static int mijia_board_parse_plan_actions(
  cJSON *plan, struct home_panel_cloud_proposal_s *proposal)
{
  cJSON *actions;
  cJSON *action;

  if (plan == NULL || cJSON_IsNull(plan))
    {
      proposal->action_count = 0;
      return 0;
    }
  actions = cJSON_GetObjectItemCaseSensitive(plan, "actions");
  if (!cJSON_IsArray(actions) || cJSON_GetArraySize(actions) < 1 ||
      cJSON_GetArraySize(actions) > HOME_PANEL_CLOUD_MAX_ACTIONS)
    {
      return -EBADMSG;
    }
  cJSON_ArrayForEach(action, actions)
    {
      struct home_panel_cloud_action_s *target =
        &proposal->actions[proposal->action_count];
      const char *device_key = mijia_json_string(action, "device_key");
      const char *semantic = mijia_json_string(action, "semantic");
      const char *value_type = mijia_json_string(action, "value_type");
      const char *purpose = mijia_json_string(action, "purpose");
      cJSON *siid = cJSON_GetObjectItemCaseSensitive(action, "siid");
      cJSON *piid = cJSON_GetObjectItemCaseSensitive(action, "piid");
      cJSON *value = cJSON_GetObjectItemCaseSensitive(action, "value");
      char *end;
      unsigned long parsed;

      if (device_key == NULL || strlen(device_key) != 8 ||
          !mijia_board_semantic_valid(semantic) || value_type == NULL ||
          !mijia_agent_text_valid(purpose, 79) || strlen(purpose) < 4 ||
          !cJSON_IsNumber(siid) || siid->valueint < 1 ||
          siid->valueint > 65535 || !cJSON_IsNumber(piid) ||
          piid->valueint < 1 || piid->valueint > 65535)
        {
          return -EBADMSG;
        }
      parsed = strtoul(device_key, &end, 16);
      if (*end != '\0' || parsed > UINT32_MAX)
        {
          return -EBADMSG;
        }
      target->device_key = parsed;
      target->siid = siid->valueint;
      target->piid = piid->valueint;
      mijia_copy_string(target->semantic, sizeof(target->semantic), semantic);
      mijia_copy_string(target->purpose, sizeof(target->purpose), purpose);
      if (strcmp(value_type, "boolean") == 0 && cJSON_IsBool(value))
        {
          target->value_is_boolean = true;
          target->value = cJSON_IsTrue(value) ? 1 : 0;
        }
      else if (strcmp(value_type, "number") == 0 &&
               cJSON_IsNumber(value))
        {
          target->value = value->valueint;
        }
      else
        {
          return -EBADMSG;
        }
      proposal->action_count++;
    }
  return 0;
}

static int mijia_board_parse_proposal(
  cJSON *item, struct home_panel_cloud_proposal_s *proposal)
{
  const char *id;
  const char *kind;
  const char *status_value;
  const char *purpose;
  const char *explanation;
  cJSON *value;

  memset(proposal, 0, sizeof(*proposal));
  if (item == NULL || cJSON_IsNull(item))
    {
      return -ENOENT;
    }
  id = mijia_json_string(item, "proposal_id");
  kind = mijia_json_string(item, "kind");
  status_value = mijia_json_string(item, "status");
  purpose = mijia_json_string(item, "purpose");
  explanation = mijia_json_string(item, "explanation");
  if (!mijia_board_proposal_id_valid(id) ||
      !mijia_agent_text_valid(purpose, 159) ||
      !mijia_agent_text_valid(explanation, 255) ||
      strlen(purpose) < 8 || strlen(explanation) < 8 ||
      mijia_board_status_parse(status_value, &proposal->status) < 0)
    {
      return -EBADMSG;
    }
  if (kind != NULL && strcmp(kind, "one_time") == 0)
    {
      proposal->kind = HOME_PANEL_CLOUD_ONE_TIME;
    }
  else if (kind != NULL && strcmp(kind, "automation") == 0)
    {
      proposal->kind = HOME_PANEL_CLOUD_AUTOMATION;
    }
  else
    {
      return -EBADMSG;
    }
  value = cJSON_GetObjectItemCaseSensitive(item, "proposal_revision");
  if (!cJSON_IsNumber(value) || value->valuedouble < 1 ||
      value->valuedouble > UINT32_MAX)
    {
      return -EBADMSG;
    }
  proposal->revision = (uint32_t)value->valuedouble;
  value = cJSON_GetObjectItemCaseSensitive(item, "expires_at");
  if (!cJSON_IsNumber(value) || value->valuedouble < 1)
    {
      return -EBADMSG;
    }
  proposal->expires_at = (uint64_t)value->valuedouble;
  value = cJSON_GetObjectItemCaseSensitive(item, "confidence");
  if (!cJSON_IsNumber(value) || value->valueint < 0 || value->valueint > 100)
    {
      return -EBADMSG;
    }
  proposal->confidence = value->valueint;
  value = cJSON_GetObjectItemCaseSensitive(item, "intents");
  if (!cJSON_IsArray(value) || cJSON_GetArraySize(value) < 1 ||
      cJSON_GetArraySize(value) > HOME_PANEL_CLOUD_MAX_ACTIONS)
    {
      return -EBADMSG;
    }
  proposal->intent_count = cJSON_GetArraySize(value);
  proposal->requires_confirmation = cJSON_IsTrue(
    cJSON_GetObjectItemCaseSensitive(item, "requires_confirmation"));
  mijia_copy_string(proposal->proposal_id, sizeof(proposal->proposal_id), id);
  mijia_copy_string(proposal->purpose, sizeof(proposal->purpose), purpose);
  mijia_copy_string(proposal->explanation, sizeof(proposal->explanation),
                    explanation);
  if (mijia_board_parse_trigger(
        cJSON_GetObjectItemCaseSensitive(item, "trigger"),
        &proposal->trigger) < 0 ||
      mijia_board_parse_plan_actions(
        cJSON_GetObjectItemCaseSensitive(item, "plan"), proposal) < 0)
    {
      return -EBADMSG;
    }
  if (proposal->kind == HOME_PANEL_CLOUD_ONE_TIME &&
      proposal->action_count == 0)
    {
      return -EBADMSG;
    }
  proposal->valid = true;
  return 0;
}

static bool mijia_board_proposal_id_valid(const char *value)
{
  size_t index;
  size_t length;

  if (value == NULL)
    {
      return false;
    }
  length = strlen(value);
  if (length < 16 || length > 64)
    {
      return false;
    }
  for (index = 0; index < length; index++)
    {
      char ch = value[index];
      if (!((ch >= 'a' && ch <= 'z') ||
            (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '_' || ch == '-'))
        {
          return false;
        }
    }
  return true;
}

static void mijia_board_publish_proposal(
  uint32_t generation, const struct home_panel_cloud_proposal_s *proposal,
  bool clear)
{
  pthread_mutex_lock(&g_mijia.lock);
  if (generation == g_mijia.request_generation)
    {
      if (clear)
        {
          memset(&g_mijia.board_proposal, 0,
                 sizeof(g_mijia.board_proposal));
          g_mijia.board_proposal.update_revision =
            ++g_mijia.next_board_update_revision;
        }
      else if (proposal != NULL &&
               (!g_mijia.board_proposal.valid ||
                proposal->revision >= g_mijia.board_proposal.revision))
        {
          g_mijia.board_proposal = *proposal;
          g_mijia.board_proposal.update_revision =
            ++g_mijia.next_board_update_revision;
        }
    }
  pthread_mutex_unlock(&g_mijia.lock);
}

static void *mijia_board_agent_worker(void *arg)
{
  struct mijia_board_agent_request_s *request = arg;
  struct home_panel_cloud_proposal_s proposal;
  char response[MIJIA_BOARD_AGENT_RESPONSE_SIZE];
  char path[128];
  char *body = NULL;
  cJSON *payload = NULL;
  cJSON *root = NULL;
  cJSON *proposal_item;
  unsigned int http_status = 0;
  bool clear = false;
  int ret = -ENOMEM;

  response[0] = '\0';
  if (request->kind == MIJIA_BOARD_AGENT_STATE)
    {
      body = mijia_build_board_state_body(request);
      if (body == NULL)
        {
          goto out;
        }
      ret = mijia_authorized_post(request->token, "/api/agent/state",
                                  body, response, sizeof(response),
                                  &http_status);
    }
  else if (request->kind == MIJIA_BOARD_AGENT_POLL)
    {
      snprintf(path, sizeof(path), "/api/agent/proposal?after=%u",
               (unsigned int)request->proposal_after);
      ret = mijia_authorized_get(request->token, path, response,
                                 sizeof(response), &http_status);
    }
  else
    {
      payload = cJSON_CreateObject();
      if (payload == NULL)
        {
          goto out;
        }
      cJSON_AddStringToObject(
        payload, "purpose",
        request->kind == MIJIA_BOARD_AGENT_CONFIRM ?
          "用户已阅读说明并同意生成离线自动化计划" :
          "反馈用户选择或设备执行结果供主动智能继续学习");
      cJSON_AddStringToObject(payload, "proposal_id",
                             request->proposal_id);
      if (request->kind == MIJIA_BOARD_AGENT_CONFIRM)
        {
          cJSON_AddBoolToObject(payload, "consent", true);
        }
      else
        {
          cJSON_AddStringToObject(payload, "result", request->feedback);
        }
      body = cJSON_PrintUnformatted(payload);
      if (body == NULL)
        {
          goto out;
        }
      ret = mijia_authorized_post(
        request->token,
        request->kind == MIJIA_BOARD_AGENT_CONFIRM ?
          "/api/agent/proposal/confirm" :
          "/api/agent/proposal/feedback",
        body, response, sizeof(response), &http_status);
      clear = request->kind == MIJIA_BOARD_AGENT_FEEDBACK &&
              (strcmp(request->feedback, "dismissed") == 0 ||
               strcmp(request->feedback, "execution_succeeded") == 0 ||
               strcmp(request->feedback, "execution_failed") == 0 ||
               strcmp(request->feedback, "automation_saved") == 0);
    }
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
  if (request->kind != MIJIA_BOARD_AGENT_FEEDBACK)
    {
      proposal_item = cJSON_GetObjectItemCaseSensitive(root, "proposal");
      ret = mijia_board_parse_proposal(proposal_item, &proposal);
      if (ret == -ENOENT)
        {
          ret = 0;
        }
      else if (ret == 0)
        {
          mijia_board_publish_proposal(request->generation, &proposal,
                                       false);
        }
    }
  else
    {
      ret = 0;
      if (clear)
        {
          mijia_board_publish_proposal(request->generation, NULL, true);
        }
      else if (strcmp(request->feedback, "accepted") == 0)
        {
          pthread_mutex_lock(&g_mijia.lock);
          if (request->generation == g_mijia.request_generation &&
              g_mijia.board_proposal.valid &&
              strcmp(g_mijia.board_proposal.proposal_id,
                     request->proposal_id) == 0)
            {
              g_mijia.board_proposal.status =
                HOME_PANEL_CLOUD_PROPOSAL_ACCEPTED;
              g_mijia.board_proposal.update_revision =
                ++g_mijia.next_board_update_revision;
            }
          pthread_mutex_unlock(&g_mijia.lock);
        }
    }

out:
  pthread_mutex_lock(&g_mijia.lock);
  /* Always release the flag; a stale-generation worker must not wedge the
   * board agent channel until the next login.
   */
  g_mijia.board_agent_busy = false;
  pthread_mutex_unlock(&g_mijia.lock);
  syslog(ret == 0 ? LOG_INFO : LOG_WARNING,
         "[HOME][AGENT] board request=%u ret=%d status=%u bytes=%u\n",
         (unsigned int)request->kind, ret, http_status,
         (unsigned int)strlen(response));
  cJSON_free(body);
  cJSON_Delete(root);
  cJSON_Delete(payload);
  free(request->model);
  memset(request->token, 0, sizeof(request->token));
  free(request);
  return NULL;
}

static int mijia_start_board_agent_request(
  struct mijia_board_agent_request_s *request)
{
  pthread_attr_t attr;
  pthread_t thread;
  int ret;

  pthread_mutex_lock(&g_mijia.lock);
  if (!g_mijia.initialized ||
      g_mijia.snapshot.state != HOME_PANEL_MIJIA_AUTHENTICATED ||
      g_mijia.token[0] == '\0')
    {
      pthread_mutex_unlock(&g_mijia.lock);
      return -EACCES;
    }
  if (g_mijia.board_agent_busy)
    {
      pthread_mutex_unlock(&g_mijia.lock);
      return -EBUSY;
    }
  request->generation = g_mijia.request_generation;
  mijia_copy_string(request->token, sizeof(request->token), g_mijia.token);
  g_mijia.board_agent_busy = true;
  pthread_mutex_unlock(&g_mijia.lock);

  ret = pthread_attr_init(&attr);
  if (ret == 0)
    {
      pthread_attr_setstacksize(&attr, MIJIA_BOARD_AGENT_THREAD_STACK);
      pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
      mijia_configure_background_thread(&attr);
      ret = pthread_create(&thread, &attr, mijia_board_agent_worker,
                           request);
      pthread_attr_destroy(&attr);
    }
  if (ret != 0)
    {
      pthread_mutex_lock(&g_mijia.lock);
      g_mijia.board_agent_busy = false;
      pthread_mutex_unlock(&g_mijia.lock);
    }
  return ret;
}

static int mijia_start_command(enum mijia_command_kind_e kind,
                               const char *identifier,
                               const char *display_name,
                               const char *property_name,
                               uint16_t siid, uint16_t piid,
                               bool value_is_boolean, bool target,
                               int number_value)
{
  struct mijia_command_request_s *request;
  pthread_attr_t attr;
  pthread_t thread;
  int ret;

  if (identifier == NULL || identifier[0] == '\0' ||
      display_name == NULL || display_name[0] == '\0')
    {
      return -EINVAL;
    }

  request = calloc(1, sizeof(*request));
  if (request == NULL)
    {
      return -ENOMEM;
    }

  pthread_mutex_lock(&g_mijia.lock);
  if (!g_mijia.initialized ||
      g_mijia.snapshot.state != HOME_PANEL_MIJIA_AUTHENTICATED ||
      g_mijia.token[0] == '\0')
    {
      pthread_mutex_unlock(&g_mijia.lock);
      free(request);
      return -EACCES;
    }
  if (g_mijia.command_busy)
    {
      pthread_mutex_unlock(&g_mijia.lock);
      free(request);
      return -EBUSY;
    }

  request->kind = kind;
  request->generation = g_mijia.request_generation;
  request->value_is_boolean = value_is_boolean;
  request->target = target;
  request->number_value = number_value;
  request->siid = siid;
  request->piid = piid;
  mijia_copy_string(request->token, sizeof(request->token), g_mijia.token);
  mijia_copy_string(request->identifier, sizeof(request->identifier),
                    identifier);
  mijia_copy_string(request->display_name, sizeof(request->display_name),
                    display_name);
  mijia_copy_string(request->property_name,
                    sizeof(request->property_name), property_name);
  g_mijia.command_busy = true;
  g_mijia.command_snapshot.state = HOME_PANEL_MIJIA_COMMAND_PENDING;
  g_mijia.command_snapshot.revision = ++g_mijia.next_command_revision;
  g_mijia.command_snapshot.code = 0;
  g_mijia.command_snapshot.target = target;
  mijia_copy_string(g_mijia.command_snapshot.device_name,
                    sizeof(g_mijia.command_snapshot.device_name),
                    display_name);
  mijia_copy_string(g_mijia.command_snapshot.message,
                    sizeof(g_mijia.command_snapshot.message),
                    kind == MIJIA_COMMAND_PROPERTY ?
                    "正在发送设备指令" :
                    kind == MIJIA_COMMAND_ACTION ?
                    "正在执行设备操作" : "正在执行场景");
  pthread_mutex_unlock(&g_mijia.lock);

  ret = pthread_attr_init(&attr);
  if (ret == 0)
    {
      pthread_attr_setstacksize(&attr, MIJIA_COMMAND_THREAD_STACK);
      pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
      mijia_configure_background_thread(&attr);
      ret = pthread_create(&thread, &attr, mijia_command_worker, request);
      pthread_attr_destroy(&attr);
    }

  if (ret != 0)
    {
      pthread_mutex_lock(&g_mijia.lock);
      g_mijia.command_busy = false;
      pthread_mutex_unlock(&g_mijia.lock);
      memset(request->token, 0, sizeof(request->token));
      free(request);
    }
  return ret;
}

static void *mijia_worker(void *arg)
{
  char home_name[64];
  char token[128];
  unsigned int device_count;
  unsigned int online_count;
  uint32_t generation;
  uint32_t server_revision;
  bool restoring;
  bool authenticated;
  bool changed;
  bool full_resync;
  int ret;

  (void)arg;

  for (;;)
    {
      pthread_mutex_lock(&g_mijia.lock);
      while (!g_mijia.request_pending && !g_mijia.restore_pending)
        {
          pthread_cond_wait(&g_mijia.condition, &g_mijia.lock);
        }

      restoring = g_mijia.restore_pending;
      g_mijia.restore_pending = false;
      g_mijia.request_pending = false;
      generation = g_mijia.request_generation;
      pthread_mutex_unlock(&g_mijia.lock);

      authenticated = false;
      if (restoring)
        {
          while (mijia_generation_active(generation))
            {
              ret = mijia_restore_session(generation, token,
                                          sizeof(token));
              if (ret == 0)
                {
                  authenticated = true;
                  break;
                }

              if (ret == -ENOENT || ret == -EBADMSG || ret == -EACCES)
                {
                  syslog(LOG_WARNING,
                         "[HOME][MIJIA] persisted session unavailable "
                         "ret=%d\n", ret);
                  mijia_publish(generation, HOME_PANEL_MIJIA_IDLE,
                                ret == -EACCES ?
                                "登录已失效，请重新登录" :
                                "点击登录米家", NULL, NULL, 0, 0);
                  break;
                }

              mijia_publish(generation, HOME_PANEL_MIJIA_STARTING,
                            "等待网络恢复登录", NULL, NULL, 0, 0);
              for (ret = 0; ret < MIJIA_RESTORE_RETRY_SECONDS; ret++)
                {
                  if (!mijia_generation_active(generation))
                    {
                      break;
                    }
                  sleep(1);
                }
            }
        }
      else
        {
          authenticated = mijia_process_login(generation, token,
                                              sizeof(token));
        }

      if (!authenticated)
        {
          memset(token, 0, sizeof(token));
          continue;
        }

      pthread_mutex_lock(&g_mijia.lock);
      server_revision = g_mijia.family_snapshot.server_revision;
      pthread_mutex_unlock(&g_mijia.lock);

      while (mijia_generation_active(generation))
        {
          ret = mijia_wait_for_changes(token, generation,
                                       server_revision, &changed,
                                       &full_resync);
          if (ret == -ECANCELED)
            {
              break;
            }
          if (ret < 0)
            {
              if (ret == -EACCES &&
                  mijia_reunlock_persisted_session(token) == 0)
                {
                  continue;
                }

              mijia_mark_sync_failure(generation, ret);
              if (!mijia_wait_for_refresh(generation))
                {
                  break;
                }
              continue;
            }
          if (!changed && !full_resync)
            {
              pthread_mutex_lock(&g_mijia.lock);
              server_revision = g_mijia.family_snapshot.server_revision;
              pthread_mutex_unlock(&g_mijia.lock);
              continue;
            }

          if (full_resync)
            {
              ret = mijia_fetch_family(token, generation, home_name,
                                       sizeof(home_name), &device_count,
                                       &online_count);
              if (ret == -EACCES &&
                  mijia_reunlock_persisted_session(token) == 0)
                {
                  ret = mijia_fetch_family(token, generation, home_name,
                                           sizeof(home_name), &device_count,
                                           &online_count);
                }
              if (ret < 0)
                {
                  if (ret != -ECANCELED)
                    {
                      mijia_mark_sync_failure(generation, ret);
                    }
                  continue;
                }
            }

          pthread_mutex_lock(&g_mijia.lock);
          server_revision = g_mijia.family_snapshot.server_revision;
          device_count = g_mijia.family_snapshot.device_count;
          online_count = g_mijia.family_snapshot.online_count;
          mijia_copy_string(home_name, sizeof(home_name),
                            g_mijia.snapshot.home_name);
          pthread_mutex_unlock(&g_mijia.lock);
          if (changed || full_resync)
            {
              mijia_refresh_summary(generation, home_name, device_count,
                                    online_count);
            }
        }

      memset(token, 0, sizeof(token));
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

  g_mijia.snapshot.state = HOME_PANEL_MIJIA_STARTING;
  g_mijia.snapshot.revision = ++g_mijia.next_revision;
  mijia_copy_string(g_mijia.snapshot.message,
                    sizeof(g_mijia.snapshot.message),
                    "正在检查米家登录");
  g_mijia.request_generation = 1;
  g_mijia.restore_pending = true;
  g_mijia.initialized = true;
  pthread_mutex_unlock(&g_mijia.lock);

  ret = pthread_attr_init(&attr);
  if (ret != 0)
    {
      return ret;
    }

  pthread_attr_setstacksize(&attr, MIJIA_CLIENT_THREAD_STACK);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  mijia_configure_background_thread(&attr);
  ret = pthread_create(&g_mijia.thread, &attr, mijia_worker, NULL);
  pthread_attr_destroy(&attr);
  if (ret != 0)
    {
      pthread_mutex_lock(&g_mijia.lock);
      g_mijia.initialized = false;
      pthread_mutex_unlock(&g_mijia.lock);
    }
  else
    {
      pthread_mutex_lock(&g_mijia.lock);
      pthread_cond_signal(&g_mijia.condition);
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
  memset(g_mijia.token, 0, sizeof(g_mijia.token));
  g_mijia.command_busy = false;
  g_mijia.agent_busy = false;
  g_mijia.agent_learning_busy = false;
  g_mijia.board_agent_busy = false;
  g_mijia.quiet_delta_count = 0;
  g_mijia.agent_learning_completed_revision = 0;
  g_mijia.agent_learning_completed_routine = 0;
  memset(&g_mijia.command_snapshot, 0,
         sizeof(g_mijia.command_snapshot));
  g_mijia.command_snapshot.revision = ++g_mijia.next_command_revision;
  memset(&g_mijia.agent_snapshot, 0, sizeof(g_mijia.agent_snapshot));
  g_mijia.agent_snapshot.revision = ++g_mijia.next_agent_revision;
  memset(&g_mijia.board_proposal, 0, sizeof(g_mijia.board_proposal));
  g_mijia.family_model_valid = false;
  memset(&g_mijia.family_model, 0, sizeof(g_mijia.family_model));
  memset(&g_mijia.family_snapshot, 0,
         sizeof(g_mijia.family_snapshot));
  pthread_cond_signal(&g_mijia.condition);
  pthread_mutex_unlock(&g_mijia.lock);
  return 0;
}

int home_panel_mijia_request_bool_property(const char *did,
                                            const char *device_name,
                                            const char *property_name,
                                            uint16_t siid,
                                            uint16_t piid,
                                            bool value)
{
  if (property_name == NULL || property_name[0] == '\0')
    {
      return -EINVAL;
    }

  return mijia_start_command(MIJIA_COMMAND_PROPERTY, did, device_name,
                             property_name, siid, piid, true, value, 0);
}

int home_panel_mijia_request_number_property(const char *did,
                                              const char *device_name,
                                              const char *property_name,
                                              uint16_t siid,
                                              uint16_t piid,
                                              int value)
{
  if (property_name == NULL || property_name[0] == '\0')
    {
      return -EINVAL;
    }

  return mijia_start_command(MIJIA_COMMAND_PROPERTY, did, device_name,
                             property_name, siid, piid, false, false,
                             value);
}

int home_panel_mijia_request_action(const char *did,
                                    const char *device_name,
                                    const char *action_name,
                                    uint16_t siid,
                                    uint16_t aiid)
{
  if (action_name == NULL || action_name[0] == '\0' ||
      siid == 0 || aiid == 0)
    {
      return -EINVAL;
    }

  return mijia_start_command(MIJIA_COMMAND_ACTION, did, device_name,
                             action_name, siid, aiid,
                             true, false, 0);
}

int home_panel_mijia_request_scene(const char *scene_id,
                                   const char *scene_name)
{
  return mijia_start_command(MIJIA_COMMAND_SCENE, scene_id, scene_name,
                             NULL, 0, 0, true, false, 0);
}

int home_panel_mijia_request_agent_analysis(
  const struct home_panel_agent_request_s *context)
{
  struct mijia_agent_request_s *request;
  pthread_attr_t attr;
  pthread_t thread;
  int ret;

  if (context == NULL || context->context_revision == 0 ||
      context->candidate_kind > 2 ||
      context->minute_of_day >= 24u * 60u ||
      context->local_confidence > 100 ||
      context->feedback_count > 0 && context->accepted_count >
        context->feedback_count ||
      !context->local_eligible)
    {
      return -EINVAL;
    }

  request = calloc(1, sizeof(*request));
  if (request == NULL)
    {
      return -ENOMEM;
    }

  pthread_mutex_lock(&g_mijia.lock);
  if (!g_mijia.initialized ||
      g_mijia.snapshot.state != HOME_PANEL_MIJIA_AUTHENTICATED ||
      g_mijia.token[0] == '\0')
    {
      pthread_mutex_unlock(&g_mijia.lock);
      free(request);
      return -EACCES;
    }
  if (g_mijia.agent_busy)
    {
      pthread_mutex_unlock(&g_mijia.lock);
      free(request);
      return -EBUSY;
    }

  request->generation = g_mijia.request_generation;
  memcpy(&request->context, context, sizeof(request->context));
  mijia_copy_string(request->token, sizeof(request->token), g_mijia.token);
  g_mijia.agent_busy = true;
  g_mijia.agent_snapshot.state = HOME_PANEL_AGENT_PENDING;
  g_mijia.agent_snapshot.revision = ++g_mijia.next_agent_revision;
  g_mijia.agent_snapshot.context_revision = context->context_revision;
  g_mijia.agent_snapshot.adjusted_confidence =
    context->local_confidence;
  g_mijia.agent_snapshot.valid_until_monotonic_ms =
    mijia_monotonic_ms() +
    (uint64_t)MIJIA_AGENT_PENDING_SECONDS * 1000u;
  g_mijia.agent_snapshot.requires_confirmation = true;
  mijia_copy_string(g_mijia.agent_snapshot.summary,
                    sizeof(g_mijia.agent_snapshot.summary),
                    "云端正在深度分析，暂不执行");
  g_mijia.agent_snapshot.reasons[0] = '\0';
  g_mijia.agent_snapshot.caution[0] = '\0';
  pthread_mutex_unlock(&g_mijia.lock);

  ret = pthread_attr_init(&attr);
  if (ret == 0)
    {
      pthread_attr_setstacksize(&attr, MIJIA_AGENT_THREAD_STACK);
      pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
      mijia_configure_background_thread(&attr);
      ret = pthread_create(&thread, &attr, mijia_agent_worker, request);
      pthread_attr_destroy(&attr);
    }

  if (ret != 0)
    {
      pthread_mutex_lock(&g_mijia.lock);
      g_mijia.agent_busy = false;
      g_mijia.agent_snapshot.state = HOME_PANEL_AGENT_ERROR;
      g_mijia.agent_snapshot.revision = ++g_mijia.next_agent_revision;
      mijia_copy_string(g_mijia.agent_snapshot.summary,
                        sizeof(g_mijia.agent_snapshot.summary),
                        "云端线程启动失败，已切换板端算法");
      pthread_mutex_unlock(&g_mijia.lock);
      memset(request->token, 0, sizeof(request->token));
      free(request);
    }
  return ret;
}

int home_panel_mijia_request_agent_learning(
  const struct home_panel_agent_learning_request_s *context)
{
  struct mijia_agent_learning_request_s *request;
  pthread_attr_t attr;
  pthread_t thread;
  int ret;

  if (context == NULL || context->profile_revision == 0 ||
      context->routine_id == 0 ||
      (context->routine_kind != 1 && context->routine_kind != 2) ||
      context->update_kind > 6 ||
      context->observation_count == 0 ||
      context->confidence > 100 ||
      context->mean_minute_of_day >= 24u * 60u ||
      context->mean_deviation_minutes > 720 ||
      context->mean_delay_seconds > 3600 ||
      context->consecutive_rejections > context->rejected_count)
    {
      return -EINVAL;
    }

  request = calloc(1, sizeof(*request));
  if (request == NULL)
    {
      return -ENOMEM;
    }

  pthread_mutex_lock(&g_mijia.lock);
  if (!g_mijia.initialized ||
      g_mijia.snapshot.state != HOME_PANEL_MIJIA_AUTHENTICATED ||
      g_mijia.token[0] == '\0')
    {
      pthread_mutex_unlock(&g_mijia.lock);
      free(request);
      return -EACCES;
    }
  if (g_mijia.agent_learning_busy)
    {
      pthread_mutex_unlock(&g_mijia.lock);
      free(request);
      return -EBUSY;
    }
  if (g_mijia.agent_learning_completed_revision ==
        context->profile_revision &&
      g_mijia.agent_learning_completed_routine == context->routine_id)
    {
      pthread_mutex_unlock(&g_mijia.lock);
      free(request);
      return -EALREADY;
    }

  request->generation = g_mijia.request_generation;
  memcpy(&request->context, context, sizeof(request->context));
  mijia_copy_string(request->token, sizeof(request->token), g_mijia.token);
  g_mijia.agent_learning_busy = true;
  pthread_mutex_unlock(&g_mijia.lock);

  ret = pthread_attr_init(&attr);
  if (ret == 0)
    {
      pthread_attr_setstacksize(&attr, MIJIA_AGENT_THREAD_STACK);
      pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
      mijia_configure_background_thread(&attr);
      ret = pthread_create(&thread, &attr,
                           mijia_agent_learning_worker, request);
      pthread_attr_destroy(&attr);
    }

  if (ret != 0)
    {
      pthread_mutex_lock(&g_mijia.lock);
      g_mijia.agent_learning_busy = false;
      pthread_mutex_unlock(&g_mijia.lock);
      memset(request->token, 0, sizeof(request->token));
      free(request);
    }
  return ret;
}

int home_panel_mijia_request_board_state(
  const struct home_panel_family_model_s *model,
  uint32_t board_revision, uint32_t proposal_after,
  uint64_t generated_at, unsigned int minute_of_day)
{
  struct mijia_board_agent_request_s *request;
  int ret;

  if (model == NULL || board_revision == 0 || generated_at == 0 ||
      minute_of_day >= 24u * 60u)
    {
      return -EINVAL;
    }
  request = calloc(1, sizeof(*request));
  if (request == NULL)
    {
      return -ENOMEM;
    }
  request->model = malloc(sizeof(*request->model));
  if (request->model == NULL)
    {
      free(request);
      return -ENOMEM;
    }
  memcpy(request->model, model, sizeof(*request->model));
  request->kind = MIJIA_BOARD_AGENT_STATE;
  request->board_revision = board_revision;
  request->proposal_after = proposal_after;
  request->generated_at = generated_at;
  request->minute_of_day = minute_of_day;
  ret = mijia_start_board_agent_request(request);
  if (ret != 0)
    {
      free(request->model);
      free(request);
    }
  return ret;
}

int home_panel_mijia_request_board_proposal(uint32_t after)
{
  struct mijia_board_agent_request_s *request = calloc(1, sizeof(*request));
  int ret;

  if (request == NULL)
    {
      return -ENOMEM;
    }
  request->kind = MIJIA_BOARD_AGENT_POLL;
  request->proposal_after = after;
  ret = mijia_start_board_agent_request(request);
  if (ret != 0)
    {
      free(request);
    }
  return ret;
}

int home_panel_mijia_confirm_board_proposal(const char *proposal_id)
{
  struct mijia_board_agent_request_s *request;
  int ret;

  if (!mijia_board_proposal_id_valid(proposal_id))
    {
      return -EINVAL;
    }
  request = calloc(1, sizeof(*request));
  if (request == NULL)
    {
      return -ENOMEM;
    }
  request->kind = MIJIA_BOARD_AGENT_CONFIRM;
  mijia_copy_string(request->proposal_id, sizeof(request->proposal_id),
                    proposal_id);
  ret = mijia_start_board_agent_request(request);
  if (ret != 0)
    {
      free(request);
    }
  return ret;
}

int home_panel_mijia_feedback_board_proposal(const char *proposal_id,
                                              const char *result)
{
  static const char *const allowed[] =
  {
    "accepted", "dismissed", "execution_succeeded",
    "execution_failed", "automation_saved"
  };
  struct mijia_board_agent_request_s *request;
  unsigned int index;
  bool valid = false;
  int ret;

  if (!mijia_board_proposal_id_valid(proposal_id) || result == NULL)
    {
      return -EINVAL;
    }
  for (index = 0; index < sizeof(allowed) / sizeof(allowed[0]); index++)
    {
      if (strcmp(result, allowed[index]) == 0)
        {
          valid = true;
          break;
        }
    }
  if (!valid)
    {
      return -EINVAL;
    }
  request = calloc(1, sizeof(*request));
  if (request == NULL)
    {
      return -ENOMEM;
    }
  request->kind = MIJIA_BOARD_AGENT_FEEDBACK;
  mijia_copy_string(request->proposal_id, sizeof(request->proposal_id),
                    proposal_id);
  mijia_copy_string(request->feedback, sizeof(request->feedback), result);
  ret = mijia_start_board_agent_request(request);
  if (ret != 0)
    {
      free(request);
    }
  return ret;
}

void home_panel_mijia_get_snapshot(
  struct home_panel_mijia_snapshot_s *snapshot)
{
  if (snapshot == NULL)
    {
      return;
    }

  pthread_mutex_lock(&g_mijia.lock);
  memcpy(snapshot, &g_mijia.snapshot, sizeof(*snapshot));
  pthread_mutex_unlock(&g_mijia.lock);
}

void home_panel_mijia_get_family_snapshot(
  struct home_panel_mijia_family_snapshot_s *snapshot)
{
  if (snapshot == NULL)
    {
      return;
    }

  pthread_mutex_lock(&g_mijia.lock);
  memcpy(snapshot, &g_mijia.family_snapshot, sizeof(*snapshot));
  pthread_mutex_unlock(&g_mijia.lock);
}

int home_panel_mijia_get_family_update(
  uint32_t previous_revision,
  struct home_panel_mijia_family_snapshot_s *snapshot,
  struct home_panel_family_model_s *model)
{
  int ret;

  if (snapshot == NULL || model == NULL)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&g_mijia.lock);
  if (!g_mijia.family_model_valid ||
      g_mijia.family_snapshot.revision == 0 ||
      g_mijia.family_snapshot.revision == previous_revision ||
      g_mijia.family_snapshot.revision != g_mijia.family_model.revision)
    {
      ret = -EAGAIN;
    }
  else
    {
      memcpy(snapshot, &g_mijia.family_snapshot, sizeof(*snapshot));
      memcpy(model, &g_mijia.family_model, sizeof(*model));
      ret = 0;
    }
  pthread_mutex_unlock(&g_mijia.lock);
  return ret;
}

int home_panel_mijia_get_family_model(
  uint32_t revision, struct home_panel_family_model_s *model)
{
  int ret;

  if (model == NULL || revision == 0)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&g_mijia.lock);
  if (!g_mijia.family_model_valid ||
      revision != g_mijia.family_snapshot.revision ||
      revision != g_mijia.family_model.revision)
    {
      ret = -ESTALE;
    }
  else
    {
      memcpy(model, &g_mijia.family_model, sizeof(*model));
      ret = 0;
    }
  pthread_mutex_unlock(&g_mijia.lock);
  return ret;
}

void home_panel_mijia_get_agent_snapshot(
  struct home_panel_agent_snapshot_s *snapshot)
{
  if (snapshot == NULL)
    {
      return;
    }

  pthread_mutex_lock(&g_mijia.lock);
  memcpy(snapshot, &g_mijia.agent_snapshot, sizeof(*snapshot));
  pthread_mutex_unlock(&g_mijia.lock);
}

void home_panel_mijia_get_board_proposal(
  struct home_panel_cloud_proposal_s *proposal)
{
  if (proposal == NULL)
    {
      return;
    }
  pthread_mutex_lock(&g_mijia.lock);
  memcpy(proposal, &g_mijia.board_proposal, sizeof(*proposal));
  pthread_mutex_unlock(&g_mijia.lock);
}

void home_panel_mijia_get_command_snapshot(
  struct home_panel_mijia_command_snapshot_s *snapshot)
{
  if (snapshot == NULL)
    {
      return;
    }

  pthread_mutex_lock(&g_mijia.lock);
  memcpy(snapshot, &g_mijia.command_snapshot, sizeof(*snapshot));
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
