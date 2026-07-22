# hk-bus-eta-rlcd — Hong Kong Bus ETA Display

A dedicated, wall-mountable real-time bus arrival display for Hong Kong, built on the
Waveshare ESP32-S3-RLCD 4.2" board. It fetches live ETA data from KMB and Citybus open
APIs for up to three fixed routes and renders them on the reflective ST7305 monochrome
display in large, legible text — including Traditional Chinese (zh-HK) destination and
bus-stop names.

This project is for hobbyists and transit enthusiasts who want a single-purpose glanceable
display showing "when's the next bus" without needing to pull out a phone.

---

## Features

- **Dual-operator ETA fetch** — Real-time arrival estimates from both KMB (九巴) and
  Citybus (城巴) open APIs, fetched every ~30 seconds with randomised jitter to avoid
  thundering-herd alignment.
- **Traditional Chinese rendering** — zh-HK destination and bus-stop names rendered on the
  display via custom U8g2 bitmap font subsets (WenQuanYi Bitmap Song, 27,618 glyphs
  covering CJK Unified Ideographs + Extension A). ASCII fallback for pure-English strings.
- **Battery percentage indicator** — ADC-based battery voltage monitoring (ADC1, GPIO4)
  with a piecewise-linear 11-point Li-ion discharge curve approximation, median filtering,
  exponential smoothing, and Wi-Fi-idle-window sampling to avoid false readings from TX
  voltage sag. **Not a precision fuel gauge** — uses a generic discharge curve, not
  per-cell calibration.
- **Decoupled task architecture** — ETA fetching and display rendering run as independent
  FreeRTOS tasks. The display re-renders on wall-clock boundaries (every 15 seconds) and
  never waits for network data. ETAs are shared via a lock-free double buffer.
- **Daily NTP resync** — Automatic clock-drift correction once per day at 06:00 via
  `stdtime.gov.hk`.
- **No cloud dependency** — Once configured, the device operates independently on your
  local Wi-Fi network. No app, no account, no subscription.

---

## Hardware Requirements

