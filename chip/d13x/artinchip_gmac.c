/****************************************************************************
 * vendor/artinchip/chips/d13x/artinchip_gmac.c
 *
 * D13x GMAC0 RMII driver for NuttX.
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <syslog.h>

#include <debug.h>
#include <nuttx/arch.h>
#include <nuttx/irq.h>
#include <nuttx/net/ethernet.h>
#include <nuttx/net/ip.h>
#include <nuttx/net/netdev.h>
#include <nuttx/net/pkt.h>
#include <nuttx/wdog.h>
#include <nuttx/wqueue.h>

#include "chip.h"
#include "include/irq.h"
#include "artinchip_gmac.h"

#define GMAC_REG(offset)             (D13X_GMAC0_BASE + (offset))

#define GMAC_MACCONF                 0x0000
#define GMAC_DMA0CONF                0x0004
#define GMAC_DMA0INTSTS              0x000c
#define GMAC_DMA0INTEN               0x0010
#define GMAC_MACTXFUNC               0x001c
#define GMAC_MACRXFUNC               0x0020
#define GMAC_TXDMA0CTL               0x0024
#define GMAC_RXDMA0CTL               0x0028
#define GMAC_MACFRMFLT               0x0040
#define GMAC_MACADDR0HIGH            0x0050
#define GMAC_MACADDR0LOW             0x0054
#define GMAC_MDIOCTL                 0x0090
#define GMAC_MDIODATA                0x0094
#define GMAC_TXDESCSTART             0x00b0
#define GMAC_RXDESCSTART             0x00b4
#define GMAC_VERSION                 0x0ffc

#define MACCONF_SWR                  (1u << 0)
#define MACCONF_SPEED_MASK           (3u << 1)
#define MACCONF_SPEED_10             (2u << 1)
#define MACCONF_SPEED_100            (3u << 1)
#define MACCONF_DUPLEX               (1u << 4)

#define MACTXFUNC_ENABLE             (1u << 0)
#define MACRXFUNC_ENABLE             (1u << 0)

#define DMA0CONF_AAL                 (1u << 26)
#define DMA0CONF_USP                 (1u << 25)
#define DMA0CONF_RXPBL_8             (8u << 19)
#define DMA0CONF_FIXED_BURST         (1u << 17)
#define DMA0CONF_PBLX8               (1u << 16)
#define DMA0CONF_PBL_8               (8u << 10)
#define DMA0CONF_ATDS                (1u << 9)
#define DMA0CONF_PRIORITY_3_1        (2u << 1)

#define DMAINT_NIS                   (1u << 16)
#define DMAINT_AIS                   (1u << 15)
#define DMAINT_FATAL                 (1u << 13)
#define DMAINT_RX_UNAVAILABLE        (1u << 7)
#define DMAINT_RX                    (1u << 6)
#define DMAINT_TX                    (1u << 0)
#define DMAINT_MASK                  (DMAINT_NIS | DMAINT_AIS | \
                                      DMAINT_FATAL | DMAINT_RX_UNAVAILABLE | \
                                      DMAINT_RX | DMAINT_TX)

#define TXDMA_POLL                   (1u << 7)
#define TXDMA_STORE_FORWARD          (1u << 5)
#define TXDMA_FLUSH                  (1u << 4)
#define TXDMA_START                  (1u << 0)
#define RXDMA_POLL                   (1u << 9)
#define RXDMA_STORE_FORWARD          (1u << 3)
#define RXDMA_START                  (1u << 0)

#define MDIO_PHY_SHIFT               11
#define MDIO_REG_SHIFT               6
#define MDIO_CLOCK_DIV102            (4u << 2)
#define MDIO_WRITE                   (1u << 1)
#define MDIO_BUSY                    (1u << 0)

#define DESC_OWN                     (1u << 31)
#define TXDESC_INTERRUPT             (1u << 30)
#define TXDESC_LAST                  (1u << 29)
#define TXDESC_FIRST                 (1u << 28)
#define TXDESC_CHAINED               (1u << 20)
#define RXDESC_LENGTH_MASK           0x3fff0000u
#define RXDESC_LENGTH_SHIFT          16
#define RXDESC_ERROR                 (1u << 15)
#define RXDESC_FIRST                 (1u << 9)
#define RXDESC_LAST                  (1u << 8)
#define RXDESC_CHAINED               (1u << 14)

#define MII_BMCR                     0
#define MII_BMSR                     1
#define MII_PHYID1                   2
#define MII_PHYID2                   3
#define MII_ADVERTISE                4
#define MII_LPA                      5
#define BMCR_RESET                   (1u << 15)
#define BMCR_ANENABLE                (1u << 12)
#define BMCR_ANRESTART               (1u << 9)
#define BMSR_LINK                    (1u << 2)
#define ADVERTISE_10HALF             (1u << 5)
#define ADVERTISE_10FULL             (1u << 6)
#define ADVERTISE_100HALF            (1u << 7)
#define ADVERTISE_100FULL            (1u << 8)

#define GPIO_GROUP_E                 4u
#define GPIO_GROUP_STRIDE            0x100u
#define GPIO_INPUT                   0x00u
#define GPIO_OUTPUT_CLEAR            0x10u
#define GPIO_OUTPUT_SET              0x14u
#define GPIO_PIN_CONFIG              0x80u
#define GPIO_FUNCTION_MASK           0x0fu
#define GPIO_DRIVE_MASK              (7u << 4)
#define GPIO_PULL_MASK               (3u << 8)
#define GPIO_DIRECTION_MASK          (3u << 16)
#define GPIO_FUNCTION_GPIO           1u
#define GPIO_FUNCTION_GMAC           2u
#define GPIO_DRIVE_LEVEL_3           (3u << 4)
#define GPIO_DIRECTION_INPUT         (1u << 16)
#define GPIO_DIRECTION_OUTPUT        (2u << 16)

#define CMU_CLK_OUT2                 (D13X_CMU_BASE + 0x00e8)
#define CMU_CLK_GMAC0                (D13X_CMU_BASE + 0x0440)
#define SYSCFG_GMAC0                 (D13X_SYSCFG_BASE + 0x0410)
#define SYSCFG_RMII_EXTCLK           (1u << 1)

#define GMAC_RX_COUNT                16
#define GMAC_TX_COUNT                2
#define GMAC_BUFFER_SIZE             1536
#define GMAC_CACHE_LINE              32
#define GMAC_LINK_POLL               SEC2TICK(1)
#define GMAC_LINK_WAIT_LOOPS          100

struct d13x_gmac_desc_s
{
  volatile uint32_t status;
  uint32_t size;
  uint32_t buffer;
  uint32_t next;
  uint32_t ext_status;
  uint32_t reserved1;
  uint32_t timestamp_low;
  uint32_t timestamp_high;
  uint32_t reserved2[8];
} __attribute__((aligned(GMAC_CACHE_LINE)));

struct d13x_gmac_s
{
  bool ifup;
  bool link;
  uint8_t phyaddr;
  uint8_t rxhead;
  uint8_t txhead;
  uint8_t txtail;
  uint8_t txpending;
  struct work_s irqwork;
  struct work_s pollwork;
  struct work_s linkwork;
  struct wdog_s linktimer;
  struct net_driver_s dev;
};

static struct d13x_gmac_s g_gmac;
static struct d13x_gmac_desc_s g_rxdesc[GMAC_RX_COUNT];
static struct d13x_gmac_desc_s g_txdesc[GMAC_TX_COUNT];
static uint8_t g_rxbuffer[GMAC_RX_COUNT][GMAC_BUFFER_SIZE]
  __attribute__((aligned(GMAC_CACHE_LINE)));
static uint8_t g_txbuffer[GMAC_TX_COUNT][GMAC_BUFFER_SIZE]
  __attribute__((aligned(GMAC_CACHE_LINE)));
static uint8_t g_netbuffer[MAX_NETDEV_PKTSIZE + CONFIG_NET_GUARDSIZE]
  __attribute__((aligned(GMAC_CACHE_LINE)));

#define BUF ((FAR struct eth_hdr_s *)g_gmac.dev.d_buf)

static void d13x_link_timer(wdparm_t arg);
static int d13x_txpoll(FAR struct net_driver_s *dev);

static void d13x_modifyreg32(uintptr_t address, uint32_t clearbits,
                             uint32_t setbits)
{
  putreg32((getreg32(address) & ~clearbits) | setbits, address);
}

static void d13x_cache_clean(const void *address, size_t length)
{
  uintptr_t current = (uintptr_t)address & ~(GMAC_CACHE_LINE - 1);
  uintptr_t end = ((uintptr_t)address + length + GMAC_CACHE_LINE - 1) &
                  ~(GMAC_CACHE_LINE - 1);

  __asm__ volatile("fence" ::: "memory");
  while (current < end)
    {
      register uintptr_t cache_address __asm__("a0") = current;
      __asm__ volatile(".long 0x0295000b" : "+r"(cache_address) :: "memory");
      current += GMAC_CACHE_LINE;
    }

  __asm__ volatile("fence\n\t"
                   "fence.i\n\t"
                   ".long 0x01a0000b" ::: "memory");
}

static void d13x_cache_invalidate(const void *address, size_t length)
{
  uintptr_t current = (uintptr_t)address & ~(GMAC_CACHE_LINE - 1);
  uintptr_t end = ((uintptr_t)address + length + GMAC_CACHE_LINE - 1) &
                  ~(GMAC_CACHE_LINE - 1);

  __asm__ volatile("fence" ::: "memory");
  while (current < end)
    {
      register uintptr_t cache_address __asm__("a0") = current;
      __asm__ volatile(".long 0x02a5000b" : "+r"(cache_address) :: "memory");
      current += GMAC_CACHE_LINE;
    }

  __asm__ volatile("fence\n\t"
                   "fence.i\n\t"
                   ".long 0x01a0000b" ::: "memory");
}

static uintptr_t d13x_gpio_reg(uint32_t offset)
{
  return D13X_GPIO_BASE + GPIO_GROUP_E * GPIO_GROUP_STRIDE + offset;
}

static void d13x_gpio_config(unsigned int pin, uint32_t function,
                             uint32_t direction)
{
  uintptr_t address = d13x_gpio_reg(GPIO_PIN_CONFIG + pin * 4);
  uint32_t value = getreg32(address);

  value &= ~(GPIO_FUNCTION_MASK | GPIO_DRIVE_MASK | GPIO_PULL_MASK |
             GPIO_DIRECTION_MASK);
  value |= function | GPIO_DRIVE_LEVEL_3 | direction;
  putreg32(value, address);
}

static void d13x_gmac_pinmux(void)
{
  static const uint8_t input_pins[] = {0, 1, 2, 3, 9};
  static const uint8_t output_pins[] = {4, 5, 7, 8, 10};
  unsigned int i;

  for (i = 0; i < sizeof(input_pins); i++)
    {
      d13x_gpio_config(input_pins[i], GPIO_FUNCTION_GMAC,
                       GPIO_DIRECTION_INPUT);
    }

  for (i = 0; i < sizeof(output_pins); i++)
    {
      d13x_gpio_config(output_pins[i], GPIO_FUNCTION_GMAC,
                       GPIO_DIRECTION_OUTPUT);
    }

  d13x_gpio_config(6, GPIO_FUNCTION_GPIO, GPIO_DIRECTION_OUTPUT);
}

static void d13x_gmac_clock_enable(void)
{
  uint32_t value;

  /* CLK_OUT2 = PLL_INT1 / 48 = 25 MHz, used as the PHY reference source. */

  putreg32((1u << 16) | (1u << 12) | 47u, CMU_CLK_OUT2);

  /* GMAC module clock = PLL_INT1 / 24 = 50 MHz. */

  value = getreg32(CMU_CLK_GMAC0);
  value &= ~0x1fu;
  value |= (1u << 13) | (1u << 12) | (1u << 8) | 23u;
  putreg32(value, CMU_CLK_GMAC0);
  d13x_modifyreg32(SYSCFG_GMAC0, 0, SYSCFG_RMII_EXTCLK);
}

