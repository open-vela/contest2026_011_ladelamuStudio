#include <stdint.h>

#define getreg32(a)     (*(volatile uint32_t *)(a))
#define putreg32(v,a)   (*(volatile uint32_t *)(a) = (v))

void d13x_clock_init(void)
{
  uint32_t reg;

  reg = getreg32(0x180200A4);
  reg |= (1 << 1);
  putreg32(reg, 0x180200A4);

  reg = getreg32(0x180200A0);
  reg |= (1 << 31) | (1 << 0);
  putreg32(reg, 0x180200A0);

  putreg32(0x240C4040, 0x18020044);
  while (!(getreg32(0x18020044) & (1 << 17)));

  putreg32(0x240C4040, 0x18020040);
  while (!(getreg32(0x18020040) & (1 << 17)));

  putreg32(0x00000001, 0x18020100);
  putreg32(0x00000001, 0x18020120);
  putreg32(0x00000100, 0x18020200);

  reg = getreg32(0x1802083C);
  reg |= (1 << 12) | (1 << 13);
  putreg32(reg, 0x1802083C);
  putreg32(0x00001001, 0x18020840);
}

void d13x_uart_init(void)
{
  uint32_t divisor = 60000000 / (16 * 115200);
  putreg32(0, 0x1871000C);
  putreg32(divisor & 0xFF, 0x18710000);
  putreg32((divisor >> 8) & 0xFF, 0x18710004);
  putreg32(0x07, 0x18710008);
  putreg32(0x03, 0x1871000C);
}

void d13x_putc(char ch)
{
  while (!(getreg32(0x18710014) & (1 << 5)));
  putreg32((uint32_t)ch, 0x18710000);
}

int d13x_getc(void)
{
  while (!(getreg32(0x18710014) & (1 << 0)));
  return (int)(getreg32(0x18710000) & 0xFF);
}

int d13x_uart_rxready(void)
{
  return (getreg32(0x18710014) & (1 << 0)) != 0;
}
