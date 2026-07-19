# Plan: Larger zh-HK Fonts via Noto Sans HK + otf2bdf

**Status**: TBC (deferred)  
**Created**: 2026-07-14  
**Tracking**: PRD.md §10 Open/TBC Decisions

---

## Goal

Replace both `dest_zh` and `stop_zh` fonts with larger rasterized versions of Noto Sans CJK HK (Bold for dest, Regular for stop) at 24px and 20px respectively, using `otf2bdf` to convert TTF→BDF, then `bdfconv` to convert BDF→U8g2 C source.

---

## Current State

| Element | BDF Source | Glyph Height | U8g2 Font Name | Binary Array Size |
|---------|-----------|-------------|----------------|-------------------|
| `dest_zh` | `wenquanyi_12pt.bdf` (16px) | 19px (asc 14 + desc 4) | `u8g2_font_zhhk_dest_18` | 1,238,287 bytes (~1.18 MB) |
| `stop_zh` | `wenquanyi_9pt.bdf` (12px) | 15px (asc 12 + desc 3) | `u8g2_font_zhhk_stop_13` | 765,685 bytes (~748 KB) |
| **Total font binary** | | | | **~2,003,972 bytes (~1.91 MB)** |

Source `.c` file sizes on disk: 3.3 MB + 2.1 MB = 5.4 MB (hex-encoded).

## Target State

| Element | Source Font | Pixel Size | U8g2 Font Name | Est. Binary Size |
|---------|-----------|-----------|----------------|-----------------|
| `dest_zh` | Noto Sans HK **Bold** | 24px | `u8g2_font_zhhk_dest_24` | ~2.8 MB (full map) |
| `stop_zh` | Noto Sans HK **Regular** | 20px | `u8g2_font_zhhk_stop_20` | ~2.3 MB (full map) |
| **Total font binary** | | | | **~5.1 MB** |

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
  Longest dest_zh: ~5 chars × 24px = 120px + "往" prefix ~24px + 2px gap = 146px → fits in 208px
  Longest stop_zh: ~6 chars × 24px = 144px → fits in 208px
  Shrink-to-fit chain handles overflow via Helvetica fallbacks.
```

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

### Risks of Partition Resize

| Concern | Risk | Mitigation |
|---------|------|-----------|
| SPIFFS data wiped | Low | Auto-recovered by `FLASH_IN_PROJECT` (re-flashes routes.json) |
| NVS wiped | None | NVS is before factory, untouched |
| OTA breakage | None | No OTA in use |
| Code changes needed | None | Partition referenced by label (`"storage"`), not offset |
| Full erase on flash | Expected | Normal behaviour; all data re-flashed fresh |

## Toolchain Requirements

| Tool | Location | Status | Action Needed |
|------|----------|--------|--------------|
| `otf2bdf` | `../u8g2/tools/font/otf2bdf/` | Source only | Build from source (needs FreeType) |
| FreeType library | System | Not installed | `brew install freetype` |
| `bdfconv` | `../u8g2/tools/font/bdfconv/` | Source only (Windows .exe exists) | `make` |
| `build1` (pre-built) | `../u8g2/tools/font/build/build1` | Exists (macOS binary) | Ready to use |
| `cjk_unified.map` | `../u8g2/tools/font/build/cjk_unified.map` | Ready | No action |
| Noto Sans HK OTF | Not downloaded | Needed | Download from GitHub |

## Execution Steps

### Phase 1: Environment Setup

```bash
# 1. Install FreeType (required by otf2bdf)
brew install freetype

# 2. Build otf2bdf
cd /Users/alanlee/Documents/trae_projects/u8g2/tools/font/otf2bdf
./configure
make

# 3. Verify bdfconv
ls -la /Users/alanlee/Documents/trae_projects/u8g2/tools/font/build/build1
# If not executable:
cd /Users/alanlee/Documents/trae_projects/u8g2/tools/font/bdfconv && make
```

### Phase 2: Download Noto Sans HK

```bash
# Download Noto Sans HK OTFs from GitHub releases
# URL: https://github.com/notofonts/noto-cjk/releases
# Asset: 08_NotoSansHKOTFs.zip (contains Regular + Bold)

mkdir -p /Users/alanlee/Documents/trae_projects/u8g2/tools/font/bdf/noto_hk
cd /Users/alanlee/Documents/trae_projects/u8g2/tools/font/bdf/noto_hk

# Download and extract
curl -L -o noto_hk.zip \
  "https://github.com/notofonts/noto-cjk/releases/download/Serif2.004/08_NotoSansHKOTFs.zip"
