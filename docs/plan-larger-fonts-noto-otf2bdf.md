# Plan: Larger zh-HK Fonts via Noto Sans CJK HK + otf2bdf

**Status**: TBC (deferred)  
**Created**: 2026-07-14  
**Revised**: 2026-07-22 (critical review fixes applied)  
**Tracking**: PRD.md §10 Open/TBC Decisions

---

## Goal

Replace both `dest_zh` and `stop_zh` fonts with larger rasterized versions of Noto Sans CJK HK (Bold for dest, Regular for stop) at 24px and 20px respectively, using `otf2bdf` to convert OTF→BDF, then `bdfconv` to convert BDF→U8g2 C source.

## Decisions (resolved 2026-07-22)

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Glyph map coverage | **D1: Full map** (27,618 glyphs) | routes.json can change without regenerating fonts |
| Font format | **Language-specific OTF** (`10_NotoSansCJKhk.zip` from `Sans2.004`) | Full CJK coverage with HK locale glyphs |
| "往" prefix font | **stop_font** (20px) | Visual distinction between direction marker and destination name |

---

## Current State

| Element | BDF Source | Glyph Height | U8g2 Font Name | Binary Array Size |
|---------|-----------|-------------|----------------|-------------------|
| `dest_zh` | `wenquanyi_12pt.bdf` (16px) | 19px (asc 14 + desc 4) | `u8g2_font_zhhk_dest_18` | 1,238,287 bytes (~1.18 MB) |
| `stop_zh` | `wenquanyi_9pt.bdf` (12px) | 15px (asc 12 + desc 3) | `u8g2_font_zhhk_stop_13` | 765,685 bytes (~748 KB) |
| **Total font binary** | | | | **~2,003,972 bytes (~1.91 MB)** |

Source `.c` file sizes on disk: 3.3 MB + 2.1 MB = 5.4 MB (hex-encoded).

**Current binary**: ~3.21 MB (factory app 4 MB partition, 22% free).

## Target State

| Element | Source Font | Pixel Size | U8g2 Font Name | Est. Binary Size |
|---------|-----------|-----------|----------------|-----------------|
| `dest_zh` | Noto Sans CJK HK **Bold** | 24px | `u8g2_font_zhhk_dest_24` | ~2.8–3.4 MB (full map) |
| `stop_zh` | Noto Sans CJK HK **Regular** | 20px | `u8g2_font_zhhk_stop_20` | ~2.3–2.8 MB (full map) |
| **Total font binary** | | | | **~5.1–6.2 MB** |

> **Size estimate caveat**: The range above accounts for RLE compression
> variability. U8g2 format 1 uses RLE on bitmap data. WenQuanYi (current)
> is a hand-tuned bitmap font with regular strokes that RLE compresses
> well. Noto Sans CJK HK (new) is a vector font rasterized by FreeType —
> Bold weight produces thicker strokes with different RLE characteristics.
> The actual size **must be verified after Phase 4** before proceeding to
> firmware code changes. See §Flash Budget for headroom analysis.

## Layout Feasibility

```
ZONE_ROW_H = 81px
COL2_AVAIL_W = 208px

Option D (24px + 20px):
  dest_h ≈ 28px (24px glyph + ascent/descent)
  stop_h ≈ 24px (20px glyph + ascent/descent)
  gap = 4px
  combined_h = 28 + 4 + 24 = 56px → fits in 81px (69% used)

Width check (24px CJK glyphs):
  Longest dest_zh: ~5 chars × 24px = 120px + "往" prefix ~20px + 2px gap = 142px → fits in 208px
  Longest stop_zh: ~6 chars × 24px = 144px → fits in 208px
  Shrink-to-fit chain handles overflow via Helvetica fallbacks.
```

> **Glyph width caveat**: WenQuanYi glyphs are exactly square (em box =
> glyph box). Noto Sans CJK HK is a proportional CJK font — while most Han
> characters are near-square, actual advance width varies (±2-3px per
> glyph). The width estimates above may be off by ±15%. The shrink-to-fit
> chain will handle overflow, but the first-pass font may trigger fallback
> more often than expected for longer stop names (e.g. 將軍澳工業邨, 6 chars).

