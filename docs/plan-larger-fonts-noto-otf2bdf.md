# Plan: Larger zh-HK Fonts via Noto Sans CJK HK + otf2bdf

**Status**: Implemented 2026-07-26  
**Created**: 2026-07-14  
**Revised**: 2026-07-22 (critical review fixes applied)  
**Tracking**: PRD.md §10 Decision #8

---

## Goal

Replace both `dest_zh` and `stop_zh` fonts with larger rasterized versions of Noto Sans CJK HK (Bold for dest, Regular for stop) at 24px and 20px respectively, using `otf2bdf` to convert OTF→BDF, then `bdfconv` to convert BDF→U8g2 C source.

## Decisions (resolved 2026-07-22)

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Glyph map coverage | **D1: Full map** (27,618+ glyphs) | routes.json can change without regenerating fonts |
| Font format | **Language-specific OTF** (`10_NotoSansCJKhk.zip` from `Sans2.004`) | Full CJK coverage with HK locale glyphs |
| "往" prefix font | **stop_font** (20px) | Visual distinction between direction marker and destination name |

---

## Current State (before this change)

| Element | BDF Source | Glyph Height | U8g2 Font Name | Binary Array Size |
|---------|-----------|-------------|----------------|-------------------|
| `dest_zh` | `wenquanyi_12pt.bdf` (16px) | 19px (asc 14 + desc 4) | `u8g2_font_zhhk_dest_18` | 1,238,287 bytes (~1.18 MB) |
| `stop_zh` | `wenquanyi_9pt.bdf` (12px) | 15px (asc 12 + desc 3) | `u8g2_font_zhhk_stop_13` | 765,685 bytes (~748 KB) |
| **Total font binary** | | | | **~2,003,972 bytes (~1.91 MB)** |

Source `.c` file sizes on disk: 3.3 MB + 2.1 MB = 5.4 MB (hex-encoded).

**Binary before change**: ~3.21 MB (factory app 4 MB partition, 22% free).

## Target State (achieved)

| Element | Source Font | Pixel Size | U8g2 Font Name | Actual Binary Size |
|---------|-----------|-----------|----------------|-----------------|
| `dest_zh` | Noto Sans CJK HK **Bold** | 24px | `u8g2_font_zhhk_dest_24` | ~2.0 MB |
| `stop_zh` | Noto Sans CJK HK **Regular** | 20px | `u8g2_font_zhhk_stop_20` | ~1.6 MB |
| **Total font binary** | | | | **~3.6 MB** |

**Binary after change**: ~4.78 MB (0x4c8770, 8 MB factory partition, 40% free).

Source `.c` file sizes on disk (hex-encoded, ~2.6× binary size):
- `u8g2_font_zhhk_dest_24.c`: 5,847,975 bytes (on disk)
- `u8g2_font_zhhk_stop_20.c`: 4,522,594 bytes (on disk)

> **Size estimate vs actual**: The naive area-scaling estimate was 5.1 MB
> total. The actual is ~3.6 MB — RLE compression was *more* efficient than
> expected, not less. The review's worst-case concern (6.2 MB) did not
> materialise. The 8 MB partition has ample headroom.

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
> chain handles overflow, but the first-pass font may trigger fallback
> more often than expected for longer stop names (e.g. 將軍澳工業邨, 6 chars).

---

## Flash Budget

### Partition Layout (before → after)

| Partition | Offset (before) | Size (before) | Offset (after) | Size (after) | Content |
|-----------|----------------|--------------|----------------|-------------|---------|
| nvs | 0x9000 | 24 KB | 0x9000 | 24 KB | Wi-Fi creds, NVS keys |
| phy_init | 0xF000 | 4 KB | 0xF000 | 4 KB | PHY calibration |
| factory | 0x10000 | **4 MB** | 0x10000 | **8 MB** | App + fonts |
| storage | 0x410000 | 2 MB | 0x810000 | 2 MB | SPIFFS (routes.json) |

**Flash total**: 16 MB.

### Headroom Analysis (actual)

| Scenario | Font binary | Total binary | 8 MB headroom |
|----------|------------|-------------|---------------|
| Actual (verified) | ~3.6 MB | ~4.78 MB | ~3.2 MB free (40%) |
| Original estimate | ~5.1 MB | ~6.4 MB | ~1.6 MB free |

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

**2. `main/display.c`** — Update font chains:

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

**3. `main/display.c`** — Update ESP_LOGI diagnostic strings:

The diagnostic logging hardcodes font-name strings for comparison. These
must be updated to match the new font symbol names:

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

- **`design.md`** §2 Typography: Update font names, glyph heights, source, and line-height table.
- **`design.md`** §3 Structural rules: Update Col 2 font references and shrink chain names.
- **`design.md`** §7 Mockup checklist: Update font name references.
- **`CLAUDE.md`** §zh-HK CJK Font Support: Update font names, sizes, source, partition layout, binary size, and map file info.
- **`PRD.md`** §10 Decision #8: Change status to "Implemented".
- **`HANDOFF.md`** §1 (Last Completed Step) and §3 (Build Status): Update after build verification.