static void d13x_phy_hard_reset(void)
{
  putreg32(1u << 6, d13x_gpio_reg(GPIO_OUTPUT_CLEAR));
  up_mdelay(50);
  putreg32(1u << 6, d13x_gpio_reg(GPIO_OUTPUT_SET));
  up_mdelay(50);
}

static int d13x_mdio_wait(void)
{
  unsigned int timeout;

  for (timeout = 0; timeout < 10000; timeout++)
    {
      if ((getreg32(GMAC_REG(GMAC_MDIOCTL)) & MDIO_BUSY) == 0)
        {
          return OK;
        }

      up_udelay(1);
    }

  return -ETIMEDOUT;
}

static int d13x_mdio_read(uint8_t phy, uint8_t reg, uint16_t *value)
{
  uint32_t command;
  int ret = d13x_mdio_wait();

  if (ret < 0)
    {
      return ret;
    }

  command = ((uint32_t)phy << MDIO_PHY_SHIFT) |
            ((uint32_t)reg << MDIO_REG_SHIFT) |
            MDIO_CLOCK_DIV102 | MDIO_BUSY;
  putreg32(command, GMAC_REG(GMAC_MDIOCTL));
  ret = d13x_mdio_wait();
  if (ret == OK)
    {
      *value = getreg32(GMAC_REG(GMAC_MDIODATA)) & 0xffff;
    }

  return ret;
}

