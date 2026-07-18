/****************************************************************************
 * D13x Hengshan-Pi redundant persistent record storage
 ****************************************************************************/

#include <nuttx/config.h>

#include <arch/board/board.h>
#include <nuttx/arch.h>
#include <nuttx/clock.h>
#include <nuttx/mutex.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* The packed layout reserves 0x0c0000..0x0fffff for userid.  That partition
 * is intentionally absent from image.target, so firmware updates do not
 * erase it.  Keep the redundant records in its final two erase sectors.
 *
 * The original implementation used the final two sectors of data.  Retain
 * those addresses as a read-only migration source for boards updated without
 * reflashing the data component.
 */

#define D13X_PERSIST_SLOT0          0x000fe000u
#define D13X_PERSIST_SLOT1          0x000ff000u
#define D13X_PERSIST_LEGACY_SLOT0   0x00efe000u
#define D13X_PERSIST_LEGACY_SLOT1   0x00eff000u
#define D13X_PERSIST_PAGE_SIZE      256u
#define D13X_PERSIST_MAX_PAYLOAD    768u

#define D13X_PERSIST_MAGIC          0x44505231u /* DPR1 */
#define D13X_PERSIST_VERSION        1u

#define D13X_QSPI_BASE              0x10400000u
#define QSPI_CFG                    (D13X_QSPI_BASE + 0x004u)
#define QSPI_TCFG                   (D13X_QSPI_BASE + 0x008u)
#define QSPI_ICR                    (D13X_QSPI_BASE + 0x010u)
#define QSPI_ISTS                   (D13X_QSPI_BASE + 0x014u)
#define QSPI_FCTL                   (D13X_QSPI_BASE + 0x018u)
#define QSPI_FSTS                   (D13X_QSPI_BASE + 0x01cu)
#define QSPI_CCFG                   (D13X_QSPI_BASE + 0x024u)
#define QSPI_TBC                    (D13X_QSPI_BASE + 0x030u)
#define QSPI_TWC                    (D13X_QSPI_BASE + 0x034u)
#define QSPI_TMC                    (D13X_QSPI_BASE + 0x038u)
#define QSPI_TXD                    (D13X_QSPI_BASE + 0x200u)
#define QSPI_RXD                    (D13X_QSPI_BASE + 0x300u)

#define QSPI_CFG_ENABLE             (1u << 0)
#define QSPI_CFG_MASTER             (1u << 1)
#define QSPI_CFG_XIP                (1u << 2)
#define QSPI_CFG_RXFULL_STOP        (1u << 7)
#define QSPI_CFG_RESET              (1u << 31)

#define QSPI_TCFG_CS_POL_LOW        (1u << 2)
#define QSPI_TCFG_CS_VALID_CTL      (1u << 3)
#define QSPI_TCFG_CS_SOFTWARE       (1u << 6)
#define QSPI_TCFG_CS_LEVEL          (1u << 7)
#define QSPI_TCFG_DROP_INVALID      (1u << 8)
#define QSPI_TCFG_RX_DELAY_ONE      (1u << 11)
#define QSPI_TCFG_LSB_FIRST         (1u << 12)
#define QSPI_TCFG_RX_DELAY_NONE     (1u << 13)
#define QSPI_TCFG_TX_DELAY          (1u << 14)
#define QSPI_TCFG_START             (1u << 31)

#define QSPI_FCTL_RX_RESET          (1u << 15)
#define QSPI_FCTL_TX_RESET          (1u << 31)
#define QSPI_FSTS_RX_COUNT_MASK     0xffu
#define QSPI_FSTS_TX_COUNT_SHIFT    16
#define QSPI_FSTS_TX_COUNT_MASK     (0xffu << QSPI_FSTS_TX_COUNT_SHIFT)
#define QSPI_ISTS_DONE              (1u << 12)
#define QSPI_ISTS_FIFO_ERRORS       (0x0fu << 8)
#define QSPI_TMC_WIDTH_MASK         ((1u << 28) | (1u << 29))

