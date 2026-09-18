/****************************************************************************
 * D13x home panel HTTPS transport
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <pthread.h>
#include <syslog.h>

#include <nuttx/arch.h>
#include <arch/board/board.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include "home_panel_https.h"

#define D13X_CE_BASE             0x10020000u
#define D13X_CMU_BASE            0x18020000u
#define D13X_CMU_CLK_CE          (D13X_CMU_BASE + 0x0418)
#define D13X_CE_CLK_BUS_EN       (1u << 12)
#define D13X_CE_CLK_MOD_RSTN     (1u << 13)
#define D13X_CE_CLK_MOD_EN       (1u << 8)
#define getreg32(address)        (*(volatile uint32_t *)(address))
#define putreg32(value, address) (*(volatile uint32_t *)(address) = (value))
#define D13X_CE_TSK_ADDR        (D13X_CE_BASE + 0x008)
#define D13X_CE_TSK_CTL         (D13X_CE_BASE + 0x00c)
#define D13X_CE_TSK_STA         (D13X_CE_BASE + 0x010)
#define D13X_CE_TSK_ERR         (D13X_CE_BASE + 0x014)
#define D13X_CE_IRQ_CTL         (D13X_CE_BASE + 0x000)
#define D13X_CE_IRQ_STA         (D13X_CE_BASE + 0x004)
#define D13X_CE_HASH_CHANNEL     (1u << 1)
#define D13X_CE_TRNG_TAG         0x50
#define D13X_CE_TRNG_BUSY        0x50
#define D13X_CE_TRNG_IDLE        0xff
#define D13X_CE_TASK_LOAD        (1u << 31)
#define D13X_CE_TRNG_TIMEOUT_US  100000

/* Diagnostic register map: CE version plus the TRNG sub-block at CE+0x5000.
 * These reads are evidence-gathering only; they do not configure hardware. */

#define D13X_CE_VER              (D13X_CE_BASE + 0xffc)
#define D13X_TRNG_BASE           (D13X_CE_BASE + 0x5000)
#define D13X_TRNG_CR             (D13X_TRNG_BASE + 0x000)
#define D13X_TRNG_MSEL           (D13X_TRNG_BASE + 0x004)
#define D13X_TRNG_SR             (D13X_TRNG_BASE + 0x008)
#define D13X_TRNG_DR             (D13X_TRNG_BASE + 0x00c)
#define D13X_TRNG_RESEED         (D13X_TRNG_BASE + 0x010)
#define D13X_TRNG_RO_CLK_EN      (D13X_TRNG_BASE + 0x014)
#define D13X_TRNG_RO_SRC_EN1     (D13X_TRNG_BASE + 0x018)
#define D13X_TRNG_RO_SRC_EN2     (D13X_TRNG_BASE + 0x01c)
#define D13X_TRNG_VERSION        (D13X_TRNG_BASE + 0x030)
#define D13X_CE_SHA256_TAG       0x42

struct webclient_tls_connection
{
  mbedtls_net_context net;
  mbedtls_ssl_context ssl;
  mbedtls_ssl_config config;
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context drbg;
  mbedtls_x509_crt ca;
  unsigned int idle_timeout_sec;
  time_t recv_deadline;
  char host[96];
  unsigned int port;
};

/* TLS session cache shared by all Mijia API requests.  Reusing the session
 * turns the per-request full handshake (ECDSA verification + ECDHE, done in
 * software) into an abbreviated handshake, which matters a lot when the
 * server is reached over a high-latency tunnel. */

static pthread_mutex_t g_tls_session_lock = PTHREAD_MUTEX_INITIALIZER;
static mbedtls_ssl_session g_tls_session;
static bool g_tls_session_ready;

static void d13x_tls_session_save(mbedtls_ssl_context *ssl)
{
  pthread_mutex_lock(&g_tls_session_lock);
  if (g_tls_session_ready)
    {
      mbedtls_ssl_session_free(&g_tls_session);
      g_tls_session_ready = false;
    }

  mbedtls_ssl_session_init(&g_tls_session);
  if (mbedtls_ssl_get_session(ssl, &g_tls_session) == 0)
    {
      g_tls_session_ready = true;
    }

  pthread_mutex_unlock(&g_tls_session_lock);
}