static int d13x_mdio_write(uint8_t phy, uint8_t reg, uint16_t value)
{
  uint32_t command;
  int ret = d13x_mdio_wait();

  if (ret < 0)
    {
      return ret;
    }

  putreg32(value, GMAC_REG(GMAC_MDIODATA));
  command = ((uint32_t)phy << MDIO_PHY_SHIFT) |
            ((uint32_t)reg << MDIO_REG_SHIFT) |
            MDIO_CLOCK_DIV102 | MDIO_WRITE | MDIO_BUSY;
  putreg32(command, GMAC_REG(GMAC_MDIOCTL));
  return d13x_mdio_wait();
}

static int d13x_phy_find(void)
{
  uint16_t id1;
  uint16_t id2;
  unsigned int phy;

  for (phy = 0; phy < 32; phy++)
    {
      if (d13x_mdio_read(phy, MII_PHYID1, &id1) == OK &&
          d13x_mdio_read(phy, MII_PHYID2, &id2) == OK &&
          id1 != 0 && id1 != 0xffff && id2 != 0 && id2 != 0xffff)
        {
          g_gmac.phyaddr = phy;
          syslog(LOG_INFO, "[D13GMAC] PHY addr=%u id=%04x:%04x\n",
                 phy, id1, id2);
          return OK;
        }
    }

  return -ENODEV;
}

