# FriendBox — Agent Guide

FriendBox is an ESP32-based embedded device for drawing and exchanging pixel art with friends over WiFi. Built with PlatformIO (Arduino framework) and LovyanGFX on a 480×480 touchscreen.

---

## Quick orientation

| Question | Answer |
|---|---|
| What does this device do? | Draw 480×480 16-color pixel art, save to SD card, send/receive via REST API |
| What hardware runs it? | ESP32 + LT7680A graphics controller + ST7701S panel + GT911 capacitive touch |
| How is it built? | PlatformIO (`pio run`), Arduino framework, C++17 |
| Where are credentials? | `include/secrets.hpp` — not committed, gitignored |

---

## Repository map

```
src/           — Firmware modules (display, canvas, ui, io, network)
include/       — Header files and hardware driver config
lib/           — Custom LovyanGFX panel driver (Panel_PCBA5981)
test/          — Unit tests (PlatformIO test runner)
platformio.ini — Build config, dependencies, board target
docs/          — System of record for design, plans, and specs
AGENTS.md      — This file (entry point, map only)

```

Full architecture: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)

---

## Module index

| Module | File(s) | Responsibility |
|---|---|---|
| Display | [src/display.cpp](src/display.cpp), [include/display.hpp](include/display.hpp) | LovyanGFX init, touch input queue, SDRAM framebuffer slots |
| Canvas | [src/canvas.cpp](src/canvas.cpp), [include/canvas.hpp](include/canvas.hpp) | 16-color palette, drawing tools, color quantization |
| UI | [src/ui.cpp](src/ui.cpp), [include/ui.hpp](include/ui.hpp) | Screen state machine, button handling, menus (**needs refactor — see tech debt**) |
| I/O | [src/io.cpp](src/io.cpp), [include/io.hpp](include/io.hpp) | SD card, NVS preferences, Hall effect sensor |
| Network | [src/network.cpp](src/network.cpp), [include/network.hpp](include/network.hpp) | WiFi, HTTP client, friend list API |
| Entry point | [src/main.cpp](src/main.cpp) | Hardware init sequence, boot screen |
| Panel driver | [lib/Panel_PCBA5981/](lib/Panel_PCBA5981/) | Low-level LT7680A + ST7701S driver |
| Display config | [include/LGFX_ESP32_PCBA5981_GT911.hpp](include/LGFX_ESP32_PCBA5981_GT911.hpp) | LovyanGFX config for primary hardware rev |

---

## Key facts for agents

- Canvas is **480×480 pixels**, stored as **4-bit palette indices**, packed 2-per-byte → 115,200 bytes per sketch
- Display uses **dual SDRAM slots**: slot 0 is the live framebuffer, slot 1 is a backing store for overlay repair
- The UI is a **single-file state machine** (`ui.cpp`, ~1,300 lines) — all screens, buttons, and dropdowns live here; refactor is tracked in tech debt
- SD card SPI pins: CS=12, SCK=16, MISO=21, MOSI=33
- Hall effect sensor on GPIO 15 (50 ms debounce) opens the main menu
- NVS namespace: `"Friendbox"` — stores palette selection and other prefs
- LT7680 documentation is available at docs/LT7680.pdf

---

## Where to look next

- **Design decisions and core beliefs:** [docs/design-docs/index.md](docs/design-docs/index.md)
- **Hardware details:** [docs/HARDWARE.md](docs/HARDWARE.md)
- **Active work and execution plans:** [docs/exec-plans/active/](docs/exec-plans/active/)
- **Tech debt:** [docs/exec-plans/tech-debt-tracker.md](docs/exec-plans/tech-debt-tracker.md)
- **Feature specs:** [docs/product-specs/index.md](docs/product-specs/index.md)