static void d13x_tls_session_apply(mbedtls_ssl_context *ssl)
{
  pthread_mutex_lock(&g_tls_session_lock);
  if (g_tls_session_ready)
    {
      (void)mbedtls_ssl_set_session(ssl, &g_tls_session);
    }

  pthread_mutex_unlock(&g_tls_session_lock);
}

/* Keep-alive connection pool: one idle TLS connection is retained so the next
 * request to the same host reuses it instead of paying a new handshake.
 */

static pthread_mutex_t g_conn_pool_lock = PTHREAD_MUTEX_INITIALIZER;
static struct webclient_tls_connection *g_conn_pool;
static char g_conn_pool_host[96];
static unsigned int g_conn_pool_port;

static bool d13x_tls_conn_alive(int fd)
{
  char probe;
  ssize_t r = recv(fd, &probe, 1, MSG_PEEK | MSG_DONTWAIT);

  if (r == 0)
    {
      return false;
    }

  if (r < 0)
    {
      return errno == EAGAIN || errno == EWOULDBLOCK;
    }

  /* Pending bytes are expected on a TLS socket (for example a session ticket
   * queued by the peer right after the handshake), so r > 0 means the
   * connection is still usable.
   */

  return true;
}

static void d13x_tls_conn_free(struct webclient_tls_connection *conn)
{
  mbedtls_x509_crt_free(&conn->ca);
  mbedtls_ctr_drbg_free(&conn->drbg);
  mbedtls_entropy_free(&conn->entropy);
  mbedtls_ssl_config_free(&conn->config);
  mbedtls_ssl_free(&conn->ssl);
  mbedtls_net_free(&conn->net);
  free(conn);
}

struct d13x_ce_task_s
{
  uint32_t words[16];
} __attribute__((aligned(64)));

#define D13X_CE_TRNG_OUTLEN 32u

/* The CE hardware engine can only fetch descriptors from SRAM, so the task
 * and its buffers must come from the SRAM heap (up_allocate_heap), not from
 * the PSRAM image.  Allocations are 64-byte aligned for cache maintenance. */

static struct d13x_ce_task_s *g_trng_task;
static uint8_t *g_trng_output;
static bool g_trng_enabled;
static bool g_trng_task_dumped;

static void *d13x_ce_alloc(size_t size)
{
  void *raw = malloc(size + 64u);

  if (raw == NULL)
    {
      return NULL;
    }

  return (void *)(((uintptr_t)raw + 63u) & ~(uintptr_t)63u);
}

struct d13x_trng_case
{
  const char *name;
  uint32_t info;
  uint32_t total;
  uint32_t in_valid;
  uint32_t in_len;
  uint32_t out_len;
};