static void d13x_gmac_set_link(bool speed100, bool full_duplex)
{
  uint32_t value = getreg32(GMAC_REG(GMAC_MACCONF));

  value &= ~(MACCONF_SPEED_MASK | MACCONF_DUPLEX);
  value |= speed100 ? MACCONF_SPEED_100 : MACCONF_SPEED_10;
  if (full_duplex)
    {
      value |= MACCONF_DUPLEX;
    }

  putreg32(value, GMAC_REG(GMAC_MACCONF));
}

static void d13x_phy_update_link(void)
{
  uint16_t bmsr;
  uint16_t advertise;
  uint16_t partner;
  uint16_t common;
  bool link;
  bool speed100 = true;
  bool full_duplex = true;

  if (d13x_mdio_read(g_gmac.phyaddr, MII_BMSR, &bmsr) < 0 ||
      d13x_mdio_read(g_gmac.phyaddr, MII_BMSR, &bmsr) < 0)
    {
      return;
    }

  link = (bmsr & BMSR_LINK) != 0;
  if (link)
    {
      if (d13x_mdio_read(g_gmac.phyaddr, MII_ADVERTISE, &advertise) == OK &&
          d13x_mdio_read(g_gmac.phyaddr, MII_LPA, &partner) == OK)
        {
          common = advertise & partner;
          if ((common & ADVERTISE_100FULL) != 0)
            {
              speed100 = true;
              full_duplex = true;
            }
          else if ((common & ADVERTISE_100HALF) != 0)
            {
              speed100 = true;
              full_duplex = false;
            }
          else if ((common & ADVERTISE_10FULL) != 0)
            {
              speed100 = false;
              full_duplex = true;
            }
          else
            {
              speed100 = false;
              full_duplex = false;
            }
        }

      d13x_gmac_set_link(speed100, full_duplex);
    }

  if (link != g_gmac.link)
    {
      g_gmac.link = link;
      if (link)
        {
          netdev_carrier_on(&g_gmac.dev);
          syslog(LOG_INFO, "[D13GMAC] link up %uM %s duplex\n",
                 speed100 ? 100 : 10, full_duplex ? "full" : "half");
          devif_poll(&g_gmac.dev, d13x_txpoll);
        }
      else
        {
          netdev_carrier_off(&g_gmac.dev);
          syslog(LOG_WARNING, "[D13GMAC] link down\n");
        }
    }
}