#define SPINOR_CMD_READ             0x03u
#define SPINOR_CMD_READ_STATUS      0x05u
#define SPINOR_CMD_WRITE_ENABLE     0x06u
#define SPINOR_CMD_PAGE_PROGRAM     0x02u
#define SPINOR_CMD_SECTOR_ERASE     0x20u
#define SPINOR_STATUS_BUSY          0x01u
#define SPINOR_STATUS_WEL           0x02u

struct d13x_persist_header_s
{
  uint32_t magic;
  uint32_t version;
  uint32_t sequence;
  uint32_t length;
  uint32_t crc32;
  uint32_t reserved[3];
};

static bool g_qspi_ready;
static mutex_t g_qspi_lock = NXMUTEX_INITIALIZER;

static inline uint32_t d13x_getreg32(uintptr_t address)
{
  return *(volatile uint32_t *)address;
}

static inline void d13x_putreg32(uint32_t value, uintptr_t address)
{
  *(volatile uint32_t *)address = value;
}

static inline uint8_t d13x_getreg8(uintptr_t address)
{
  return *(volatile uint8_t *)address;
}

static inline void d13x_putreg8(uint8_t value, uintptr_t address)
{
  *(volatile uint8_t *)address = value;
}

#define getreg32 d13x_getreg32
#define putreg32 d13x_putreg32
#define getreg8  d13x_getreg8
#define putreg8  d13x_putreg8

static uint32_t d13x_crc32(const uint8_t *data, size_t length)
{
  uint32_t crc = 0xffffffffu;
  size_t i;
  unsigned int bit;

  for (i = 0; i < length; i++)
    {
      crc ^= data[i];
      for (bit = 0; bit < 8; bit++)
        {
          crc = (crc >> 1) ^ (0xedb88320u &
                              (uint32_t)-(int32_t)(crc & 1u));
        }
    }

  return ~crc;
}

static void d13x_qspi_initialize(void)
{
  uint32_t value;

  if (g_qspi_ready)
    {
      return;
    }

  /* tinySPL has already enabled the clock and configured the pins.  Reset
   * only the controller state and retain its known-good clock divider.
   */

  putreg32(QSPI_CFG_ENABLE | QSPI_CFG_MASTER | QSPI_CFG_RXFULL_STOP |
           QSPI_CFG_RESET, QSPI_CFG);
  value = getreg32(QSPI_CFG);
  value &= ~QSPI_CFG_XIP;
  value |= QSPI_CFG_ENABLE | QSPI_CFG_MASTER | QSPI_CFG_RXFULL_STOP;
  putreg32(value, QSPI_CFG);

  /* QSPI0 input is 100 MHz after tinySPL.  Divider-1 value 1 gives a
   * conservative 50 MHz for erase/program and is accepted by all supported
   * 3-byte-address SPI-NOR parts.
   */

  putreg32(1u << 8, QSPI_CCFG);

  value = getreg32(QSPI_TCFG);
  value &= ~((3u << 0) | QSPI_TCFG_CS_POL_LOW |
             QSPI_TCFG_CS_VALID_CTL | (3u << 4) |
             QSPI_TCFG_CS_SOFTWARE | QSPI_TCFG_CS_LEVEL |
             QSPI_TCFG_DROP_INVALID | QSPI_TCFG_RX_DELAY_ONE |
             QSPI_TCFG_LSB_FIRST | QSPI_TCFG_RX_DELAY_NONE |
             QSPI_TCFG_TX_DELAY | QSPI_TCFG_START);
  value |= QSPI_TCFG_CS_POL_LOW | QSPI_TCFG_CS_SOFTWARE |
           QSPI_TCFG_CS_LEVEL | QSPI_TCFG_DROP_INVALID |
           QSPI_TCFG_TX_DELAY;
  putreg32(value, QSPI_TCFG);

  value = getreg32(QSPI_TMC);
  value &= ~QSPI_TMC_WIDTH_MASK;
  putreg32(value, QSPI_TMC);
  putreg32(0, QSPI_ICR);
  putreg32(0xffffffffu, QSPI_ISTS);
  g_qspi_ready = true;
}

static void d13x_qspi_select(bool selected)
{
  uint32_t value = getreg32(QSPI_TCFG);

  if (selected)
    {
      value &= ~QSPI_TCFG_CS_LEVEL;
    }
  else
    {
      value |= QSPI_TCFG_CS_LEVEL;
    }

  putreg32(value, QSPI_TCFG);
}