---

## Flash Budget

### Current Partition Layout

| Partition | Offset | Size | Content |
|-----------|--------|------|---------|
| nvs | 0x9000 | 24 KB | Wi-Fi creds, NVS keys |
| phy_init | 0xF000 | 4 KB | PHY calibration |
| factory | 0x10000 | **4 MB** | App + fonts (will overflow) |
| storage | 0x410000 | 2 MB | SPIFFS (routes.json) |

**Flash total**: 16 MB. Used: ~6 MB. **~10 MB free.**

### Required Change: Resize factory partition to 8 MB

```
factory:  0x10000  (8 MB)    ← was 4 MB
storage:  0x810000 (2 MB)    ← was 0x410000, content unchanged
```

### Headroom Analysis

| Scenario | Font binary | Total binary | 8 MB headroom |
|----------|------------|-------------|---------------|
| Best case (RLE efficient) | ~5.1 MB | ~6.4 MB | ~1.6 MB free |
| Expected case | ~5.6 MB | ~6.9 MB | ~1.1 MB free |
| Worst case (RLE poor on Bold) | ~6.2 MB | ~7.5 MB | ~0.5 MB free |

**Action**: After Phase 4 conversion, verify actual font binary size. If
total font binary exceeds 6.5 MB, consider switching to D2 (reduced map)
before proceeding.

### Risks of Partition Resize

| Concern | Risk | Mitigation |
|---------|------|-----------|
| SPIFFS data wiped | Low | Auto-recovered by `FLASH_IN_PROJECT` (re-flashes routes.json) |
| NVS wiped | None | NVS is before factory, untouched |
| OTA breakage | None | No OTA in use |
| Code changes needed | None | Partition referenced by label (`"storage"`), not offset |
| Full erase on flash | Expected | Normal behaviour; all data re-flashed fresh |

---

## Toolchain Requirements

| Tool | Location | Status | Action Needed |
|------|----------|--------|--------------|
| `otf2bdf` | `../u8g2/tools/font/otf2bdf/` | Source only (binary not built) | Build from source (needs FreeType) |
| FreeType library | System | Not installed | `brew install freetype pkg-config` |
| `bdfconv` | `../u8g2/tools/font/bdfconv/` | Built (macOS binary exists) | Ready to use |
| `build1` (pre-built) | `../u8g2/tools/font/build/build1` | Exists (macOS binary, executable) | Ready to use — **use this** |
| `cjk_unified.map` | `../u8g2/tools/font/build/cjk_unified.map` | Exists (ASCII + CJK only) | **Must expand** — see Phase 3 |
| Noto Sans CJK HK OTF | Not downloaded | Needed | Download from GitHub — see Phase 2 |

> **`build1` vs `bdfconv`**: Both are bdfconv binaries. Use `build1` from
> `tools/font/build/` — it is the pre-built binary recommended by the U8g2
> build system. The `bdfconv` binary in `tools/font/bdfconv/` is also
> functional but `build1` is the canonical choice.

---

## Execution Steps

### Phase 1: Environment Setup

```bash
# 1. Install FreeType and pkg-config (required by otf2bdf configure)
brew install freetype pkg-config

# 2. Build otf2bdf
cd /Users/alanlee/Documents/trae_projects/u8g2/tools/font/otf2bdf
./configure
make
# Verify:
./otf2bdf -h

# 3. Verify build1 (bdfconv) is executable
ls -la /Users/alanlee/Documents/trae_projects/u8g2/tools/font/build/build1
# If not executable:
cd /Users/alanlee/Documents/trae_projects/u8g2/tools/font/bdfconv && make
```

> **FreeType configure note**: `otf2bdf` is old software (v3.1, 2008). Its
> `configure` script expects `freetype-config`. Modern Homebrew FreeType
> installs may only provide `pkg-config freetype2`. If `./configure` fails
> with "freetype-config not found", set:
> ```bash
> export PKG_CONFIG_PATH="$(brew --prefix freetype)/lib/pkgconfig:$PKG_CONFIG_PATH"
> ```
> Then re-run `./configure`.

### Phase 2: Download Noto Sans CJK HK

