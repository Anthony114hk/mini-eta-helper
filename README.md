# 🚌 ESP32 KMB Bus ETA Display

![Hardware: ESP32 CYD (240×320 ST7789 + XPT2046 touch)](https://img.shields.io/badge/hardware-ESP32_CYD-blue)
![Framework: Arduino + LovyanGFX](https://img.shields.io/badge/framework-Arduino%20%2B%20LovyanGFX-green)
![License: MIT](https://img.shields.io/badge/license-MIT-yellow)

Hong Kong KMB bus real-time arrival display for the **ESP32 CYD** (Cheap Yellow Display) — 240×320 ST7789 LCD with resistive touch. Also shows weather (HKO Open Data), warnings, and forecast.

## Features

- 🚌 **Real-time KMB ETA** for up to 5 bus stops (Hong Kong)
- 🌦️ **HKO weather** — temperature, humidity, rainfall, warnings, forecast
- 📺 **Scrolling marquee** for long weather strings
- ⚙️ **WiFi + stop config portal** (WiFiManager + WebServer)
- 🔄 **OTA updates from GitHub Releases** — GPIO 0 button long-press 3s (touch chip dead on this CYD)

## Hardware

- ESP32-2432S028R (CYD v1/v2) — 240×320 ST7789 IPS, XPT2046 touch
- USB-C for power + flashing

## Pin Map

| Function | GPIO |
|---|---|
| LCD BL | 27 |
| LCD CS | 5 |
| LCD DC | 2 |
| LCD RST | 4 |
| Touch CS | 33 |
| Touch CLK | 26 |
| Touch DIN | 32 |
| Touch DO | 39 |
| Touch IRQ | 36 |

## Build & Flash

1. Install Arduino IDE + ESP32 board support
2. Install libraries:
   - `LovyanGFX`
   - `ArduinoJson` (v7)
   - `WiFiManager`
   - `Preferences` (built-in)
3. Open `kmb-eta-display.ino`, select board "ESP32 Dev Module" (or CYD variant)
4. Upload

## First Boot

1. On first boot (no WiFi config), device opens `ESP32_Smart_Clock` AP
2. Connect phone → 192.168.4.1 → enter WiFi + up to 5 KMB stop IDs
3. Save → device reboots and shows bus ETAs

Stop IDs are KMB format, e.g. `20080C0DBE40B5D2` (route+bound+stop+seq hash).

## OTA Updates

1. Build firmware in Arduino IDE
2. Export compiled binary: **Sketch → Export Compiled Binary** → produces `.ino.bin`
3. Rename to `kmb-eta-display.bin`
4. Create a [GitHub Release](https://github.com/yourname/yourrepo/releases/new):
   - Tag: `v1.0.0`, `v1.0.1`, ...
   - Attach `kmb-eta-display.bin`
   - Publish
5. On ESP32: **hold GPIO 0 button for 3+ seconds** → OTA page appears
6. Page shows current vs latest version + an "立即升級" button
7. Short-press GPIO 0 button to upgrade (or hold 1.5s+ to cancel) → device downloads, flashes, and reboots

## Touch Diagnostic

If the touch chip is suspected faulty (no response to finger taps), flash `touch_test.ino` instead of the main firmware:

1. Open `touch_test.ino` in Arduino IDE
2. Sketch → Upload
3. Open Serial Monitor @ 115200 baud
4. Touch any of the 16 zones (0–F) on the 4×4 grid
5. Tap the center zone ("5" with red "R" badge) to reset all counters

The test renders a 4×4 grid of zones. Each tap:
- Flips the zone from black to green
- Increments a per-zone counter (bottom-right corner)
- Logs the raw X/Y, Z1 pressure, and zone ID to Serial

If no touch registers within 5 seconds, a red "TOUCH NOT RESPONDING" banner appears and Serial logs the warning every 5 s. This confirms the chip is dead or the pinout is wrong.

For a deeper sweep across known chip types and pin combinations, flash `touch_diagnostic.ino` instead — it scans 12 XPT2046 pin sets × bit-bang + HSPI × 6 commands plus 9 FT6336/FT6206 + 2 GT911 I²C combos (≈165 combinations total). Hold a finger on the screen for the full ~60 s scan.

> **Note for this CYD:** the touch controller has been scan-confirmed dead — `touch_diagnostic.ino` reported `❌` across all 165 combinations on 2026-09-12. The firmware above is retained as a reusable diagnostic for other CYDs; this specific board relies on the GPIO 0 button for user input.

## License

MIT