| Item | Detail |
|------|--------|
| **Board** | [Waveshare ESP32-S3-RLCD-4.2](https://www.waveshare.com/esp32-s3-rlcd-4.2.htm) |
| **Display** | ST7305 reflective monochrome controller, 400 × 300 px, 1-bit, no backlight |
| **SoC** | ESP32-S3 (dual-core LX7 @ 240 MHz), 512 KB SRAM, 16 MB Flash, 8 MB PSRAM |
| **Power** | 18650 Li-ion battery (recommended) or USB-C. Battery-powered, fixed installation. |
| **Enclosure** | None provided — the Waveshare board has mounting holes; a 3D-printed or off-the-shelf enclosure is left to the builder. |

**Framework**: ESP-IDF (native), **not** Arduino. The display driver is a custom U8g2
back-end derived from the Waveshare official reference driver.

---

## Getting Started

### Prerequisites

- [ESP-IDF](https://github.com/espressif/esp-idf) v5.x (target chip: `esp32s3`).
  This project uses the ESP-IDF component registry (`idf_component.yml`) for cJSON and
  expects the ESP-IDF build system (`idf.py`). No minimum IDF version is hard-enforced,
  but v5.1 or later is recommended.
- A USB-C cable for flashing and serial monitoring.

### Build and Flash

```bash
# 1. Clone the repository
git clone https://github.com/acmlee/hk-bus-eta-rlcd.git
cd hk-bus-eta-rlcd

# 2. Set the target chip
idf.py set-target esp32s3

# 3. Configure Wi-Fi credentials
idf.py menuconfig
# Navigate to "WiFi Configuration" and enter your SSID and password.
# The defaults are placeholders — change them.

# 4. Build
idf.py build

# 5. Flash and monitor
idf.py flash monitor
```

> **Note**: The default `idf.py monitor` baud rate is 115200. Press `Ctrl+]` to exit
> the monitor.

### First Boot

On first power-on, the display will show a full-black test pattern for 500 ms, then
transition to the dashboard. The device will:
1. Connect to Wi-Fi (up to 5 retries at boot)
2. Synchronise time via NTP (stdtime.gov.hk, HKT UTC+8)
3. Load route configuration from SPIFFS (`routes.json`)
4. Begin the ETA fetch and render cycle

If Wi-Fi or SNTP is slow, the dashboard will show partial state (e.g. "----" for the
clock) within 10 seconds rather than staying blank.

---

## Configuration

### Routes (`routes.json`)

Route configuration is stored in `spiffs_data/routes.json` and flashed to the SPIFFS
partition. Edit this file before building to set your own routes (up to 3).

Example from the repository:

```json
{
  "routes": [
    { "operator": "KMB", "route": "30X",  "stop_id": "92A8281D80524F78",
      "stop_en": "Tai Ho Road Tsuen Wan", "stop_zh": "荃灣大河道",
      "dest_en": "Whampoa Garden",        "dest_zh": "黃埔花園" },
    { "operator": "KMB", "route": "238X", "stop_id": "88E1C9BB0B80711F",
      "stop_en": "Riviera Gardens Bus Terminus", "stop_zh": "海濱花園總站",
      "dest_en": "China Ferry Terminal",  "dest_zh": "中港碼頭" },
    { "operator": "CTB", "route": "930X", "stop_id": "003449",
      "stop_en": "Luen Yan Street",      "stop_zh": "聯仁街",
      "dest_en": "Causeway Bay",         "dest_zh": "銅鑼灣" }
  ],
  "refresh_seconds": 15
}
```

| Field | Description |
|-------|-------------|
| `operator` | `"KMB"` or `"CTB"` (Citybus) |
| `route` | Route number, e.g. `"30X"`, `"930X"` |
| `stop_id` | Stop ID for the API endpoint. KMB uses hex IDs; Citybus uses numeric IDs. Obtain these from the respective open data portals. |
| `stop_zh` / `stop_en` | Bus-stop name in Traditional Chinese and English. `stop_zh` is rendered first; `stop_en` is used as fallback if `stop_zh` is absent or empty. |
| `dest_zh` / `dest_en` | Destination label. `dest_zh` is rendered first. For KMB terminal stops, `dest_en` is also used for direction filtering (case-insensitive matching) to exclude the opposite-direction ETAs returned by the API. |
| `refresh_seconds` | Display render interval in seconds. The ETA fetch interval is independent (~30 s). |

### Wi-Fi

Wi-Fi credentials are configured via `idf.py menuconfig` under the "WiFi Configuration"
menu. They are stored in `sdkconfig` (not hardcoded in source code). The defaults are
placeholders — change them before building.

---

## Architecture Overview

The firmware runs two independent FreeRTOS tasks after boot:

- **`eta_fetch_task`** (priority: `tskIDLE_PRIORITY+2`) — Owns the ETA fetch loop and
  Wi-Fi modem-sleep toggling. Fetches data for all configured routes every ~30 seconds
  with ±10% randomised jitter. Writes results into the **inactive** half of a double
  buffer, then atomically flips the active index.
- **`display_task`** (priority: `tskIDLE_PRIORITY+3`) — Owns wall-clock render boundary
  alignment, daily NTP resync, and `render_dashboard()`. Reads
  from the **active** double-buffer and never waits for network data. Re-renders the
  display on `:00`/`:15`/`:30`/`:45` boundaries.

The two tasks share ETA data through a lock-free double-buffer mechanism: two full
`route_data_t[3]` arrays plus an atomically-swapped word-sized active index. No mutex
or semaphore is needed — the writer and reader never access the same buffer
simultaneously.

For full design details, see [PRD.md](PRD.md), [design.md](design.md), and
[CLAUDE.md](CLAUDE.md) in this repository.

---

## zh-HK Font Rendering

Traditional Chinese (zh-HK) destination and bus-stop names are rendered using custom
U8g2 bitmap fonts generated from **WenQuanYi Bitmap Song** via `bdfconv` (from the U8g2
toolchain):

| Font | Source | Glyph Height | Glyphs | Binary Size |
|------|--------|-------------|--------|-------------|
| `u8g2_font_zhhk_dest_18` | `wenquanyi_12pt.bdf` (16 px) | 19 px | 27,618 | ~1.2 MB |
| `u8g2_font_zhhk_stop_13` | `wenquanyi_9pt.bdf` (12 px) | 15 px | 27,618 | ~766 KB |

Coverage: ASCII (32–128) + CJK Unified Ideographs (U+4E00–U+9FFF) + CJK Extension A
(U+3400–U+4DBF).

The destination line is prefixed with "往" (drawn in the stop-font size) to indicate
direction. If a zh-HK field is absent in `routes.json`, the firmware falls back to
Helvetica bitmap fonts for English text.

A plan to upgrade to larger Noto Sans CJK HK fonts (24px/20px) exists at
[`docs/plan-larger-fonts-noto-otf2bdf.md`](docs/plan-larger-fonts-noto-otf2bdf.md) but
is deferred — the current WenQuanYi fonts are confirmed working on the physical display.

---

## Known Limitations

This section is honest about what the project **does not do**. For the full list, see
[PRD.md §8](PRD.md#8-out-of-scope).

- **Three routes maximum** — The display layout is designed for exactly 3 routes. No
  runtime route switching (no buttons, no touch).
- **Battery percentage is approximate** — Uses a generic 18650 Li-ion discharge curve,
  not calibrated to the specific cell in your device. The voltage is sampled during
  Wi-Fi-idle windows and median-filtered to avoid TX sag artefacts, but the underlying
  curve is an approximation. Expect ±5–10% accuracy.
- **No deep sleep** — The device maintains a persistent Wi-Fi connection and never enters
  deep sleep. ETA fetch runs every ~30 s; display refreshes every 15 s.
- **No OTA updates** — Firmware updates require a USB reflash via `idf.py flash`.
- **No audio, no touch, no BLE** — These features exist on the board hardware but are
  unused in this firmware.
- **SNTP only** — The onboard PCF85063 RTC is present but unused. Time sync relies on
  NTP via `stdtime.gov.hk`, with a daily resync at 06:00.
- **Memory** — Must operate without relying on PSRAM. PSRAM is available (8 MB) but the
  core display and ETA pipeline function with internal SRAM only.

---

## License

This project's original source code is licensed under the MIT License (see
[LICENSE](LICENSE)). This firmware bundles or is derived from several third-party
components under their own separate licenses, including a **GPL-licensed font component**
(WenQuanYi Bitmap Song). See [THIRD-PARTY-LICENSES.md](THIRD-PARTY-LICENSES.md) for
full details before redistributing compiled binaries.

---

## Credits

- **Waveshare** — For the ST7305 reference driver (`u8g2_st7305`) that this project's
  display driver is based on. ([Repository](https://github.com/waveshareteam/ESP32-S3-RLCD-4.2))
- **data.gov.hk** — For the KMB and Citybus open ETA APIs that make this project useful.
- **WenQuanYi (文泉驿)** — For the Bitmap Song font, which provides the CJK glyph data
  for the custom zh-HK font subsets. ([Project site](http://wenq.org/))
- **U8g2** — The display graphics library by olikraus that handles all font rendering
  and display communication. ([Repository](https://github.com/olikraus/u8g2))
- **cJSON** — The lightweight JSON parser used for API response parsing, provided via
  the ESP-IDF component registry.