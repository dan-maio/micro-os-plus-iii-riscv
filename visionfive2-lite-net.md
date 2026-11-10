# VisionFive 2 Lite — `smp-test-usb-cdc-net`

**Board:** StarFive VisionFive 2 Lite (JH-7110S, four SiFive U74 RV64GC, up to 1.25 GHz)
**Test:** `test/visionfive2-lite/smp-test-usb-cdc-net`
**State:** the SMP/thread/USB side runs on hardware; the Ethernet data path is **not
finished** — this document records exactly how far it got and what is left.

---

## 1. What the test is

A USB CDC-ACM console *and* a real network stack, on the kernel's SMP scheduler:

- **USB CDC-ACM** (TinyUSB + the board's Cadence USBSS-DRD driver) — the same stack
  the `smp-test-usb-cdc-acm` test uses, as the log/console channel.
- **Ethernet on GMAC0** — a polled DesignWare GMAC 5.20 driver written for this port
  (`test/boards/visionfive2-lite/net/gmac.cpp`), with lwIP 2.2.1 on top for DHCP,
  ICMP and an RFC 862 echo server.
- **Seven threads** across the four harts, one job each.

The verdict is PASS when the carrier is up, DHCP leases an address and the gateway
answers an ICMP echo. Without a cable the run reports the reason and fails; it never
hangs.

## 2. The threads

| thread | priority | stack | affinity | job |
|---|---|---|---|---|
| `usb` | above normal | 32 KiB | CPU 0 | TinyUSB device task, polled controller (the CDC glue's TX ring takes producers on one core) |
| `console` | normal | 32 KiB | CPU 0 | CDC lines from the host: `status`, `ping`, or an echo; Escape ends the run |
| `eth-rx` | high | 64 KiB | any | drains the GMAC receive ring into lwIP |
| `net` | above normal | 64 KiB | any | lwIP timers + carrier monitoring over MDIO, every 10 ms |
| `app` | normal | 128 KiB | any | the sequence: carrier, DHCP, ping the gateway, ping the optional peer, verdict |
| `stats` | below normal | 32 KiB | any | counters every 5 s |
| `mon` | low | 32 KiB | any | 120 s watchdog so a silent network cannot wedge the board |

Concurrency rules (the reason the threads can share the stack):

- **lwIP (`NO_SYS`) is not thread safe**, so every entry point — `input`, `poll`,
  `ping`, `status` — takes **one recursive mutex** inside `net-lwip.cpp`.  The GMAC
  transmit path runs under the same lock, because lwIP's echo replies (rx thread) and
  the app's pings both end in `gmac::send()`.
- The **receive ring** is touched only by `eth-rx`; the **transmit ring** only under
  that lock.
- The **UART has one writer**: `net_lwip::log()`, which also mirrors every line into a
  RAM buffer (section 5).
- Only the USB threads are pinned: the board's CDC glue requires one core, and a
  pinned-and-spinning thread proved to be the least robust thing to do during
  bring-up, so the rest are placed by the scheduler.

## 3. The GMAC0 driver

Register facts (JH-7110, `jh7110.dtsi`):

| Item | Value |
|---|---|
| Base | `0x16030000`, `snps,dwmac-5.20` |
| Clocks | AONCRG `GMAC0_AHB` (pclk), `GMAC0_AXI` (stmmaceth), `GMAC0_TX`/`TX_INV`; SYSCRG `GMAC0_PTP`, `GMAC0_GTXC`, `GMAC0_GTXCLK` |
| Resets | AONCRG `GMAC0_AXI` (0), `GMAC0_AHB` (1), in `AONCRG+0x38` |
| PHY interface field | `aon_syscon` (`0x17010000`) + `0xc`, bits 18..20; **1 = RGMII**, 4 = RMII |
| PHY | Motorcomm YT8531 at **MDIO address 0**, `rgmii-id` (delays in the PHY), `tx-clk` inversion and 1500 ps internal delays set by the device tree |
| DMA | one channel: `DMA_BUS_MODE 0x1000`, `SYS_BUS 0x1004`, channel 0 at `0x1100`, MTL queues at `0x0d00`/`0x0d30`, 16-byte descriptors, `txpbl/rxpbl = 16`, fixed burst |

The driver implements: clock-gate and reset bring-up with a runtime probe of the
`gmac0_tx` clock mux, MDIO clause-22 access with the CSR divider programmed, PHY
auto-negotiation and speed/duplex read-out, the MTL queues (store-and-forward plus the
queue enables), 16 RX / 8 TX descriptor rings, MAC address and filter, and the
polled `send()`/`poll()` pair.  JH-7110 DMA is **not cache coherent**, so every
descriptor and buffer is written back / invalidated through the L2 controller's
`FLUSH64` (`vf2_dcache_flush()`, the helper the USB driver already used).

## 4. Build and run

```sh
cmake -S . -B test/build-vf2 -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/riscv-none-elf.cmake \
      -DBOARD=visionfive2-lite
cmake --build test/build-vf2 --target smp-test-usb-cdc-net-hwd -j
BUILD=test/build-vf2 test/boards/visionfive2-lite/hw.sh smp-test-usb-cdc-net
```

Options (CMake cache):

| Option | Default | What it does |
|---|---|---|
| `NET_TEST_PEER` | `""` | optional second host on the LAN to ping |
| `NET_TEST_INHERIT_GMAC` | `OFF` | skip our clock/reset bring-up and take over a GMAC the boot chain already started (for a firmware whose U-Boot brings the MAC up) |
| `NET_TEST_STAY_ALIVE` | `OFF` | keep running after the verdict, so the RAM log can be read over JTAG (bring-up mode) |

## 5. Reading the log without the serial port

Every console line is also appended to a buffer in DDR:

```
0x43000000  magic 'VF2L' (0x5646324c)
0x43000004  length
0x43000008  text, NUL-terminated as it is written
```

`net_lwip::log()` writes the text and flushes the range through the L2 controller, so
a debugger read (which bypasses the caches) sees it.  Dump it after a run:

```sh
openocd -s <scripts> -f jh7110_picoprobe.cfg \
  -c "init" -c "targets jh7110.cpu1" -c "halt" \
  -c "dump_image /tmp/vf2ram.bin 0x43000000 0x6000" -c "shutdown"
```

## 6. Status on hardware — what was measured

| Probe | Value | Meaning |
|---|---|---|
| `GMAC VERSION` (0x110) | `0x4152` | DW GMAC 5.20 answering |
| `GMAC_HW_FEATURE0` (0x11c) | `0x1a2173f7` | register file alive, GMII/MII/RGMII present |
| `aoncrg[2]`, `[3]` | `0x80000000` | `GMAC0_AHB` / `GMAC0_AXI` gates enabled |
| `syscrg[8]`, `[11]` | `2`, `4` | AXI divider 2, APB divider 4 — the bus clocks run |
| `syscrg[108/109/111]` | `0x80000008` / `0xa` / `0x20` | GMAC0 GTXCLK, PTP and GTXC enabled with dividers |
| `aon[5]` (gmux `gmac0_tx`) | `0x80000000` | TX clock mux enabled, parent 0 = the external RGMII clock |
| `syscon` (`0x17010000+0xc`) | `0x4d540` → bits 18..20 = 1 | PHY interface **RGMII**, already set by the boot chain |
| `DMA_BUS_MODE` (0x1000) | `0x1` | **soft reset never clears** |
| MDIO, addresses 0..7 | timeout (`T`) | **the PHY never answers** |

So the MAC is clocked and readable, but its **DMA and MDIO do not work**.  Both sit
in the application-clock (AXI/stmmaceth) domain, and the register-level evidence says
that domain is enabled — which is why this is not a one-line fix.

One more fact, measured: **`dhcp` in this board's U-Boot crashes OpenSBI**
(`system_opcode_insn: Failed to access CSR 0xc01 from M-mode` — U-Boot reads the
`time` CSR, which the U74 only implements for S-mode).  U-Boot's networking therefore
never runs here, so it never brings GMAC0 up, and the "let U-Boot do it" shortcut
(`NET_TEST_INHERIT_GMAC`) has nothing to inherit.

## 7. What is left

1. **The application-clock/reset order.**  Our gate writes are visible but the DMA
   still does not run.  The next step is to reproduce the vendor's bring-up order
   exactly (`dwmac_starfive.c` + `clk-starfive-jh7110-aon.c` + `dwmac4_dma.c`), i.e.
   reset → clock-unprepare/prepare sequence → PHY-interface field → DMA init, and to
   read back `DMA_CH0_CTRL` after a write to prove whether the DMA register file
   latches at all (that probe is already in the log as `dma_wrback=`).
2. **MDIO.**  Once the DMA block latches, MDIO either follows (same clock domain) or
   needs its CSR divider (`MDIO_ADDR` bits 11..8) confirmed against the real
   stmmaceth frequency.
3. **PHY bring-up.**  The YT8531 needs its 25 MHz reference and the `tx-clk`
   configuration the device tree applies; if MDIO still times out after (1), that is
   the next suspect.

## 8. Files

```
test/boards/visionfive2-lite/net/gmac.{hpp,cpp}      GMAC0 driver (DW 5.20, polled)
test/boards/visionfive2-lite/net/net-lwip.{hpp,cpp}  lwIP glue, DHCP/ICMP/echo, RAM log
test/boards/visionfive2-lite/net/lwipopts.h          lwIP configuration
test/boards/visionfive2-lite/net/arch/cc.h           lwIP platform glue
test/visionfive2-lite/smp-test-usb-cdc-net/main.cpp   the seven threads and the sequence
test/visionfive2-lite/tests.cmake                    sources, includes, options
test/third_party/lwip/                               lwIP 2.2.1, shared with the MQ-Pro test
```

The MangoPi MQ-Pro's `wifi-net` test was moved onto the same shared lwIP tree
(`test/third_party/lwip`) in the same change.
