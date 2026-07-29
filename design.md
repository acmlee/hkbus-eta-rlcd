# Design: hk-bus-eta-rlcd — Monochrome Display Pattern Reference

> A style reference for HTML mockups and the ST7305 firmware renderer. Not a full design system — rules, not recipes.

---

## 1. Contrast Rules

The display has exactly two states per pixel: **on** (ink/black, `#000`) and **off** (substrate/white/light grey). Everything below follows from that constraint.

- **Pure black (`#000`) on light background only.** No greys, no `#333` / `#666` approximations. A pixel is either black or it isn't.
- **No anti-aliasing.** Type and shapes are 1-bit aliased. Do not attempt pixel-smoothing on font glyphs or vector edges.
- **No gradients, drop shadows, or transparency.** Impossible in 1-bit without dithering, and dithering looks noisy on reflective displays at typical viewing distances.
- **Inverse text (white-on-black) for emphasis bands** — e.g. header and footer bars. Use solid black rectangles with white text knocked out. Reserve this for ≤20% of the screen area; large inverted areas exaggerate refresh artefacts on RLCD.
- **Dithering is forbidden** except for one purpose: simulating "greyed out" / dimmed state on a temporary overlay (50% checkerboard pattern, 2×2 px tile, applied to the affected row segments only). Currently unused in the firmware but retained as a design option.

---

## 2. Typography

### Font source
- **U8g2 bitmap fonts only** (`.u8g2_font_*`). No TrueType/OpenType rasterisation at runtime.
- **zh-HK glyphs**: custom fonts compiled into firmware — `u8g2_font_zhhk_dest_24` (Noto Sans CJK HK Bold 24px, 27,942 glyphs) for destination text, `u8g2_font_zhhk_stop_20` (Noto Sans CJK HK Regular 20px, 27,942 glyphs) for bus-stop names. Coverage: ASCII (32–128) + CJK Symbols and Punctuation (U+3000–U+303F) + CJK Unified Ideographs (U+4E00–U+9FFF) + CJK Extension A (U+3400–U+4DBF) + Halfwidth and Fullwidth Forms (U+FF00–U+FFEF). Generated via `otf2bdf` + `bdfconv` from Noto Sans CJK HK OTF files. `display.c` uses `u8g2_DrawUTF8`/`u8g2_GetUTF8Width` for CJK text rendering.

### Minimum readable sizes (400×300 display, 60–100 cm viewing distance)

| Role | Min glyph height | U8g2 weight | Example |
|---|---|---|---|
| Header time (HH:MM) | 28 px | Bold | `u8g2_font_logisoso28_tf` |
| Header temperature (NN°C) | 22 px | Bold | `u8g2_font_profont22_mf` |
| Header label ("HK Bus ETA") | 10 px | Regular | `u8g2_font_helvR10_tr` |
| Route number | 29 px | Bold | `u8g2_font_profont29_mf` |
| Destination (zh-HK) | 24 px | Bold | `u8g2_font_zhhk_dest_24` (custom Noto Sans CJK HK Bold) |
| Bus-stop name (zh-HK) | 20 px | Regular | `u8g2_font_zhhk_stop_20` (custom Noto Sans CJK HK Regular) |
| ETA — soonest (1st) value | 29 px | Bold | `u8g2_font_profont29_mf` |
| ETA — 2nd/3rd values | 22 px | Regular | `u8g2_font_profont22_mf` |
| ETA "min" suffix | 12 px | Regular | `u8g2_font_profont12_mf` |
| Footer labels | 12 px | Regular | `u8g2_font_profont12_mf` |

### Weight usage
- **Bold** for data the user scans first: time, route number, destination name, and the soonest ETA value.
- **Regular** for the header label ("HK Bus ETA") — it is a static title, not scannable data.
- **Regular** for supporting information: bus-stop name, 2nd/3rd ETA values, footer labels, page indicator.
- **No italic or oblique.** Bitmap fonts rarely include these, and simulated oblique (shear) looks broken in 1-bit.

