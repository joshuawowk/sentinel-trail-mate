# Trail Mate on the M5Stack Tab5 under Sentinel Bootloader

[Sentinel Bootloader](https://github.com/joshuawowk/sentinel-bootloader) is a firmware
launcher for the M5Stack Tab5: several firmware images live side by side in flash, any of
them starts from a touch screen, and a reset returns to the launcher. This page describes
the Sentinel build of Trail Mate: what it changes, how to build and install it, and what
does and does not work on the Tab5.

## What the Sentinel build changes

The build is the regular `tab5` ESP-IDF target with an extra profile, switched on with
`-DTRAIL_MATE_SENTINEL=ON`. The standalone `tab5` build is unchanged.

| Area | Standalone `tab5` | Sentinel build |
|---|---|---|
| Partition table | `partitions.csv`: two 6 MiB app slots, 3.9 MiB `ffat` | `builds/esp_idf/targets/tab5/partitions_sentinel.csv`: one app row, 24 KiB `nvs`, 1 MiB `ffat`, 64 KiB `coredump`. Sentinel places every row in its pool |
| Optimisation | `-O2` everywhere (about 5.0 MB image) | `-Os`, with LVGL and the display port kept at `-O2` (about 3.9 MB image) |
| Console | UART0, with USB-Serial-JTAG as secondary | USB-Serial-JTAG (USB-C) only, as every Sentinel app |
| Leaving Trail Mate | Restart only | **Return to Sentinel** tile (house icon) on the main menu, and the console's `launcher` command |
| ESP32-C6 | Trail Mate's HostLink companion protocol | Left alone: it runs Sentinel's shared esp-hosted firmware |
| Battery charging | Charger left disabled (`CHG_EN` low) | Charger enabled at start |
| Updates | None on the Tab5 | Sentinel offers updates from this repository's releases (`SENTINEL_APP_INFO`) |

### Return to Sentinel

The last tile of the main menu, **Return to Sentinel**, shows a house. It opens a dialog
with **Cancel** and **Return to Sentinel**; Esc on a keyboard and Cancel close it.
Confirming erases the `otadata` row and restarts, and Sentinel's bootloader then starts its
launcher (see "Returning to the launcher" in Sentinel's
[firmware-authors.md](https://github.com/joshuawowk/sentinel-bootloader/blob/main/docs/firmware-authors.md)).
Pressing the Tab5's reset button or a power cycle also returns to the launcher.

Restarts that Trail Mate makes itself (Settings that need a restart, a crash) keep Trail Mate
running; after three crashes in a row Sentinel starts its launcher.

### The ESP32-C6

The Tab5's ESP32-C6 is present and working, but under Sentinel it runs the esp-hosted slave
firmware (2.12.x) that the launcher and every other installed firmware use for Wi-Fi and
Bluetooth. Trail Mate's own C6 companion firmware speaks a different SDIO protocol
(HostLink), so the Sentinel build does not probe the C6 and never offers to replace its
firmware. The C6 Companion page is hidden, and the boot log says
`C6 is shared with Sentinel (esp-hosted); Trail Mate's C6 companion stays off`.

As a result, the features that Trail Mate runs through its C6 companion are not available in
this build: Wi-Fi (station, access point, TCP proxy, map and time sync over Wi-Fi) and
ESP-NOW Team links. LoRa (M5-Bus LoRa module), GNSS, maps from the SD card, chat, Team over
LoRa and the rest of the offline feature set do not use the C6.

### Storage

- **NVS** is Trail Mate's own and starts empty after every new install. Updating in place
  through Sentinel keeps it.
- **`ffat`** (resource packs, Reticulum pages) is installed blank at 1 MiB and formatted on
  the first start. Larger packs belong on the SD card.
- **SD card**: Trail Mate uses SDMMC (4-bit) mode. A card that another firmware used in SPI
  mode since the Tab5 was powered on stays in SPI mode and does not answer
  (`sdmmc_init_ocr ... 0x107`); switch the Tab5 off and on. See "The SD card" in Sentinel's
  [compatibility.md](https://github.com/joshuawowk/sentinel-bootloader/blob/main/docs/compatibility.md).
- **Core dump**: a crash leaves an ELF core dump in the `coredump` row; Trail Mate copies it
  to the SD card on the next start, and `sentinelctl.py coredump ID build.tab5_sentinel/trail-mate.elf`
  reads it from the launcher.

## Build

ESP-IDF **v5.5.4** (the version Trail Mate's CI uses):

```bash
source ~/esp/esp-idf-v5.5.4/export.sh
idf.py -B build.tab5_sentinel -DTRAIL_MATE_IDF_TARGET=tab5 -DTRAIL_MATE_SENTINEL=ON build
python tools/sentinel/package_tab5.py --build build.tab5_sentinel --dist dist
```

`package_tab5.py` writes `dist/trail-mate-tab5-sentinel.bin`, a merged image (bootloader,
partition table at `0x8000`, app). Sentinel needs the merged image, not the bare app: the
table is how it learns the `ffat` and `coredump` rows. The script refuses a build without
those rows, without Sentinel's app information at `0x120`, or with an image larger than its
app row.

The `.github/workflows/sentinel-tab5.yml` workflow builds both Tab5 images and attaches
`trail-mate-tab5-sentinel.bin` to tagged releases.

## Install

With Sentinel on the Tab5, either copy `trail-mate-tab5-sentinel.bin` to the SD card and use
**Add → SD card**, or from a computer on the USB-C cable:

```bash
python3 sentinel-bootloader/tools/sentinelctl.py install dist/trail-mate-tab5-sentinel.bin "Trail Mate"
```

A newer build with the same project name (`trail-mate`) updates the installed one in place
and keeps its NVS and `ffat`. Sentinel shows the version from the app descriptor
(`PROJECT_VER`) and, because the image names this repository, offers **Update** when a newer
release has the asset `trail-mate-tab5-sentinel.bin`.

## Console

The Sentinel build answers on USB-Serial-JTAG with Sentinel's console protocol (`@ ` reply
lines, `@ok` / `@err`, `@data` base64 blobs), so the tools in
[sentinel-apps](https://github.com/joshuawowk/sentinel-apps) work with it:

```bash
tools/sappctl.py info              # app Trail Mate project trail-mate version ...
tools/sappctl.py shot shot.png     # the panel's frame buffer, upright
tools/sappctl.py cmd tap 640 360   # touch at a logical (landscape) point
tools/sappctl.py launcher          # Return to Sentinel
```

| Command | Does |
|---|---|
| `info` | App, project, version, ESP-IDF version, uptime, free memory |
| `launcher` / `sentinel` | Return to Sentinel |
| `restart` | Restart Trail Mate (stays in Trail Mate) |
| `shot` | Screenshot of the DSI frame buffer (RLE16, as `sentinelctl.py shot` decodes) |
| `tap X Y [ms]` | Inject a touch at a logical point |
| `help` | List the commands |

With other ESP32 boards on USB, point `sappctl.py` at the Tab5 with `-p` or `SAPP_PORT`
(`/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_<MAC>-if00`).

## Reverse-engineering notes

The port was checked against the shipped upstream images (v1.2.3, T-Display-P4 TFT, the
closest ESP32-P4 target; there is no upstream Tab5 release) as well as the source:

- REA's firmware-region scan (Binwalk 3.1.0) of the merged web-flash image finds only
  crypto constant tables and small zlib/GPG false positives: the image is bootloader +
  partition table + app, with no filesystem images (`nvs`, `ffat`, `coredump` ship blank).
  The decoded table is `nvs 20K, otadata 8K, app0 6M, app1 6M, ffat 3968K, coredump 64K`.
- REA 6.7.0 opens the ESP32-P4 ELF but has no deep-analysis provider for it (its Ghidra lane
  wants Ghidra 12.1.x and double-float RISC-V; the P4 is `rv32imafc` single-float), so the
  symbol-level questions were answered with the ESP-IDF toolchain's `nm`/`objdump`.
- The image links a self-update path (`esp_ota_begin`, `esp_ota_set_boot_partition`) used by
  Settings → firmware update. It is not offered on the Tab5 (`firmware_target_id()` has no
  Tab5 entry), and under Sentinel `esp_ota_begin()` would fail cleanly
  (`ESP_ERR_OTA_PARTITION_CONFLICT`).
- Partition labels mounted by name: `nvs`, `ffat` (FAT with wear levelling, format on
  failure) and `coredump`. Nothing reads USB-Serial-JTAG on the Tab5 build, which is what
  lets the Sentinel console own its receive side.
- The C6 is reached only through Trail Mate's HostLink SDIO protocol (`essl_sdio_*`); there
  is no esp-hosted host in the image and no C6 flashing path for the Tab5.
