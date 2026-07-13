/****************************************************************************
 * arch/risc-v/src/common/riscv_idle.c
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/irq.h>
#include <nuttx/arch.h>
#include <nuttx/board.h>
#include <arch/board/board.h>

#include "riscv_internal.h"

#ifdef CONFIG_ARCH_CHIP_D133EBS
#  define D13X_CMU_CLK_GTC       0x1802090c
#  define D13X_GTC_CNTCR         0x19050000
#  define D13X_CORET_MTIME_LO    0x2000bff8
#  define D13X_CORET_MTIME_HI    0x2000bffc
#  define D13X_CORET_FREQUENCY   4000000ull
#  define D13X_CORET_TICK_COUNT  \
    (D13X_CORET_FREQUENCY * CONFIG_USEC_PER_TICK / 1000000ull)

static uint64_t d13x_coret_current(void)
{
  uint32_t high1;
  uint32_t high2;
  uint32_t low;

  do
    {
      high1 = getreg32(D13X_CORET_MTIME_HI);
      low = getreg32(D13X_CORET_MTIME_LO);
      high2 = getreg32(D13X_CORET_MTIME_HI);
    }
  while (high1 != high2);

  return ((uint64_t)high1 << 32) | low;
}

static void d13x_timer_poll(void)
{
  static uint64_t next_tick;
  uint64_t now;

  if (next_tick == 0)
    {
      /* The GTC provides the E907 CORET 4 MHz time base.  Keep the unsafe
       * CLIC timer source masked and advance NuttX time from idle context.
       */

      putreg32(0x3100, D13X_CMU_CLK_GTC);
      putreg32(1, D13X_GTC_CNTCR);
      next_tick = d13x_coret_current() + D13X_CORET_TICK_COUNT;
      return;
    }

  now = d13x_coret_current();
  while ((int64_t)(now - next_tick) >= 0)
    {
      next_tick += D13X_CORET_TICK_COUNT;
      nxsched_process_timer();
    }
}
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: up_idle
 *
 * Description:
 *   up_idle() is the logic that will be executed when there is no other
 *   ready-to-run task.  This is processor idle time and will continue until
 *   some interrupt occurs to cause a context switch from the idle task.
 *
 *   Processing in this state may be processor-specific. e.g., this is where
 *   power management operations might be performed.
 *
 ****************************************************************************/

void up_idle(void)
{
#if defined(CONFIG_SUPPRESS_INTERRUPTS) || defined(CONFIG_SUPPRESS_TIMER_INTS)
#ifdef CONFIG_ARCH_CHIP_D133EBS
  d13x_timer_poll();

#ifdef CONFIG_D13X_UART0
  extern void d13x_uart_rxpoll(void);

  d13x_uart_rxpoll();
#endif

#ifdef CONFIG_D13X_TOUCH_GT911
  extern void d13x_touch_poll(void);

  d13x_touch_poll();
#endif
#else
  nxsched_process_timer();
#endif
#else

  /* Does the board support an IDLE LED to indicate that the board is in the
   * IDLE state?
   */

#ifdef CONFIG_ARCH_LEDS_CPU_ACTIVITY
  board_autoled_off(LED_CPU);
#endif

#ifdef CONFIG_D13X_UART0
  extern void d13x_uart_rxpoll(void);

  d13x_uart_rxpoll();
#endif

#ifdef CONFIG_D13X_TOUCH_GT911
  extern void d13x_touch_poll(void);

  d13x_touch_poll();
#endif

#if defined(CONFIG_D13X_UART0) || defined(CONFIG_D13X_TOUCH_GT911)
  return;
#endif

  /* This would be an appropriate place to put some MCU-specific logic to
   * sleep in a reduced power mode until an interrupt occurs to save power
   */

  asm("WFI");

#endif
}