static void d13x_link_work(FAR void *arg)
{
  (void)arg;
  if (g_gmac.ifup)
    {
      net_lock();
      d13x_phy_update_link();
      net_unlock();
      wd_start(&g_gmac.linktimer, GMAC_LINK_POLL, d13x_link_timer, 0);
    }
}

static void d13x_link_timer(wdparm_t arg)
{
  (void)arg;
  if (work_available(&g_gmac.linkwork))
    {
      work_queue(LPWORK, &g_gmac.linkwork, d13x_link_work, NULL, 0);
    }
}

static void d13x_desc_initialize(void)
{
  unsigned int i;

  memset(g_rxdesc, 0, sizeof(g_rxdesc));
  memset(g_txdesc, 0, sizeof(g_txdesc));
  for (i = 0; i < GMAC_RX_COUNT; i++)
    {
      g_rxdesc[i].size = RXDESC_CHAINED | GMAC_BUFFER_SIZE;
      g_rxdesc[i].buffer = (uintptr_t)g_rxbuffer[i];
      g_rxdesc[i].next = (uintptr_t)&g_rxdesc[(i + 1) % GMAC_RX_COUNT];
      g_rxdesc[i].status = DESC_OWN;
    }

  for (i = 0; i < GMAC_TX_COUNT; i++)
    {
      g_txdesc[i].status = TXDESC_CHAINED;
      g_txdesc[i].buffer = (uintptr_t)g_txbuffer[i];
      g_txdesc[i].next = (uintptr_t)&g_txdesc[(i + 1) % GMAC_TX_COUNT];
    }

  g_gmac.rxhead = 0;
  g_gmac.txhead = 0;
  g_gmac.txtail = 0;
  g_gmac.txpending = 0;
  d13x_cache_clean(g_rxdesc, sizeof(g_rxdesc));
  d13x_cache_clean(g_txdesc, sizeof(g_txdesc));
  d13x_cache_clean(g_rxbuffer, sizeof(g_rxbuffer));
  d13x_cache_clean(g_txbuffer, sizeof(g_txbuffer));
}

static int d13x_transmit(FAR struct d13x_gmac_s *priv)
{
  FAR struct d13x_gmac_desc_s *desc;
  unsigned int index;

  if (priv->dev.d_len == 0 || priv->dev.d_len > GMAC_BUFFER_SIZE ||
      priv->txpending >= GMAC_TX_COUNT)
    {
      return -EBUSY;
    }

  index = priv->txhead;
  desc = &g_txdesc[index];
  d13x_cache_invalidate(desc, sizeof(*desc));
  if ((desc->status & DESC_OWN) != 0)
    {
      return -EBUSY;
    }

  memcpy(g_txbuffer[index], priv->dev.d_buf, priv->dev.d_len);
  desc->size = priv->dev.d_len;
  desc->status = TXDESC_CHAINED | TXDESC_FIRST | TXDESC_LAST |
                 TXDESC_INTERRUPT | DESC_OWN;
  d13x_cache_clean(g_txbuffer[index], priv->dev.d_len);
  d13x_cache_clean(desc, sizeof(*desc));
  priv->txhead = (index + 1) % GMAC_TX_COUNT;
  priv->txpending++;
  NETDEV_TXPACKETS(priv->dev);

  putreg32(getreg32(GMAC_REG(GMAC_TXDMA0CTL)) | TXDMA_POLL,
           GMAC_REG(GMAC_TXDMA0CTL));
  priv->dev.d_len = 0;
  return OK;
}

static int d13x_txpoll(FAR struct net_driver_s *dev)
{
  FAR struct d13x_gmac_s *priv = dev->d_private;

  if (priv->txpending >= GMAC_TX_COUNT)
    {
      return 1;
    }

  return d13x_transmit(priv) < 0 ? 1 : 0;
}

static void d13x_reply(FAR struct d13x_gmac_s *priv)
{
  if (priv->dev.d_len > 0)
    {
      d13x_transmit(priv);
    }
}