```bash
# Download Noto Sans CJK HK OTFs from GitHub releases
# Release: Sans2.004 (not Serif2.004 — that is a different font family)
# Asset: 10_NotoSansCJKhk.zip (language-specific OTF, HK locale, full CJK coverage)

mkdir -p /Users/alanlee/Documents/trae_projects/u8g2/tools/font/bdf/noto_hk
cd /Users/alanlee/Documents/trae_projects/u8g2/tools/font/bdf/noto_hk

# Download and extract
curl -L -o noto_hk.zip \
  "https://github.com/notofonts/noto-cjk/releases/download/Sans2.004/10_NotoSansCJKhk.zip"
unzip noto_hk.zip
# Files: NotoSansCJKhk-Regular.otf, NotoSansCJKhk-Bold.otf
#        (note the "CJKhk" infix — not "NotoSansHK")
```

> **Critical**: The asset name is `10_NotoSansCJKhk.zip` (language-specific
> OTF with full CJK coverage + HK locale), **not** `08_NotoSansHKOTFs.zip`.
> The extracted files are `NotoSansCJKhk-Regular.otf` and
> `NotoSansCJKhk-Bold.otf` (note the `CJKhk` infix).

### Phase 3: Expand Map File

The current `cjk_unified.map` covers only ASCII (32-128) + CJK Unified
Ideographs (U+4E00–U+9FFF) + CJK Extension A (U+3400–U+4DBF). This is
insufficient — CJK punctuation and fullwidth forms common in HK stop names
(e.g. `·` U+00B7, `　` U+3000, `（）` U+FF08/FF09, `—` U+2014) would fall
back to the Helvetica default font, causing **vertical misalignment** on
the CJK line (Helvetica baseline/ascent differs from Noto Sans CJK HK).

Create an expanded map:

```bash
MAP_DIR=/Users/alanlee/Documents/trae_projects/u8g2/tools/font/build
cat > $MAP_DIR/cjk_unified_expanded.map << 'EOF'
32-128,
$3000-$303F,
$3400-$4DBF,
$4E00-$9FFF,
$FF00-$FFEF,
EOF
```

**Ranges added**:
- `U+3000–U+303F`: CJK Symbols and Punctuation (ideographic space `　`, `·`, `【】`, `（）`, `＝`, `～`)
- `U+FF00–U+FFEF`: Halfwidth and Fullwidth Forms (fullwidth ASCII, `！？，．`)

**Use this expanded map** in Phase 4 instead of the original `cjk_unified.map`.

### Phase 4: Convert OTF → BDF → U8g2 C Source

```bash
BDF_DIR=/Users/alanlee/Documents/trae_projects/u8g2/tools/font/bdf
OTF_DIR=$BDF_DIR/noto_hk
MAP=/Users/alanlee/Documents/trae_projects/u8g2/tools/font/build/cjk_unified_expanded.map
BDFCONV=/Users/alanlee/Documents/trae_projects/u8g2/tools/font/build/build1
OTF2BDF=/Users/alanlee/Documents/trae_projects/u8g2/tools/font/otf2bdf/otf2bdf
OUT=/Users/alanlee/Documents/trae_projects/hkbus-eta-rlcd/main/fonts

# Destination font: Noto Sans CJK HK Bold, 24px
$OTF2BDF -p 24 -r 72 -o $BDF_DIR/noto_hk_bold_24.bdf $OTF_DIR/NotoSansCJKhk-Bold.otf
$BDFCONV -v -f 1 -M $MAP $BDF_DIR/noto_hk_bold_24.bdf \
  -o $OUT/u8g2_font_zhhk_dest_24.c \
  -n u8g2_font_zhhk_dest_24 \
  -d $BDF_DIR/helvB24.bdf

# Stop font: Noto Sans CJK HK Regular, 20px
$OTF2BDF -p 20 -r 72 -o $BDF_DIR/noto_hk_reg_20.bdf $OTF_DIR/NotoSansCJKhk-Regular.otf
$BDFCONV -v -f 1 -M $MAP $BDF_DIR/noto_hk_reg_20.bdf \
  -o $OUT/u8g2_font_zhhk_stop_20.c \
  -n u8g2_font_zhhk_stop_20 \
  -d $BDF_DIR/helvR18.bdf
```

