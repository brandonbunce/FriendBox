# Hardware Reference

## Primary target: PCBA5981

| Component | Part | Notes |
|---|---|---|
| MCU | ESP32 (dual-core, 240 MHz) | Arduino framework via PlatformIO |
| Graphics controller | LT7680A | Drives ST7701S via parallel interface; handles framebuffer in SDRAM |
| Display panel | ST7701S | 480×480 pixels, MIPI |
| Touch controller | GT911 | Capacitive, I2C |
| SD card | SDIO 4-bit @ 40 MHz | CLK=13, CMD=2, DAT0=48, DAT1=47, DAT2=1, DAT3=14 |
| Hall effect sensor | GPIO 15 | Magnetic menu trigger, 50 ms debounce |

## GPIO pin map

| GPIO | Function |
|---|---|
| 1 | SD card DAT2 (SDIO) |
| 2 | SD card CMD (SDIO) |
| 13 | SD card CLK (SDIO) |
| 14 | SD card DAT3 (SDIO) |
| 21 | Hall effect sensor (active when magnet present) |
| 47 | SD card DAT1 (SDIO) |
| 48 | SD card DAT0 (SDIO) |

Remaining display and touch pins are managed by the Panel_PCBA5981 driver and LGFX config header — do not reassign without updating both.

### Reserved pins on N16R8 modules — do not reuse

The ESP32-S3-WROOM-1 N16R8 packaging dedicates **GPIO 26–37 to the SoC's internal SPI bus for the onboard flash and octal PSRAM**. Specifically GPIO 33–37 are the octal PSRAM data lines (SPIIO4–SPIIO7) and SPIDQS. Any peripheral driving these pins corrupts PSRAM mid-transaction and triggers a silent reset with no error message — symptom we hit during the SDIO migration when CMD was on 35 and DAT2 on 36.

**Don't put any GPIO function on 26–37**. They look free in pinout tables but are physically used by the module's internal flash/PSRAM bus.

### SD card history

| Revision | Mode | Pins | Effective throughput |
|---|---|---|---:|
| Initial | SPI @ 4 MHz (Arduino default) | CS=14, SCK=12, MISO=47, MOSI=48 | ~0.35 MB/s |
| Intermediate | SPI @ 40 MHz (after explicit `SD.begin(.., 40 MHz)`) | same pins | ~1.3 MB/s |
| Current | SDIO 4-bit @ 40 MHz | listed above | ~4.7 MB/s |

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