### Destination vs. bus-stop hierarchy
Each route row must show **both** the destination (往 — where the bus is going) and the physical bus-stop name (the stop being monitored), stacked vertically within Col 2:
- **Destination is the primary line** — larger, bold, placed first (top).
- **Bus-stop name is the secondary line** — smaller, regular weight, placed directly beneath the destination, optionally prefixed with a small "at" marker to visually separate it from the destination line.
- This pairing exists because the same route number can stop at more than one physical stop in a user's viewing context — showing only the destination without the stop name creates ambiguity about which physical location the ETA applies to.

### Line-height
- For 29 px glyphs, allow 40 px row height (1.38×).
- For 24 px glyphs, allow 32 px row height (1.33×).
- For 22 px glyphs, allow 30 px row height (1.36×).
- For 20 px glyphs, allow 28 px row height (1.4×).
- For 12 px footer, allow 16 px row height (1.33×).

---

## 3. Layout Patterns

### Screen structure (landscape 400 × 300)

┌──────────────────────────────────────────────────────────┐
│ HEADER BAND (solid black bg, white text, ~36 px high)          │ ← top ~12%
│ HK Bus ETA  28°C                             14:32             │
├──────────────────────────────────────────────────────────┤ ← 1px dividing line
│ 1A 往 尖沙咀碼頭 5 8 14 │
│ 尖沙咀 廣東道 min │ ← 3 rows,
├──────────────────────────────────────────────────────────┤ ~230 px total
│ 1 往 尖沙咀碼頭 2 9 19 │
│ 梳士巴利道 min │
├──────────────────────────────────────────────────────────┤
│ 5B 往 堅尼地城 -- -- -- │
│ 堅尼地城海旁 │
├──────────────────────────────────────────────────────────┤ ← 1px dividing line
│ FOOTER BAND (solid black bg, white text, ~18 px high) │ ← bottom ~6%
│ Updated 14:32:00  Page 1/2               Battery: 72% │
└──────────────────────────────────────────────────────────┘


### Structural rules

1. **Header band** — Full-width filled black rect, ~36 px high. Three text elements, all white-on-black with ≥12 px left/right padding:
   - **"HK Bus ETA"** left-aligned, regular weight, ~10 px (`u8g2_font_helvR10_tr`).
   - **Temperature (NN°C)** positioned ~16 px right of the title text, bold, 22 px (`u8g2_font_profont22_mf`). Shares the title's baseline (y=24). Omitted entirely when data is unavailable or stale (see §8).
   - **Current time HH:MM** right-aligned, bold, 28 px (`u8g2_font_logisoso28_tf`), baseline y=32.
   - No icon, no other label. Temperature must never overlap the time element.
2. **Route rows** — Column layout with explicit column boundaries:
    - **Col 1** (route number): fixed width ~60 px, left-aligned, bold, vertically centred across the full row height. At least 16 px gap between Col 1 and Col 2.
    - **Col 2** (destination + bus-stop, stacked): elastic, fills remaining space up to the ETA column. 16 px left padding from Col 1 boundary.
        - **Top line — destination (zh-HK)**: bold, ~24px (`u8g2_font_zhhk_dest_24`), rendered via `u8g2_DrawUTF8`. Falls back to `u8g2_font_helvB14_tr` with "To " prefix if `dest_zh` is absent in `routes.json`.
        - **Bottom line — bus-stop name (zh-HK)**: regular weight, ~20px (`u8g2_font_zhhk_stop_20`), rendered via `u8g2_DrawUTF8`. Falls back to `u8g2_font_helvR10_tr` if `stop_zh` is absent in `routes.json`.
        - If either line overflows the available width, progressively shrink that line's font size independently (do not truncate, do not wrap to a second line). Destination shrink chain: zhhk_dest_24→helvB14→B12→B10→B08. Stop shrink chain: zhhk_stop_20→helvR10→R08.
    - **Col 3** (ETA values ×3): fixed width ~120 px, right-aligned, split into 3 sub-columns of ~40 px each.
        - **1st (soonest) ETA**: 29px, bold (`u8g2_font_profont29_mf`) — this is the value the user scans first.
        - **2nd and 3rd ETA**: 22px, regular weight (`u8g2_font_profont22_mf`), spaced evenly to the right of the 1st value with visible gaps (8–12 px) between each value so they are clearly separated at a glance.
        - A single shared "min" suffix label may appear once beneath or beside the group rather than repeated 3×, to avoid visual clutter.
        - If a route has fewer than 3 upcoming buses, remaining slots show "--" in the same alignment and weight as their column position (2nd/3rd style), not the bold 1st-position style.
