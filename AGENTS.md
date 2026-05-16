# FriendBox — Agent Guide

The FriendBox is desk appliance to to send animations, drawings, and more to friends. Creations called "sketches". When receive message, appliance will notify by flashing the screen, prompting to remove the lid and see message. It is extended by web application with a built in viewer, social media network called "SketchWall", and drawing tool that is built within FriendBox constraints..

Technologically, FriendBox is an ESP32 device with 480x480 LT7680-powered display, documented in `docs/`. Because of limited hardware, expected that everything fully utilizes the ESP32, no room for wasted performance or memory-leaks. LovyanGFX (similar to TFT-eSPI) handles core functions for the display. If needed, expensive tasks offload via the REST API Python server.

Being an appliance, stability/error handling is critical. FriendBox at release cannot have issues that would result in random crashes or lost data.

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
| UI core | [src/ui/ui_core.cpp](src/ui/ui_core.cpp), [include/ui_core.hpp](include/ui_core.hpp) | `ScreenHandlers` registry, `changeScreenContext` dispatcher, `UIButton`, `cleanupUIOutOfContext` |
| UI screens | [src/ui/](src/ui/) | One file per screen module; each implements `init` / `onEnter` / `draw` / `handleTouch` and registers via `screens[]` in `ui_core.cpp`. SEND and FILE_BROWSER still inline in `ui_core.cpp` pending extraction. |
| I/O | [src/io.cpp](src/io.cpp), [include/io.hpp](include/io.hpp) | SD card, NVS preferences, Hall effect sensor, FBOX playback pipeline (dual-core producer/consumer) |
| FBOX source | [src/fbox_source.cpp](src/fbox_source.cpp), [include/fbox_source.hpp](include/fbox_source.hpp) | Sequential byte-source abstraction for FBOX playback: `FboxSourceSD`, `FboxSourcePSRAM`, `FboxSourceHTTP`, plus a `FboxSourceCrc` wrapper that accumulates CRC32 |
| Audio | [src/audio.cpp](src/audio.cpp), [include/audio.hpp](include/audio.hpp) | IMA ADPCM decode into PSRAM PCM buffer (no I2S output yet) |
| Network | [src/network.cpp](src/network.cpp), [include/network.hpp](include/network.hpp) | WiFi, HTTP client, friend list API |
| Entry point | [src/main.cpp](src/main.cpp) | Hardware init sequence, boot screen |
| Panel driver | [lib/Panel_PCBA5981/](lib/Panel_PCBA5981/) | Low-level LT7680A + ST7701S driver |
| Display config | [include/LGFX_ESP32_PCBA5981_GT911.hpp](include/LGFX_ESP32_PCBA5981_GT911.hpp) | LovyanGFX config for primary hardware rev |

---

## Key facts for agents

- Standards and expectations for FriendBox are not set in stone, please prompt for further input if things should go in another direction.
- Drawing canvas is **480×480 pixels**, stored as **4-bit palette indices**, packed 2-per-byte → 115,200 bytes per sketch.
- .fbox v3 files are the canonical format for sketches and animations: 128-byte header, frame size table, type-prefixed RLE frames (I or P/XOR-delta), trailing IMA ADPCM audio, CRC32 over [62..EOF]. Spec in [docs/design-docs/fbox-codec.md](docs/design-docs/fbox-codec.md).
- Frames are expected to be offloaded to the LT7680's SDRAM. It is capable of having up to ~72 slots available for our use when using 4bpp frame indices as indicated in `docs/LT7680.pdf`.
- LovyanGFX (similar to TFT-eSPI) handles the core functions for the display. If needed and if possible, new helper functions should be implemented as extensions to already existing functions in LGFX. For example, button.drawButton() could be extended to draw using the LT7680's 2D graphics engine, reducing CPU time on the ESP32.
- Canvas helper functions (loading frame data to a specific slot, pulling frame data from a specific slot, switching display to draw from a different slot) should all be abstracted away with extensions to LGFX. Code in `/src` shouldn't have to deal with writing to registers.
- Animation playback is dual-core: producer task on core 0 decodes into PSRAM slots, main task on core 1 streams to LT7680. Source-agnostic via the `FboxSource` interface — SD, HTTP, and PSRAM impls share the same playback core. See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md#iocpp--iohpp) for the pipeline diagram and invariants.

---

## Where to look next

- **Design decisions and core beliefs:** [docs/design-docs/index.md](docs/design-docs/index.md)
- **Hardware details:** [docs/HARDWARE.md](docs/HARDWARE.md)
- **Active work and execution plans:** [docs/exec-plans/active/](docs/exec-plans/active/)
- **Tech debt:** [docs/exec-plans/tech-debt-tracker.md](docs/exec-plans/tech-debt-tracker.md)
- **Feature specs:** [docs/product-specs/index.md](docs/product-specs/index.md)
