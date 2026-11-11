# VisionFive 2 Lite — `smp-test-usb-cdc-net`

**Board:** StarFive VisionFive 2 Lite (JH-7110S, four SiFive U74 RV64GC, up to 1.25 GHz)
**Test:** `test/visionfive2-lite/smp-test-usb-cdc-net`
**State:** **works on hardware** — carrier at 1 Gb/s, DHCP lease (and a static
address option), ICMP echo replies (4/4, 0 retransmissions), USB CDC console, PASS.
Section 6 records the measurements and the five bugs that had to be fixed to get
there.

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
| PHY | Motorcomm **YT8531S** (PHY ID `0x4f51:0xe91b`) at **MDIO address 0**, `rgmii-id` |
| TX clock | AONCRG clock 5 (`gmac0_tx`), mux in bits 25:24: **parent 1 = `gmac0_rmii_rtx`** (the Lite's device tree assigns it with `starfive,tx-use-rgmii-clk`; the MAC does not generate the RGMII TX clock) |
| DMA | one channel: `DMA_BUS_MODE 0x1000`, `SYS_BUS 0x1004`, channel 0 at `0x1100`, MTL queues at `0x0d00`/`0x0d30`, 16-byte descriptors, `txpbl/rxpbl = 16`, fixed burst |

The driver implements: clock-gate and reset bring-up (with `gmac0_tx` on its
`gmac0_rmii_rtx` parent), MDIO clause-22 access, PHY identification and speed/duplex
read-out, the MTL queues (store-and-forward plus the
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

## 6. What was measured, and the four bugs it exposed

| Probe | Value | Meaning |
|---|---|---|
| `GMAC VERSION` (0x110) | `0x4152` | DW GMAC 5.20 answering |
| `GMAC_HW_FEATURE0` (0x11c) | `0x1a2173f7` | register file alive, GMII/MII/RGMII present |
| `aoncrg[2]`, `[3]` | `0x80000000` | `GMAC0_AHB` / `GMAC0_AXI` gates enabled |
| `aon[5]` (`gmac0_tx`) | `0x81000000` | TX clock enabled, **parent 1 = `gmac0_rmii_rtx`** |
| `syscon` (`0x17010000+0xc`) | `0x4d540` → bits 18..20 = 1 | PHY interface **RGMII** |
| MDIO @0, all dividers | `0x4f51:0xe91b`, BMSR `0x796d` | YT8531S, link up, auto-neg complete |
| `DMA_BUS_MODE` (0x1000) | `0x00000000` after `SWR` | DMA reset clears |
| MTL TX/RX FIFO counters | `unf 0 ovf 0` | no FIFO drops |
| DHCP | `192.168.1.65/24`, gateway `192.168.1.1` | full four-way exchange |
| ICMP, 20 echoes | **20/20 replies, 0 retransmissions, ~3 ms** | the wire is clean |

### 6.1 MDIO: the busy bit *starts* the frame

`MDIO_ADDR` bit 0 (`GBUSY`) is not only a status: it must be **written as 1** with the
command, and the MAC clears it when the frame is done (Linux:
`stmmac_mdio_format_addr()` ORs `MII_ADDR_GBUSY` into the address word).  Polling it
but never setting it sent no frame at all: the bit read back 0 immediately, `DATA`
returned the zero written just before, and the PHY looked dead.  The field layout is
Linux's `dwmac4_core.c`: **PA in 25:21, GR in 20:16, CSR divider in 11:8**.  (Getting
that wrong is silent: the MAC still completes the frame, it just talks to a different
PHY address.)

### 6.2 The TX clock parent: the cause of the *partial* loss

`gmac0_tx` (AONCRG + 0x14, mux bits 25:24) was on parent 0, the SoC's generated
125 MHz `gtxclk`.  The Lite's device tree assigns **parent 1, `gmac0_rmii_rtx`**,
together with `starfive,tx-use-rgmii-clk` — on this board the MAC does not generate
the RGMII transmit clock, the PHY's clock does.  With parent 0 the link came up and
passed *some* frames (DHCP sometimes, ICMP 25–50 %), which is exactly what an
unsynchronised transmit clock looks like: no error counters anywhere, just loss.  With
parent 1: 20/20 echoes, 0 retransmissions.

### 6.3 The MAC speed/duplex latch: why TX could die completely

`start()` reads the PHY once and latches `MAC_CONFIG.FES`/`DM` from it.  On a cold
board the PHY is still negotiating at that moment, so `st.link` is false, the speed
defaults to 100 Mb/s half duplex, and the MAC keeps that forever -- on a 1 Gb/s link.
The symptom is brutal and misleading: reception keeps working (the RGMII receive clock
does not depend on `FES`), the transmit DMA drains every descriptor and reports no
error, and **not one frame reaches the wire** (measured with a 200-frame broadcast
burst: the PC's NIC counted none of them).  Register dump of the broken state:
`MAC_CONFIG = 0x00026003` -- `FES` and `DM` both clear.

The fix is to apply the speed/duplex whenever the link resolves (`apply_speed()` called
from `link_up()`), not once during bring-up.  With it: 4/4 echoes on both the static
address and DHCP.

### 6.4 Thread creation: a higher-priority creator starves itself

The main thread runs at `normal`.  Threads created at `above_normal` start at once and
never yield back, so `os_main` stopped halfway through the list — with no trap and no
error, just a log that ended mid-sentence.  The fix is to hold the creator at
`realtime` for the burst of creations and restore its priority afterwards.  The USB
thread also had to `sleep_for(1)` instead of `yield()`: a ready `above_normal` thread
that only yields is a permanent starvation of everything below it.

### 6.5 One console writer

The test's own `log()` wrote to UART0 with a mutex of its own, so its lines never
reached the RAM/USB log, and two writers interleaved on the serial port.  Every line
now goes through `net_lwip::log()` (one mutex, RAM copy + UART).

Also fixed on the way: lwIP (`NO_SYS`) is serialised with a single recursive mutex
around `input`/`poll`/`ping`/`status`; the PHY is **not** touched (an auto-negotiation
restart, the 1500 ps RGMII delays and the drive-strength writes each made the data path
worse on this board); and the console's UART write is paced but bounded, so a closed
terminal can never hang a test.

## 7. Known limitations

- `hw.sh`'s own verdict path is unreliable on this board: the attach sometimes leaves
  the harts at the entry point and OpenOCD exits `4` (`OPENOCD DIED`) with no
  `RESULT:` line.  The test's verdict is in its log (`[test] PASS`), and the RAM log
  can always be read over JTAG (section 5).
- `dhcp` in this board's U-Boot crashes OpenSBI (`system_opcode_insn: Failed to access
  CSR 0xc01 from M-mode` — U-Boot reads `time`, which the U74 only implements for
  S-mode).  U-Boot therefore never brings GMAC0 up, and `NET_TEST_INHERIT_GMAC` has
  nothing to inherit.
- Each echo is still retransmitted up to three times if unanswered, and the DHCP wait
  is 15 s: on a busy LAN one lost frame should fail a report, not the test.

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
