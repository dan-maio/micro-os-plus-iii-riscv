/*
 * clocks.cpp -- RP2350 clock tree: XOSC 12 MHz -> PLL_SYS -> clk_sys =
 * clk_peri = 150 MHz, clk_ref = XOSC; then the TICKS generators (1 us from
 * clk_ref): watchdog, TIMER0 and the RISC-V platform timer.
 *
 * The sequence is the Cortex-M pico2 board's (micro-os-plus-iii-cortexm,
 * test/boards/pico2/src/clocks.cpp, fixed 150 MHz path), with the Arm
 * barrier replaced by a RISC-V fence.
 *
 * Copyright (c) 2026 Dan. MIT license.
 */

#include <cstdint>

#include <rp2350.hpp>

using rp2350::reg;

namespace
{
  constexpr std::uint32_t XOSC_CTRL = 0x00, XOSC_STATUS = 0x04, XOSC_STARTUP = 0x0C;
  constexpr std::uint32_t PLL_CS = 0x00, PLL_PWR = 0x04, PLL_FBDIV_INT = 0x08,
                          PLL_PRIM = 0x0C;
  constexpr std::uint32_t CLK_REF_CTRL = 0x30, CLK_SYS_CTRL = 0x3C,
                          CLK_PERI_CTRL = 0x48;
  constexpr std::uint32_t CLK_DIV = 0x04, CLK_SELECTED = 0x08;

  constexpr std::uint32_t XOSC_FREQ_1_15MHZ = 0x0AA0, XOSC_ENABLE = 0x0FAB;
  constexpr std::uint32_t XOSC_STABLE = 1u << 31;
  constexpr std::uint32_t PLL_PWR_PD = 1u << 0, PLL_PWR_POSTDIVPD = 1u << 3,
                          PLL_PWR_VCOPD = 1u << 5, PLL_CS_LOCK = 1u << 31;
  constexpr std::uint32_t CLK_REF_SRC = 0x3, CLK_REF_SRC_XOSC = 2;
  constexpr std::uint32_t CLK_SYS_SRC = 0x3, CLK_SYS_SRC_AUX = 1;
  constexpr std::uint32_t CLK_SYS_AUX = 0xE0, CLK_SYS_AUX_LSB = 5;
  constexpr std::uint32_t CLK_REF_AUX = 0x60, CLK_REF_AUX_LSB = 5;
  constexpr std::uint32_t CLK_PERI_AUX = 0xE0, CLK_PERI_AUX_LSB = 5,
                          CLK_PERI_ENABLE = 1u << 11;
  constexpr std::uint32_t RESET_PLL_SYS = 1u << 14, RESET_IO_BANK0 = 1u << 6,
                          RESET_PADS_BANK0 = 1u << 9;
  constexpr std::uint32_t CLK_DIV_UNDIVIDED = 1u << 16;

  // 12 MHz * 125 = 1500 MHz VCO, / 5 / 2 = 150 MHz.
  constexpr std::uint32_t FBDIV = 125, PD1 = 5, PD2 = 2;

  void
  reset_block (std::uint32_t mask)
  {
    reg (rp2350::RESETS_BASE + 0x00) |= mask;
    __asm__ volatile("nop\n nop" ::: "memory");
  }

  void
  xosc_init (void)
  {
    reg (rp2350::XOSC_BASE + XOSC_STARTUP) = 0xC4;
    reg (rp2350::XOSC_BASE + XOSC_CTRL) = (XOSC_ENABLE << 12) | XOSC_FREQ_1_15MHZ;
    while (!(reg (rp2350::XOSC_BASE + XOSC_STATUS) & XOSC_STABLE))
      {
      }
  }

  void
  pll_sys_program (void)
  {
    const std::uint32_t b = rp2350::PLL_SYS_BASE;
    reset_block (RESET_PLL_SYS);
    rp2350::unreset_block (RESET_PLL_SYS);
    reg (b + PLL_CS) = 1; // REFDIV = 1
    reg (b + PLL_FBDIV_INT) = FBDIV;
    reg (b + PLL_PWR) &= ~(PLL_PWR_PD | PLL_PWR_VCOPD);
    while (!(reg (b + PLL_CS) & PLL_CS_LOCK))
      {
      }
    reg (b + PLL_PRIM) = (PD1 << 16) | (PD2 << 12);
    reg (b + PLL_PWR) &= ~PLL_PWR_POSTDIVPD;
  }

  void
  clk_configure (std::uint32_t ctrl, std::uint32_t src_mask,
                 std::uint32_t aux_mask, std::uint32_t aux_lsb,
                 std::uint32_t src, std::uint32_t auxsrc)
  {
    if (src == CLK_SYS_SRC_AUX)
      {
        reg (ctrl) &= ~src_mask;
        while (!(reg (ctrl + CLK_SELECTED) & 1u))
          {
          }
      }
    reg (ctrl) = (reg (ctrl) & ~aux_mask) | (auxsrc << aux_lsb);
    reg (ctrl) = (reg (ctrl) & ~src_mask) | src;
    while (!(reg (ctrl + CLK_SELECTED) & (1u << src)))
      {
      }
    reg (ctrl + CLK_DIV) = CLK_DIV_UNDIVIDED;
  }
} // namespace

