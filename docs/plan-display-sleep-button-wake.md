# Plan: Display Sleep + Button Wake (replacing voltage-profile dimming)

**Status**: Pending
**Created**: 2026-07-17
**Tracking**: PRD.md §10 Open/TBC Decisions (new entry)
**Replaces**: PRD.md §10 Resolved Decision #7 (time-based voltage/contrast mode — found ineffective)

---

## Background

PRD §10 Decision #7 specified a time-based display voltage/contrast mode: high-contrast
drive voltages during 06:00–10:00, reduced voltages all other hours, via partial register
writes (0xC0–0xC5). This was implemented as `u8g2_st7305_set_voltage_profile()` in
`components/u8g2_st7305/u8g2_st7305.c` and triggered from `display_task` in `main/main.c`.

**Finding**: The voltage-tweak approach does not produce a visible contrast difference on
the ST7305 reflective monochrome LCD. The deltas are ~7–9% of drive voltage, which is below
the visible threshold for this panel type. The low profile also doesn't meaningfully reduce
power or glare — the display remains fully driven, just at slightly lower analog levels.

This plan replaces the voltage-tweak approach with a simpler, more effective one: **display
sleep (0x28) outside the morning window, with a button-triggered wake for ad-hoc glances.**

---

## Goal

- Display **off** (sleep mode) outside the morning window to save power.
- Display **on** during the morning window (default 07:00–10:00, configurable via Kconfig).
- **Button-triggered wake** at any time outside the morning window: press → display on for
  a timeout (default 15 min, configurable via Kconfig) → auto-sleep.
