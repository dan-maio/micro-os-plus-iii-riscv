/*
 * smp-test-usb-cdc-net (VisionFive 2 Lite)
 *
 * A USB CDC-ACM console (TinyUSB + the board's Cadence driver) *and* a real
 * network: the JH-7110's GMAC0 gigabit MAC, lwIP, DHCP, ICMP and a TCP echo
 * server -- on all four U74 harts, with one thread per job.
 *
 * Threads (affinity in brackets):
 *
 *   usb     [0]  TinyUSB device task + the CJK CDC glue (polled controller;
 *                the glue's TX ring takes producers on one core only)
 *   console [0]  CDC lines from the host, echoed back; "status" prints the
 *                network state, Escape ends the run early
 *   eth-rx  [1]  drains the GMAC receive ring into lwIP
 *   net     [2]  lwIP timers + carrier monitoring (MDIO), every 10 ms
 *   app     [3]  the test sequence: link, DHCP, ping the gateway, ping the
 *                optional peer, verdict
 *   stats   [1]  counters every 5 s
 *   mon     [2]  a 120 s watchdog so a silent network cannot wedge the board
 *
 * Concurrency rules (the reason the threads can share the stack):
 *
 *   - lwIP (NO_SYS) is not thread-safe, so every entry point -- input, poll,
 *     ping, status -- takes ONE recursive mutex in net-lwip.cpp.  The GMAC
 *     transmit path runs under the same lock, because lwIP's echo replies
 *     (rx thread) and the app's pings both end in gmac::send().
 *   - The receive ring is touched only by the rx thread; the TX ring only
 *     under the lwIP lock.  The UART has one writer at a time through
 *     net_lwip::log().
 *   - Affinities spread the work over the four harts; `ethernet` and `net`
 *     never run on the same core, so a burst of frames cannot delay the
 *     timers by more than one iteration.
 *
 * The verdict: PASS when the carrier is up, DHCP leases an address and the
 * gateway answers an ICMP echo.  With no cable (no link) the test reports
 * SKIP -- "no ethernet link" is a wiring fact, not a software failure.
 *
 * Copyright (c) 2026 Dan. MIT license.
 */

#include <cmsis-plus/rtos/os.h>

#include <board.hpp>
#include <gmac.hpp>
#include <led.hpp>
#include <net-lwip.hpp>
#include <riscv-smp.hpp>
#include <uart.hpp>
#include <vf2-usb.h>

#include <hw_result.hpp>

#include "tusb.h"
#include "usb_cdc.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

using namespace os::rtos;

namespace
{
  // The MAC is locally administered; a real board would read the EEPROM.
  constexpr std::uint8_t kMac[6] = {0x02, 0x56, 0x46, 0x4c, 0x33, 0x01};

  // What to ping second: a host on the LAN, if the build names one.
#ifndef NET_TEST_PEER
#define NET_TEST_PEER ""
#endif

  volatile bool run_done = false;
  volatile bool run_pass = false;
  volatile bool usb_up = false;
  volatile bool usb_mounted = false;

  mutex_recursive console_mutex{ "console" };

  void
  log (const char* fmt, ...)
  {
    char b[256];
    va_list ap;
    va_start (ap, fmt);
    const int n = std::vsnprintf (b, sizeof (b), fmt, ap);
    va_end (ap);
    if (n <= 0)
      {
        return;
      }
    console_mutex.lock ();
    uart::write (b, static_cast<std::size_t> (
                        n < static_cast<int> (sizeof (b)) ? n : sizeof (b) - 1));
    console_mutex.unlock ();
  }

  void
  finish (bool pass)
  {
    run_pass = pass;
    run_done = true;
  }

  // ---------------------------------------------------------------- threads

  void*
  usb_task (void*)
  {
    this_thread::thread ().cpu_affinity (1u << 0);
    while (port_cpu_id () != 0)
      {
        this_thread::yield ();
      }
    if (!vf2_usb_hw_init ())
      {
        log ("[usb] bring-up failed (OTG DID 0x%08x)\n",
             static_cast<unsigned> (vf2_usb_otg_did ()));
        finish (false);
        return nullptr;
      }
    tusb_init ();
    usb_up = true;
    log ("[usb] tusb_init done (Cadence cdns3, full speed)\n");
    for (;;)
      {
        tud_int_handler (0);
        if (tud_task_event_ready ())
          {
            tud_task ();
          }
        mu_usb_cdc_task ();
        this_thread::yield ();
      }
    return nullptr;
  }