namespace clocks
{
  void
  init (void)
  {
    // Flash SCK = clk_sys / QMI M0 CLKDIV. The boot ROM set the divider for
    // its slow boot clock; keep SCK <= 50 MHz at 150 MHz (div 3) before the
    // clock goes up -- raising the divider only slows flash reads.
    constexpr std::uint32_t QMI_M0_TIMING = rp2350::QMI_BASE + 0x0Cu;
    reg (QMI_M0_TIMING) = (reg (QMI_M0_TIMING) & ~0xFFu) | 3u;
    __asm__ volatile("fence iorw, iorw" ::: "memory");

    const std::uint32_t c = rp2350::CLOCKS_BASE;
    xosc_init ();

    // Move clk_sys / clk_ref off any AUX source before touching the PLL.
    reg (c + CLK_SYS_CTRL) &= ~CLK_SYS_SRC;
    while (!(reg (c + CLK_SYS_CTRL + CLK_SELECTED) & 1u))
      {
      }
    reg (c + CLK_REF_CTRL) &= ~CLK_REF_SRC;
    while (!(reg (c + CLK_REF_CTRL + CLK_SELECTED) & 1u))
      {
      }

    pll_sys_program ();

    clk_configure (c + CLK_REF_CTRL, CLK_REF_SRC, CLK_REF_AUX, CLK_REF_AUX_LSB,
                   CLK_REF_SRC_XOSC, 0);
    clk_configure (c + CLK_SYS_CTRL, CLK_SYS_SRC, CLK_SYS_AUX, CLK_SYS_AUX_LSB,
                   CLK_SYS_SRC_AUX, 0 /* AUXSRC = PLL_SYS */);

    // clk_peri = clk_sys: the UART's reference clock.
    reg (c + CLK_PERI_CTRL) &= ~CLK_PERI_ENABLE;
    reg (c + CLK_PERI_CTRL)
        = (reg (c + CLK_PERI_CTRL) & ~CLK_PERI_AUX) | (0u << CLK_PERI_AUX_LSB);
    reg (c + CLK_PERI_CTRL + CLK_DIV) = CLK_DIV_UNDIVIDED;
    reg (c + CLK_PERI_CTRL) |= CLK_PERI_ENABLE;

    rp2350::unreset_block (RESET_IO_BANK0 | RESET_PADS_BANK0);

    // Tick generators: 12 clk_ref cycles = 1 us.
    reg (rp2350::TICKS_BASE + 0x34) = 12; // WATCHDOG_CYCLES
    reg (rp2350::TICKS_BASE + 0x30) = 1;  // WATCHDOG_CTRL.ENABLE
    reg (rp2350::TICKS_BASE + 0x1C) = 12; // TIMER0_CYCLES
    reg (rp2350::TICKS_BASE + 0x18) = 1;  // TIMER0_CTRL.ENABLE
    reg (rp2350::TICKS_RISCV_CYCLES) = 12;
    reg (rp2350::TICKS_RISCV_CTRL) = 1;
  }

  // PLL_USB -> clk_usb = 48 MHz for the USB device controller, before
  // tusb_init() (the Cortex-M pico2 board's usb_init(), register for
  // register). Returns true if PLL_USB locked and clk_usb runs.
  bool
  usb_init (void)
  {
    constexpr std::uint32_t PLL_USB_BASE = 0x40058000u;
    constexpr std::uint32_t RESET_PLL_USB = 1u << 15;
    constexpr std::uint32_t RESET_USBCTRL = 1u << 28;
    const std::uint32_t b = PLL_USB_BASE;
    const std::uint32_t c = rp2350::CLOCKS_BASE;
    constexpr std::uint32_t CLK_USB_CTRL = 0x60, CLK_USB_ENABLE = 1u << 11,
                            CLK_USB_AUX = 0xE0, CLK_USB_AUX_LSB = 5;

    // Hold the controller in reset while PLL_USB / clk_usb change; take
    // clk_usb down first; power-cycle PLL_USB so the writes below take
    // effect after a warm reset (a live, locked PLL ignores them).
    reset_block (RESET_USBCTRL);
    reg (c + CLK_USB_CTRL) &= ~CLK_USB_ENABLE;
    reset_block (RESET_PLL_USB);
    rp2350::unreset_block (RESET_PLL_USB);
    reg (b + PLL_CS) = 1;          // REFDIV = 1
    reg (b + PLL_FBDIV_INT) = 100; // VCO = 12 * 100 = 1200 MHz
    reg (b + PLL_PWR) &= ~(PLL_PWR_PD | PLL_PWR_VCOPD);
    bool locked = false;
    for (std::uint32_t n = 0; n < 5000000u; ++n)
      {
        if (reg (b + PLL_CS) & PLL_CS_LOCK)
          {
            locked = true;
            break;
          }
      }
    if (!locked)
      {
        return false;
      }
    reg (b + PLL_PRIM) = (5u << 16) | (5u << 12); // /5 /5 = 48 MHz
    reg (b + PLL_PWR) &= ~PLL_PWR_POSTDIVPD;

    // clk_usb = PLL_USB, /1.
    reg (c + CLK_USB_CTRL + CLK_DIV) = CLK_DIV_UNDIVIDED;
    reg (c + CLK_USB_CTRL)
        = (reg (c + CLK_USB_CTRL) & ~CLK_USB_AUX) | (0u << CLK_USB_AUX_LSB);
    reg (c + CLK_USB_CTRL) |= CLK_USB_ENABLE;
    return true;
  }
} // namespace clocks