static void d13x_ce_probe(void)
{
  static const uint32_t sha256_iv[8] =
  {
    0x67e6096au, 0x85ae67bbu, 0x72f36e3cu, 0x3af54fa5u,
    0x7f520e51u, 0x8c68059bu, 0xabd9831fu, 0x19cde05bu
  };
  static const struct d13x_trng_case cases[] =
  {
    { "t0", 0x0u, 32u, 1u, 0u, 32u },
    { "t1", 0x1u, 32u, 1u, 0u, 32u },
    { "t2", 0x2u, 32u, 1u, 0u, 32u },
    { "t3", 0x3u, 32u, 1u, 0u, 32u },
  };
  void *raw;
  uint32_t *task;
  uint8_t *in;
  uint8_t *out;
  uint32_t *iv;
  uint32_t i;
  uint32_t j;
  uint32_t ter;

  raw = malloc(512u + 64u);
  if (raw == NULL)
    {
      syslog(LOG_ERR, "[HOME][HTTPS] CE probe alloc failed\n");
      return;
    }

  task = (uint32_t *)(((uintptr_t)raw + 63u) & ~(uintptr_t)63u);
  in = (uint8_t *)task + 64u;
  out = (uint8_t *)task + 128u;
  iv = (uint32_t *)((uint8_t *)task + 192u);

  syslog(LOG_INFO,
         "[HOME][HTTPS] CE probe ver=%08lx icr=%08lx isr=%08lx tsr=%08lx ter=%08lx\n",
         (unsigned long)getreg32(D13X_CE_VER),
         (unsigned long)getreg32(D13X_CE_IRQ_CTL),
         (unsigned long)getreg32(D13X_CE_IRQ_STA),
         (unsigned long)getreg32(D13X_CE_TSK_STA),
         (unsigned long)getreg32(D13X_CE_TSK_ERR));
  syslog(LOG_INFO,
         "[HOME][HTTPS] TRNG probe ver=%08lx cr=%08lx msel=%08lx sr=%08lx clk=%08lx src1=%08lx src2=%08lx dr=%08lx\n",
         (unsigned long)getreg32(D13X_TRNG_VERSION),
         (unsigned long)getreg32(D13X_TRNG_CR),
         (unsigned long)getreg32(D13X_TRNG_MSEL),
         (unsigned long)getreg32(D13X_TRNG_SR),
         (unsigned long)getreg32(D13X_TRNG_RO_CLK_EN),
         (unsigned long)getreg32(D13X_TRNG_RO_SRC_EN1),
         (unsigned long)getreg32(D13X_TRNG_RO_SRC_EN2),
         (unsigned long)getreg32(D13X_TRNG_DR));

  /* Try to enable the CE+0x5000 TRNG ring-oscillator sources.  If the block
   * is real these writes stick; if it is not decoded they alias CE registers
   * and are harmless. */
  putreg32(0xffffffffu, D13X_TRNG_RO_CLK_EN);
  putreg32(0xffffffffu, D13X_TRNG_RO_SRC_EN1);
  putreg32(0xffffffffu, D13X_TRNG_RO_SRC_EN2);
  putreg32(0x1u, D13X_TRNG_RESEED);
  syslog(LOG_INFO,
         "[HOME][HTTPS] TRNG ena clk=%08lx src1=%08lx src2=%08lx cr=%08lx sr=%08lx\n",
         (unsigned long)getreg32(D13X_TRNG_RO_CLK_EN),
         (unsigned long)getreg32(D13X_TRNG_RO_SRC_EN1),
         (unsigned long)getreg32(D13X_TRNG_RO_SRC_EN2),
         (unsigned long)getreg32(D13X_TRNG_CR),
         (unsigned long)getreg32(D13X_TRNG_SR));

  /* SHA256 self-test: probe the IV-flag bit position (bit9 per the header,
   * bit11 per the Message Digest descriptor doc) and the no-IV case. */
  {
    static const struct
    {
      const char *name;
      uint32_t cfg;
      uint32_t with_iv;
    } sc[] =
    {
      { "iv9",  D13X_CE_SHA256_TAG | (1u << 9),  1u },
      { "iv11", D13X_CE_SHA256_TAG | (1u << 11), 1u },
      { "noiv", D13X_CE_SHA256_TAG,              0u },
    };
    uint32_t c;

    for (c = 0; c < (uint32_t)(sizeof(sc) / sizeof(sc[0])); c++)
      {
        memset(task, 0, 64u);
        memset(in, 0x42, 16u);
        memcpy(iv, sha256_iv, sizeof(sha256_iv));
        task[0] = sc[c].cfg;
        task[2] = sc[c].with_iv ? (uint32_t)(uintptr_t)iv : 0u;
        task[9] = 0x2u;
        task[10] = 16u;
        task[11] = (uint32_t)(uintptr_t)in;
        task[12] = 16u;
        task[13] = (uint32_t)(uintptr_t)iv;
        task[14] = 32u;
        up_flush_dcache((uintptr_t)task, (uintptr_t)task + 64u);
        up_flush_dcache((uintptr_t)in, (uintptr_t)in + 64u);
        up_flush_dcache((uintptr_t)iv, (uintptr_t)iv + 64u);
        putreg32(D13X_CE_HASH_CHANNEL, D13X_CE_IRQ_STA);
        putreg32(0x0000ff00u, D13X_CE_TSK_ERR);
        putreg32((uint32_t)(uintptr_t)task, D13X_CE_TSK_ADDR);
        putreg32(D13X_CE_TASK_LOAD | D13X_CE_SHA256_TAG, D13X_CE_TSK_CTL);
        for (i = 0; i < 200000u; i++)
          {
            ter = getreg32(D13X_CE_TSK_ERR);
            if (((ter >> 8) & 0xffu) != 0 ||
                (getreg32(D13X_CE_IRQ_STA) & D13X_CE_HASH_CHANNEL) != 0)
              {
                break;
              }
          }
        putreg32(D13X_CE_HASH_CHANNEL, D13X_CE_IRQ_STA);
        up_invalidate_dcache((uintptr_t)iv, (uintptr_t)iv + 64u);
        syslog(LOG_INFO,
               "[HOME][HTTPS] SHA256 %s iter=%lu ter=%08lx tsr=%08lx isr=%08lx d0=%08lx d1=%08lx\n",
               sc[c].name, (unsigned long)i, (unsigned long)ter,
               (unsigned long)getreg32(D13X_CE_TSK_STA),
               (unsigned long)getreg32(D13X_CE_IRQ_STA),
               (unsigned long)iv[0], (unsigned long)iv[1]);
      }
  }

  for (i = 0; i < (uint32_t)(sizeof(cases) / sizeof(cases[0])); i++)
    {
      memset(task, 0, 64u);
      memset(out, 0, 32u);
      task[0] = D13X_CE_TRNG_TAG;
      task[9] = cases[i].info;
      task[10] = cases[i].total;
      task[11] = cases[i].in_valid ? (uint32_t)(uintptr_t)in : 0u;
      task[12] = cases[i].in_len;
      task[13] = (uint32_t)(uintptr_t)out;
      task[14] = cases[i].out_len;
      up_flush_dcache((uintptr_t)task, (uintptr_t)task + 64u);
      up_flush_dcache((uintptr_t)in, (uintptr_t)in + 64u);
      up_flush_dcache((uintptr_t)out, (uintptr_t)out + 64u);
      putreg32(D13X_CE_HASH_CHANNEL, D13X_CE_IRQ_STA);
      putreg32(0x0000ff00u, D13X_CE_TSK_ERR);
      putreg32((uint32_t)(uintptr_t)task, D13X_CE_TSK_ADDR);
      putreg32(D13X_CE_TASK_LOAD | D13X_CE_TRNG_TAG, D13X_CE_TSK_CTL);
      for (j = 0; j < 1000000u; j++)
        {
          ter = getreg32(D13X_CE_TSK_ERR);
          if (((ter >> 8) & 0xffu) != 0 ||
              (getreg32(D13X_CE_IRQ_STA) & D13X_CE_HASH_CHANNEL) != 0)
            {
              break;
            }
        }
      putreg32(D13X_CE_HASH_CHANNEL, D13X_CE_IRQ_STA);
      up_invalidate_dcache((uintptr_t)out, (uintptr_t)out + 64u);
      syslog(LOG_INFO,
             "[HOME][HTTPS] TRNG try %s iter=%lu ter=%08lx tsr=%08lx isr=%08lx d=%08lx%08lx%08lx%08lx\n",
             cases[i].name, (unsigned long)j, (unsigned long)ter,
             (unsigned long)getreg32(D13X_CE_TSK_STA),
             (unsigned long)getreg32(D13X_CE_IRQ_STA),
             (unsigned long)((uint32_t *)out)[0],
             (unsigned long)((uint32_t *)out)[1],
             (unsigned long)((uint32_t *)out)[2],
             (unsigned long)((uint32_t *)out)[3]);
    }

  free(raw);
}