- **Button press while display is awake (outside morning window)**: reset the wake timeout.
- **Button press during the morning window does nothing** — the display is already on.
- Remove or deprecate the voltage-profile code path (it doesn't work as intended).

---

## Hardware: Wake Button

### On-board KEY button (GPIO18)

The Waveshare ESP32-S3-RLCD-4.2 has two side buttons: **BOOT** (GPIO0, strapping pin used
for download mode) and **KEY** (GPIO18, custom-function button). This plan uses the **KEY**
button on **GPIO18**.

**Verification**: Confirmed against the Waveshare reference factory firmware at
`../waveshare-reference/02_Example/ESP-IDF/10_FactoryProgram/components/port_bsp/button_bsp.c`:
- Line 17: `#define GP18_KEY_PIN 18` — the "KEY" button is on GPIO18.
- The consumer task `KEY_LoopTask` in `user_app.cpp` confirms this is the silkscreen "KEY"
  button, not the BOOT button.
- Both buttons are active-low (`BOOT_Active 0`), configured with internal pull-up.

**Advantages**:
- No extra hardware — the button is already on the board.
- GPIO18 is not a strapping pin (unlike GPIO0/BOOT), so there are no boot-mode side effects.
- GPIO18 is not claimed by the display init or any other peripheral in this project. The
  current display config uses GPIO5, 11, 12, 40, 41 — GPIO18 is free.

**Electrical characteristics**:
- Active-low: the button pulls GPIO18 to GND when pressed. Read as: `0 = pressed`,
  `1 = released`.
- Internal pull-up enabled (as in the Waveshare reference). No external resistor needed.

---

## Design

### State machine

```
                  ┌──────────────────────────────────┐
                  │                                  │
                  ▼                                  │
  ┌─────────────────────────────┐                   │
  │   DISPLAY_OFF (sleep mode)   │                   │
  │  u8g2_SetPowerSave(u, 1)     │                   │
  │  ETA fetch PAUSED            │      button       │
  │  Wi-Fi modem-sleep (deep)    │ ──────────────►   │
  │  display_task skips render   │                   │
  └─────────────────────────────┘                   │
          ▲                                         │
          │ wake timeout expires                     │
          │ (or outside morning window)              │
          │                                         │
  ┌─────────────────────────────┐                   │
  │    DISPLAY_ON (active)       │                   │
  │  u8g2_SetPowerSave(u, 0)     │                   │
  │  ETA fetch RUNNING           │                   │
  │  render_dashboard() runs     │                   │
  │  every 15 s as before        │                   │
  └─────────────────────────────┘                   │
          ▲                                         │
          │ 07:00 boundary crossed (morning window)  │
          │ OR button pressed while sleeping          │
          │ (button press also triggers immediate     │
          │  ETA fetch)                               │
          │                                         │
          └──────────────────────────────────────────┘
```

### Wake logic

- **Boot (pre-SNTP)**: display on, ETA fetch running. Display stays on until SNTP sync
  completes (or 5-minute fallback timeout — see §9 below). After sync, the normal
  sleep/wake logic takes over.
- **Morning window (default 07:00–10:00, configurable)**: display always on. `display_task`
  renders every 15 s as it does now. ETA fetch runs every ~30 s as it does now. **Button
  press does nothing** — display is already on, no wake timeout is started.
- **Outside morning window**: display sleeps. `display_task` still ticks every 500 ms (to
  check the time boundary and button) but skips `render_dashboard()` while asleep. **ETA
  fetch is paused** — `eta_fetch_task` blocks on a task notification, not `vTaskDelay`.
  Wi-Fi switches to `WIFI_PS_MAX_MODEM` (deeper power saving) for the entire sleep period.
- **Button press while asleep**: wake display for the configured timeout (default 15 min).
  **Triggers an immediate ETA fetch** (signals `eta_fetch_task` via task notification).
  Wi-Fi switches back to `WIFI_PS_MIN_MODEM` before the fetch.
  `display_task` renders immediately on wake (does not wait for the next 15 s boundary).
  After timeout, sleep again (ETA fetch pauses again, Wi-Fi back to `WIFI_PS_MAX_MODEM`).
- **Button press while awake (outside morning window, within wake timeout)**: reset the
  wake timer (`wake_until = now + timeout`). Each press resets — the counter is consumed
  and processed every 500 ms tick, so rapid presses all reset the timeout.

### Button reading approach

Two options:

1. **Polling in `display_task`** (simplest): Check GPIO18 at the top of each ~15 s tick.
   - **Pro**: No interrupt setup, no concurrency concerns.
   - **Con**: Up to 15 s latency between press and wake. For a glanceable display, this is
     too slow — the user would press the button and stare at a blank screen.
   - **Verdict**: Unacceptable UX.

2. **GPIO interrupt + flag** (recommended): Configure GPIO18 as interrupt-on-falling-edge.
   The ISR sets a `volatile bool s_wake_requested` flag. `display_task` checks the flag at
   the top of each tick and wakes immediately if set.
   - **Pro**: Near-instant wake (within 1 tick of the next 15-s boundary, or we can shorten
     the sleep interval when display is off).
   - **Con**: ISR setup, flag management. Minimal — this is a textbook pattern.
   - **Verdict**: Use this.

**Latency refinement**: When display is asleep, `display_task` can shorten its tick
interval (e.g. poll every 500 ms instead of every 15 s) to reduce wake latency without
rendering. This keeps power low (no SPI traffic) while making button response feel instant.

---

## Implementation Plan

### Phase 1: Button driver

New file `main/button.c` + `main/button.h`:
- `button_init(void)`: configure GPIO18 (hardcoded — the KEY button is physically wired to
  this pin) as input with internal pull-up, falling-edge interrupt.
- ISR increments a `volatile uint32_t s_press_count` counter (atomic increment).
- `button_consume_presses(void)`: returns the count and resets to 0 (atomic swap). Returns
  0 if no presses since last call.
- `button_get_press_count(void)`: returns current count without clearing (for debug).
- **Debouncing**: The ISR fires on falling edge. Contact bounce may cause multiple fires
  within ~10 ms. Since `display_task` polls every 500 ms, all bounces within one poll
  interval are accumulated into the count. A single physical press increments the counter
  by 1–3 (typical bounce). The consumer treats any non-zero count as "at least one press"
  and resets the timeout. Exact count is not critical — the semantics are "was there a
  press since last poll?" No hardware/software debounce timer needed.
- **Each press resets**: Every non-zero return from `button_consume_presses()` causes
  `display_task` to reset `wake_until = now + timeout`. Multiple presses within one poll
  interval still result in a single reset (they're consumed together). Presses across
  multiple poll intervals each reset independently.

### Phase 2: Display sleep/wake in `main.c`

Modify `display_task`:
- Add state: `bool display_awake` (true at boot).
- Add wake-timeout tracking: `time_t wake_until` (epoch). If `now < wake_until`, display
  stays awake.
- Add SNTP-sync tracking: `bool sntp_synced` (false at boot, set true when `time()`
  returns a post-2023 epoch OR 5 minutes have elapsed since boot).
- Add boot-time tracking: `time_t boot_time` (set from `xTaskGetTickCount()` at task start,
  used for the 5-minute SNTP fallback).
- **Boot initialiser**: Before the main loop, set `display_awake = true` (display is on
  from `display_init()`). Do NOT set `wake_until` yet — the display stays on unconditionally
  until SNTP syncs (or the 5-minute fallback fires).
- At top of each tick:
  1. Read `time(&now)`. Check if SNTP has synced: if `now > 1700000000` (a 2023 epoch),
     set `sntp_synced = true`.
  2. If `!sntp_synced`: check if 5 minutes have elapsed since boot
     (`(xTaskGetTickCount() - boot_ticks) * portTICK_PERIOD_MS >= 300000`). If so, set
     `sntp_synced = true` (fallback — treat as synced with a guessed time). Display stays
     on, render dashboard. Sleep 500 ms. Continue to next tick.
  3. **First tick after SNTP sync (or fallback)**: Transition from "boot on" to "normal
     operation". If inside morning window: display stays on, clear `wake_until`. If outside
     morning window: set `wake_until = now + timeout` (simulated button press — gives the
     user the full wake timeout after sync, then sleeps).
  4. Determine `in_morning_window` using Kconfig values (with midnight wrap-around logic).
  5. If `in_morning_window`: display must be on. Clear `wake_until`. Ensure
     `display_awake = true`. Set `s_fetch_allowed = true`. Set Wi-Fi to
     `WIFI_PS_MIN_MODEM`.
  6. If `!in_morning_window`:
     a. Check `button_consume_presses()` → if > 0, set `wake_until = now + timeout` and
        signal `eta_fetch_task` to fetch immediately (via `xTaskNotifyGive`).
     b. Determine `should_be_awake = (now < wake_until)`.
     c. If `should_be_awake && !display_awake`:
        - Set Wi-Fi to `WIFI_PS_MIN_MODEM` (wake radio from MAX_MODEM).
        - `u8g2_SetPowerSave(&g_lcd.u8g2, 0)`, `display_awake = true`.
        - Set `s_fetch_allowed = true`, signal `eta_fetch_task` (fetch on wake).
     d. If `!should_be_awake && display_awake`:
        - `u8g2_SetPowerSave(&g_lcd.u8g2, 1)`, `display_awake = false`.
        - Set `s_fetch_allowed = false`.
        - Set Wi-Fi to `WIFI_PS_MAX_MODEM` (deeper power saving during sleep).
  7. If `display_awake`: render dashboard immediately (not aligned to 15 s boundary — on
     wake, render right away). For subsequent ticks while awake, align to 15 s boundaries
     as before.
  8. If `!display_awake`: skip render, sleep 500 ms.

### Phase 2b: ETA fetch task changes in `main.c`

Modify `eta_fetch_task`:
- Replace the `vTaskDelay(pdMS_TO_TICKS(delay_ms))` at the bottom of the loop with
  `xTaskNotifyWait(0, 0, NULL, pdMS_TO_TICKS(delay_ms))`.
  - This blocks for `delay_ms` (30 s + jitter) as before, **but** if `display_task` sends
    a notification via `xTaskNotifyGive()`, the fetch task wakes immediately and starts
    the next fetch cycle. The notification acts as an "early wake" from the delay.
  - If the notification arrives while the fetch is in progress (not blocked on the delay),
    it is latched — the next `xTaskNotifyWait` call returns immediately without blocking.
- **Fetch gating via `s_fetch_allowed`**: `eta_fetch_task` checks `s_fetch_allowed` at the
  top of its loop:
  - If `!s_fetch_allowed`: block on `xTaskNotifyWait(0, 0, NULL, portMAX_DELAY)` — sleeps
    until notified (no periodic fetches during display sleep).
  - If `s_fetch_allowed`: run the fetch cycle, then block on
    `xTaskNotifyWait(0, 0, NULL, pdMS_TO_TICKS(30000))` (30 s delay with early-wake).
  - When `display_task` wakes the display, it sets `s_fetch_allowed = true` and calls
    `xTaskNotifyGive(eta_fetch_task)` to unblock the fetch task immediately.

  This gives: fetch runs immediately on wake, then every ~30 s while awake, and pauses
  completely while asleep.

### Phase 3: Remove voltage-profile code

- Delete `u8g2_st7305_set_voltage_profile()` and the two profile arrays from
  `components/u8g2_st7305/u8g2_st7305.c`.
- Delete the declaration from `components/u8g2_st7305/u8g2_st7305.h`.
- Remove the `profile_high` / `last_voltage_profile_high` logic from `main/main.c`.
- Update `CLAUDE.md` and `HANDOFF.md` to reflect the replacement.

### Phase 3b: Footer sleep-countdown indicator

Modify `render_footer()` in `main/display.c` and its callers:

**Current footer layout** (18px tall, black band, white text):
```
[Updated 14:32:00]                          [Battery:  72%]
```

**New footer layout** (sleep countdown inserted between "Updated" and "Battery"):
```
[Updated 14:32:00]  [Sleep in 12 min]          [Battery:  72%]
```

Or during morning window:
```
[Updated 14:32:00]  [Morning Window]           [Battery:  72%]
```

**Changes to `render_footer()`**:
- Add a new parameter: `const char *sleep_status_str` (NULL = no indicator, empty string =
  hide indicator, non-empty = render the string).
- After drawing `updated_str` at x=14, draw `sleep_status_str` at x=14+W(updated_str)+12
  (12px gap after the "Updated HH:MM:SS" text).
- The string is built by `display_task` before calling `render_dashboard()`.

**Changes to `render_dashboard()`**:
- Add `const char *sleep_status_str` parameter, pass through to `render_footer()`.

**Changes to `display_task` in `main.c`**:
- Before each render, build `sleep_status_str`:
  - If `!display_awake`: pass NULL (footer not rendered anyway — display is asleep).
  - If `in_morning_window`: `snprintf(buf, sizeof(buf), "Morning Window")`.
  - If `!in_morning_window && display_awake`: compute remaining minutes:
    `int remain_min = (int)((wake_until - now) / 60);` clamped to [0, timeout_min].
    `snprintf(buf, sizeof(buf), "Sleep in %d min", remain_min);`
- Pass `sleep_status_str` to `render_dashboard()`.

**Text width budget**: Footer is 400px wide. "Updated HH:MM:SS" ≈ 120px (profont12).
"Sleep in XX min" ≈ 85px. "Battery: 100%" ≈ 90px. Total ≈ 120 + 12 + 85 + gap + 90 = ~320px.
Fits comfortably within 400px - 28px (left/right padding) = 372px. **No concern.**

**Update cadence**: The countdown updates every render cycle (every 15 s while awake). The
minute value changes at most once per minute, so the 15 s cadence is more than sufficient.
No need to render more frequently just for the countdown.

### Phase 3c: design.md updates

Update `design.md` to document the footer layout change:

**§3 Layout Patterns — rule 4 (Footer band)**:
- Current text: "Two text elements: left-aligned 'Updated HH:MM:SS', right-aligned
  'Battery: XX%'. No icon, no border — the black fill is the boundary."
- New text: "Three text elements: left-aligned 'Updated HH:MM:SS', sleep-countdown
  indicator ('Sleep in XX min' or 'Morning Window') placed 12px to the right of the
  Updated text, right-aligned 'Battery: XX%'. No icon, no border — the black fill is the
  boundary. The sleep-countdown indicator is only rendered when display sleep is enabled
  (`CONFIG_DISPLAY_SLEEP_ENABLED=y`); when disabled, the footer reverts to two elements."

**§3 Screen structure ASCII diagram**:
- Current footer line: `│ Updated 14:32:00          Battery: 72% │`
- New footer line: `│ Updated 14:32:00  Sleep in 12 min   Battery: 72% │`

**§2 Typography — table**:
- No new font needed. The sleep-countdown indicator uses the same `u8g2_font_profont12_mf`
  as the existing footer labels (12 px regular). Add a note in the "Footer labels" row:
  "(also used for sleep-countdown indicator)".

**§7 Mockup Self-Verification Checklist**:
- Add new item: "- [ ] Footer shows sleep-countdown indicator ('Sleep in XX min' outside
  morning window, 'Morning Window' during morning window) between 'Updated HH:MM:SS' and
  'Battery: XX%' when display sleep is enabled. Indicator is absent when display sleep is
  disabled."

### Phase 4: Config (Kconfig)

Add a new menu in `main/Kconfig.projbuild` (the project-level Kconfig that appears under the
top-level menu in `idf.py menuconfig`):

```
menu "Display Sleep Configuration"

    config DISPLAY_SLEEP_ENABLED
        bool "Enable display sleep"
        default y
        help
            When enabled, the display sleeps outside the morning window and wakes
            on button press. When disabled, the display is always on.

    config DISPLAY_WAKE_TIMEOUT_MIN
        int "Wake timeout (minutes)"
        default 15
        range 1 1440
        depends on DISPLAY_SLEEP_ENABLED
        help
            How long the display stays on after a button press (outside the
            morning window), in minutes. Default 15 min.

    config DISPLAY_MORNING_START_HOUR
        int "Morning window start hour"
        default 7
        range 0 23
        depends on DISPLAY_SLEEP_ENABLED
        help
            Start of the always-on morning window (0-23, local time). Default 7 (07:00).

    config DISPLAY_MORNING_END_HOUR
        int "Morning window end hour"
        default 10
        range 1 24
        depends on DISPLAY_SLEEP_ENABLED
        help
            End of the always-on morning window (1-24, local time). Default 10 (10:00).

endmenu
```

**Where the user finds these in `idf.py menuconfig`:**

When the user runs `idf.py menuconfig`, they will see these under:
```
Application Configuration → Display Sleep Configuration
```

Wait — actually, `main/Kconfig.projbuild` items appear at the **top level** of the menuconfig
tree (not nested under "Application Configuration"). The existing Wi-Fi config in this
project's `main/Kconfig.projbuild` appears as a top-level "WiFi Configuration" menu. So the
new options will appear as a top-level menu item:

```
--- Top-level menu ---
    WiFi Configuration  >
    Display Sleep Configuration  >
    ...
```

The four config symbols the user would set:
1. **`DISPLAY_SLEEP_ENABLED`** — bool, toggle with Y/N. Default `y`.
2. **`DISPLAY_WAKE_TIMEOUT_MIN`** — int, type the number. Default `15` (minutes).
3. **`DISPLAY_MORNING_START_HOUR`** — int, type the number. Default `7`.
4. **`DISPLAY_MORNING_END_HOUR`** — int, type the number. Default `10`.

**Note on GPIO18**: The GPIO pin is hardcoded to 18 in `button_init()` (matching the
board's physical KEY button). It is **not** a Kconfig option — there is no reason to make it
configurable, since only GPIO18 has the KEY button wired to it.

---

## Power Budget (rough estimate)

| State | Current draw | Notes |
|-------|-------------|-------|
| Display on + Wi-Fi active (fetching) | ~80–120 mA | Current baseline |
| Display on + Wi-Fi modem-sleep (between fetches) | ~50–80 mA | Display on, radio cycling |
| Display off + Wi-Fi modem-sleep (fetch paused) | ~15–30 mA | Display sleep + no fetch cycles + modem-sleep |
| Display off + Wi-Fi off (future deep-sleep) | ~5–10 mA | Not in scope — PRD §10 Open #4 |

With ETA fetch paused during sleep, the device spends most of its sleep time in Wi-Fi
modem-sleep with no HTTP activity — the lowest power state short of deep-sleep. The display
sleep saves ~20–40 mA (ST7305 panel + SPI controller), and pausing fetches eliminates the
periodic ~80–120 mA spikes every 30 s.

---

## Files to Change

| File | Change |
|------|--------|
| `main/button.h` | **New** — button driver header |
| `main/button.c` | **New** — GPIO18 interrupt-based button driver |
| `main/main.c` | Modify `display_task`: add sleep/wake state machine, remove voltage-profile logic. Modify `eta_fetch_task`: notification-based wake + fetch gating. Build `sleep_status_str` for footer. |
| `main/display.c` | Modify `render_footer()`: add `sleep_status_str` parameter. Modify `render_dashboard()`: pass through `sleep_status_str`. |
| `main/display.h` | Update `render_footer()` and `render_dashboard()` signatures. |
| `main/CMakeLists.txt` | Add `button.c` to SRCS |
| `main/Kconfig.projbuild` | Add "Display Sleep Configuration" menu with 4 options |
| `components/u8g2_st7305/u8g2_st7305.c` | Delete `u8g2_st7305_set_voltage_profile()` + profile arrays |
| `components/u8g2_st7305/u8g2_st7305.h` | Delete `u8g2_st7305_set_voltage_profile()` declaration |
| `design.md` | §3 rule 4 (Footer band): add sleep-countdown indicator as third element. §7 checklist: add verification item for sleep indicator. |
| `CLAUDE.md` | Update display-power-management section to reflect sleep+wake approach |
| `HANDOFF.md` | Add entry noting replacement of voltage-profile with sleep+wake |
| `PRD.md` | §10 Decision #7: mark voltage approach as superseded; §10 Open: add new entry |

---

## Critical Review (post-update)

### 1. NTP resync at 06:00 — still works, but edge case

The daily NTP resync in `display_task` triggers at `tm_hour == 6`. With the new morning
window defaulting to 07:00–10:00, the resync fires at 06:00 — one hour before the morning
window starts. If the display is asleep at 06:00, the NTP resync runs regardless of display
state (it's a network call, not a display call), and `render_dashboard()` is only called if
`display_awake` is true. **No change needed**, but worth noting that the NTP resync hour
(hardcoded 6) is independent of the morning-window Kconfig values.

### 2. 500 ms poll interval — power impact is negligible

When display is asleep, `display_task` loops every 500 ms instead of every 15 s. This means
~120 wakeups/min instead of ~4. Each wakeup does: `time()`, `localtime()`, check
`button_consume_presses()`, check morning window, `vTaskDelay(500ms)`. No SPI traffic, no
rendering. The ESP32-S3 at idle with Wi-Fi modem-sleep draws ~15–30 mA; the extra CPU
wakeups from 500 ms polling add negligible power (microamps of average current). **No
concern.**

### 3. Button press during wake timeout (outside morning window) — resets timeout

Every button press during the wake period resets the timeout to the full duration (default
15 min) from the moment of the press. This is the expected UX — the user keeps the display
on by interacting with it. A user pressing the button repeatedly would keep resetting it
indefinitely. This is fine for the intended use case (glance at the display, press to keep
it on). **No concern.**

### 4. `DISPLAY_SLEEP_ENABLED = n` — fallback behaviour

If the user disables sleep via Kconfig (`DISPLAY_SLEEP_ENABLED = n`), the display should
behave exactly as it does today minus the voltage-profile switching: always on, render every
15 s, no button handling, ETA fetch every ~30 s as before. The implementation must `#ifdef`
the button init and the sleep/wake logic so that the disabled path is a pure always-on loop.
`button.c` is not compiled when sleep is disabled (CMake guard). **Handled in Implementation
Details §2 and §3.**

### 5. `u8g2_SetPowerSave` — verify ST7305 sleep command

The U8g2 ST7305 driver at `u8g2_st7305.c:226` maps `u8g2_SetPowerSave(u, 0/1)` to
`0x28` (sleep out) / `0x29` (sleep in) via `st7305_write_cmd()`. This is the standard
ST7305 DISPOFF (0x28) / DISPON (0x29) sequence. **However**, the ST7305 datasheet §7.9.2
shows `0x28` as "Display OFF" (a single-byte command with no parameters), which is correct
for this controller. The Waveshare reference driver uses the same mapping. **Verified — no
concern.**

### 6. Voltage-profile removal — check for other callers

`u8g2_st7305_set_voltage_profile()` is currently called from `display_task` in `main.c`
(line 391) and declared in `u8g2_st7305.h`. The `st7305_voltage_profile_high[]` and
`st7305_voltage_profile_low[]` arrays are in `u8g2_st7305.c`. The init sequence in
`st7305_full_init()` also writes the high-profile values directly (as the init-time
baseline). **After removal**, the init sequence should keep the high-profile values as the
permanent init values (they are the Waveshare reference baseline). Only the runtime
`set_voltage_profile()` function and the two profile arrays need deletion. The init
sequence is untouched. **No concern.**

### 7. Wake timeout in minutes vs. seconds

The Kconfig uses `DISPLAY_WAKE_TIMEOUT_MIN` (minutes, default 15). The implementation
converts to seconds: `wake_until = now + (CONFIG_DISPLAY_WAKE_TIMEOUT_MIN * 60)`. This is
cleaner than seconds for menuconfig (the user types "15" not "900"). **No concern.**

### 8. Morning window crossing midnight

If a user sets `DISPLAY_MORNING_START_HOUR = 22` and `DISPLAY_MORNING_END_HOUR = 6`, the
window crosses midnight. The current check `hour ∈ [start, end)` would fail for hour=23
(because `23 >= 22` is true but `23 < 6` is false). **This is a bug**. The implementation
must handle wrap-around: if `start >= end`, the window is `[start, 24) ∪ [0, end)`.

**Fix**: In the morning-window check:
```c
bool in_morning_window;
if (CONFIG_DISPLAY_MORNING_START_HOUR < CONFIG_DISPLAY_MORNING_END_HOUR) {
    in_morning_window = (hour >= CONFIG_DISPLAY_MORNING_START_HOUR
                         && hour < CONFIG_DISPLAY_MORNING_END_HOUR);
} else {
    /* Window crosses midnight */
    in_morning_window = (hour >= CONFIG_DISPLAY_MORNING_START_HOUR
                         || hour < CONFIG_DISPLAY_MORNING_END_HOUR);
}
```

This should be noted in the implementation, even though the default (7, 10) doesn't cross
midnight. **Added to the plan as a required implementation detail.**

### 9. Boot behaviour — display on until SNTP sync (or 5-min fallback), then simulated button press

On first boot, `display_task` starts. The display is on (from `display_init()`). SNTP has
not yet synced — `time()` returns a pre-2023 epoch. The display stays on unconditionally,
rendering the dashboard (ETAs will show "--" until the first fetch completes). ETA fetch
runs normally during this period.

Once SNTP syncs (detected by `now > 1700000000`), the normal sleep/wake logic takes over:
- If inside the morning window: display stays on, no wake timeout.
- If outside the morning window: set `wake_until = now + timeout` (simulated button press).
  The display stays on for the full wake timeout, then sleeps.

**5-minute fallback**: If SNTP hasn't synced within 5 minutes of boot (detected via
`xTaskGetTickCount()`), set `sntp_synced = true` anyway and enter normal sleep/wake logic.
The time will be wrong (1970 epoch), so the morning-window check will evaluate against
hour=0 — likely outside the default 07:00–10:00 window, so the display will get a simulated
button press (15 min wake timeout) and then sleep. This is a safe failure mode: the device
doesn't stay on forever wasting power, and the user can wake it with the button. The clock
will correct itself if SNTP eventually syncs (the daily 07:00 resync will fire once `time()`
returns a valid epoch).

**Why not just set `wake_until` at boot?** Because before SNTP sync, `time()` returns 1970,
and `wake_until = 1970 + 900` would already be in the past from the perspective of real
time passing. The SNTP-sync gate (with 5-min fallback) ensures the simulated button press
happens at a meaningful real-world time, or at worst after 5 minutes of "display on" time.

### 10. `button.c` not compiled when `DISPLAY_SLEEP_ENABLED = n`

Phase 4's Kconfig has `DISPLAY_WAKE_TIMEOUT_MIN` etc. with `depends on DISPLAY_SLEEP_ENABLED`.
But `button.c` is added to `main/CMakeLists.txt` unconditionally. The implementation should
either:
- (a) Guard `button.c` in CMakeLists with `if(CONFIG_DISPLAY_SLEEP_ENABLED)`, or
- (b) Keep `button.c` always compiled but make `button_init()` a no-op when disabled.

Option (a) is cleaner. **Added to the plan as a required implementation detail.**

### 11. ETA fetch pause during sleep — task notification + gate flag

When the display sleeps, ETA fetches pause completely (no HTTP traffic, no Wi-Fi power
spikes). When the display wakes (button press or morning window), an immediate fetch is
triggered. This is implemented via:
- `volatile bool s_fetch_allowed`: set true when display awake, false when asleep.
- `xTaskNotifyGive(eta_fetch_task)`: sent on wake to trigger immediate fetch.
- `eta_fetch_task` blocks on `xTaskNotifyWait` with `portMAX_DELAY` when
  `!s_fetch_allowed`, and with `pdMS_TO_TICKS(30000)` when `s_fetch_allowed`.

**Concern**: The `s_fetch_allowed` flag is read by `eta_fetch_task` and written by
`display_task`. These are different FreeRTOS tasks on different cores (potentially). The
flag is a single `bool` (1 byte), and aligned single-byte writes/reads are atomic on
ESP32-S3. No spinlock or atomic API needed — `volatile` is sufficient for this flag's
memory ordering on ESP32-S3 (single-core, FreeRTOS tick-based scheduling). **No concern.**

### 12. Immediate render on wake — render alignment

When the display wakes (button press), `display_task` renders immediately rather than
waiting for the next 15 s boundary (`:00`, `:15`, `:30`, `:45`). This means the render
cadence is temporarily irregular after wake — the first render happens at the wake moment,
then subsequent renders align to the next 15 s boundary. This is acceptable: the user wants
to see the dashboard immediately on button press, not 14 s later. The 15 s alignment is a
power-saving optimisation for the continuous-render case, not a correctness requirement.

**Implementation note**: After the immediate wake render, the next sleep duration should be
`15 - (sec % 15)` seconds (aligning to the next boundary), not a full 15 s. This is already
how the current code works (`next_seconds = 15 - (sec % 15)`).

### 13. Wi-Fi power-state switching — `WIFI_PS_MAX_MODEM` during sleep

When the display sleeps, Wi-Fi switches to `WIFI_PS_MAX_MODEM` (deeper power saving, longer
beacon listen interval). When the display wakes (button press or morning window), Wi-Fi
switches back to `WIFI_PS_MIN_MODEM` before the first fetch. This adds ~1–2 s latency to
the first fetch after wake (radio needs to re-sync with the AP). Given the wake timeout is
15 min, this latency is invisible to the user.

**Concern**: `esp_wifi_set_ps()` is not instantaneous — it negotiates with the AP. Calling
it on every wake/sleep transition adds a small overhead. However, this happens at most once
per 15 min (on button press) or twice per day (morning window boundaries), so the overhead
is negligible. **No concern.**

**Concern**: The existing `eta_fetch_task` already toggles between `WIFI_PS_NONE` (during
fetch) and `WIFI_PS_MIN_MODEM` (between fetches). With the new scheme, `display_task` sets
`WIFI_PS_MAX_MODEM` on sleep, but `eta_fetch_task` sets `WIFI_PS_NONE` at the start of its
fetch cycle and `WIFI_PS_MIN_MODEM` at the end. This could conflict if the fetch task runs
while the display is asleep — but it won't, because `s_fetch_allowed` is false during sleep,
so the fetch task is blocked. The Wi-Fi PS state is only changed by `display_task` during
sleep (to `MAX_MODEM`) and by `eta_fetch_task` during wake (between `NONE` and `MIN_MODEM`).
**No race — no concern.**

---

## Implementation Details (from critical review)

The following details must be handled during implementation (not optional):

1. **Midnight wrap-around**: Morning-window check must handle `start >= end` (window
   crosses midnight) using the two-branch logic shown in §8 above.

2. **CMake guard**: `button.c` should only be added to SRCS when
   `CONFIG_DISPLAY_SLEEP_ENABLED` is set:
   ```cmake
   if(CONFIG_DISPLAY_SLEEP_ENABLED)
       list(APPEND SRCS button.c)
   endif()
   ```

3. **Compile-time guard in `main.c`**: The sleep/wake logic in `display_task` should be
   wrapped in `#if CONFIG_DISPLAY_SLEEP_ENABLED` / `#endif`, with the else path being the
   current always-on render loop (minus voltage-profile code, which is removed regardless).

4. **NTP resync independence**: The daily NTP resync at `tm_hour == 6` is independent of
   the morning-window Kconfig values. This is intentional and correct — the resync should
   not depend on the display sleep schedule.

5. **Boot wake via SNTP gate (with 5-min fallback)**: `display_task` must track
   `sntp_synced` (false at boot, set true when `now > 1700000000` OR 5 minutes have
   elapsed since boot via `xTaskGetTickCount()`). Before sync: display stays on
   unconditionally, ETA fetch runs normally. On first tick after sync (or fallback):
   transition to normal sleep/wake logic with `wake_until = now + timeout` (simulated
   button press).

6. **ETA fetch pause/gate**: `eta_fetch_task` must block on
   `xTaskNotifyWait(0, 0, NULL, portMAX_DELAY)` when `!s_fetch_allowed`, and on
   `xTaskNotifyWait(0, 0, NULL, pdMS_TO_TICKS(30000))` when `s_fetch_allowed`.
   `display_task` sets `s_fetch_allowed = true` on wake and sends `xTaskNotifyGive()` to
   trigger an immediate fetch. `display_task` sets `s_fetch_allowed = false` on sleep.

7. **Immediate render on wake**: When the display wakes (button press), render immediately
   rather than waiting for the next 15 s boundary. Subsequent renders align to 15 s
   boundaries as before.

8. **Wi-Fi power-state switching**: `display_task` must set `WIFI_PS_MAX_MODEM` when the
   display sleeps and `WIFI_PS_MIN_MODEM` when it wakes. `eta_fetch_task` continues to
   toggle between `WIFI_PS_NONE` (during fetch) and `WIFI_PS_MIN_MODEM` (between fetches)
   as before — no conflict because the fetch task is blocked during sleep.

9. **Footer sleep-countdown indicator**: `render_footer()` and `render_dashboard()` must
   accept a new `const char *sleep_status_str` parameter. `display_task` builds the string:
   `"Morning Window"` during morning window, `"Sleep in %d min"` with remaining minutes
   when awake outside morning window, NULL when asleep. The indicator is drawn 12px to the
   right of the "Updated HH:MM:SS" text in the footer band.