**8. Build and verify**:

```bash
idf.py build
# Check binary size — must be under 8 MB (0x800000)
```

### Phase 6: Hardware Verification

1. Flash to device: `idf.py flash`
2. Verify CJK glyphs render correctly on the physical ST7305 display
3. Check all routes from `routes.json` display correctly
4. Verify "往" prefix renders at stop_font size (20px)
5. Verify ETA values still render correctly (profont29/profont22 unchanged)
6. Verify no text overflow or truncation
7. Check render time stays within the refresh interval (CPU is at 160 MHz — larger glyphs may slow rendering)

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

| Risk | Severity | Mitigation | Status |
|------|----------|------------|--------|
| Font binary larger than estimated (RLE variability) | **CRITICAL** | Phase 4.5 validation checkpoint — verify actual size before code changes. | **Not triggered** — actual ~3.6 MB, well under 6.5 MB threshold. |
| Download URL / asset name wrong | **CRITICAL** | Fixed in Phase 2 — use `Sans2.004` tag, `10_NotoSansCJKhk.zip`. | **Resolved** — correct URL used. |
| CJK punctuation falls back to Helvetica (misalignment) | **CRITICAL** | Phase 3 expands map to include U+3000-303F and U+FF00-FFEF. | **Resolved** — expanded map applied. |
| Poor rendering on 1-bit RLCD | HIGH | Rasterized vector fonts lack hand-tuning. Must test on physical hardware (Phase 6). Try `-n` / `-a` hinting flags. | **Verified** — glyphs render correctly on ST7305. |
| `display.c` ESP_LOGI font-name strings not updated | HIGH | Phase 5 step 3 — update hardcoded comparison strings. | **Resolved** — strings updated. |
| `otf2bdf` build fails (legacy `freetype-config`) | MEDIUM | Phase 1 note — use `PKG_CONFIG_PATH` with `brew --prefix freetype`. | **Resolved** — build succeeded. |
| Flash partition overflow (>8 MB) | MEDIUM | Phase 4.5 checks size. 8 MB partition has ample headroom. | **Not triggered** — 40% free. |
| Render performance (160 MHz CPU, larger glyphs) | MEDIUM | Phase 6 step 7 — verify render time < refresh interval. | **Verified** — no observable delay. |
| `design.md` / `CLAUDE.md` / `PRD.md` become stale | MEDIUM | Phase 5 step 7 — update all docs. | **Resolved** — all docs updated. |
| Long text overflow (proportional glyph widths) | LOW | Shrink-to-fit chain handles via Helvetica fallbacks. | **Not triggered** — all current routes fit at primary font. |
| "往" prefix visual balance | LOW | Drawn in stop_font (20px) — verify in Phase 6. | **Verified** — visual balance acceptable. |

---

## Appendix: Font Size Estimation

### Original estimate (naive area-scaling)

Current 16px font = 1.18 MB for 27,618 glyphs. At 24px (1.5× linear),
bitmap data per glyph scales ~2.25× (area), so ~2.66 MB. At 20px (1.25×),
~1.84 MB. Total: ~5.1 MB.

### Review-adjusted estimate (RLE variability)

U8g2 format 1 uses RLE compression on bitmap data. RLE efficiency depends
on glyph stroke density and pattern regularity, not just pixel area:

- **WenQuanYi Bitmap Song** (old): Hand-tuned bitmap font with dense,
  regular strokes. RLE compresses well.
- **Noto Sans CJK HK Bold** (new, dest): Vector font rasterized by FreeType.
  Bold weight produces thicker strokes with different RLE characteristics.
- **Noto Sans CJK HK Regular** (new, stop): Thinner strokes, RLE efficiency
  likely closer to WenQuanYi.

Revised estimate range: 5.1–6.2 MB total (vs. 5.1 MB naive estimate).

### Actual result

| Font | Estimated | Actual | Delta |
|------|-----------|--------|-------|
| `u8g2_font_zhhk_dest_24` (24px Bold) | ~2.8 MB | ~2.0 MB | -29% |
| `u8g2_font_zhhk_stop_20` (20px Regular) | ~2.3 MB | ~1.6 MB | -30% |
| **Total** | **~5.1 MB** | **~3.6 MB** | **-29%** |

The actual binary was 29% smaller than the naive estimate. Noto Sans CJK
HK's vector outlines produced sparser bitmaps than expected, leading to
better RLE compression than the hand-tuned WenQuanYi bitmap font. The
review's worst-case concern (6.2 MB) did not materialise.

### File sizes on disk

The `.c` source files are hex-encoded (~2.6× binary size):
- `u8g2_font_zhhk_dest_24.c`: 5,847,975 bytes (on disk)
- `u8g2_font_zhhk_stop_20.c`: 4,522,594 bytes (on disk)