static void d13x_trng_enable(void)
{
  uint32_t value;

  if (g_trng_enabled)
    {
      return;
    }

  value = getreg32(D13X_CMU_CLK_CE);
  putreg32(value | D13X_CE_CLK_BUS_EN | D13X_CE_CLK_MOD_EN,
           D13X_CMU_CLK_CE);
  up_udelay(2);
  value = getreg32(D13X_CMU_CLK_CE);
  putreg32(value | D13X_CE_CLK_MOD_RSTN, D13X_CMU_CLK_CE);
  up_udelay(2);
  /* Match the ArtInChip CE initialization: enable all completion channels
   * and clear stale completion/error bits before submitting a TRNG task. */
  putreg32(0x7u, D13X_CE_IRQ_CTL);
  putreg32(0xfu, D13X_CE_IRQ_STA);
  putreg32(0xffffffffu, D13X_CE_TSK_ERR);
  syslog(LOG_INFO, "[HOME][HTTPS] CE enabled clk=%08lx sta=%08lx irq=%08lx\n",
         (unsigned long)getreg32(D13X_CMU_CLK_CE),
         (unsigned long)getreg32(D13X_CE_TSK_STA),
         (unsigned long)getreg32(D13X_CE_IRQ_STA));
  d13x_ce_probe();

  g_trng_task = d13x_ce_alloc(sizeof(struct d13x_ce_task_s));
  g_trng_output = d13x_ce_alloc(D13X_CE_TRNG_OUTLEN);
  syslog(LOG_INFO, "[HOME][HTTPS] CE buffers task=%08lx out=%08lx\n",
         (unsigned long)(uintptr_t)g_trng_task,
         (unsigned long)(uintptr_t)g_trng_output);
  g_trng_enabled = true;
}

