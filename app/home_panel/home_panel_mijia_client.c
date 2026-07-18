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

#define MIJIA_HTTP_BUFFER_SIZE       1024
#define MIJIA_JSON_BUFFER_SIZE       2048
#define MIJIA_DELTA_RESPONSE_SIZE    16384
#define MIJIA_LOGIN_POLL_SECONDS     2
#define MIJIA_LOGIN_MAX_POLLS        90
#define MIJIA_CLIENT_THREAD_STACK    16384
#define MIJIA_COMMAND_THREAD_STACK   16384
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
  size_t qr_size;
  bool command_busy;
  char token[128];
  bool family_model_valid;
  struct home_panel_family_model_s family_model;
  struct home_panel_mijia_snapshot_s snapshot;
  struct home_panel_mijia_family_snapshot_s family_snapshot;
  struct home_panel_mijia_command_snapshot_s command_snapshot;
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
  MIJIA_COMMAND_SCENE
};

struct mijia_command_request_s
{
  enum mijia_command_kind_e kind;
  uint32_t generation;
  bool target;
  uint16_t siid;
  uint16_t piid;
  char token[128];
  char identifier[80];
  char property_name[48];
  char display_name[48];
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

  webclient_set_defaults(&context);
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

  parsed_model = malloc(sizeof(*parsed_model));
  if (parsed_model == NULL)
    {
      ret = -ENOMEM;
      goto out;
    }

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
  g_mijia.family_snapshot.room_count = room_count;
  g_mijia.family_snapshot.device_count = *device_count;
  g_mijia.family_snapshot.online_count = *online_count;
  g_mijia.family_snapshot.scene_count = scene_count;
  g_mijia.family_snapshot.detail_error_count = detail_error_count;
  g_mijia.family_snapshot.consecutive_failures = 0;
  g_mijia.family_snapshot.stale = false;
  syslog(LOG_INFO,
         "[HOME][SYNC] revision=%u server=%u bytes=%u homes=%u rooms=%u "
         "devices=%u online=%u scenes=%u detail_errors=%u\n",
         (unsigned int)g_mijia.family_snapshot.revision,
         server_revision, (unsigned int)sink.length, home_count, room_count,
         *device_count, *online_count, scene_count, detail_error_count);
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
  free(parsed_model);
  free(response);
  return ret;
}

static int mijia_wait_for_changes(const char *token, uint32_t generation,
                                  uint32_t after, bool *changed,
                                  bool *full_resync)
{
  static unsigned int quiet_delta_count;
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
  int online_count_delta = 0;
  bool model_changed = false;
  bool resync_required;
  bool server_stale;
  int length;
  int ret = -ENOMEM;

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
      ret = -EBADMSG;
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
                  pthread_mutex_unlock(&g_mijia.lock);
                  ret = -EBADMSG;
                  goto out;
                }

              if (cJSON_IsTrue(previous) != cJSON_IsTrue(value))
                {
                  online_count_delta += cJSON_IsTrue(value) ? 1 : -1;
                }

              apply_ret = home_panel_mijia_model_apply_online(
                &g_mijia.family_model, did, cJSON_IsTrue(value));
              if (apply_ret == -ENOENT)
                {
                  continue;
                }
              if (apply_ret < 0)
                {
                  pthread_mutex_unlock(&g_mijia.lock);
                  ret = apply_ret;
                  goto out;
                }

              model_changed |= apply_ret > 0;
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
                  pthread_mutex_unlock(&g_mijia.lock);
                  ret = -EBADMSG;
                  goto out;
                }

              if (code_item->valueint != 0)
                {
                  continue;
                }

              apply_ret = home_panel_mijia_model_apply_property(
                &g_mijia.family_model, did, (uint16_t)siid,
                (uint16_t)piid, cJSON_IsBool(value), cJSON_IsTrue(value),
                cJSON_IsNumber(value),
                cJSON_IsNumber(value) ? value->valueint : 0);
              if (apply_ret == -ENOENT)
                {
                  continue;
                }
              if (apply_ret < 0)
                {
                  pthread_mutex_unlock(&g_mijia.lock);
                  ret = apply_ret;
                  goto out;
                }

              model_changed |= apply_ret > 0;
            }
        }
    }

  g_mijia.family_snapshot.server_revision = revision;
  g_mijia.family_snapshot.generated_at = generated_at;
  g_mijia.family_snapshot.json_size = sink.length;
  g_mijia.family_snapshot.consecutive_failures = 0;
  g_mijia.family_snapshot.stale = false;
  if (online_count_delta > 0)
    {
      unsigned int delta = (unsigned int)online_count_delta;

      if (delta <= g_mijia.family_snapshot.device_count -
                   g_mijia.family_snapshot.online_count)
        {
          g_mijia.family_snapshot.online_count += delta;
        }
    }
  else if (online_count_delta < 0)
    {
      unsigned int delta = (unsigned int)(-online_count_delta);

      if (delta <= g_mijia.family_snapshot.online_count)
        {
          g_mijia.family_snapshot.online_count -= delta;
        }
    }
  if (model_changed)
    {
      home_panel_mijia_model_refresh_rooms(&g_mijia.family_model);
      g_mijia.family_snapshot.revision = ++g_mijia.next_family_revision;
      g_mijia.family_model.revision = g_mijia.family_snapshot.revision;
    }
  pthread_mutex_unlock(&g_mijia.lock);

  *changed = model_changed;
  if (model_changed)
    {
      quiet_delta_count = 0;
      syslog(LOG_INFO,
             "[HOME][SYNC] delta server=%u bytes=%u methods=%u "
             "properties=%u online=%u\n",
             revision, (unsigned int)sink.length,
             (unsigned int)cJSON_GetArraySize(changes),
             property_changes, online_changes);
    }
  else
    {
      quiet_delta_count++;
      if (quiet_delta_count % 30 == 0)
        {
          syslog(LOG_INFO,
                 "[HOME][SYNC] quiet deltas=%u server=%u bytes=%u\n",
                 quiet_delta_count, revision, (unsigned int)sink.length);
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
      if (state != HOME_PANEL_MIJIA_COMMAND_PENDING)
        {
          g_mijia.command_busy = false;
        }
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
      cJSON_AddBoolToObject(payload, "value", request->target);
      path = "/api/device/property";
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
             "response=%.160s\n",
             ret, http_status, (unsigned int)request->kind, response);
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

static int mijia_start_command(enum mijia_command_kind_e kind,
                               const char *identifier,
                               const char *display_name,
                               const char *property_name,
                               uint16_t siid, uint16_t piid, bool target)
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
  request->target = target;
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
                    "正在发送设备指令" : "正在执行场景");
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
  memset(&g_mijia.command_snapshot, 0,
         sizeof(g_mijia.command_snapshot));
  g_mijia.command_snapshot.revision = ++g_mijia.next_command_revision;
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
                             property_name, siid, piid, value);
}

int home_panel_mijia_request_scene(const char *scene_id,
                                   const char *scene_name)
{
  return mijia_start_command(MIJIA_COMMAND_SCENE, scene_id, scene_name,
                             NULL, 0, 0, false);
}

void home_panel_mijia_get_snapshot(
  struct home_panel_mijia_snapshot_s *snapshot)
{
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