static int d13x_qspi_wait_done(void)
{
  clock_t deadline = clock_systime_ticks() + MSEC2TICK(200);
  uint32_t status;

  for (;;)
    {
      status = getreg32(QSPI_ISTS);
      if ((status & QSPI_ISTS_FIFO_ERRORS) != 0)
        {
          putreg32(status, QSPI_ISTS);
          return -EIO;
        }

      if ((status & QSPI_ISTS_DONE) != 0)
        {
          putreg32(status, QSPI_ISTS);
          return 0;
        }

      if ((int32_t)(clock_systime_ticks() - deadline) >= 0)
        {
          return -ETIMEDOUT;
        }
    }
}

static int d13x_qspi_transfer(const uint8_t *tx, uint8_t *rx,
                              size_t length)
{
  clock_t deadline;
  size_t remaining = length;
  uint32_t count;
  uint32_t value;

  if (length == 0 || (tx == NULL && rx == NULL) || length > 0xffffffu)
    {
      return -EINVAL;
    }

  putreg32(QSPI_FCTL_RX_RESET | QSPI_FCTL_TX_RESET | (32u << 16) | 32u,
           QSPI_FCTL);
  putreg32(0xffffffffu, QSPI_ISTS);
  putreg32((uint32_t)length, QSPI_TBC);
  putreg32(tx != NULL ? (uint32_t)length : 0u, QSPI_TWC);
  value = getreg32(QSPI_TMC) & QSPI_TMC_WIDTH_MASK;
  if (tx != NULL)
    {
      value |= (uint32_t)length;
    }
  putreg32(value, QSPI_TMC);
  putreg32(getreg32(QSPI_TCFG) | QSPI_TCFG_START, QSPI_TCFG);

  deadline = clock_systime_ticks() + MSEC2TICK(200);
  while (remaining > 0)
    {
      if (tx != NULL)
        {
          count = (getreg32(QSPI_FSTS) & QSPI_FSTS_TX_COUNT_MASK) >>
                  QSPI_FSTS_TX_COUNT_SHIFT;
          count = 64u - count;
          if (count > remaining)
            {
              count = remaining;
            }
          while (count-- > 0)
            {
              putreg8(*tx++, QSPI_TXD);
              remaining--;
            }
        }
      else
        {
          count = getreg32(QSPI_FSTS) & QSPI_FSTS_RX_COUNT_MASK;
          if (count > remaining)
            {
              count = remaining;
            }
          while (count-- > 0)
            {
              *rx++ = getreg8(QSPI_RXD);
              remaining--;
            }
        }

      if (remaining > 0 &&
          (int32_t)(clock_systime_ticks() - deadline) >= 0)
        {
          return -ETIMEDOUT;
        }
    }

  return d13x_qspi_wait_done();
}

static int d13x_spinor_command(const uint8_t *command, size_t command_length,
                               uint8_t *data, size_t data_length)
{
  int ret;

  d13x_qspi_initialize();
  d13x_qspi_select(true);
  ret = d13x_qspi_transfer(command, NULL, command_length);
  if (ret == 0 && data != NULL && data_length > 0)
    {
      ret = d13x_qspi_transfer(NULL, data, data_length);
    }
  d13x_qspi_select(false);
  return ret;
}

static int d13x_spinor_write_command(const uint8_t *command,
                                     size_t command_length,
                                     const uint8_t *data,
                                     size_t data_length)
{
  int ret;

  d13x_qspi_initialize();
  d13x_qspi_select(true);
  ret = d13x_qspi_transfer(command, NULL, command_length);
  if (ret == 0 && data != NULL && data_length > 0)
    {
      ret = d13x_qspi_transfer(data, NULL, data_length);
    }
  d13x_qspi_select(false);
  return ret;
}

static int d13x_spinor_status(uint8_t *status)
{
  uint8_t command = SPINOR_CMD_READ_STATUS;

  return d13x_spinor_command(&command, 1, status, 1);
}

