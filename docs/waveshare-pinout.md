# ESP32-S3-RLCD-4.2 — Waveshare Board Reference

Source: https://docs.waveshare.net/ESP32-S3-RLCD-4.2/

## Product Overview

An ESP32-S3-based fully-reflective-screen AIoT development board supporting dual-mode Wi-Fi + BLE communication. Features a 4.2" fully reflective display (RLCD), low power consumption, e-ink-like display quality with faster refresh response. Onboard audio codec circuitry, dual microphones, speaker, SHTC3 high-precision temperature/humidity sensor, Micro SD card slot, RTC interface, and battery charge/discharge management circuitry. Reserved USB, UART, I2C, and multiple GPIO interfaces for expansion.

Suited for: AI voice applications, temperature/humidity monitoring, IoT control, DIY desktop smart displays, e-calendars, AI agents, and product prototyping.

### SKUs

| SKU | Product |
|---|---|
| 33298 | ESP32-S3-RLCD-4.2 |
| 33507 | ESP32-S3-RLCD-4.2-EN |

### Safety Notes

1. When connecting Type-C power or the 18650 battery, do not use the screen as a load-bearing point.
2. The screen is a precision, fragile component — handle gently during assembly and transport; never drop or impact it.
3. Screen cracking or display malfunction resulting from improper handling as above is not covered under warranty.

## Product Features

- High-performance Xtensa 32-bit LX7 dual-core processor, up to 240MHz
- 2.4GHz Wi-Fi and Bluetooth 5 (LE) support, built-in antenna
- Built-in 512KB SRAM and 384KB ROM; stacked package integrates 16MB Flash and 8MB PSRAM
- 4.2" fully reflective display, 300×400 resolution, reflective imaging, no backlight required
- Dual-microphone array supporting noise reduction and echo cancellation — suited for accurate voice recognition and near/far-field wake word applications
- Onboard PCF85063 RTC real-time clock and SHTC3 temperature/humidity sensor for precise time management and environmental monitoring
- Onboard 18650 lithium battery holder and RTC backup battery holder (requires rechargeable RTC battery) — supports both main power and independent RTC power modes
- Onboard Micro SD card slot for external image/file storage
- Onboard KEY and BOOT side buttons, both customizable
- Reserved 2×8 pin header (2.54mm pitch) for external expansion

## Onboard Components

| # | Component | Description |
|---|---|---|
| 1 | ESP32-S3-WROOM-1-N16R8 | Wi-Fi/BLE SoC, 240MHz, stacked 16MB Flash + 8MB PSRAM |
| 2 | ES7210 | ADC chip for echo cancellation circuit |
| 3 | ES8311 | Low-power audio codec chip |
| 4 | BOOT button | Hold BOOT + power cycle to force download mode |
| 5 | PWR button | Long-press to power off, single-click to power on |
| 6 | KEY button | Custom function button |
| 7 | SHTC3 | Temperature/humidity sensor |
| 8 | PCF85063 | RTC clock chip, supports time-keeping |
| 9 | MX1.25 2PIN speaker connector | Audio output signal, external speaker |
| 10 | RTC independent power connector | PH1.0 rechargeable RTC battery only |
| 11 | 2×8PIN 2.54mm header | GPIO expansion |
| 12 | 18650 battery holder | Main power source |
| 13 | Dual-microphone array | Paired with ES7210 for echo cancellation |
| 14 | CHG LED | Off when battery fully charged |
| 15 | WRN LED | Lit continuously if battery connected in reverse |
| 16 | Type-C port | Firmware flashing and log output |
| 17 | Micro SD slot | FAT32-formatted card support for data expansion |

## Display Driver Pin Mapping (ST7305, SPI)

> Confirmed pin mapping used throughout this project's tutorial and PRD.md. Cross-reference against `st7305-datasheet.pdf` in this same `docs/` folder for driver init/timing details.

| GPIO | Function |
|---|---|
| `GPIO11` | Display CLK |
| `GPIO12` | Display MOSI |
| `GPIO40` | Display CS |
| `GPIO5` | Display DC |
| `GPIO41` | Display RST |

## Development Frameworks Supported

The ESP32-S3-RLCD-4.2 supports both **Arduino IDE** and **ESP-IDF** development frameworks — choose based on project needs and personal preference.

- **Arduino IDE** — Convenient, flexible, easy to learn open-source electronics prototyping platform. Minimal prerequisite knowledge needed; large global community with abundant open-source code, project examples, tutorials, and libraries that encapsulate complex functionality for rapid development. Best for beginners and non-specialists.
- **ESP-IDF** (Espressif IoT Development Framework) — Professional C-based development framework from Espressif for ESP-series chips, including compiler, debugger, and flashing tools. Supports command-line or IDE-integrated development (e.g. VS Code with the Espressif IDF extension, offering code navigation, project management, and debugging features). Recommended for developers with a professional background or higher performance requirements, and better suited to complex project development — **this is the framework used throughout this tutorial.**

## Hardware Specs Summary

| Spec | Value |
|---|---|
| CPU | ESP32-S3 dual-core LX7 @ 240 MHz |
| SRAM | 512 KB |
| ROM | 384 KB |
| PSRAM | 8 MB |
| Flash | 16 MB |
| Display | ST7305 · 400×300 (300×400 per Waveshare's orientation convention) · 1-bit monochrome reflective |
| Wireless | Wi-Fi 4 (2.4GHz) + BLE 5 |
| Extras | SHTC3 temp/humidity sensor, PCF85063 RTC, MicroSD (FAT32), dual-mic array, ES7210 + ES8311 audio codec |
| Power | USB Type-C, 18650 Li-ion holder + RTC backup battery holder |
| Buttons | BOOT, PWR, KEY (custom) |
