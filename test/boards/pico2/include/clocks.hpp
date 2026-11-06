/*
 * clocks.hpp -- the Pico 2 clock tree (src/clocks.cpp).
 *
 * Copyright (c) 2026 Dan. MIT license.
 */
#pragma once

namespace clocks
{
  // clk_sys = clk_peri = 150 MHz, TICKS 1 us (called by the board's early
  // start-up).
  void
  init (void);

  // PLL_USB -> clk_usb 48 MHz, for the USB device (before tusb_init()).
  bool
  usb_init (void);
} // namespace clocks