static int d13x_spinor_wait_ready(void)
{
  clock_t deadline = clock_systime_ticks() + SEC2TICK(3);
  uint8_t status;
  int ret;

  do
    {
      ret = d13x_spinor_status(&status);
      if (ret < 0 || (status & SPINOR_STATUS_BUSY) == 0)
        {
          return ret;
        }

      up_udelay(100);
    }
  while ((int32_t)(clock_systime_ticks() - deadline) < 0);

  return -ETIMEDOUT;
}

static int d13x_spinor_write_enable(void)
{
  uint8_t command = SPINOR_CMD_WRITE_ENABLE;
  uint8_t status;
  int ret;

  ret = d13x_spinor_write_command(&command, 1, NULL, 0);
  if (ret == 0)
    {
      ret = d13x_spinor_status(&status);
      if (ret == 0 && (status & SPINOR_STATUS_WEL) == 0)
        {
          ret = -EACCES;
        }
    }

  return ret;
}

static int d13x_spinor_read(uint32_t address, void *buffer, size_t length)
{
  uint8_t command[4] =
  {
    SPINOR_CMD_READ,
    (uint8_t)(address >> 16),
    (uint8_t)(address >> 8),
    (uint8_t)address
  };

  return d13x_spinor_command(command, sizeof(command), buffer, length);
}

int board_flash_read(uint32_t address, void *buffer, size_t length)
{
  int ret;

  if (buffer == NULL || length == 0 || address >= 0x01000000u ||
      length > 0x01000000u - address)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&g_qspi_lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = d13x_spinor_read(address, buffer, length);
  nxmutex_unlock(&g_qspi_lock);
  return ret;
}

static int d13x_spinor_erase(uint32_t address)
{
  uint8_t command[4] =
  {
    SPINOR_CMD_SECTOR_ERASE,
    (uint8_t)(address >> 16),
    (uint8_t)(address >> 8),
    (uint8_t)address
  };
  int ret;

  ret = d13x_spinor_write_enable();
  if (ret == 0)
    {
      ret = d13x_spinor_write_command(command, sizeof(command), NULL, 0);
    }

  return ret == 0 ? d13x_spinor_wait_ready() : ret;
}

static int d13x_spinor_program(uint32_t address, const void *buffer,
                               size_t length)
{
  const uint8_t *source = buffer;
  size_t chunk;
  uint8_t command[4];
  int ret;

  while (length > 0)
    {
      chunk = D13X_PERSIST_PAGE_SIZE -
              (address & (D13X_PERSIST_PAGE_SIZE - 1));
      if (chunk > length)
        {
          chunk = length;
        }

      command[0] = SPINOR_CMD_PAGE_PROGRAM;
      command[1] = (uint8_t)(address >> 16);
      command[2] = (uint8_t)(address >> 8);
      command[3] = (uint8_t)address;
      ret = d13x_spinor_write_enable();
      if (ret == 0)
        {
          ret = d13x_spinor_write_command(command, sizeof(command),
                                           source, chunk);
        }
      if (ret == 0)
        {
          ret = d13x_spinor_wait_ready();
        }
      if (ret < 0)
        {
          return ret;
        }

      address += chunk;
      source += chunk;
      length -= chunk;
    }

  return 0;
}

static bool d13x_sequence_newer(uint32_t first, uint32_t second)
{
  return (int32_t)(first - second) > 0;
}

static int d13x_read_slot(uint32_t address,
                          struct d13x_persist_header_s *header,
                          uint8_t *payload)
{
  int ret;

  ret = d13x_spinor_read(address, header, sizeof(*header));
  if (ret < 0)
    {
      return ret;
    }

  if (header->magic != D13X_PERSIST_MAGIC ||
      header->version != D13X_PERSIST_VERSION ||
      header->length == 0 || header->length > D13X_PERSIST_MAX_PAYLOAD)
    {
      return -ENOENT;
    }

  ret = d13x_spinor_read(address + sizeof(*header), payload,
                         header->length);
  if (ret < 0)
    {
      return ret;
    }

  return d13x_crc32(payload, header->length) == header->crc32 ?
         0 : -EBADMSG;
}

