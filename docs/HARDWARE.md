# Hardware Reference

## Primary target: PCBA5981

| Component | Part | Notes |
|---|---|---|
| MCU | ESP32 (dual-core, 240 MHz) | Arduino framework via PlatformIO |
| Graphics controller | LT7680A | Drives ST7701S via parallel interface; handles framebuffer in SDRAM |
| Display panel | ST7701S | 480×480 pixels, MIPI |
| Touch controller | GT911 | Capacitive, I2C |
| SD card | SPI | CS=12, SCK=16, MISO=21, MOSI=33 |
| Hall effect sensor | GPIO 15 | Magnetic menu trigger, 50 ms debounce |

## GPIO pin map

| GPIO | Function |
|---|---|
| 12 | SD card CS |
| 15 | Hall effect sensor (active when magnet present) |
| 16 | SD card SCK |
| 21 | SD card MISO |
| 33 | SD card MOSI |

Remaining display and touch pins are managed by the Panel_PCBA5981 driver and LGFX config header — do not reassign without updating both.

## Memory layout

- **SDRAM** (external, managed by LT7680A): holds framebuffer and backing store
  - Slot 0: live 480×480 canvas display buffer
  - Slot 1: backing store — snapshot before UI overlays; restored when overlay closes
- **Flash**: PlatformIO `min_spiffs.csv` partition scheme (minimal SPIFFS, maximizes app partition)
- **NVS**: preferences stored in `"Friendbox"` namespace

## Planned hardware features

- Hall effect lid sensor (detect open/close)
- LED array
- Speaker / audio output

## Alternative hardware (legacy)

The `LGFX_ESP32_ST7796S_XPT2046.hpp` header supports an older board variant with resistive touch. Not the active development target.