> **`-d` flag**: Provides fallback glyphs for codepoints not in the map
> file. Helvetica BDF files (`helvB24.bdf`, `helvR18.bdf`) exist in
> `$BDF_DIR` and cover basic Latin. With the expanded map, only truly
> exotic characters fall back to Helvetica.

> **Hinting flags**: If rasterized glyphs look poor on the 1-bit RLCD, try
> these `otf2bdf` flags and compare:
> - `-n` (disable hinting) — may produce cleaner strokes on 1-bit
> - `-a` (force auto-hinting) — uses FreeType's auto-hinter instead of OT hints

### Phase 4.5: Validation Checkpoint (CRITICAL)

**Do not proceed to Phase 5 until all checks pass.**

```bash
OUT=/Users/alanlee/Documents/trae_projects/hkbus-eta-rlcd/main/fonts

# 1. Verify generated files exist and are non-empty
ls -la $OUT/u8g2_font_zhhk_dest_24.c $OUT/u8g2_font_zhhk_stop_20.c

# 2. Check glyph count (bdfconv -v outputs total glyphs converted)
#    Should be ~27,618 + punctuation (~200) ≈ 27,800
#    Look for "glyph count" or similar in bdfconv -v output

# 3. Verify "往" (U+5F80) is present in both fonts
grep -i "5f80" $OUT/u8g2_font_zhhk_dest_24.c
grep -i "5f80" $OUT/u8g2_font_zhhk_stop_20.c

# 4. Verify all routes.json CJK characters are covered
#    Current routes: 黃埔花園荃灣大河道中港碼頭海濱花園總站銅鑼灣聯仁街
for char in 黃 埔 花 園 荃 灣 大 河 道 中 港 碼 頭 海 濱 總 站 銅 鑼 灣 聯 仁 街; do
    codepoint=$(printf '%04x' "'$char")
    if ! grep -qi "$codepoint" $OUT/u8g2_font_zhhk_dest_24.c; then
        echo "MISSING: $char (U+$codepoint) in dest font"
    fi
    if ! grep -qi "$codepoint" $OUT/u8g2_font_zhhk_stop_20.c; then
        echo "MISSING: $char (U+$codepoint) in stop font"
    fi
done

# 5. Check binary size of font data arrays
#    The .c files are hex-encoded (~2.6× binary size)
#    Extract the array size from bdfconv -v output, or estimate:
dest_c_size=$(wc -c < $OUT/u8g2_font_zhhk_dest_24.c)
stop_c_size=$(wc -c < $OUT/u8g2_font_zhhk_stop_20.c)
echo "dest_24.c: $dest_c_size bytes on disk (~$((dest_c_size / 3)) binary)"
echo "stop_20.c: $stop_c_size bytes on disk (~$((stop_c_size / 3)) binary)"
echo "Estimated total font binary: ~$(( (dest_c_size + stop_c_size) / 3 )) bytes"

# 6. PROCEED CHECK: If estimated total font binary > 6.5 MB, STOP and
#    consider switching to D2 (reduced map) before continuing.
```

### Phase 5: Update Firmware Code

**1. `main/fonts/fonts.h`** — Replace declarations:

```c
extern const uint8_t u8g2_font_zhhk_dest_24[] U8G2_FONT_SECTION("u8g2_font_zhhk_dest_24");
extern const uint8_t u8g2_font_zhhk_stop_20[] U8G2_FONT_SECTION("u8g2_font_zhhk_stop_20");
```

Update the header comment to reflect the new font specs (Noto Sans CJK HK,
24px Bold / 20px Regular, expanded map coverage).

**2. `main/display.c`** — Update font chains (lines ~115–130):