static int d13x_read_pair(uint32_t slot0, uint32_t slot1, void *buffer,
                          size_t capacity, size_t *length)
{
  struct d13x_persist_header_s headers[2];
  uint8_t payloads[2][D13X_PERSIST_MAX_PAYLOAD];
  int results[2];
  unsigned int selected;

  results[0] = d13x_read_slot(slot0, &headers[0], payloads[0]);
  results[1] = d13x_read_slot(slot1, &headers[1], payloads[1]);
  if (results[0] < 0 && results[1] < 0)
    {
      memset(payloads, 0, sizeof(payloads));
      return results[0] != -ENOENT ? results[0] : results[1];
    }

  selected = results[0] == 0 &&
             (results[1] < 0 ||
              d13x_sequence_newer(headers[0].sequence,
                                   headers[1].sequence)) ? 0 : 1;
  if (capacity < headers[selected].length)
    {
      memset(payloads, 0, sizeof(payloads));
      return -ENOSPC;
    }

  memcpy(buffer, payloads[selected], headers[selected].length);
  *length = headers[selected].length;
  memset(payloads, 0, sizeof(payloads));
  return 0;
}

int board_persist_read(void *buffer, size_t capacity, size_t *length)
{
  bool migrate = false;
  int ret;

  if (buffer == NULL || length == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&g_qspi_lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = d13x_read_pair(D13X_PERSIST_SLOT0, D13X_PERSIST_SLOT1, buffer,
                       capacity, length);
  if (ret < 0)
    {
      ret = d13x_read_pair(D13X_PERSIST_LEGACY_SLOT0,
                           D13X_PERSIST_LEGACY_SLOT1,
                           buffer, capacity, length);
      if (ret == 0)
        {
          migrate = true;
        }
    }

  nxmutex_unlock(&g_qspi_lock);

  /* Best-effort one-time migration into userid.  Do this after releasing the
   * QSPI lock because board_persist_write() takes the same lock.
   */

  if (migrate)
    {
      board_persist_write(buffer, *length);
    }

  return ret;
}

int board_persist_write(const void *buffer, size_t length)
{
  struct d13x_persist_header_s headers[2];
  uint8_t payload[D13X_PERSIST_MAX_PAYLOAD];
  uint8_t verify[D13X_PERSIST_MAX_PAYLOAD];
  uint32_t address;
  uint32_t sequence;
  int results[2];
  int ret;

  if (buffer == NULL || length == 0 || length > sizeof(payload))
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&g_qspi_lock);
  if (ret < 0)
    {
      return ret;
    }

  results[0] = d13x_read_slot(D13X_PERSIST_SLOT0, &headers[0], payload);
  results[1] = d13x_read_slot(D13X_PERSIST_SLOT1, &headers[1], payload);
  if (results[0] == 0 &&
      (results[1] < 0 || d13x_sequence_newer(headers[0].sequence,
                                             headers[1].sequence)))
    {
      address = D13X_PERSIST_SLOT1;
      sequence = headers[0].sequence + 1;
    }
  else if (results[1] == 0)
    {
      address = D13X_PERSIST_SLOT0;
      sequence = headers[1].sequence + 1;
    }
  else
    {
      address = D13X_PERSIST_SLOT0;
      sequence = 1;
    }

  memset(&headers[0], 0xff, sizeof(headers[0]));
  headers[0].magic = D13X_PERSIST_MAGIC;
  headers[0].version = D13X_PERSIST_VERSION;
  headers[0].sequence = sequence;
  headers[0].length = length;
  headers[0].crc32 = d13x_crc32(buffer, length);

  ret = d13x_spinor_erase(address);
  if (ret == 0)
    {
      ret = d13x_spinor_program(address + sizeof(headers[0]), buffer,
                                length);
    }
  if (ret == 0)
    {
      /* Program the validity header last.  A power loss cannot make an
       * incomplete payload supersede the previous valid slot.
       */
      ret = d13x_spinor_program(address, &headers[0], sizeof(headers[0]));
    }
  if (ret == 0)
    {
      ret = d13x_read_slot(address, &headers[1], verify);
    }
  if (ret == 0 &&
      (headers[1].sequence != sequence || headers[1].length != length ||
       memcmp(buffer, verify, length) != 0))
    {
      ret = -EIO;
    }

  memset(payload, 0, sizeof(payload));
  memset(verify, 0, sizeof(verify));
  nxmutex_unlock(&g_qspi_lock);
  return ret;
}
