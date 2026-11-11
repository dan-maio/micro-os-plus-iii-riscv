/*
 * file-server.hpp -- file transfer over Wi-Fi to the RAM disk: a TCP server
 * on port 5000 and a UDP "here I am" beacon on port 5001.
 *
 * The protocol is the MangoPi MQ-Pro's (host/wifi_fs.py works unchanged):
 * text lines ending in LF, raw bytes only where a size announced them.
 *
 *   ls                  -> "<size> <name>" per file, then "OK"
 *   df                  -> "TOTAL <bytes> FREE <bytes>", then "OK"
 *   put <name> <size>   -> "READY", <size> raw bytes, then "OK <crc32>"
 *                          (CRC-32 of the file as read back from the disk)
 *   get <name>          -> "DATA <size> <crc32>", <size> raw bytes, "OK"
 *   rm <name>           -> "OK"
 *   format              -> "OK"
 *   verdict PASS|FAIL [text] -> "OK"; the test then prints its RESULT
 *   failure             -> "ERR <reason>" (the connection stays usable)
 *
 * The beacon: once a second, a UDP broadcast to port 5001 with
 *   "VF2-RAM-DISK <ip> 5000\n"
 * so the PC finds the board without reading its UART.
 *
 * Everything here runs on the thread that owns the radio and lwIP (NO_SYS):
 * the lwIP callbacks, FatFs (not reentrant) and the beacon.
 *
 * Copyright (c) 2026 Dan. MIT license.
 */
#pragma once

#include <cstdint>

namespace file_server
{
  inline constexpr std::uint16_t tcp_port = 5000;
  inline constexpr std::uint16_t beacon_port = 5001;

  /// Mount (or create) the FAT filesystem on the RAM disk; print it.
  bool
  mount_disk (void);

  /// Start listening on tcp_port.  After wifi_net::init().
  bool
  start (void);

  /// Called from the radio thread's loop: sends the beacon once a second
  /// while there is an address.
  void
  poll (void);

  struct counters
  {
    std::uint32_t files_received{ 0 };
    std::uint32_t files_sent{ 0 };
    std::uint64_t bytes_received{ 0 };
    std::uint64_t bytes_sent{ 0 };
    std::uint32_t connections{ 0 };
    std::uint32_t beacons{ 0 };
  };

  [[nodiscard]] const counters&
  stats (void);

  /// The PC's verdict, once it sent one.
  [[nodiscard]] bool
  verdict_received (void);
  [[nodiscard]] bool
  verdict_pass (void);
  [[nodiscard]] const char*
  verdict_text (void);

  /// The RAM disk's file list on the console.
  void
  list_files (const char* title);
} // namespace file_server
