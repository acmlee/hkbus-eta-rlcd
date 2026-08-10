# Plan: PCF85063 RTC — "update, not reset" the built-in clock

Status: Draft — approved for planning only, no code written yet.

## 1. Goal

The board has an onboard **PCF85063 RTC** (I2C addr `0x51`) with a
backup-battery holder. Today the firmware never touches it: at every boot
the clock resets to epoch-0 and depends entirely on an SNTP fetch from
`stdtime.gov.hk` (blocking up to 10 s, nothing displayed before sync).

Enhancement: use the RTC as the persistent wall clock so time survives
power-off, and **update** the RTC from `stdtime.gov.hk` — once after boot
sync and once daily at 06:00 — instead of leaving it stale (or "resetting"
it to a hardcoded value as the Waveshare examples do).

**Reuse-first strategy (this revision):** vendor the Waveshare PCF85063
driver + I2C wrapper code byte-for-byte where possible, and add only a thin
C shim + integration calls. No hand-written I2C/BCD driver.

## 2. Reuse source (preferred)

Use the dedicated example **`04_I2C_PCF85063`** under
`../waveshare-reference/02_Example/ESP-IDF/04_I2C_PCF85063` — it is the
smallest example exercising exactly this RTC (no LVGL/audio). Its
`components/port_bsp/i2c_equipment.cpp` is byte-identical to the Factory
Program's (`10_FactoryProgram`), so either can serve as the copy source.

Reference files we rely on (all confirmed read):

| File | Purpose |
|---|---|
| `components/port_bsp/i2c_bsp.h/.cpp` | `I2cMasterBus` — I2C bus init + read/write helpers (new `i2c_master` driver, ESP-IDF ≥ 5.0) |
| `components/port_bsp/i2c_equipment.h/.cpp` | `Rtc_Setup` / `Rtc_SetTime` / `Rtc_GetTime` + `rtcTimeStruct_t` |
| `components/user_app/user_app.cpp` | Creates `I2cMasterBus I2cbus(14,13,0)` → **SCL=GPIO14, SDA=GPIO13**, then `Rtc_Setup(&I2cbus, 0x51)` |
| `components/ExternLib/SensorLib/src/SensorPCF85063.hpp` | PCF85063 driver (header-only class) — BCD get/set, RAM-check init, `start`/`stop` |
| `components/ExternLib/SensorLib/src/SensorRTC.h` | `RTC_DateTime`, base class with ready-made `hwClockRead()` / `hwClockWrite()` (RTC⇄system-time) and `getDateTime(struct tm*)` |
| `components/ExternLib/SensorLib/src/SensorPlatform.hpp` + `src/platform/*` + `src/platform/espidf/*` | Comm layer (Custom-callback path used by the wrapper) |
| `components/ExternLib/SensorLib/Kconfig` | Selects `CONFIG_SENSORLIB_ESP_IDF_NEW_API=y` (new I2C API path) |

### 2.1 Files copied verbatim (no modification)

Trimmed vendored copy of the RTC path (subset of SensorLib — **not** the
whole library; excludes bosch/BHI260/touch which would add ~MB of unused
firmware blobs):

