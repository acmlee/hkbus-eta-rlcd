# Plan: Battery Optimizations — Without Function or Fetch-Frequency Reduction

**Status**: Implemented (2026-08-24 — Phases 1–4; build PASS). Implementation notes: Phase 1 gained a required `CONFIG_FREERTOS_USE_TICKLESS_IDLE` option and a required GPIO light-sleep wakeup for the KEY button (both discovered during implementation — see below). **On-device hang fix (rev 0.2 silicon, 2026-08-25)**: the first boot with PM enabled hung inside display init on the first automatic light-sleep entry — this board's ESP32-S3 is **rev v0.2**, and early S3 silicon hangs on light-sleep entry when the CPU / cache-tag memory are powered down. `CONFIG_PM_POWER_DOWN_CPU_IN_LIGHT_SLEEP` and `CONFIG_PM_POWER_DOWN_TAGMEM_IN_LIGHT_SLEEP` are now **disabled** (both were `=y` but inert until PM was enabled). `esp_pm_configure` was also moved to the end of `app_main` so all boot-time init runs with PM off (byte-identical boot until the tasks start). **On-device blank-screen fix (2026-08-25)**: after the hang fix the display went blank — every light sleep floated the ST7305 RST line because the boot-time GPIO sleep isolation (`CONFIG_ESP_SLEEP_GPIO_RESET_WORKAROUND`) switches all GPIOs to input/floating during sleep, resetting the display controller. Fixed with `gpio_sleep_sel_dis()` on the 5 display pins (MOSI/SCLK/DC/CS/RST) so they keep driving during light sleep. **SNTP resilience**: the boot-time SNTP sync timed out (RTC year 2000 → clock untrusted → all ETAs hidden); `eta_fetch_task` now forces an NTP resync every 60 s while the clock is untrusted instead of waiting for the next hourly SNTP poll. **USB-link + flashing fix (2026-08-25)**: light sleep powers down the USB-Serial-JTAG controller, so the console link to a host PC drops on every sleep entry. Replaced the hand-rolled gate with the **built-in IDF mechanism `CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION=y`** (USJ connection monitor: holds its own `ESP_PM_NO_LIGHT_SLEEP` lock while a host is attached — SOF-based, so power-bank-only chargers don't count — and releases it when the host detaches). Because the monitor's boot grace is only ~10 ms, `app_main` adds a **30 s boot grace** (`s_boot_grace_lock` + one-shot `esp_timer`, released by `usb_boot_grace_expired()`) so a freshly-booted device can be flashed via the auto-reset before light sleep engages. For an already-running (asleep) device: **hold BOOT + tap RST** to enter the ROM download mode (the ROM never sleeps) — always works. Phase 1b deferred. Phase 2 implemented as DFS (not hard 80 MHz). Phase 4 listen interval is a connect-time setting (no runtime API in IDF v6). **VERIFIED ON DEVICE 2026-08-25** — user confirmed power consumption greatly reduced (light sleep + DFS working) and USB flashing/serial monitoring working; display and ETA rendering unaffected. Remaining optional follow-ups: exact idle-current measurement and the 24 h Wi-Fi soak for a final runtime figure.
**Created**: 2026-08-23
**Tracking**: PRD.md §10 "Power" rows + Resolved Decision on active power-saving; related to (but explicitly **not** covering) Open Decision #4 (deep sleep) and #5 (display sleep + button wake — `docs/plan-display-sleep-button-wake.md`).

---

## Background

The device is battery-powered and currently draws ~40–60 mA average while in service. The existing power-saving is **Wi-Fi modem-sleep (`WIFI_PS_MIN_MODEM`) between fetch cycles** — that's the whole active story. The dominant idle current is the **SoC itself busy-idling at 160 MHz** while both tasks sit in `vTaskDelay` (30 s fetch wait, 10 s render wait, 50 ms button poll): `CONFIG_PM_ENABLE` is **off** in sdkconfig, so there is no light sleep and no frequency scaling.

