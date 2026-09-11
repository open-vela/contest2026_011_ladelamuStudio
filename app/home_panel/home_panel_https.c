/****************************************************************************
 * D13x home panel HTTPS transport
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include "home_panel_https.h"

struct webclient_tls_connection
{
  mbedtls_net_context net;
  mbedtls_ssl_context ssl;
  mbedtls_ssl_config config;
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context drbg;
  mbedtls_x509_crt ca;
};

static const char g_gts_root_r1[] =
  "-----BEGIN CERTIFICATE-----\n"
  "MIIFVzCCAz+gAwIBAgINAgPlk28xsBNJiGuiFzANBgkqhkiG9w0BAQwFADBHMQsw\n"
  "CQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEU\n"
  "MBIGA1UEAxMLR1RTIFJvb3QgUjEwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAw\n"
  "MDAwWjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZp\n"
  "Y2VzIExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjEwggIiMA0GCSqGSIb3DQEBAQUA\n"
  "A4ICDwAwggIKAoICAQC2EQKLHuOhd5s73L+UPreVp0A8of2C+X0yBoJx9vaMf/vo\n"
  "27xqLpeXo4xL+Sv2sfnOhB2x+cWX3u+58qPpvBKJXqeqUqv4IyfLpLGcY9vXmX7w\n"
  "Cl7raKb0xlpHDU0QM+NOsROjyBhsS+z8CZDfnWQpJSMHobTSPS5g4M/SCYe7zUjw\n"
  "TcLCeoiKu7rPWRnWr4+wB7CeMfGCwcDfLqZtbBkOtdh+JhpFAz2weaSUKK0Pfybl\n"
  "qAj+lug8aJRT7oM6iCsVlgmy4HqMLnXWnOunVmSPlk9orj2XwoSPwLxAwAtcvfaH\n"
  "szVsrBhQf4TgTM2S0yDpM7xSma8ytSmzJSq0SPly4cpk9+aCEI3oncKKiPo4Zor8\n"
  "Y/kB+Xj9e1x3+naH+uzfsQ55lVe0vSbv1gHR6xYKu44LtcXFilWr06zqkUspzBmk\n"
  "MiVOKvFlRNACzqrOSbTqn3yDsEB750Orp2yjj32JgfpMpf/VjsPOS+C12LOORc92\n"
  "wO1AK/1TD7Cn1TsNsYqiA94xrcx36m97PtbfkSIS5r762DL8EGMUUXLeXdYWk70p\n"
  "aDPvOmbsB4om3xPXV2V4J95eSRQAogB/mqghtqmxlbCluQ0WEdrHbEg8QOB+DVrN\n"
  "VjzRlwW5y0vtOUucxD/SVRNuJLDWcfr0wbrM7Rv1/oFB2ACYPTrIrnqYNxgFlQID\n"
  "AQABo0IwQDAOBgNVHQ8BAf8EBAMCAYYwDwYDVR0TAQH/BAUwAwEB/zAdBgNVHQ4E\n"
  "FgQU5K8rJnEaK0gnhS9SZizv8IkTcT4wDQYJKoZIhvcNAQEMBQADggIBAJ+qQibb\n"
  "C5u+/x6Wki4+omVKapi6Ist9wTrYggoGxval3sBOh2Z5ofmmWJyq+bXmYOfg6LEe\n"
  "QkEzCzc9zolwFcq1JKjPa7XSQCGYzyI0zzvFIoTgxQ6KfF2I5DUkzps+GlQebtuy\n"
  "h6f88/qBVRRiClmpIgUxPoLW7ttXNLwzldMXG+gnoot7TiYaelpkttGsN/H9oPM4\n"
  "7HLwEXWdyzRSjeZ2axfG34arJ45JK3VmgRAhpuo+9K4l/3wV3s6MJT/KYnAK9y8J\n"
  "ZgfIPxz88NtFMN9iiMG1D53Dn0reWVlHxYciNuaCp+0KueIHoI17eko8cdLiA6Ef\n"
  "MgfdG+RCzgwARWGAtQsgWSl4vflVy2PFPEz0tv/bal8xa5meLMFrUKTX5hgUvYU/\n"
  "Z6tGn6D/Qqc6f1zLXbBwHSs09dR2CQzreExZBfMZQsNhFRAbd03OIozUhfJFfbdT\n"
  "6u9AWpQKXCBfTkBdYiJ23//OYb2MI3jSNwLgjt7RETeJ9r/tSQdirpLsQBqvFAnZ\n"
  "0E6yove+7u7Y/9waLd64NnHi/Hm3lCXRSHNboTXns5lndcEZOitHTtNCjv0xyBZm\n"
  "2tIMPNuzjsmhDYAPexZ3FL//2wmUspO8IFgV6dtxQ/PeEMMA3KgqlbbC1j+Qa3bb\n"
  "bP6MvPJwNQzcmRk13NfIRmPVNnGuV/u3gm3c\n"
  "-----END CERTIFICATE-----\n";

static int https_connect(void *ctx, const char *hostname, const char *port,
                         unsigned int timeout_second,
                         struct webclient_tls_connection **connp)
{
  struct webclient_tls_connection *conn;
  const unsigned char *seed = (const unsigned char *)"d13x-home-panel";
  uint32_t verify;
  int ret;

  (void)ctx;
  (void)timeout_second;

  conn = calloc(1, sizeof(*conn));
  if (conn == NULL)
    {
      return -ENOMEM;
    }

  mbedtls_net_init(&conn->net);
  mbedtls_ssl_init(&conn->ssl);
  mbedtls_ssl_config_init(&conn->config);
  mbedtls_entropy_init(&conn->entropy);
  mbedtls_ctr_drbg_init(&conn->drbg);
  mbedtls_x509_crt_init(&conn->ca);

  ret = mbedtls_ctr_drbg_seed(&conn->drbg, mbedtls_entropy_func,
                              &conn->entropy, seed, strlen((const char *)seed));
  if (ret != 0)
    {
      ret = -EIO;
      goto err;
    }

  ret = mbedtls_x509_crt_parse(&conn->ca,
                               (const unsigned char *)g_gts_root_r1,
                               sizeof(g_gts_root_r1));
  if (ret < 0)
    {
      ret = -EIO;
      goto err;
    }

  ret = mbedtls_ssl_config_defaults(&conn->config, MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT);
  if (ret != 0)
    {
      ret = -EIO;
      goto err;
    }

  mbedtls_ssl_conf_authmode(&conn->config, MBEDTLS_SSL_VERIFY_REQUIRED);
  mbedtls_ssl_conf_ca_chain(&conn->config, &conn->ca, NULL);
  mbedtls_ssl_conf_rng(&conn->config, mbedtls_ctr_drbg_random, &conn->drbg);

  ret = mbedtls_net_connect(&conn->net, hostname, port,
                            MBEDTLS_NET_PROTO_TCP);
  if (ret != 0)
    {
      ret = -EHOSTUNREACH;
      goto err;
    }

  ret = mbedtls_ssl_setup(&conn->ssl, &conn->config);
  if (ret != 0)
    {
      ret = -EIO;
      goto err;
    }

  ret = mbedtls_ssl_set_hostname(&conn->ssl, hostname);
  if (ret != 0)
    {
      ret = -EINVAL;
      goto err;
    }

  mbedtls_ssl_set_bio(&conn->ssl, &conn->net, mbedtls_net_send,
                      mbedtls_net_recv, NULL);
  do
    {
      ret = mbedtls_ssl_handshake(&conn->ssl);
    }
  while (ret == MBEDTLS_ERR_SSL_WANT_READ ||
         ret == MBEDTLS_ERR_SSL_WANT_WRITE);

  if (ret != 0)
    {
      ret = -EIO;
      goto err;
    }

  verify = mbedtls_ssl_get_verify_result(&conn->ssl);
  if (verify != 0)
    {
      ret = -EACCES;
      goto err;
    }

  *connp = conn;
  return 0;

err:
  mbedtls_x509_crt_free(&conn->ca);
  mbedtls_ctr_drbg_free(&conn->drbg);
  mbedtls_entropy_free(&conn->entropy);
  mbedtls_ssl_config_free(&conn->config);
  mbedtls_ssl_free(&conn->ssl);
  mbedtls_net_free(&conn->net);
  free(conn);
  return ret;
}

static ssize_t https_send(void *ctx, struct webclient_tls_connection *conn,
                          const void *buffer, size_t length)
{
  int ret;

  (void)ctx;
  ret = mbedtls_ssl_write(&conn->ssl, buffer, length);
  if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
    {
      return -EAGAIN;
    }

  return ret < 0 ? -EIO : ret;
}

static ssize_t https_recv(void *ctx, struct webclient_tls_connection *conn,
                          void *buffer, size_t length)
{
  int ret;

  (void)ctx;
  ret = mbedtls_ssl_read(&conn->ssl, buffer, length);
  if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
    {
      return -EAGAIN;
    }

  return ret < 0 ? -EIO : ret;
}

static int https_close(void *ctx, struct webclient_tls_connection *conn)
{
  (void)ctx;
  (void)mbedtls_ssl_close_notify(&conn->ssl);
  mbedtls_x509_crt_free(&conn->ca);
  mbedtls_ctr_drbg_free(&conn->drbg);
  mbedtls_entropy_free(&conn->entropy);
  mbedtls_ssl_config_free(&conn->config);
  mbedtls_ssl_free(&conn->ssl);
  mbedtls_net_free(&conn->net);
  free(conn);
  return 0;
}

static int https_poll(void *ctx, struct webclient_tls_connection *conn,
                      struct webclient_poll_info *info)
{
  (void)ctx;
  info->fd = conn->net.fd;
  info->flags = WEBCLIENT_POLL_INFO_WANT_READ |
                WEBCLIENT_POLL_INFO_WANT_WRITE;
  return 0;
}

static const struct webclient_tls_ops g_https_ops =
{
  .connect = https_connect,
  .send = https_send,
  .recv = https_recv,
  .close = https_close,
  .get_poll_info = https_poll,
  .init_connection = NULL,
};

void home_panel_https_configure(struct webclient_context *context)
{
  context->tls_ops = &g_https_ops;
  context->tls_ctx = NULL;
}