static int d13x_trng_poll(void *context, unsigned char *output, size_t length,
                          size_t *output_length)
{
  uint32_t status;
  uint32_t error;
  unsigned int elapsed;
  size_t copied = 0;

  (void)context;
  *output_length = 0;
  if (length == 0)
    {
      return 0;
    }

  /* Entropy is taken from the NuttX OS CSPRNG: getrandom() reads the
   * /dev/urandom random pool (CONFIG_DEV_URANDOM).  The D13x CE task engine
   * is not usable from this port, so the system pool is the working source. */

  {
    ssize_t n = getrandom(output, length, 0);

    if (n < 0)
      {
        syslog(LOG_ERR, "[HOME][HTTPS] getrandom failed errno=%d\n", errno);
        return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
      }

    *output_length = (size_t)n;
    return 0;
  }

  d13x_trng_enable();
  if (g_trng_task == NULL || g_trng_output == NULL)
    {
      syslog(LOG_ERR, "[HOME][HTTPS] CE TRNG buffer allocation failed\n");
      return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
    }

  while (copied < length)
    {
      size_t chunk = length - copied;
      if (chunk > D13X_CE_TRNG_OUTLEN)
        {
          chunk = D13X_CE_TRNG_OUTLEN;
        }

      memset(g_trng_task, 0, sizeof(struct d13x_ce_task_s));
      memset(g_trng_output, 0, D13X_CE_TRNG_OUTLEN);
      g_trng_task->words[0] = D13X_CE_TRNG_TAG;
      /* TRNG has no input stream; only the 256-bit output is described. */
      g_trng_task->words[9] = 0x3u;
      g_trng_task->words[13] = (uint32_t)(uintptr_t)g_trng_output;
      g_trng_task->words[14] = D13X_CE_TRNG_OUTLEN;
      up_flush_dcache((uintptr_t)g_trng_task,
                      (uintptr_t)g_trng_task + sizeof(struct d13x_ce_task_s));
      up_flush_dcache((uintptr_t)g_trng_output,
                      (uintptr_t)g_trng_output + D13X_CE_TRNG_OUTLEN);

      if (!g_trng_task_dumped)
        {
          g_trng_task_dumped = true;
          syslog(LOG_INFO,
                 "[HOME][HTTPS] TRNG task w0=%08lx w9=%08lx w10=%08lx w11=%08lx w12=%08lx w13=%08lx w14=%08lx w15=%08lx\n",
                 (unsigned long)g_trng_task->words[0],
                 (unsigned long)g_trng_task->words[9],
                 (unsigned long)g_trng_task->words[10],
                 (unsigned long)g_trng_task->words[11],
                 (unsigned long)g_trng_task->words[12],
                 (unsigned long)g_trng_task->words[13],
                 (unsigned long)g_trng_task->words[14],
                 (unsigned long)g_trng_task->words[15]);
        }

      /* The task-status algorithm byte is not the completion indication on
       * this CE revision.  The vendor HAL uses the hash-channel IRQ status.
       */
      putreg32(D13X_CE_HASH_CHANNEL, D13X_CE_IRQ_STA);
      putreg32(0x0000ff00u, D13X_CE_TSK_ERR);
      putreg32((uint32_t)(uintptr_t)g_trng_task, D13X_CE_TSK_ADDR);
      putreg32(D13X_CE_TASK_LOAD | D13X_CE_TRNG_TAG, D13X_CE_TSK_CTL);

      for (elapsed = 0; elapsed < D13X_CE_TRNG_TIMEOUT_US; elapsed++)
        {
          status = getreg32(D13X_CE_TSK_STA);
          error = getreg32(D13X_CE_TSK_ERR);
          if (((error >> 8) & 0xffu) != 0)
            {
              putreg32(error & 0x0000ff00u, D13X_CE_TSK_ERR);
              syslog(LOG_ERR,
                     "[HOME][HTTPS] CE TRNG error=%08lx status=%08lx\n",
                     (unsigned long)error, (unsigned long)status);
              return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
            }

          if ((getreg32(D13X_CE_IRQ_STA) & D13X_CE_HASH_CHANNEL) != 0)
            {
              putreg32(D13X_CE_HASH_CHANNEL, D13X_CE_IRQ_STA);
              up_invalidate_dcache((uintptr_t)g_trng_output,
                                   (uintptr_t)g_trng_output +
                                   D13X_CE_TRNG_OUTLEN);
              memcpy(output + copied, g_trng_output, chunk);
              copied += chunk;
              break;
            }

          up_udelay(1);
        }

      if (elapsed == D13X_CE_TRNG_TIMEOUT_US)
        {
          syslog(LOG_ERR, "[HOME][HTTPS] CE TRNG timeout status=%08lx\n",
                 (unsigned long)status);
          return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
        }
    }

  *output_length = copied;
  return 0;
}

