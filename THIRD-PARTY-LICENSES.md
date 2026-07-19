# Third-Party Licenses and Attributions

This project's own source code is licensed under the MIT License (see LICENSE).
However, this firmware bundles or is derived from several third-party components
with their own licenses, listed below. Anyone building, modifying, or
redistributing this firmware should review these terms.

---

## WenQuanYi Bitmap Song Font (GPL v2 or later)

**Component**: Compiled zh-HK (Traditional Chinese) bitmap font subsets used for
destination and bus-stop name rendering
(`main/fonts/u8g2_font_zhhk_dest_18.c`, `main/fonts/u8g2_font_zhhk_stop_13.c`).

**Source**: WenQuanYi Bitmap Song, http://wenq.org/wqy2/index.cgi?BitmapSong_en

**License**: GNU General Public License v2.0 or later (GPLv2+)

**Important notice**: The font data compiled into the above files is derived
from a GPL-licensed source. GPL is a copyleft license. While this project's
own original source code is MIT-licensed, the embedded font data itself
remains subject to GPLv2+ terms. Depending on interpretation, GPL's copyleft
provisions may extend to the compiled firmware binary as a "combined work"
when this font data is statically linked into it. If you intend to
redistribute compiled binaries of this firmware, you should independently
assess your GPL compliance obligations, or consider substituting an
OFL-licensed font source (e.g. Noto Sans CJK / Source Han Sans) for the zh-HK
glyphs to avoid this ambiguity entirely.

A full copy of the GPLv2 license text is included in this repository at
`third_party_licenses/GPLv2.txt` (or linked at
https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).

---

## U8g2 Graphics Library

**Component**: Display rendering library (`components/u8g2/`)

**Source**: https://github.com/olikraus/u8g2

**License**: BSD-style (2-Clause / 3-Clause, see upstream repository for exact
terms)

**Notice**: Permissively licensed, no copyleft obligations. Retained here for
attribution and to comply with the license's copyright-notice-preservation
requirement.

---

## Waveshare ESP32-S3-RLCD-4.2 Reference Driver

**Component**: ST7305 display driver architecture (`components/u8g2_st7305/`),
adapted from Waveshare's official reference implementation.

**Source**: Waveshare ESP32-S3-RLCD-4.2 official reference repository/example
(`02_Example/ESP-IDF/11_U8G2Test/components/u8g2_st7305`)

**License**: Apache License 2.0

**Source**: https://github.com/waveshareteam/ESP32-S3-RLCD-4.2/

**Notice**: This project's ST7305 driver, including init sequence, register
values, pixel layout (u8g2_ll_hvline_vertical_top_lsb), and DRAW_TILE
callback approach, is adapted from Waveshare's official reference driver
for this exact board, as documented in this project's CLAUDE.md.

---

## KMB and Citybus Open Data APIs

**Component**: Real-time bus arrival data fetched at runtime (not bundled
code, but a runtime data dependency).

**Sources**:
- KMB ETA API: https://data.etabus.gov.hk
- Citybus ETA API: https://rt.data.gov.hk

**Terms**: Provided under data.gov.hk's Terms and Conditions of Use for open
data. See https://data.gov.hk for full terms. This project consumes these
APIs as a data source only; no API code or data is redistributed as part of
this repository.

---

## Noto Sans CJK / Source Han Sans (if applicable)

[Only include this section if the Noto/otf2bdf larger-font work has actually
been completed and integrated — confirm against HANDOFF.md before including.]

**Component**: [font files, if used]

**License**: SIL Open Font License 1.1 (OFL-1.1)

**Notice**: Permissively licensed for embedding, no copyleft obligations on
the surrounding project. Retained here for attribution per OFL's
requirements.