```
components/pcf85063_rtc/
├── CMakeLists.txt                     (new)
├── Kconfig                            (copy — enables SENSORLIB_ESP_IDF_NEW_API)
├── include/
│   └── rtc_pcf85063.h                 (new — C API, extern "C")
├── src/                               (copied verbatim)
│   ├── SensorLib.h                    (defines _BV, log_e/log_w/log_i, lowByte…)
│   ├── SensorLib_Version.h
│   ├── DevicesPins.h                  (included by SensorLib.h)
│   ├── REG/PCF85063Constants.h
│   ├── SensorRTC.h
│   ├── SensorPCF85063.hpp
│   ├── SensorPlatform.hpp
│   └── platform/
│       ├── SensorCommBase.hpp
│       ├── SensorCommCustom.hpp
│       ├── SensorCommCustom.cpp
│       ├── SensorCommCustomHal.hpp
│       ├── SensorCommDebug.hpp
│       ├── SensorCommDebug.cpp
│       ├── SensorCommStatic.hpp
│       ├── SensorCommStatic.cpp
│       └── espidf/
│           ├── SensorCommEspIDF_HW.hpp
│           ├── SensorCommEspIDF_I2C.hpp
│           ├── SensorCommEspIDF_SPI.hpp
│           └── SensorEspIDF_Lock.hpp
└── port/                              (copied verbatim)
    ├── i2c_bsp.h
    ├── i2c_bsp.cpp
    ├── i2c_equipment.h
    └── i2c_equipment.cpp
```

Dependency map (verified by reading the include chain):

- `SensorPCF85063.hpp` → `REG/PCF85063Constants.h`, `SensorRTC.h`, `SensorPlatform.hpp`
- `SensorRTC.h` → `sys/time.h`, `SensorPlatform.hpp`
- `SensorPlatform.hpp` → the 4 `platform/espidf/*.hpp` + `SensorCommCustom(.hpp)`, `SensorCommCustomHal.hpp`, `SensorCommDebug(.hpp)`, `SensorCommStatic(.hpp)`
- `SensorCommBase.hpp` → `SensorLib.h` (brings `_BV`, `log_*`)
- `SensorCommEspIDF_I2C.hpp` → `driver/i2c_master.h` only when `CONFIG_SENSORLIB_ESP_IDF_NEW_API=y` (Kconfig default)
- `i2c_equipment.cpp` → `i2c_bsp.h`, `SensorPCF85063.hpp`

Not needed (excluded): `SensorRtcHelper.*` (requires PCF8563, unused),
`SensorCommEnhanced.hpp`, touch/bosch/gauge files, Arduino platform files.

### 2.2 The only modified reused file

`port/i2c_equipment.h` gets **`extern "C"` guards** so its three functions
link against C callers. Everything else in it stays identical.

## 3. New code (thin layer only)

### 3.1 `components/pcf85063_rtc/include/rtc_pcf85063.h` + `rtc_wrap.cpp`

Small `extern "C"` shim (~60 lines) in the component:

- `bool rtc_init(void)` — create the I2C bus on GPIO13/14 (port 0,
  300 kHz — same as the example), call `Rtc_Setup(&bus, 0x51)`; log
  warning and return `false` if `rtc.begin()` fails (chip offline —
  feature silently disabled, firmware stays fully SNTP-only).
- `bool rtc_get_time(struct tm *out)` — `Rtc_GetTime` → fill `tm`;
  sanity check **year ≥ 2024** (BCD year 0x00 = 2000 means first boot /
  dead backup battery) → return `false` on invalid.
- `bool rtc_set_time(const struct tm *in)` — `Rtc_SetTime` (HKT wall time).
- `void rtc_store_system_time(void)` — read `time(NULL)` + `localtime_r`
  → `Rtc_SetTime`. Equivalent to the library's built-in
  `SensorRTC::hwClockWrite()` (reuse it directly if possible).
- `bool rtc_restore_system_time(void)` — `rtc_get_time` → `mktime` →
  `settimeofday`. (Prefer the library's `hwClockRead()`; note its
  `settimeofday` is `#if __BSD_VISIBLE`-guarded — if that macro is not
  active under ESP-IDF, do the `settimeofday` call in the shim instead.
  `hwClockWrite()` has no such guard.)