static const char g_gts_root_r4[] =
  "-----BEGIN CERTIFICATE-----\n"
  "MIIDejCCAmKgAwIBAgIQf+UwvzMTQ77dghYQST2KGzANBgkqhkiG9w0BAQsFADBX\n"
  "MQswCQYDVQQGEwJCRTEZMBcGA1UEChMQR2xvYmFsU2lnbiBudi1zYTEQMA4GA1UE\n"
  "CxMHUm9vdCBDQTEbMBkGA1UEAxMSR2xvYmFsU2lnbiBSb290IENBMB4XDTIzMTEx\n"
  "NTAzNDMyMVoXDTI4MDEyODAwMDA0MlowRzELMAkGA1UEBhMCVVMxIjAgBgNVBAoT\n"
  "GUdvb2dsZSBUcnVzdCBTZXJ2aWNlcyBMTEMxFDASBgNVBAMTC0dUUyBSb290IFI0\n"
  "MHYwEAYHKoZIzj0CAQYFK4EEACIDYgAE83Rzp2iLYK5DuDXFgTB7S0md+8Fhzube\n"
  "Rr1r1WEYNa5A3XP3iZEwWus87oV8okB2O6nGuEfYKueSkWpz6bFyOZ8pn6KY019e\n"
  "WIZlD6GEZQbR3IvJx3PIjGov5cSr0R2Ko4H/MIH8MA4GA1UdDwEB/wQEAwIBhjAd\n"
  "BgNVHSUEFjAUBggrBgEFBQcDAQYIKwYBBQUHAwIwDwYDVR0TAQH/BAUwAwEB/zAd\n"
  "BgNVHQ4EFgQUgEzW63T/STaj1dj8tT7FavCUHYwwHwYDVR0jBBgwFoAUYHtmGkUN\n"
  "l8qJUC99BM00qP/8/UswNgYIKwYBBQUHAQEEKjAoMCYGCCsGAQUFBzAChhpodHRw\n"
  "Oi8vaS5wa2kuZ29vZy9nc3IxLmNydDAtBgNVHR8EJjAkMCKgIKAehhxodHRwOi8v\n"
  "Yy5wa2kuZ29vZy9yL2dzcjEuY3JsMBMGA1UdIAQMMAowCAYGZ4EMAQIBMA0GCSqG\n"
  "SIb3DQEBCwUAA4IBAQAYQrsPBtYDh5bjP2OBDwmkoWhIDDkic574y04tfzHpn+cJ\n"
  "odI2D4SseesQ6bDrarZ7C30ddLibZatoKiws3UL9xnELz4ct92vID24FfVbiI1hY\n"
  "+SW6FoVHkNeWIP0GCbaM4C6uVdF5dTUsMVs/ZbzNnIdCp5Gxmx5ejvEau8otR/Cs\n"
  "kGN+hr/W5GvT1tMBjgWKZ1i4//emhA1JG1BbPzoLJQvyEotc03lXjTaCzv8mEbep\n"
  "8RqZ7a2CPsgRbuvTPBwcOMBBmuFeU88+FSBX6+7iP0il8b4Z0QFqIwwMHfs/L6K1\n"
  "vepuoxtGzi4CZ68zJpiq1UvSqTbFJjtbD4seiMHl\n"
  "-----END CERTIFICATE-----\n";

static int https_connect(void *ctx, const char *hostname, const char *port,
                         unsigned int timeout_second,
                         struct webclient_tls_connection **connp)
{
  struct webclient_tls_connection *conn;
  uint32_t verify;
  int ret;

  (void)ctx;