The second-largest per-cycle cost is **HTTP/TLS**: every cycle fires 7 HTTPS requests (up to 6 routes + 1 weather), each a fresh client with a **full TLS handshake** ([http_util.c](file:///Users/alanlee/Documents/trae_projects/hkbus-eta-rlcd/main/http_util.c) does `init → perform → cleanup` per call). The ESP32-S3 has **no hardware crypto accelerator** — each handshake is ~100–500 ms of 160 MHz software mbedTLS plus extra radio round-trips.

This plan attacks *energy per unit of function*: every displayed feature, fetch cadence, render cadence, and configurable behaviour stays byte-for-byte identical. Nothing is removed, paused, or slowed — each watt-hour is just used more efficiently.

---

## Goal

- Reduce average battery current with **zero** change to:
  - ETA fetch cadence (30 s in service, 300 s OOS — PRD FR 15)
  - Weather piggyback cadence (600 s) and 30-min TTL
  - Render cadence (wall-clock `refresh_seconds`, default 10 s) and full-buffer redraw
  - Page-toggle latency (< 100 ms), all pages' routes fetched every cycle (PRD FR 14)
  - Boot-to-dashboard ≤ 10 s (PRD §10 #10), clock-trust gate, RTC update, footer/header content
- Every change independently revertible (config-isolated or trivially undoable).

## Non-Goals

- **No display sleep** (0x28/0x10) as a scheduled feature — the always-on display is a requirement; the image fades when the panel stops being driven. Kept only as an on-device experiment (§Phase 5e), test-before-adopt.
- **No pausing the ETA fetch task** — that is `plan-display-sleep-button-wake.md` territory (Open Decision #5) and reduces function.
- **No deep sleep** (Open Decision #4) — device must hold a persistent Wi-Fi connection.
- **No dirty-zone partial writes** — removed previously (header/footer boundary bug, see CLAUDE.md).
- **No data-source or endpoint changes.**

---

## Current Power Profile

| State | Current draw | Notes |
|-------|-------------|-------|
| Idle (both tasks in `vTaskDelay`) | ~30–50 mA | CPU active-idle at 160 MHz + radio in MIN_MODEM modem-sleep. **No light sleep** (`CONFIG_PM_ENABLE` off). |
| Fetch burst (~2–4 s / cycle, 7 HTTPS) | ~80–120 mA | Radio on (`WIFI_PS_NONE`) + CPU doing 7 TLS handshakes (software crypto). |
| Render burst (few ms SPI @ 24 MHz) | negligible | Full-buffer send every 10 s. |
| **Average, in service (30 s cycle)** | **~40–60 mA** | |
| **Average, OOS (300 s cycle)** | **~35–50 mA** | Night window; 300 s fetch interval. |

Estimated battery life (2000 mAh usable): roughly **1.5–2 days** at current drain.

---

## Design — Phases

### Phase 1: Automatic light sleep (largest win)

**What**: `CONFIG_PM_ENABLE=y` in sdkconfig + `esp_pm_configure({.light_sleep_enable = true})` in `app_main()`. Enable the IRAM options that let the SoC reach its low-µA light-sleep state while Wi-Fi is connected:

- `CONFIG_PM_ENABLE=y`
- `CONFIG_PM_SLP_IRAM_OPT=y`
- `CONFIG_ESP_WIFI_SLP_IRAM_OPT=y`
- `CONFIG_FREERTOS_USE_TICKLESS_IDLE=y` — **required, discovered during implementation**: the esp_pm Kconfig help states automatic light sleep is *disabled* without this; PM_ENABLE does not select it. sdkconfig.defaults is kept in sync.
- `CONFIG_PM_POWER_DOWN_CPU_IN_LIGHT_SLEEP=n` + `CONFIG_PM_POWER_DOWN_TAGMEM_IN_LIGHT_SLEEP=n` — **required for this board (ESP32-S3 rev v0.2)**: the first on-device boot with PM enabled hung inside display init on the first automatic light-sleep entry. Early S3 silicon (< v0.3) hangs on light-sleep entry when the CPU / cache-tag memory are powered down; both options were `=y` in sdkconfig but inert until PM was enabled.

**Why it's compatible with this firmware**:

- The device's entire life is `vTaskDelay`; with tickless idle (auto-selected by PM), every blocked interval becomes light sleep.
- Wi-Fi stays connected because `WIFI_PS_MIN_MODEM` is already the resting state ([main.c:367](file:///Users/alanlee/Documents/trae_projects/hkbus-eta-rlcd/main/main.c#L367)) — light sleep is the documented companion of modem-sleep.
- During the fetch burst the task is busy and `WIFI_PS_NONE` is set → the PM framework naturally blocks light sleep while the radio is active, then re-enters it after `WIFI_PS_MIN_MODEM` is restored. No code change needed for the toggle.
- Light sleep retains RAM, GPIO output levels (display CS/DC held), and the system clock (SoC RTC timer — the external PCF85063 is not disturbed). ADC1 and SPI power down and restore on wake — both are only touched while awake.
- The clock-trust poll (500 ms while untrusted) and the 50 ms button poll still sleep in the gaps.
- **KEY button requires a light-sleep wakeup (implemented)** — during light sleep the GPIO peripheral is powered down, so the old edge-triggered ISR would silently miss presses made entirely inside a sleep window. `button.c` now configures GPIO18 as `GPIO_INTR_LOW_LEVEL` (doubling as the wakeup source via `gpio_wakeup_enable` + `esp_sleep_enable_gpio_wakeup`) with a pending-latch + release-watchdog so one press-and-release still counts exactly once. `esp_sleep_enable_gpio_wakeup()` is available because `CONFIG_PM_POWER_DOWN_PERIPHERAL_IN_LIGHT_SLEEP` is not enabled.
- **Display pins must keep driving during light sleep (implemented)** — the boot-time sleep-GPIO isolation (`CONFIG_ESP_SLEEP_GPIO_RESET_WORKAROUND`, `esp_sleep_config_gpio_isolate()`) switches **every** GPIO to input/floating on each light-sleep entry. That floats the ST7305 RST line (GPIO41) → the controller resets to display-off → blank panel with no re-init. `u8g2_st7305_init()` now calls `gpio_sleep_sel_dis()` on MOSI (12), SCLK (11), DC (5), CS (40) and RST (41) so they keep their output levels during sleep. (Side effect: I2C 13/14, UART 43/44, ADC 4 remain isolated — harmless, no transactions while asleep.)
- **SNTP resilience while the clock is untrusted (implemented)** — a failed boot SNTP sync used to leave the clock untrusted (and all ETAs hidden) until the next hourly SNTP poll. `eta_fetch_task` now forces an NTP resync every 60 s while `s_clock_trusted == false`.
- **USB console link + flashing vs light sleep (implemented)** — light sleep powers down the USB-Serial-JTAG controller, so the console link to a host PC drops on every sleep entry (host re-enumerates on wake). `CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION=y` enables the built-in USJ connection monitor, which holds an `ESP_PM_NO_LIGHT_SLEEP` lock while a host is attached (SOF-based — a power-bank-only charger never counts) and releases it when the host detaches. Additionally `app_main` keeps the device awake for the first **30 s after boot** (`usb_boot_grace_expired()` one-shot timer) so a freshly-booted device can be flashed via the auto-reset before light sleep engages. For an already-running (asleep) device, **hold BOOT + tap RST** to enter the ROM download mode (the ROM never sleeps) — always works. Monitoring over USB thus disables light sleep (expected — dev activity); battery life is unaffected when unplugged.

**Files**: `sdkconfig` (+ `sdkconfig.defaults`), `main/main.c` (`app_main` — `esp_pm_configure` + boot-grace lock/timer; `eta_fetch_task` — 60 s NTP retry while untrusted; `usb_boot_grace_expired()` callback), `main/button.c` / `.h` (level-triggered ISR + GPIO wakeup), `components/u8g2_st7305/u8g2_st7305.c` (`gpio_sleep_sel_dis` on the 5 display pins), `main/CMakeLists.txt` (REQUIRES += `esp_driver_usb_serial_jtag`).

**Expected**: idle current ~30–50 mA → ~1–3 mA; in-service average → ~8–12 mA. **~4–5× improvement.**

**Risk**: Wi-Fi stability under light sleep is the only real unknown (AP-dependent). Must be validated with a power meter + 24 h soak. Rollback is a config change.

---

### Phase 1b (deferred): Event-driven button wait in `display_task`

**Status**: **Deferred** (2026-08-24). The plan's Phase 1b proposed replacing the 50 ms polling loop in `display_task` with an event-driven `xTaskNotifyWait`. It was **not** implemented:

- Under light sleep the poll is already nearly free — each `vTaskDelay(≤50 ms)` chunk sleeps the gap, so the marginal saving over event-driven wait is only ~1–2 mA.
- The essential half of Phase 1b *was* required and is done: the KEY button is now a GPIO light-sleep wakeup source (see Phase 1) so presses are never missed. The 50 ms poll then simply reads the latched press — page-toggle latency stays < 100 ms with zero ISR→task plumbing.
- Skipping the event-driven wait also avoids a GPIO-wakeup/ISR-trigger conflict: `gpio_wakeup_enable()` only accepts level wakeup and overrides the pin's ISR trigger, which would have changed the button's edge semantics.

The 50 ms poll loop in `display_task` is unchanged.

---

### Phase 2: CPU idle down-scaling via DFS (implemented; no hard 80 MHz)

**What (as implemented)**: instead of hard-setting 80 MHz in sdkconfig, `app_main` calls `esp_pm_configure` with `max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ` (160) and `min_freq_mhz = 80` — DFS scales the CPU down to 80 MHz whenever idle, and `eta_fetch_task` holds an `ESP_PM_CPU_FREQ_MAX` lock (`esp_pm_lock_acquire`) during the fetch burst so the TLS handshakes stay at 160 MHz.

**Why**: rendering (10 s budget) and JSON parsing are light; a hard 80 MHz would slow the TLS handshakes ~2× toward the 5 s `timeout_ms` of [http_util.c:81](file:///Users/alanlee/Documents/trae_projects/hkbus-eta-rlcd/main/http_util.c#L81). DFS gets the idle power saving without the crypto slowdown. The wifi driver does **not** auto-request max frequency under PM (verified: only a modem-sleep lock exists in `esp_wifi`), hence the explicit lock.

**Files**: `main/main.c` only (no sdkconfig change for this phase).

---

### Phase 3: In-burst HTTP/TLS connection reuse

**What**: Add a persistent per-host client to `http_util.c` (3 hosts max: KMB `data.etabus.gov.hk`, Citybus `rt.data.gov.hk`, HKO `data.weather.gov.hk`). Within one fetch cycle, reuse the same handle for all requests to that host — `esp_http_client_set_url()` between calls, no `cleanup()` until the burst ends. On `ESP_ERR_HTTP_CONNECT`/connection-closed, close and re-init (fallback). Keep the existing one-shot `http_get_body()` for callers that don't need reuse.

**Why**: The 7 requests in a cycle fire back-to-back (2–4 s window). Keeping the handle alive across that burst collapses 7 handshakes → **3** (one per host). **Cross-cycle reuse at 30 s spacing is not relied upon** — servers typically close idle connections, so the handle is deliberately dropped at the end of each cycle.

**Expected**: shorter fetch burst (less time at 80–120 mA) + far less 160 MHz crypto time. With Phase 2 (80 MHz) the crypto-time saving is larger in wall-clock terms.

**Risk**: Server sends `Connection: close` → reuse silently becomes per-request (no worse than today). The reconnect fallback must be exercised by the existing fetch-failure path (last-known-good preservation already handles the outcome).

**Files**: `main/http_util.c`, `main/http_util.h`, call sites in `main/eta_fetcher.c` (KMB/CTB) and `main/weather_hko.c`.

---

### Phase 4: Deeper modem sleep during the OOS night window

**What (as implemented)**: In `eta_fetch_task`, when the OOS state is active (fetch interval already 300 s), the radio rests at `WIFI_PS_MAX_MODEM` instead of `WIFI_PS_MIN_MODEM`. **Correction discovered during implementation**: IDF v6 has no runtime `esp_wifi_set_listen_interval()` API — the listen interval is a connect-time `wifi_config_t.sta.listen_interval` field, set to 5 (AP beacon periods) in `wifi_init_sta()`. Since the firmware only ever uses `WIFI_PS_MAX_MODEM` during OOS, a fixed 5-beacon interval is safe; in service (`WIFI_PS_MIN_MODEM`) the listen interval is irrelevant.

**Why**: During 300 s gaps the radio still wakes for every beacon (~100 ms DTIM) under MIN_MODEM. MAX_MODEM + the 5-beacon listen interval cuts those wakeups ~5× for the whole night. Fetch frequency is untouched — the extra beacon-sync latency before the first night fetch is invisible (the wake path already tolerates it).

**Risk**: Low. Only caveat: PS mode must be restored to `WIFI_PS_NONE` at the top of the next fetch (already the case).

**Files**: `main/main.c` (`wifi_init_sta` config + `eta_fetch_task` OOS block).

---

### Phase 5: Optional / test-first items (independent, low priority)

| # | Item | Expectation | Gate |
|---|------|-------------|------|
| 5a | SPI clock 24 → 40 MHz ([u8g2_st7305.c:13](file:///Users/alanlee/Documents/trae_projects/hkbus-eta-rlcd/components/u8g2_st7305/u8g2_st7305.c#L13)) | Halves the few-ms render transfer; reference driver uses 40 MHz | on-device render check |
| 5b | `CONFIG_ESP_PHY_MAX_WIFI_TX_POWER` 20 → ~14–17 dBm | Less TX current on every request | reliability soak (user decision — link budget is unknown) |
| 5c | Power down unused on-board peripherals (SHTC3, audio codec) if their rails are GPIO-controlled | A few mA quiescent | hardware check of `docs/waveshare-pinout.md` + schematic |
| 5d | Reduce `timeout_ms` 5000 → 3000 | Less radio-on time waiting on dead servers | none (fetch-failure path handles it) |
| 5e | **Experiment only**: display-off (0x28) between renders | ~10–20 mA panel saving **if** the reflective image holds >10 s | on-device: if visible fade within a refresh interval, abandon (expected) |

5e is expected to fail (the reason it is not a scheduled phase) — the panel stops being driven during 0x28 and the image decays. It is listed so the option is on record with its gate.

---

## Power Budget (estimated)

| State | Today | +P1 (light sleep) | +P1–P3 combined |
|-------|-------|-------------------|-----------------|
| Idle | 30–50 mA | 1–3 mA | 1–3 mA |
| Fetch burst | 80–120 mA, 2–4 s | same | 60–100 mA, shorter (3 handshakes; 80 MHz) |
| In-service average (30 s cycle) | ~40–60 mA | ~8–12 mA | ~5–8 mA |
| OOS average (300 s cycle) | ~35–50 mA | ~3–5 mA | ~2–4 mA |

Battery life (2000 mAh usable): ~1.5–2 days → roughly **1–2 weeks** (in-service), longer at night. **These are estimates — measure, don't trust the table.**

---

## Runtime Estimation (anchored to measured baseline)

The measured baseline is **26–30 h** on battery. The estimates below are computed as **improvement multipliers** (average-current ratio) and applied to that measured runtime — this is robust to the exact battery capacity, which only scales the absolute runtimes.

### Duty-cycle model (per 30 s in-service cycle)

| Component | Today | With P1 (light sleep) |
|----------|-------|------------------------|
| Fetch burst (~6 HTTPS reqs, ~2.5 s @ ~100 mA) | ~250 mAs | unchanged |
| Idle (~27.5 s @ ~40 mA active-idle) | ~1100 mAs | ~27.5 s @ ~3.5 mA (light sleep + MIN_MODEM) |
| Render bursts (10 s full-buffer SPI) | <1% — negligible | unchanged |

Today's average ≈ **45 mA**, consistent with 26–30 h on a ~1.2–1.3 Ah usable battery. Weather (1 request / 20 cycles) and render bursts are in the noise.

### Projected averages, multipliers, and runtime

| Scenario | Est. avg current | Multiplier | Runtime on 26–30 h baseline |
|----------|------------------|-----------|------------------------------|
| **Today** | ~44 mA | 1× | **26–30 h** |
| **+ P1 light sleep** | ~10 mA | ~4.4× | **~4.8–5.5 days** |
| **+ P1 + P3 TLS reuse** (7→3 handshakes, burst ~2.5→1.4 s) | ~7.2 mA | ~6.1× | **~6.6–7.6 days** |
| **+ P1 + P3 + P4** (OOS MAX_MODEM) | ~7.0 mA | ~6.3× | **~6.8–7.9 days** |

P4 adds little to the **daily** average because the default OOS window is only 4.5 h/day (01:00–05:30); it matters more if the night window is widened.

### Bounds and caveats

- **Conservative**: if light-sleep idle lands at 5–8 mA (DTIM/beacon rate, AP behaviour, RSSI), the combined multiplier drops to ~4–4.5× → **~4.5–5.5 days**.
- **Optimistic**: idle at ~2 mA (P1b event-driven button wait) → up to **~8.5 days**.
- **Phase 2 (80 MHz) is near a wash after P1** — the fetch burst is radio/crypto-bound, not power-bound; its only real value is if light sleep underperforms.
- Real-world spread also comes from beacon/DTIM settings, Wi-Fi signal (TX retries), and battery age/chemistry.

**Bottom line**: expect roughly **1 week** (range ~4.5–8 days) from light sleep + TLS reuse — a **~5–6× improvement** — with light sleep alone delivering most of it (~4.4×).

---

## Files to Change

| File | Change |
|------|--------|
| `sdkconfig` + `sdkconfig.defaults` | `CONFIG_PM_ENABLE=y`, `CONFIG_PM_SLP_IRAM_OPT=y`, `CONFIG_ESP_WIFI_SLP_IRAM_OPT=y`, `CONFIG_FREERTOS_USE_TICKLESS_IDLE=y` (Phase 1). CPU stays at 160 MHz (Phase 2 = DFS at runtime). |
| `main/main.c` | `esp_pm_configure()` (light sleep + DFS) + `esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX)` in `app_main` (P1+P2); acquire/release the lock around the fetch burst in `eta_fetch_task` (P2); `wifi_config_t.sta.listen_interval = 5` (P4); `WIFI_PS_MAX_MODEM` re-assert while OOS (P4); `http_close_reuse_clients()` at cycle end (P3) |
| `main/button.c` / `.h` | Level-triggered ISR + `gpio_wakeup_enable`/`esp_sleep_enable_gpio_wakeup` + pending-latch/release-watchdog (P1, required) |
| `main/http_util.c` / `.h` | Persistent per-host client handles + `http_get_body_reuse()` + `http_close_reuse_clients()` (P3) |
| `main/eta_fetcher.c`, `main/weather_hko.c` | Use `http_get_body_reuse()` (P3) |
| `main/CMakeLists.txt` | REQUIRES += `esp_pm esp_timer` |
| `docs/CLAUDE.md` | New/updated power-management section |
| `PRD.md` | Update "Power" rows; note in §10 that active power-saving now includes light sleep |
| `HANDOFF.md` | Session log (on implementation) |

---

## Critical Review

1. **TLS keep-alive across 30 s cycles is dead on arrival** — an early draft proposed reusing connections between cycles. At 30 s spacing most servers close idle connections, so the win would rarely materialise. **Reframed** to *within-burst* reuse (7 back-to-back requests, 3 hosts → 3 handshakes). This is the defensible version; the fallback (server closes) degrades to today's behaviour, never worse.
2. **Light sleep requires the IRAM options to actually reach the low-µA state** — without `CONFIG_PM_SLP_IRAM_OPT` / `CONFIG_ESP_WIFI_SLP_IRAM_OPT`, the CPU must keep flash access paths hot for beacon processing, raising sleep current. Included in Phase 1. **Must be measured** — the ~30–50→1–3 mA claim is the theoretical state.
3. **The 50 ms button poll fragments sleep** — each poll costs ~1–3 ms of entry/exit. 20/s is real overhead and shortens the windows. P1b (event-driven wait) removes it; it's optional only because Phase 1 still wins by a large margin without it.
4. **80 MHz slows the TLS handshake ~2×** — acceptable inside the 5 s timeout for these endpoints, but it's the one change with a worst-case interaction (a slow network day during boot). Ordered after Phase 1 measurement; revertible in one config line.
5. **Display-off between renders was considered and rejected** — the ST7305 is a reflective LCD; 0x28 stops the panel drive and the image fades within the refresh interval. Kept as Phase 5e with an explicit abandon gate, so the decision is on record.
6. **The fetch task's `WIFI_PS_NONE` toggle and light sleep do not fight** — while `WIFI_PS_NONE` is set the radio is busy, so the PM framework blocks light sleep automatically; after `WIFI_PS_MIN_MODEM` is restored it re-enters. No locking or ordering change needed in [eta_fetch_task](file:///Users/alanlee/Documents/trae_projects/hkbus-eta-rlcd/main/main.c#L349).
7. **No functional regressions identified** — the clock-trust gate, RTC update-on-sync, weather TTL, OOS state machine, double-buffer flip, and page toggle all run on paths that light sleep does not touch (task code executes only while awake).

---

## Implementation Order + Verification Checklist

**Order (as implemented, 2026-08-24)**: Phase 1 (light sleep + button wakeup) → Phase 2 (DFS + CPU lock) → Phase 3 (TLS reuse) → Phase 4 (OOS MAX_MODEM). Build: PASS. **On-device verification: PASSED 2026-08-25** — power greatly reduced, USB flashing/monitoring works, display/ETAs fine (items marked ✔ below; the unmarked items are optional long-run measurements).

**Measurement**: baseline the device with a bench supply / INA219 / series multimeter (or the existing battery ADC as a crude voltage-decay trend). Re-measure after each phase.

- [ ] Baseline current recorded (idle, in-service average, OOS average)
- [x] P1: light sleep active — idle current drops to single-digit mA ✔ (24 h Wi-Fi soak not yet run — optional)
- [x] P1: **display keeps showing content through light sleep** (no blanking/reset from the GPIO sleep isolation) — the frame persists between renders ✔
- [x] P1: **clock-untrusted path recovers** — forced NTP resync clears `Connecting...`/`----` and ETAs flow ✔
- [ ] P1: boot-to-dashboard still ≤ 10 s; clock-trust gate, RTC "updated from SNTP", footer "Connecting..." all behave as before
- [ ] P1: **KEY button page-toggle still works with light sleep active** — presses made while the SoC is asleep wake it and toggle within < 100 ms; holding the button toggles exactly once (release-watchdog)
- [x] P1: **USB console link + flashing** ✔ — 30 s boot grace lets the auto-reset flash work; `CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION` keeps the link up while a host is attached; unplug → light sleep resumes
- [ ] P2: TLS handshakes complete well inside the 5 s timeout (watch the `HTTP error`/timeout logs); render cadence unaffected
- [ ] P3: KMB/CTB/HKO fetch logs show ~3 handshakes per cycle (or reuse); fetch-failure path (AP off) still preserves last-known-good ETAs for 180 s
- [ ] P4: OOS logs show MAX_MODEM transition at night; 300 s cadence and 05:30 restore unchanged; weather still updates every ~600 s
- [ ] Page toggle latency unchanged (< 100 ms); all pages render
- [ ] Final averages recorded and compared against the budget table

**Rollback**: Phase 1 is sdkconfig lines (revert = restore old values; the button and PM code is inert without `CONFIG_PM_ENABLE`). Phase 3/4 are isolated code paths (revert = keep `http_get_body()` one-shot / drop the MAX_MODEM branch). No single change is load-bearing for anything else.