static void d13x_receive(FAR struct d13x_gmac_s *priv)
{
  FAR struct d13x_gmac_desc_s *desc;
  uint32_t status;
  unsigned int length;

  for (;;)
    {
      desc = &g_rxdesc[priv->rxhead];
      d13x_cache_invalidate(desc, sizeof(*desc));
      status = desc->status;
      if ((status & DESC_OWN) != 0)
        {
          break;
        }

      length = (status & RXDESC_LENGTH_MASK) >> RXDESC_LENGTH_SHIFT;
      if (length >= 4)
        {
          length -= 4;
        }

      if ((status & (RXDESC_ERROR | RXDESC_FIRST | RXDESC_LAST)) ==
          (RXDESC_FIRST | RXDESC_LAST) && length <= MAX_NETDEV_PKTSIZE)
        {
          d13x_cache_invalidate(g_rxbuffer[priv->rxhead], length);
          memcpy(priv->dev.d_buf, g_rxbuffer[priv->rxhead], length);
          priv->dev.d_len = length;
          NETDEV_RXPACKETS(priv->dev);

#ifdef CONFIG_NET_PKT
          pkt_input(&priv->dev);
#endif
#ifdef CONFIG_NET_IPv4
          if (BUF->type == HTONS(ETHTYPE_IP))
            {
              NETDEV_RXIPV4(&priv->dev);
              ipv4_input(&priv->dev);
              d13x_reply(priv);
            }
          else
#endif
#ifdef CONFIG_NET_IPv6
          if (BUF->type == HTONS(ETHTYPE_IP6))
            {
              NETDEV_RXIPV6(&priv->dev);
              ipv6_input(&priv->dev);
              d13x_reply(priv);
            }
          else
#endif
#ifdef CONFIG_NET_ARP
          if (BUF->type == HTONS(ETHTYPE_ARP))
            {
              arp_input(&priv->dev);
              NETDEV_RXARP(&priv->dev);
              d13x_reply(priv);
            }
          else
#endif
            {
              NETDEV_RXDROPPED(&priv->dev);
            }
        }
      else
        {
          NETDEV_RXERRORS(priv->dev);
        }

      desc->status = DESC_OWN;
      d13x_cache_clean(desc, sizeof(*desc));
      priv->rxhead = (priv->rxhead + 1) % GMAC_RX_COUNT;
    }

  putreg32(getreg32(GMAC_REG(GMAC_RXDMA0CTL)) | RXDMA_POLL,
           GMAC_REG(GMAC_RXDMA0CTL));
}

static void d13x_txdone(FAR struct d13x_gmac_s *priv)
{
  FAR struct d13x_gmac_desc_s *desc;

  while (priv->txpending > 0)
    {
      desc = &g_txdesc[priv->txtail];
      d13x_cache_invalidate(desc, sizeof(*desc));
      if ((desc->status & DESC_OWN) != 0)
        {
          break;
        }

      NETDEV_TXDONE(priv->dev);
      priv->txtail = (priv->txtail + 1) % GMAC_TX_COUNT;
      priv->txpending--;
    }

  if (priv->txpending < GMAC_TX_COUNT)
    {
      devif_poll(&priv->dev, d13x_txpoll);
    }
}

static void d13x_interrupt_work(FAR void *arg)
{
  FAR struct d13x_gmac_s *priv = arg;
  uint32_t status;

  net_lock();
  status = getreg32(GMAC_REG(GMAC_DMA0INTSTS));
  putreg32(status, GMAC_REG(GMAC_DMA0INTSTS));

  if ((status & DMAINT_FATAL) != 0)
    {
      syslog(LOG_ERR, "[D13GMAC] fatal DMA status=%08lx\n",
             (unsigned long)status);
      NETDEV_RXERRORS(priv->dev);
    }

  if ((status & (DMAINT_RX | DMAINT_RX_UNAVAILABLE)) != 0)
    {
      d13x_receive(priv);
    }

  if ((status & DMAINT_TX) != 0)
    {
      d13x_txdone(priv);
    }

  net_unlock();
  up_enable_irq(D13X_IRQ_GMAC0);
}

