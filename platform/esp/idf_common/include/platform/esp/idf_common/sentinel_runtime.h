#pragma once

// Integration with Sentinel Bootloader (https://github.com/joshuawowk/sentinel-bootloader),
// the M5Stack Tab5 firmware launcher. Only built with -DTRAIL_MATE_SENTINEL=ON.
//
// Under Sentinel the app runs from a one-slot partition view (its own ota_0, NVS and data
// rows). Restarts the app makes itself keep it running; a blank otadata makes Sentinel's
// bootloader start the launcher instead.

namespace platform::esp::idf_common::sentinel
{

// True when this image was built for Sentinel (TRAIL_MATE_SENTINEL).
bool enabled();

// Erase otadata and restart: Sentinel's bootloader then starts its launcher.
// Does not return.
[[noreturn]] void return_to_sentinel();

// Board defaults Sentinel expects of every firmware on the Tab5, applied once the board's
// IO expanders are initialised: battery charging stays enabled (Trail Mate's Tab5 BSP
// otherwise leaves CHG_EN low, so the pack would not charge while Trail Mate runs).
void on_board_initialized();

// Start the Sentinel console on USB-Serial-JTAG. It speaks Sentinel's console protocol
// ("@ " reply lines, "@ok"/"@err", "@data" base64 blobs) so sentinel-apps' sappctl.py and
// devrun.py can drive Trail Mate: info, launcher, restart, shot, tap, help.
// Safe to call more than once; a no-op when not built for Sentinel.
void start_console();

} // namespace platform::esp::idf_common::sentinel