  {
    unsigned int want_port = (unsigned int)atoi(port);
    struct webclient_tls_connection *reuse = NULL;
    struct webclient_tls_connection *stale = NULL;

    pthread_mutex_lock(&g_conn_pool_lock);
    if (g_conn_pool != NULL)
      {
        if (g_conn_pool_port == want_port &&
            strcmp(g_conn_pool_host, hostname) == 0 &&
            d13x_tls_conn_alive(g_conn_pool->net.fd))
          {
            reuse = g_conn_pool;
          }
        else
          {
            stale = g_conn_pool;
          }

        g_conn_pool = NULL;
      }

    pthread_mutex_unlock(&g_conn_pool_lock);

    if (stale != NULL)
      {
        d13x_tls_conn_free(stale);
      }

    if (reuse != NULL)
      {
        reuse->idle_timeout_sec = timeout_second != 0 ? timeout_second : 60;
        reuse->recv_deadline = time(NULL) + (time_t)reuse->idle_timeout_sec;
        *connp = reuse;
        return 0;
      }
  }

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
  strncpy(conn->host, hostname, sizeof(conn->host) - 1);
  conn->port = (unsigned int)atoi(port);

  ret = mbedtls_entropy_add_source(&conn->entropy, d13x_trng_poll, NULL,
                                   32, MBEDTLS_ENTROPY_SOURCE_STRONG);
  if (ret != 0)
    {
      syslog(LOG_ERR, "[HOME][HTTPS] TRNG source registration failed mbedtls=%d\n",
             ret);
      ret = -EIO;
      goto err;
    }

  ret = mbedtls_ctr_drbg_seed(&conn->drbg, mbedtls_entropy_func,
                              &conn->entropy,
                              (const unsigned char *)"d13x-home-panel", 16);
  if (ret != 0)
    {
      syslog(LOG_ERR, "[HOME][HTTPS] drbg seed failed mbedtls=%d\n", ret);
      ret = -EIO;
      goto err;
    }

  ret = mbedtls_x509_crt_parse(&conn->ca,
                               (const unsigned char *)g_gts_root_r4,
                               sizeof(g_gts_root_r4));
  if (ret < 0)
    {
      syslog(LOG_ERR, "[HOME][HTTPS] CA parse failed mbedtls=%d\n", ret);
      ret = -EIO;
      goto err;
    }

  ret = mbedtls_ssl_config_defaults(&conn->config, MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT);
  if (ret != 0)
    {
      syslog(LOG_ERR, "[HOME][HTTPS] ssl config failed mbedtls=%d\n", ret);
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
      syslog(LOG_ERR, "[HOME][HTTPS] TCP connect failed host=%s mbedtls=%d\n",
             hostname, ret);
      ret = -EHOSTUNREACH;
      goto err;
    }

  /* The webclient applies SO_RCVTIMEO/SO_SNDTIMEO only to the plain sockets
   * it creates itself.  The TLS transport creates its own socket inside
   * mbedtls_net_connect(), so the inactivity timeout must be applied here or
   * mbedtls_ssl_read() can block forever on a stalled peer. */

  {
    struct timeval tv;

    conn->idle_timeout_sec = timeout_second != 0 ? timeout_second : 60;
    conn->recv_deadline = time(NULL) + (time_t)conn->idle_timeout_sec;

    /* Short receive timeout: on expiry mbedtls reports a receive error and
     * https_recv() retries until the overall idle deadline elapses.  Sends
     * (including the ones inside mbedtls_ssl_handshake(), which do not pass
     * through https_send()) get the full idle window because mbedtls turns a
     * blocking-socket timeout into a hard send error. */

    tv.tv_sec = 15;
    tv.tv_usec = 0;
    (void)setsockopt(conn->net.fd, SOL_SOCKET, SO_RCVTIMEO,
                     &tv, sizeof(tv));

    tv.tv_sec = (time_t)conn->idle_timeout_sec;
    (void)setsockopt(conn->net.fd, SOL_SOCKET, SO_SNDTIMEO,
                     &tv, sizeof(tv));
  }

  ret = mbedtls_ssl_setup(&conn->ssl, &conn->config);
  if (ret != 0)
    {
      syslog(LOG_ERR, "[HOME][HTTPS] ssl setup failed mbedtls=%d\n", ret);
      ret = -EIO;
      goto err;
    }

  ret = mbedtls_ssl_set_hostname(&conn->ssl, hostname);
  if (ret != 0)
    {
      syslog(LOG_ERR, "[HOME][HTTPS] SNI setup failed mbedtls=%d\n", ret);
      ret = -EINVAL;
      goto err;
    }