static int d13x_interrupt(int irq, FAR void *context, FAR void *arg)
{
  FAR struct d13x_gmac_s *priv = arg;

  (void)irq;
  (void)context;
  up_disable_irq(D13X_IRQ_GMAC0);
  if (work_available(&priv->irqwork))
    {
      work_queue(LPWORK, &priv->irqwork, d13x_interrupt_work, priv, 0);
    }
  else
    {
      up_enable_irq(D13X_IRQ_GMAC0);
    }

  return OK;
}

static int d13x_ifup(FAR struct net_driver_s *dev)
{
  FAR struct d13x_gmac_s *priv = dev->d_private;
  unsigned int timeout;

  d13x_desc_initialize();
  putreg32((uintptr_t)g_txdesc, GMAC_REG(GMAC_TXDESCSTART));
  putreg32((uintptr_t)g_rxdesc, GMAC_REG(GMAC_RXDESCSTART));
  putreg32(DMAINT_MASK, GMAC_REG(GMAC_DMA0INTEN));
  putreg32(getreg32(GMAC_REG(GMAC_MACTXFUNC)) | MACTXFUNC_ENABLE,
           GMAC_REG(GMAC_MACTXFUNC));
  putreg32(getreg32(GMAC_REG(GMAC_MACRXFUNC)) | MACRXFUNC_ENABLE,
           GMAC_REG(GMAC_MACRXFUNC));
  putreg32(TXDMA_STORE_FORWARD | TXDMA_FLUSH,
           GMAC_REG(GMAC_TXDMA0CTL));
  for (timeout = 0; timeout < 100000; timeout++)
    {
      if ((getreg32(GMAC_REG(GMAC_TXDMA0CTL)) & TXDMA_FLUSH) == 0)
        {
          break;
        }
    }

  putreg32(TXDMA_STORE_FORWARD | TXDMA_START,
           GMAC_REG(GMAC_TXDMA0CTL));
  putreg32(RXDMA_STORE_FORWARD | RXDMA_START,
           GMAC_REG(GMAC_RXDMA0CTL));
  priv->ifup = true;
  priv->link = false;
  netdev_carrier_off(dev);
  up_enable_irq(D13X_IRQ_GMAC0);
  syslog(LOG_INFO, "[D13GMAC] eth0 up, waiting for carrier\n");

  /* The asynchronous netinit thread starts DHCP immediately after ifup.
   * Wait for RMII auto-negotiation here so its first DISCOVER is not lost
   * while the interface still has no carrier.
   */

  for (timeout = 0; timeout < GMAC_LINK_WAIT_LOOPS && !priv->link; timeout++)
    {
      d13x_phy_update_link();
      if (!priv->link)
        {
          up_mdelay(50);
        }
    }

  wd_start(&priv->linktimer, GMAC_LINK_POLL, d13x_link_timer, 0);
  return OK;
}

static int d13x_ifdown(FAR struct net_driver_s *dev)
{
  FAR struct d13x_gmac_s *priv = dev->d_private;

  up_disable_irq(D13X_IRQ_GMAC0);
  wd_cancel(&priv->linktimer);
  putreg32(0, GMAC_REG(GMAC_DMA0INTEN));
  d13x_modifyreg32(GMAC_REG(GMAC_MACTXFUNC), MACTXFUNC_ENABLE, 0);
  d13x_modifyreg32(GMAC_REG(GMAC_MACRXFUNC), MACRXFUNC_ENABLE, 0);
  d13x_modifyreg32(GMAC_REG(GMAC_TXDMA0CTL), TXDMA_START, 0);
  d13x_modifyreg32(GMAC_REG(GMAC_RXDMA0CTL), RXDMA_START, 0);
  priv->ifup = false;
  priv->link = false;
  netdev_carrier_off(dev);
  return OK;
}

static void d13x_txavail_work(FAR void *arg)
{
  FAR struct d13x_gmac_s *priv = arg;

  net_lock();
  if (priv->ifup && priv->link && priv->txpending < GMAC_TX_COUNT)
    {
      devif_poll(&priv->dev, d13x_txpoll);
    }

  net_unlock();
}

static int d13x_txavail(FAR struct net_driver_s *dev)
{
  FAR struct d13x_gmac_s *priv = dev->d_private;

  if (work_available(&priv->pollwork))
    {
      work_queue(LPWORK, &priv->pollwork, d13x_txavail_work, priv, 0);
    }

  return OK;
}

