#include <stdint.h>

extern void d13x_clock_init(void);
extern void d13x_uart_init(void);
extern void d13x_putc(char ch);
extern int d13x_getc(void);
extern int d13x_uart_rxready(void);

static void print_str(const char *str)
{
  while (*str)
  {
    d13x_putc(*str++);
  }
}

int main(void)
{
  d13x_clock_init();
  d13x_uart_init();

  print_str("\r\nD13x Bootloader v1.0\r\n");
  print_str("Waiting for USB command...\r\n");

  while (1)
  {
    if (d13x_uart_rxready())
    {
      int ch = d13x_getc();
      d13x_putc(ch);
      if (ch == '\r' || ch == '\n')
      {
        d13x_putc('\r');
        d13x_putc('\n');
      }
    }
  }

  return 0;
}