3. **Row dividers** — 1 px horizontal line (single row of black pixels) between each route row. Not between header and row 1 (the header band itself is the separator).
4. **Footer band** — Full-width filled black rect. Three text elements: left-aligned "Updated HH:MM:SS" (or "Connecting..." if Wi-Fi is not connected), "Page X/Y" indicator placed 10 px to the right of the Updated text (hidden in single-page mode), right-aligned "Battery: XX%". No icon, no border — the black fill is the boundary.
5. **Testing / init pattern** — On boot, show all-pixels-on (full black) for 500 ms before transitioning to the dashboard. No splash logo, no progress bar — just the flash.

---

## 4. Data-Density Patterns

| Content | Rows that fit comfortably | Row height | Notes |
|---|---|---|---|
| 3 routes (destination + stop + 3 ETAs each) + header + footer | 3 rows | ~72 px each | **Target for this device** — the two-line Col 2 (destination/stop) and 3-value Col 3 both fit within this row height at their minimum sizes |
| 4 routes (if needed) | 4 rows | ~54 px each | Tight — destination drops toward 14 px, bus-stop toward 11 px, 2nd/3rd ETA toward 13 px |
| 5 routes | 5 rows | ~42 px each | Not viable with the two-line destination/stop pattern at readable sizes — would require dropping the bus-stop line or reducing to a single ETA value |

The PRD mandates exactly 3 routes. At 3 rows the display is sparsely populated by design — each row has room for the two-line destination/stop text and all 3 ETA values without cramping.

---

## 5. Reference Patterns (E-Ink / Monochrome Dashboard)

| Type | What to borrow |
|---|---|
| **E-ink weather stations** (OpenWeather 2.9", Waveshare 4.2") | Header band solid black, data in labelled columns, footer for last-updated. The banded top/bottom structure is proven at this form factor. |
| **Transit information displays** (London Tube dot-matrix, MTR platform displays) | Right-aligned minutes with monospaced digits, route number as the primary visual anchor, destination as secondary text, multiple upcoming arrival times shown left-to-right with the soonest emphasised. |
| **Casio / calculator-style LCD** (Classic watch face) | Large bold digits for the primary metric, small supporting text below. Effective despite severe resolution limits. |
| **Berlin U-Bahn e-paper platform signs** | Single-row-per-line structure, no gridlines — whitespace and 1px dividers separate rows. Inverse white-on-black for the top status bar. |

---

## 6. Anti-Patterns (Do Not Use)

| Anti-pattern | Why it fails on 1-bit RLCD |
|---|---|
| **Colour for status** (green=OK, red=late) | There is no colour. Use text ("--", "Delayed") or shape (inverse row) instead. |
| **Hairline fonts / ultra-light weights** | 1 px strokes disappear or look broken on reflective displays at normal viewing distance. Minimum stroke: 2 px for any standalone line. |
| **Low-contrast greys for "disabled" state** | Grey and white are the same colour on this display. Use the checkerboard dither pattern for dimmed state, or omit the element entirely. |
| **Icons without text labels** | A WiFi icon means nothing if the user can't see the fine detail. Always pair with text. |
| **Thin borders around every cell** | Creates visual noise. Use 1 px horizontal dividers *between* rows only, not around them. No vertical gridlines in data rows. |
| **Overlapping or clipped text** | In 1-bit, clipped text looks like a rendering bug. Ensure every string fits its allocated column; shrink font if needed. |
| **Animated transitions** | RLCD refresh is not fast enough for smooth animation. No fade, slide, or wipe effects. Instant frame replacement only. |
| **Equal-weight ETA values** | If all 3 ETA values are the same size/weight, the user has to re-read all 3 to find the soonest bus. The 1st value must be visually dominant. |
| **Destination and bus-stop at equal size** | If both lines in Col 2 are the same size, the user can't tell at a glance which line is "where the bus goes" vs. "where I'm standing." Destination must read as the primary line. |