```c
static const uint8_t *dest_font_chain[] = {
    u8g2_font_zhhk_dest_24,   /* primary ~24px CJK + ASCII */
    u8g2_font_helvB14_tr,     /* fallback 1 ~14px ASCII only */
    u8g2_font_helvB12_tr,     /* fallback 2 ~12px ASCII only */
    u8g2_font_helvB10_tr,     /* fallback 3 ~10px ASCII only */
    u8g2_font_helvB08_tr,     /* minimum ~8px ASCII only */
};

static const uint8_t *stop_font_chain[] = {
    u8g2_font_zhhk_stop_20,   /* primary ~20px CJK + ASCII */
    u8g2_font_helvR10_tr,    /* fallback 1 ~10px ASCII only */
    u8g2_font_helvR08_tr,    /* fallback 2 ~8px ASCII only */
};
```

**3. `main/display.c`** — Update ESP_LOGI diagnostic strings (lines ~194–201):

The current diagnostic logging hardcodes font-name strings for comparison:

```c
dest_font == u8g2_font_zhhk_dest_18 ? "zhhk_dest_18" :
...
stop_font == u8g2_font_zhhk_stop_13 ? "zhhk_stop_13" :
```

These must be updated to the new font names:

```c
dest_font == u8g2_font_zhhk_dest_24 ? "zhhk_dest_24" :
dest_font == u8g2_font_helvB14_tr ? "helvB14" :
dest_font == u8g2_font_helvB12_tr ? "helvB12" :
dest_font == u8g2_font_helvB10_tr ? "helvB10" : "helvB08"

stop_font == u8g2_font_zhhk_stop_20 ? "zhhk_stop_20" :
stop_font == u8g2_font_helvR10_tr ? "helvR10" : "helvR08"
```

**4. `partitions.csv`** — Resize factory to 8 MB:

```csv
nvs,      data, nvs,     0x9000,    0x6000,
phy_init, data, phy,     0xF000,    0x1000,
factory,  app,  factory, 0x10000,   0x800000,
storage,  data, spiffs,  0x810000,  0x200000,
```

**5. `main/CMakeLists.txt`** — Update source file list:

```cmake
idf_component_register(SRCS "main.c" "display.c" "eta_fetcher.c" "route_config.c"
                          "battery.c"
                          "fonts/u8g2_font_zhhk_dest_24.c" "fonts/u8g2_font_zhhk_stop_20.c"
                       INCLUDE_DIRS "." "fonts"
                       REQUIRES nvs_flash esp_wifi esp_event esp_http_client spiffs
                                 u8g2 u8g2_st7305 esp_driver_spi esp_driver_gpio
                                 mbedtls esp_adc)

target_compile_definitions(${COMPONENT_LIB} PRIVATE U8G2_USE_LARGE_FONTS)

spiffs_create_partition_image(storage "${CMAKE_SOURCE_DIR}/spiffs_data" FLASH_IN_PROJECT)
```

**6. Delete old font files**:

```bash
rm main/fonts/u8g2_font_zhhk_dest_18.c
rm main/fonts/u8g2_font_zhhk_stop_13.c
```

**7. Update documentation**:

- **`design.md`** §2 Typography: Update font names (`u8g2_font_zhhk_dest_24` / `u8g2_font_zhhk_stop_20`), glyph heights (24px / 20px), and source (Noto Sans CJK HK via otf2bdf). Update line-height table to add 24px and 20px entries.
- **`design.md`** §3 Structural rules: Update Col 2 font references (lines referencing `u8g2_font_zhhk_dest_18` and `u8g2_font_zhhk_stop_13`).
- **`design.md`** §7 Mockup checklist: Update font name references.
- **`CLAUDE.md`** §zh-HK CJK Font Support: Update font names, sizes, source (Noto Sans CJK HK), partition layout (8 MB factory, storage at 0x810000), binary size, and map file info (expanded map with CJK punctuation).
- **`PRD.md`** §10 Decision #8: Change status from "TBC" to the implementation outcome.
- **`HANDOFF.md`** §1 (Last Completed Step) and §3 (Build Status): Update after build verification.

**8. Build and verify**:

```bash
idf.py build
# Check binary size — must be under 8 MB (0x800000)
# If build fails due to partition size, verify partitions.csv is updated
```

### Phase 6: Hardware Verification