static int d13x_gmac_hardware_initialize(void)
{
  uint16_t bmcr;
  uint32_t timeout;
  uint32_t value;
  int ret;

  d13x_gmac_clock_enable();
  d13x_gmac_pinmux();
  up_mdelay(1);

  d13x_modifyreg32(GMAC_REG(GMAC_MACCONF), 0, MACCONF_SWR);
  for (timeout = 0; timeout < 100000; timeout++)
    {
      if ((getreg32(GMAC_REG(GMAC_MACCONF)) & MACCONF_SWR) == 0)
        {
          break;
        }

      up_udelay(1);
    }

  if (timeout == 100000)
    {
      return -ETIMEDOUT;
    }

  d13x_phy_hard_reset();
  putreg32(MDIO_CLOCK_DIV102, GMAC_REG(GMAC_MDIOCTL));
  ret = d13x_phy_find();
  if (ret < 0)
    {
      return ret;
    }

  ret = d13x_mdio_write(g_gmac.phyaddr, MII_BMCR, BMCR_RESET);
  if (ret < 0)
    {
      return ret;
    }

  for (timeout = 0; timeout < 500; timeout++)
    {
      up_mdelay(1);
      if (d13x_mdio_read(g_gmac.phyaddr, MII_BMCR, &bmcr) == OK &&
          (bmcr & BMCR_RESET) == 0)
        {
          break;
        }
    }

  if (timeout == 500)
    {
      return -ETIMEDOUT;
    }

  ret = d13x_mdio_write(g_gmac.phyaddr, MII_BMCR,
                        BMCR_ANENABLE | BMCR_ANRESTART);
  if (ret < 0)
    {
      return ret;
    }

  value = MACCONF_SPEED_100 | MACCONF_DUPLEX;
  putreg32(value, GMAC_REG(GMAC_MACCONF));
  putreg32(DMA0CONF_AAL | DMA0CONF_USP | DMA0CONF_RXPBL_8 |
           DMA0CONF_FIXED_BURST | DMA0CONF_PBLX8 | DMA0CONF_PBL_8 |
           DMA0CONF_ATDS | DMA0CONF_PRIORITY_3_1,
           GMAC_REG(GMAC_DMA0CONF));
  putreg32(0, GMAC_REG(GMAC_MACFRMFLT));
  return OK;
}

int d13x_gmac_initialize(void)
{
  static const uint8_t mac[6] = {0x02, 0x13, 0x58, 0x88, 0x00, 0x01};
  uint32_t low;
  uint32_t high;
  int ret;

  memset(&g_gmac, 0, sizeof(g_gmac));
  ret = d13x_gmac_hardware_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "[D13GMAC] hardware init failed: %d version=%08lx\n",
             ret, (unsigned long)getreg32(GMAC_REG(GMAC_VERSION)));
      return ret;
    }

  memcpy(g_gmac.dev.d_mac.ether.ether_addr_octet, mac, sizeof(mac));
  low = mac[0] | ((uint32_t)mac[1] << 8) | ((uint32_t)mac[2] << 16) |
        ((uint32_t)mac[3] << 24);
  high = mac[4] | ((uint32_t)mac[5] << 8);
  putreg32(low, GMAC_REG(GMAC_MACADDR0LOW));
  putreg32(high, GMAC_REG(GMAC_MACADDR0HIGH));

  g_gmac.dev.d_buf = g_netbuffer;
  g_gmac.dev.d_ifup = d13x_ifup;
  g_gmac.dev.d_ifdown = d13x_ifdown;
  g_gmac.dev.d_txavail = d13x_txavail;
  g_gmac.dev.d_private = &g_gmac;
  ret = irq_attach(D13X_IRQ_GMAC0, d13x_interrupt, &g_gmac);
  if (ret < 0)
    {
      return ret;
    }

  up_disable_irq(D13X_IRQ_GMAC0);
  ret = netdev_register(&g_gmac.dev, NET_LL_ETHERNET);
  if (ret == OK)
    {
      syslog(LOG_INFO,
             "[D13GMAC] registered eth0 version=%08lx mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
             (unsigned long)getreg32(GMAC_REG(GMAC_VERSION)),
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }

  return ret;
}