  void*
  console_task (void*)
  {
    this_thread::thread ().cpu_affinity (1u << 0);
    while (port_cpu_id () != 0)
      {
        this_thread::yield ();
      }
    static char line[MU_CDC_RX_BUFSIZE];
    bool was = false;
    for (;;)
      {
        const bool m = mu_usb_cdc_mounted () != 0;
        if (m != was)
          {
            usb_mounted = m;
            log (m ? "[usb] mounted (host configured the device)\n"
                   : "[usb] unmounted\n");
            was = m;
          }
        const int n = mu_usb_cdc_readline (line, sizeof (line));
        if (n >= 0)
          {
            for (int i = 0; i < n; ++i)
              {
                if (static_cast<unsigned char> (line[i]) == 0x1B)
                  {
                    log ("\n[usb] Escape: ending the run early\n");
                    finish (run_pass);
                  }
              }
            if (std::strncmp (line, "status", 6) == 0)
              {
                net_lwip::print_status ();
              }
            else if (std::strncmp (line, "ping", 4) == 0)
              {
                (void)net_lwip::ping ("gw", 2);
              }
            else
              {
                log ("[usb] you typed: %s\n", line);
              }
            mu_usb_cdc_write_str ("ok\r\n");
            mu_usb_cdc_flush ();
          }
        sysclock.sleep_for (10);
      }
    return nullptr;
  }

  void*
  rx_task (void*)
  {
    static std::uint8_t frame[1600];
    for (;;)
      {
        int n;
        unsigned drained = 0;
        while ((n = gmac::poll (frame, sizeof (frame))) >= 0 && drained < 64u)
          {
            net_lwip::input (frame, static_cast<std::size_t> (n));
            ++drained;
          }
        if (drained == 0)
          {
            sysclock.sleep_for (1);
          }
      }
    return nullptr;
  }

  void*
  net_task (void*)
  {
    bool was_up = false;
    bool announced = false;
    for (;;)
      {
        net_lwip::poll ();

        // The carrier is an MDIO read: keep it on this thread only.
        const bool up = gmac::link_up ();
        if (up != was_up)
          {
            was_up = up;
            net_lwip::set_link (up);
            log ("[net] carrier %s (%u Mb/s %s duplex)\n",
                 up ? "up" : "lost",
                 static_cast<unsigned> (gmac::status ().speed_mbps),
                 gmac::status ().full_duplex ? "full" : "half");
          }
        if (up && !announced)
          {
            announced = true;
            log ("[net] link established, DHCP starting\n");
          }
        sysclock.sleep_for (10);
      }
    return nullptr;
  }

  void*
  app_task (void*)
  {

    log ("[app] waiting for the carrier (10 s) ...\n");
    bool up = false;
    for (unsigned i = 0; i < 1000u && !up; ++i)
      {
        up = gmac::status ().link;
        if (!up)
          {
            sysclock.sleep_for (10);
          }
      }
    if (!up)
      {
        log ("[app] SKIP: no ethernet link (cable? PHY?) phy id %04x:%04x "
             "bmsr 0x%04x\n",
             static_cast<unsigned> (gmac::status ().phy_id1),
             static_cast<unsigned> (gmac::status ().phy_id2),
             static_cast<unsigned> (gmac::status ().bmsr));
        finish (false);
        return nullptr;
      }

    if (!net_lwip::wait_for_address (15000))
      {
        log ("[app] no DHCP lease within 15 s\n");
        net_lwip::print_status ();
        finish (false);
        return nullptr;
      }
    net_lwip::print_status ();

    log ("[app] ping gateway ...\n");
    const unsigned replies = net_lwip::ping ("gw", 4);
    log ("[app] gateway replies: %u/4\n", replies);

#ifdef NET_TEST_PEER
    if (NET_TEST_PEER[0] != '\0')
      {
        log ("[app] ping peer %s ...\n", NET_TEST_PEER);
        (void)net_lwip::ping (NET_TEST_PEER, 2);
      }
#endif

    net_lwip::print_status ();
    finish (replies > 0);
    return nullptr;
  }

  void*
  stats_task (void*)
  {
    for (;;)
      {
        sysclock.sleep_for (5000);
        if (run_done)
          {
            continue;
          }
        const gmac::counters& g = gmac::stats ();
        const net_lwip::counters& n = net_lwip::stats ();
        log ("[stats] gmac rx %u tx %u err %u/%u busy %u phy %u; lwip rx %u "
             "tx %u drop %u\n",
             static_cast<unsigned> (g.rx_frames),
             static_cast<unsigned> (g.tx_frames),
             static_cast<unsigned> (g.rx_errors),
             static_cast<unsigned> (g.tx_errors),
             static_cast<unsigned> (g.tx_busy),
             static_cast<unsigned> (g.phy_polls),
             static_cast<unsigned> (n.rx_frames),
             static_cast<unsigned> (n.tx_frames),
             static_cast<unsigned> (n.rx_dropped));
      }
    return nullptr;
  }