---

## 7. Mockup Self-Verification Checklist

Before presenting any HTML mockup or firmware render output as final, verify:
- [ ] Every visible element uses exactly 2 colours: `#000` (black) and the background colour. No greys, no tints.
- [ ] No anti-aliased edges on any text or shape.
- [ ] Text sizes meet the minimums in §2 for their role.
- [ ] zh-HK text uses a bitmap font (not system-rendered anti-aliased). Custom fonts `u8g2_font_zhhk_dest_24` and `u8g2_font_zhhk_stop_20` are compiled into the firmware.
- [ ] Header and footer are solid black bands with white inverted text.
- [ ] Header shows "HK Bus ETA" left-aligned, temperature (NN°C) to the right of the title at 22 px bold, and current time HH:MM right-aligned at 28 px bold. Temperature is omitted entirely when data is unavailable or stale.
- [ ] Route rows have exactly 1 px horizontal dividers between them — no vertical dividers, no box borders.
- [ ] Each route row shows exactly 3 ETA values, right-aligned in a fixed-width column group.
- [ ] The 1st (soonest) ETA value is visually larger/bolder than the 2nd and 3rd values.
- [ ] "--" renders in the same alignment and matching weight as the ETA position it occupies (bold if in the 1st slot, regular if 2nd/3rd).
- [ ] Each route row shows both the destination (top line, bold, larger) and the bus-stop name (bottom line, regular, smaller) in zh-HK. Verify CJK glyphs render correctly via `u8g2_DrawUTF8` and that `routes.json` provides `dest_zh`/`stop_zh` fields.
- [ ] Destination text is visually larger than the bus-stop name text in every row.
- [ ] Neither destination nor bus-stop text is truncated or wrapped; if either overflows, that line's font size is reduced independently.
- [ ] "Greyed out" / dimmed state uses a checkerboard dither (2×2 px tile), not a lighter colour.
- [ ] No icons without accompanying text labels.
- [ ] Footer shows "Updated HH:MM:SS" (or "Connecting..." if Wi-Fi is not connected), "Battery: XX%" right-aligned, "Page X/Y" indicator (profont12, 10 px after "Updated HH:MM:SS") when multi-page mode is active. Hidden in single-page mode.
- [ ] Pressing the KEY button (GPIO18) toggles between page 1 and page 2 (if configured). The page indicator updates immediately.
- [ ] Date/time format: `HH:MM` in header, `HH:MM:SS` in footer "Updated" label (24-hour).
- [ ] The boot flash (500 ms all-pixels-on) is the only transition — no animations elsewhere.
- [ ] Checkerboard dither (if used) is applied only to the route content area, never to the header or footer bands.

---

## 8. External Data: Temperature

The header temperature element is sourced from the Hong Kong Observatory (HKO) "Current Weather Report" open data API (`rhrread`). It is **not** a bus-ETA data source — it is ambient context.

- **Source**: `https://data.weather.gov.hk/weatherAPI/opendata/weather.php?dataType=rhrread&lang=en`
- **Station**: Configurable via `routes.json` `"weather"."station"` (default: `"Hong Kong Observatory"`).
- **Fetch cadence**: Every 20th `eta_fetch_task` cycle (~10 min), piggybacked on the existing Wi-Fi-awake window — no separate task.
- **Stale TTL**: 30 minutes. If the last successful fetch is older than 30 min, temperature is hidden (not shown as stale or placeholder).
- **Format**: `NN°C` (e.g. `28°C`), 22 px bold, `u8g2_font_profont22_mf`. Omitted entirely when data is unavailable.
- **Failure behaviour**: Hide. No placeholder, no `--°C`. The header reverts to title + time only.