1. Flash to device: `idf.py flash`
2. Verify CJK glyphs render correctly on the physical ST7305 display
3. Check all 3 routes from `routes.json` display correctly:
   - 30X → 黃埔花園 / 荃灣大河道
   - 238X → 中港碼頭 / 海濱花園總站
   - 930X → 銅鑼灣 / 聯仁街
4. Verify "往" prefix renders at stop_font size (20px)
5. Verify ETA values still render correctly (profont29/profont17 unchanged)
6. Verify no text overflow or truncation
7. Check render time stays within the 10 s refresh interval (CPU is at 160 MHz — larger glyphs may slow rendering)

---

## Rollback Procedure

If the new fonts render poorly on the physical display or the build fails:

```bash
# Restore old font files from git
git checkout main/fonts/u8g2_font_zhhk_dest_18.c
git checkout main/fonts/u8g2_font_zhhk_stop_13.c

# Restore old code references
git checkout main/fonts/fonts.h
git checkout main/display.c
git checkout main/CMakeLists.txt
git checkout partitions.csv

# Rebuild and flash
idf.py build && idf.py flash
```

---

## Risk Assessment

| Risk | Severity | Mitigation |
|------|----------|------------|
| Font binary larger than estimated (RLE variability) | **CRITICAL** | Phase 4.5 validation checkpoint — verify actual size before code changes. If >6.5 MB, switch to D2 (reduced map). |
| Download URL / asset name wrong | **CRITICAL** | Fixed in Phase 2 — use `Sans2.004` tag, `10_NotoSansCJKhk.zip`. |
| CJK punctuation falls back to Helvetica (misalignment) | **CRITICAL** | Phase 3 expands map to include U+3000-303F and U+FF00-FFEF. |
| Poor rendering on 1-bit RLCD | HIGH | Rasterized vector fonts lack hand-tuning. Must test on physical hardware (Phase 6). Try `-n` / `-a` hinting flags. |
| `display.c` ESP_LOGI font-name strings not updated | HIGH | Phase 5 step 3 — update hardcoded comparison strings. |
| `otf2bdf` build fails (legacy `freetype-config`) | MEDIUM | Phase 1 note — use `PKG_CONFIG_PATH` with `brew --prefix freetype`. |
| Flash partition overflow (>8 MB) | MEDIUM | Phase 4.5 checks size. 8 MB partition has 1.1–1.6 MB headroom expected. |
| Render performance (160 MHz CPU, larger glyphs) | MEDIUM | Phase 6 step 7 — verify render time < 10 s refresh interval. |
| `design.md` / `CLAUDE.md` / `PRD.md` become stale | MEDIUM | Phase 5 step 7 — update all docs. |
| Long text overflow (proportional glyph widths) | LOW | Shrink-to-fit chain handles via Helvetica fallbacks. |
| "往" prefix visual balance | LOW | Drawn in stop_font (20px) — verify in Phase 6. |

---

## Appendix: Font Size Estimation

Current 16px font = 1.18 MB for 27,618 glyphs. At 24px (1.5× linear), a
naive area-scaling estimate gives ~2.66 MB. At 20px (1.25×), ~1.84 MB.

**However**, this estimate is unreliable for U8g2 format 1. U8g2 uses RLE
compression on bitmap data. RLE efficiency depends on glyph stroke density
and pattern regularity, not just pixel area:

- **WenQuanYi Bitmap Song** (current): Hand-tuned bitmap font with dense,
  regular strokes. RLE compresses well.
- **Noto Sans CJK HK Bold** (new, dest): Vector font rasterized by FreeType.
  Bold weight produces thicker strokes with different RLE characteristics.
  RLE efficiency may be 10-30% lower than WenQuanYi at equivalent size.
- **Noto Sans CJK HK Regular** (new, stop): Thinner strokes, RLE efficiency
  likely closer to WenQuanYi.

**Revised estimate range**: 5.1–6.2 MB total (vs. 5.1 MB naive estimate).
The actual size **must be verified after Phase 4 conversion**.

Current `.c` file sizes on disk (hex-encoded, ~2.6× binary size):
- `u8g2_font_zhhk_dest_18.c`: 3,290,726 bytes (on disk)
- `u8g2_font_zhhk_stop_13.c`: 2,058,277 bytes (on disk)