  void*
  monitor_task (void*)
  {
    for (unsigned i = 0; i < 120u; ++i)
      {
        sysclock.sleep_for (1000);
        if (run_done)
          {
            return nullptr;
          }
      }
    log ("[mon] 120 s without a verdict\n");
    finish (false);
    return nullptr;
  }

  thread::stack::element_t usb_stack[4096];
  thread::stack::element_t console_stack[4096];
  thread::stack::element_t rx_stack[8192];
  thread::stack::element_t net_stack[8192];
  thread::stack::element_t app_stack[16384];
  thread::stack::element_t stats_stack[4096];
  thread::stack::element_t mon_stack[4096];
} // namespace

int
os_main (int /*argc*/, char* /*argv*/[])
{
  led::init ();
  uart::init (board::UART_BAUD);
  net_lwip::log ("\n=============================================\n");
  net_lwip::log ("  " PORT_BANNER_LONG " - micro-os-plus-iii\n");
  net_lwip::log ("  smp-test-usb-cdc-net : GMAC0 + lwIP + USB CDC, 7 threads\n");
  net_lwip::log ("=============================================\n");

  // The MAC first: the PHY needs time to negotiate, and the other threads
  // must not poll a half-configured DMA engine.
  const bool gmac_ok = gmac::start (kMac);
  {
    static char diag[1024];
    gmac::dump_registers (diag, sizeof (diag));
    log ("[gmac] %s\n", diag);
  }
  log ("[gmac] version 0x%04x, dma reset %s, phy id 0x%04x:0x%04x, link %s "
       "%u Mb/s%s%s\n",
       static_cast<unsigned> (gmac::status ().version),
       gmac::dma_reset_ok () ? "ok" : "DID NOT CLEAR",
       static_cast<unsigned> (gmac::status ().phy_id1),
       static_cast<unsigned> (gmac::status ().phy_id2),
       gmac::status ().link ? "up" : "down",
       static_cast<unsigned> (gmac::status ().speed_mbps),
       gmac_ok ? "" : ", start failed: ", gmac_ok ? "" : gmac::last_error ());
  if (!gmac_ok)
    {
      hw_result::fail ();
    }

  if (!net_lwip::init (kMac))
    {
      log ("[net] lwIP init failed\n");
      hw_result::fail ();
    }
  if (gmac::status ().link)
    {
      net_lwip::set_link (true);
    }

  thread::attributes attr = thread::initializer;

  attr.th_stack_address = usb_stack;
  attr.th_stack_size_bytes = sizeof (usb_stack);
  attr.th_priority = thread::priority::above_normal;
  static thread usb_th{ "usb", usb_task, nullptr, attr };

  attr.th_stack_address = console_stack;
  attr.th_stack_size_bytes = sizeof (console_stack);
  attr.th_priority = thread::priority::normal;
  static thread console_th{ "console", console_task, nullptr, attr };

  attr.th_stack_address = rx_stack;
  attr.th_stack_size_bytes = sizeof (rx_stack);
  attr.th_priority = thread::priority::high;
  static thread rx_th{ "eth-rx", rx_task, nullptr, attr };

  attr.th_stack_address = net_stack;
  attr.th_stack_size_bytes = sizeof (net_stack);
  attr.th_priority = thread::priority::above_normal;
  static thread net_th{ "net", net_task, nullptr, attr };

  attr.th_stack_address = app_stack;
  attr.th_stack_size_bytes = sizeof (app_stack);
  attr.th_priority = thread::priority::normal;
  static thread app_th{ "app", app_task, nullptr, attr };

  attr.th_stack_address = stats_stack;
  attr.th_stack_size_bytes = sizeof (stats_stack);
  attr.th_priority = thread::priority::below_normal;
  static thread stats_th{ "stats", stats_task, nullptr, attr };

  attr.th_stack_address = mon_stack;
  attr.th_stack_size_bytes = sizeof (mon_stack);
  attr.th_priority = thread::priority::low;
  static thread mon_th{ "mon", monitor_task, nullptr, attr };

  // The report: a thread of the runner (the USB console stays alive until the
  // verdict, so the host can still type while the network is tested).
  for (;;)
    {
      led::toggle ();
      sysclock.sleep_for (250);
      if (run_done)
        {
          break;
        }
    }

  const bool pass = run_pass;
  log ("\n[test] %s (usb %s%s)\n", pass ? "PASS" : "FAIL",
       usb_up ? "up" : "not up", usb_mounted ? ", mounted" : "");
#ifdef NET_TEST_STAY_ALIVE
  // Bring-up runs: keep the run alive so the RAM log can be read over JTAG
  // after the verdict (the semihosting exit would end the OpenOCD session).
  log ("[test] stay-alive (NET_TEST_STAY_ALIVE): halting here\n");
  for (;;)
    {
      sysclock.sleep_for (1000);
    }
#endif
  if (pass)
    {
      hw_result::ok ();
    }
  hw_result::fail ();
}
