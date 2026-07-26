# Third-Party Licenses and Attributions

This project's own source code is licensed under the MIT License (see LICENSE).
However, this firmware bundles or is derived from several third-party components
with their own licenses, listed below. Anyone building, modifying, or
redistributing this firmware should review these terms.

---

## Noto Sans CJK Font (SIL Open Font License 1.1)

**Component**: Compiled zh-HK (Traditional Chinese) bitmap font subsets used for
destination and bus-stop name rendering
(`main/fonts/u8g2_font_zhhk_dest_24.c`, `main/fonts/u8g2_font_zhhk_stop_20.c`).

**Source**: Noto Sans CJK HK, https://github.com/notofonts/noto-cjk

**License**: SIL Open Font License 1.1 (OFL-1.1)

**Notice**: The font data compiled into the above files is derived from Noto Sans CJK HK,
which is licensed under the SIL Open Font License 1.1. OFL-1.1 is a permissive font license
that explicitly allows embedding fonts in documents and software without requiring copyleft
attribution for the surrounding project. The OFL requires that the font software itself
(not the project as a whole) retains its license terms when distributed. A full copy of the
OFL-1.1 license text is included in this repository at `third_party_licenses/OFL.txt` (or
linked at https://openfontlicense.org/open-font-license-official-text/).

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

## Hong Kong Observatory (HKO) Open Data API

**Component**: Current air temperature data fetched at runtime (not bundled
code, but a runtime data dependency) for the header-band temperature display
(`main/weather_hko.c`).

**Source**: HKO "Current Weather Report" rhrread open data API:
https://data.weather.gov.hk/weatherAPI/opendata/weather.php?dataType=rhrread&lang=en

**Terms**: Provided under the Hong Kong Observatory's Open Data Licence.
See https://www.hko.gov.hk/en/abouthko/opendata.htm for full terms. This
project consumes this API as a data source only; no API code or data is
redistributed as part of this repository.