unzip noto_hk.zip
# Files: NotoSansHK-Regular.otf, NotoSansHK-Bold.otf
```

### Phase 3: Create Reduced Map File (Critical for Flash Size)

The full `cjk_unified.map` (27,618 glyphs) at 24px would produce ~5+ MB — too large for the 4 MB app partition without resize.

Two options:
- **D1**: Use full map + resize factory to 8 MB (simplest, ~5.1 MB total)
- **D2**: Use reduced map (~3,000 chars from actual routes.json data) + keep 4 MB factory (~500 KB total)

Recommendation: start with **D1** (full map, 8 MB factory) to avoid the complexity of character extraction.

### Phase 4: Convert OTF → BDF → U8g2 C Source

```bash
BDF_DIR=/Users/alanlee/Documents/trae_projects/u8g2/tools/font/bdf
OTF_DIR=$BDF_DIR/noto_hk
MAP=/Users/alanlee/Documents/trae_projects/u8g2/tools/font/build/cjk_unified.map
BDFCONV=/Users/alanlee/Documents/trae_projects/u8g2/tools/font/build/build1
OTF2BDF=/Users/alanlee/Documents/trae_projects/u8g2/tools/font/otf2bdf/otf2bdf
OUT=/Users/alanlee/Documents/trae_projects/hkbus-eta-rlcd/main/fonts

# Destination font: Noto Sans HK Bold, 24px
$OTF2BDF -p 24 -r 72 -o $BDF_DIR/noto_hk_bold_24.bdf $OTF_DIR/NotoSansHK-Bold.otf
$BDFCONV -v -f 1 -M $MAP $BDF_DIR/noto_hk_bold_24.bdf \
  -o $OUT/u8g2_font_zhhk_dest_24.c \
  -n u8g2_font_zhhk_dest_24 \
  -d $BDF_DIR/helvB24.bdf

# Stop font: Noto Sans HK Regular, 20px
$OTF2BDF -p 20 -r 72 -o $BDF_DIR/noto_hk_reg_20.bdf $OTF_DIR/NotoSansHK-Regular.otf
$BDFCONV -v -f 1 -M $MAP $BDF_DIR/noto_hk_reg_20.bdf \
  -o $OUT/u8g2_font_zhhk_stop_20.c \
  -n u8g2_font_zhhk_stop_20 \
  -d $BDF_DIR/helvR18.bdf
```

### Phase 5: Update Firmware Code

**1. `main/fonts/fonts.h`** — Replace declarations:

```c
extern const uint8_t u8g2_font_zhhk_dest_24[] U8G2_FONT_SECTION("u8g2_font_zhhk_dest_24");
extern const uint8_t u8g2_font_zhhk_stop_20[] U8G2_FONT_SECTION("u8g2_font_zhhk_stop_20");
```

**2. `main/display.c`** — Update font chains (lines ~115–130):

```c
static const uint8_t *dest_font_chain[] = {
    u8g2_font_zhhk_dest_24,   /* primary ~24px CJK + ASCII */
    u8g2_font_helvB14_tr,     /* fallback 1 */
    u8g2_font_helvB12_tr,     /* fallback 2 */
    u8g2_font_helvB10_tr,     /* fallback 3 */
    u8g2_font_helvB08_tr,     /* minimum */
};

static const uint8_t *stop_font_chain[] = {
    u8g2_font_zhhk_stop_20,   /* primary ~20px CJK + ASCII */
    u8g2_font_helvR10_tr,    /* fallback 1 */
    u8g2_font_helvR08_tr,    /* fallback 2 */
};
```

**3. `partitions.csv`** — Resize factory to 8 MB:

```csv
nvs,      data, nvs,     0x9000,    0x6000,
phy_init, data, phy,     0xF000,    0x1000,
factory,  app,  factory, 0x10000,   0x800000,
storage,  data, spiffs,  0x810000,  0x200000,
```

**4. `main/CMakeLists.txt`** — Update source file list:

```cmake
SRCS "main.c" "display.c" "eta_fetcher.c" "route_config.c"
     "fonts/u8g2_font_zhhk_dest_24.c" "fonts/u8g2_font_zhhk_stop_20.c"
```

**5. Delete old font files**:

```bash
rm main/fonts/u8g2_font_zhhk_dest_18.c
rm main/fonts/u8g2_font_zhhk_stop_13.c
```

## Risk Assessment

| Risk | Severity | Mitigation |
|------|----------|------------|
| App partition overflow (>4 MB) | HIGH | Resize factory to 8 MB in partitions.csv |
| Poor rendering on 1-bit RLCD | HIGH | Rasterized vector fonts lack hand-tuning. Must test on physical hardware. |
| FreeType build issues on macOS | Medium | `brew install freetype` then `./configure && make` |
| Font hinting artifacts | Medium | Try `-n` (disable hinting) and `-a` (force auto-hinting) flags, compare results |
| "往" prefix size mismatch | Low | Drawn in stop_font — at 20px it will be larger; verify visual balance |
| Long text overflow | Low | Shrink-to-fit chain handles via Helvetica fallbacks |

## Appendix: Font Size Estimation

Current 16px font = 1.18 MB for 27,618 glyphs. At 24px (1.5× linear), bitmap data per glyph scales ~2.25× (area), so ~2.66 MB. At 20px (1.25×), ~1.84 MB. Rounded up for RLE overhead.

Current `.c` file sizes on disk (hex-encoded, ~2.6× binary size):
- `u8g2_font_zhhk_dest_18.c`: 3,290,726 bytes (on disk)
- `u8g2_font_zhhk_stop_13.c`: 2,058,277 bytes (on disk)