  mbedtls_ssl_set_bio(&conn->ssl, &conn->net, mbedtls_net_send,
                      mbedtls_net_recv, NULL);
  d13x_tls_session_apply(&conn->ssl);

  {
    clock_t hs_start = clock();

    do
      {
        ret = mbedtls_ssl_handshake(&conn->ssl);
      }
    while (ret == MBEDTLS_ERR_SSL_WANT_READ ||
           ret == MBEDTLS_ERR_SSL_WANT_WRITE);

    if (ret == 0)
      {
        syslog(LOG_INFO,
               "[HOME][HTTPS] handshake ok host=%s elapsed=%ldms\n",
               hostname,
               (long)((clock() - hs_start) * 1000 / CLOCKS_PER_SEC));
      }
  }

  if (ret != 0)
    {
      syslog(LOG_ERR, "[HOME][HTTPS] handshake failed host=%s mbedtls=%d\n",
             hostname, ret);
      ret = -EIO;
      goto err;
    }

  verify = mbedtls_ssl_get_verify_result(&conn->ssl);
  if (verify != 0)
    {
      syslog(LOG_ERR, "[HOME][HTTPS] certificate verify failed flags=%08lx\n",
             (unsigned long)verify);
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

  for (;;)
    {
      ret = mbedtls_ssl_write(&conn->ssl, buffer, length);
      if (ret > 0)
        {
          conn->recv_deadline = time(NULL) + (time_t)conn->idle_timeout_sec;
          return ret;
        }

      if (ret == MBEDTLS_ERR_SSL_WANT_READ ||
          ret == MBEDTLS_ERR_SSL_WANT_WRITE ||
          (ret == MBEDTLS_ERR_NET_SEND_FAILED &&
           (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT)))
        {
          if (time(NULL) < conn->recv_deadline)
            {
              continue;
            }

          return -EAGAIN;
        }

      return ret < 0 ? -EIO : ret;
    }
}

static ssize_t https_recv(void *ctx, struct webclient_tls_connection *conn,
                          void *buffer, size_t length)
{
  int ret;

  (void)ctx;

  for (;;)
    {
      ret = mbedtls_ssl_read(&conn->ssl, buffer, length);
      if (ret > 0)
        {
          conn->recv_deadline = time(NULL) + (time_t)conn->idle_timeout_sec;
          return ret;
        }

      /* A TLS close_notify means the peer finished sending; report it as EOF
       * so connection-close-delimited HTTP responses are accepted. */

      if (ret == 0 || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
        {
          return 0;
        }

      /* Inactivity on a blocking socket with SO_RCVTIMEO is reported either
       * as WANT_READ or as NET_RECV_FAILED with errno EAGAIN/ETIMEDOUT.
       * Retry until the overall idle deadline before giving up. */

      if (ret == MBEDTLS_ERR_SSL_WANT_READ ||
          ret == MBEDTLS_ERR_SSL_WANT_WRITE ||
          (ret == MBEDTLS_ERR_NET_RECV_FAILED &&
           (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT)))
        {
          if (time(NULL) < conn->recv_deadline)
            {
              continue;
            }

          syslog(LOG_WARNING, "[HOME][HTTPS] recv idle timeout mbedtls=%d\n",
                 ret);
          return -EAGAIN;
        }

      syslog(LOG_WARNING, "[HOME][HTTPS] recv mbedtls=%d errno=%d\n", ret,
             errno);
      return -EIO;
    }
}

static int https_close(void *ctx, struct webclient_tls_connection *conn)
{
  (void)ctx;

  /* Keep the session ticket, then return the connection to the pool instead of
   * tearing it down.  Do not send close_notify here: that closes the TLS
   * session and defeats reuse.
   */

  d13x_tls_session_save(&conn->ssl);

  if (d13x_tls_conn_alive(conn->net.fd))
    {
      pthread_mutex_lock(&g_conn_pool_lock);
      if (g_conn_pool == NULL)
        {
          g_conn_pool = conn;
          strncpy(g_conn_pool_host, conn->host, sizeof(g_conn_pool_host) - 1);
          g_conn_pool_host[sizeof(g_conn_pool_host) - 1] = '\0';
          g_conn_pool_port = conn->port;
          pthread_mutex_unlock(&g_conn_pool_lock);
          return 0;
        }

      pthread_mutex_unlock(&g_conn_pool_lock);
    }

  d13x_tls_conn_free(conn);
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