The shim owns the `static I2cMasterBus I2cbus(14, 13, 0)` global (moved
out of the example's `user_app.cpp`).

**Timezone decision:** RTC stores **HKT wall time** (consistent with
`hwClockWrite`/`hwClockRead` semantics and the firmware-wide `TZ=HKT-8`).
RTC→system uses `mktime` (interprets fields as HKT), system→RTC uses
`localtime_r`. Must run after `setenv("TZ", "HKT-8")` — it already runs
first in `time_sync_init()`.

### 3.2 Build glue

- `components/pcf85063_rtc/CMakeLists.txt`:
  `idf_component_register(SRC_DIRS "src" "port" SRCS "rtc_wrap.cpp"
  INCLUDE_DIRS "include" "src" "src/REG" "src/platform" "src/platform/espidf" "port"
  PRIV_REQUIRES esp_driver_i2c esp_driver_gpio esp_timer esp_log driver)`
  (`driver` kept as in the example for the compat umbrella; C++ sources
  compile automatically with the IDF toolchain.)
- `main/CMakeLists.txt`: add `pcf85063_rtc` to `REQUIRES`.
- No partition changes. Flash delta: a few KB.

## 4. Integration in `main/main.c` (existing code, small diffs)

Boot order today: NVS → display → button → battery → Wi-Fi →
`time_sync_init()` (SNTP, blocking ≤10 s) → config → tasks.

1. **`app_main`**: call `rtc_init()` right before `time_sync_init()`.
2. **`time_sync_init()`** (before `esp_netif_sntp_init`):
   `if (rtc_restore_system_time()) log "RTC restored HH:MM:SS"`.
   Header now shows the correct date instantly — no more `-- --- (---)`
   wait, and the clock still works if Wi-Fi/SNTP fails.
3. **After `ntp_wait_for_sync()` succeeds** in `time_sync_init()`:
   `rtc_store_system_time()` — **update** the RTC from `stdtime.gov.hk`
   (corrects drift accumulated since the last sync).
4. **Daily 06:00**: in the existing `display_task` resync branch
   ([main.c `ntp_resync()` caller](file:///Users/alanlee/Documents/trae_projects/hkbus-eta-rlcd/main/main.c#L467-L478)),
   call `rtc_store_system_time()` right after `ntp_resync()` returns.
   Same `tm_yday` once-per-day guard already in place. Optional: log the
   drift observed (RTC vs SNTP) for long-term drift-rate tracking.

No rendering changes. `EPOCH_SYNC_THRESHOLD` display logic stays — it now
simply passes on the first render loop because the RTC restores a valid
epoch before the task starts.

## 5. Behaviour matrix

| Scenario | Behaviour |
|---|---|
| Normal boot, RTC valid | Header shows correct time immediately; SNTP then updates RTC |
| Boot, Wi-Fi down, RTC valid | Clock still correct from RTC (SNTP timeout warning only) |
| First boot / dead backup battery (RTC year 2000 or < 2024) | `rtc_restore_system_time()` skipped; SNTP sets clock; RTC written after first sync |
| 06:00 daily, Wi-Fi OK | RTC updated from stdtime.gov.hk (existing resync + one write) |
| RTC chip offline / I2C fault | Log warning; feature disabled; pure-SNTP behaviour identical to today |
| Power-off with rechargeable RTC battery fitted | RTC keeps time; next boot has correct clock |

## 6. Risks / notes

- **GPIO13/14 ownership**: free in this firmware (display 11/12/40/5/41,
  KEY 18, battery ADC 4). Bus is shared with SHTC3 + audio codec but the
  firmware never touches I2C today — no contention.
- **Backup battery requirement (user-facing)**: time retention during
  power-off needs a rechargeable RTC battery in the PH1.0 holder
  ([waveshare-pinout.md](file:///Users/alanlee/Documents/trae_projects/hkbus-eta-rlcd/docs/waveshare-pinout.md#L50)).
- **`__BSD_VISIBLE`** uncertainty on `hwClockRead()` — resolved at
  implementation time (shim fallback exists).
- **PCF85063 init quirk** (kept verbatim): `initImpl()` probes the RAM
  register to distinguish PCF8563; failure → `Rtc_Setup` logs
  "InitFailure" and returns. Handled gracefully.
- **No DST handling** (HK has none) — HKT wall time is unambiguous.

## 7. Review of prior plan

The earlier plan called for a hand-written C I2C/BCD driver. Revised to
**vendor the upstream Waveshare code verbatim** (driver + I2C bus +
wrapper), which:
- eliminates ~100 lines of hand-ported BCD/register code and its bugs;
- keeps register values, masks, and the RAM-check init identical to the
  board vendor's tested code;
- adds only a ~60-line `extern "C"` shim and 4 small call sites.
Boot-restore and 06:00-update integration points are unchanged.

## 8. Verification steps (post-implementation)

1. `idf.py build` clean (C++ component compiles, links against C main).
2. Flash; confirm log: `RTC restored …` at boot and no `InitFailure`.
3. With RTC valid, power-cycle while Wi-Fi disabled → header date correct.
4. At 06:00, confirm log of daily resync + RTC write.
5. Remove battery / fresh RTC → verify first-boot path (no bogus 2000 date).

## 9. Appendix — Decision record: reuse verbatim vs. hand-written driver

**Decision (2026-08-10): Option A — reuse official example source verbatim.**
Recorded here for future reference; the hand-written driver remains an
optional future refactor only if it earns its keep.

| Criterion | **A. Reuse official source verbatim** (chosen) | **B. Refer-only, own C driver** (rejected) |
|---|---|---|
| **Correctness risk** | Low — vendor-tested on this exact board (BCD masking, clock-enable bit, 24 h mode, PCF8563-vs-PCF85063 RAM probe all proven) | High — a hand port can silently drop details (e.g. SEC reg bit 7 clock-integrity flag that the reference masks with `0x7F`); needs hardware validation before it can be trusted as a clock |
| **Implementation effort** | Small: copy ~19 small files + ~60-line `extern "C"` shim | Medium: ~150–200 lines of new C + I2C bring-up + BCD/register debug |
| **Code footprint** | Vendored C++ comm layer + mini framework (~19 files, few KB) | 1–2 small `.c/.h` files only |
| **Codebase consistency** | First C++ component in an all-C project; upstream style (Arduino-ish macros, `ESP_ERROR_CHECK`, void returns) differs from project conventions | Pure C; fits `esp_err_t`-checked, no-silent-failure style natively |
| **Verification burden** | Light — build + boot + log check (§8) | Heavy — every register path is new; must prove timekeeping stable across power cycles |
| **Upstream sync** | Free: future Waveshare fixes re-merge cleanly (files kept verbatim) | None — fixes must be re-derived manually |
| **Transparency / debuggability** | Indirect: register access goes through a virtualized comm layer; debugging requires tracing the framework | Direct: every register write visible in one file |
| **Extras for free** | `hwClockRead`/`hwClockWrite`, `RTC_DateTime`, alarm/clock-output APIs if ever needed | Must be re-implemented |
| **Build coupling** | Kconfig symbol `SENSORLIB_ESP_IDF_NEW_API` + `#if` version branches (legacy I2C fallback if symbol missing) | Direct `esp_driver_i2c` use, no config/version coupling |
| **License** | MIT — clean to vendor | n/a |
| **Project precedent** | Matches — the project already vendors Waveshare driver code (`u8g2_st7305`) | n/a |

**Rationale for choosing A:** the feature is a *clock* — a silent BCD/flag
bug would corrupt the displayed time with no crash, the worst failure
class. Vendor-tested code on this exact board removes that risk, matches
the project's existing precedent of adopting Waveshare driver code, and
avoids converting a small implementation task into a hardware-validation
task. The vendored-file cost is small and the files stay byte-identical to
upstream so future fixes merge cleanly.

**Possible future path (not now):** once the reference behaviour is fully
understood from runtime logs, the vendored driver could be replaced by a
hand-written C driver at low risk — but that stays an optional refactor,
not part of this plan.
