# Reset the VisionFive 2 Lite to the U-Boot prompt

This is the method used with the Eclipse projects in this workspace. It
resets the board and sends a key **before and during** the boot, so U-Boot’s
autoboot countdown is interrupted and Linux is not started.

The board’s QSG says: *“When you power on VisionFive 2 Lite, the serial port
will print countdown, usually starting from 3. Hit any key to stop autoboot
before the number decreases to 0.”* The same is true after a board reset.

## Files

| file | purpose |
|---|---|
| `vf2-reset-to-uboot.sh` | sends the key stream and performs the reset |
| `*/jh7110_wdt_reset.cfg` | **software reset** through the JH7110 watchdog |
| `*/jh7110_wdt_disable.cfg` | stop a watchdog left running after the reset |
| `*/jh7110_daplink_srst.cfg` | OpenOCD config with SRST enabled (pin 36) |
| `*/jh7110_daplink.cfg` | the normal JTAG config (diagnostics) |

## Method 1 — watchdog reset from JTAG (no reset pin needed)

This is the default. The JH7110 watchdog is accessible from JTAG and resets
the whole chip. It is **locked** by default; the script unlocks it with the
key from the mainline `starfive-wdt.c` driver:

```text
0x13070000  WDOGLOAD
0x13070008  WDOGCONTROL   bit0 enable, bit1 reset enable
0x1307000C  WDOGINTCLR
0x13070C00  WDOGLOCK      write 0x1ACCE551 to unlock
```

Run:

```bash
cd /home/dan/Workspaces/milk-v
./vf2-reset-to-uboot.sh /dev/ttyUSB0
```

What it does:

1. starts sending `CR` on UART0 every 50 ms (60 s by default);
2. arms the JH7110 watchdog through JTAG (`jh7110_wdt_reset.cfg`);
3. the SoC resets after roughly 5–21 s (depending on the WDT clock);
4. the boot ROM → SPL → OpenSBI → U-Boot run while the keys are flowing;
5. after the first timeout the script tries to attach again and disable the
   watchdog (`jh7110_wdt_disable.cfg`), so it does not reset a second time;
6. U-Boot stops at:

```text
StarFive #
```

Notes:

- JTAG must be usable when you start. If Linux has taken over the JTAG pins,
  power-cycle once first.
- A SoC reset can leave the DAPLink confused. I measured
  `CMSIS-DAP command mismatch` after the watchdog fired; unplug/replug the
  probe if a new OpenOCD session cannot attach.
- If the board keeps resetting, the watchdog survived the reset (it is in the
  always-on domain). Run:

  ```bash
  openocd -f /home/dan/Workspaces/milk-v/mmu-test-vf2/jh7110_wdt_disable.cfg
  ```

  or power-cycle the board.

Explicit form if you use another project:

```bash
PROJ=/home/dan/Workspaces/milk-v/smp-mat-test-vf2 \
  RESET=wdt ./vf2-reset-to-uboot.sh /dev/ttyUSB0 60
```

## Method 2 — SRST through the JTAG probe

Wire the probe’s **RST/SRST** to the 40-pin header **pin 36** (GND pin 39),
then:

```bash
cd /home/dan/Workspaces/milk-v
RESET=srst ./vf2-reset-to-uboot.sh /dev/ttyUSB0 15
```

This uses `jh7110_daplink_srst.cfg`:

```tcl
reset_config srst_only srst_nogate
```

OpenOCD asserts SRST (a real board reset), releases it, and the key stream is
already running. This is the cleanest method if you can add the wire.

## Method 3 — no reset wire: power-cycle while keys are sent

```bash
cd /home/dan/Workspaces/milk-v
RESET=power ./vf2-reset-to-uboot.sh /dev/ttyUSB0 15
```

The script starts sending keys and prints:

```text
reset-to-uboot: no OpenOCD reset -- power-cycle / press RESET now.
```

Power-cycle the board during the window. U-Boot sees the keys and stops.

## Why `reset halt` / `reset run` are not enough

Measured on the VisionFive 2 Lite v1.1:

```text
U74 PC before reset: 0x40204f8e
U74 PC after  reset halt + resume: 0x40204f8e
U74 PC after  reset run:           0x40204f8e
```

OpenOCD’s RISC-V Debug Module reset only halts/resumes the U74 harts; it does
**not** reboot the SoC. The JH-7110S Bare-Metal notes say the same: `reset
halt` resets the core logic and halts, ready to load new code — it is not a
board power-on reset.

The real reset sources are:

- the **JH7110 watchdog** — Method 1, no pins;
- the **pad reset** on pin 36 — Method 2;
- **power cycling** — Method 3.

## What the script does

1. Configures `/dev/ttyUSB0` at 115200 8N1, raw.
2. Starts sending `CR` every 50 ms.
3. After 200 ms:
   * `RESET=wdt` (default) — arms the watchdog via `jh7110_wdt_reset.cfg`,
     waits, then disarms it;
   * `RESET=srst` — `jh7110_daplink_srst.cfg`, `reset run`;
   * `RESET=power` — no OpenOCD reset; you power-cycle;
   * `RESET=halt` / `RESET=run` — diagnostic only; they do not reboot.
4. Keeps sending keys through the boot, then stops.

## Troubleshooting

| symptom | check |
|---|---|
| OpenOCD cannot attach at all | board must be powered and at a point where JTAG is muxed (U-Boot); power-cycle and try immediately. Replug the DAPLink after a SoC reset if it reports CMSIS-DAP errors. |
| Linux still boots | Is the key stream active before the reset? Run the script with `RESET=power` and power-cycle during the window. |
| Board keeps rebooting after `RESET=wdt` | run `jh7110_wdt_disable.cfg`, or power-cycle. |
| No `StarFive #` | keys must arrive during the 3 s U-Boot countdown; keep the script running and reset/power-cycle. |
| Nothing on serial | `/dev/ttyUSB0` is the CH340 UART0 cable; 115200 8N1; pins 8 (TX) / 10 (RX). |
| Serial busy | `tio`/`picocom` owns the port; the script can write, but close it for reliable interaction. |
| OpenOCD scan-chain warning | The first TAP ID warning (`0x20000c2f` vs `0x07110cfd`) is expected on this board; debugging works. |

## After you have the prompt

Once at `StarFive #`, the board is ready for the Eclipse debug launch
(`load_image` from OpenOCD/GDB), exactly as `hw.sh` expects.
