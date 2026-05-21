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
| FBOX source | [src/fbox_source.cpp](src/fbox_source.cpp), [include/fbox_source.hpp](include/fbox_source.hpp) | Sequential byte-source abstraction for FBOX playback: `FboxSourceSD`, `FboxSourceHTTP`, an async `FboxSourceRingBuffered` (PSRAM loader task), and a `FboxSourceCrc` wrapper that accumulates CRC32 |
| Audio (ADPCM) | [src/audio.cpp](src/audio.cpp), [include/audio.hpp](include/audio.hpp) | IMA ADPCM block decoder (`ImaAdpcmDecoder::decodeOneBlock`). One block per video frame in v4 — no buffered/post-EOF decode path |
| Audio (I2S) | [src/audio_i2s.cpp](src/audio_i2s.cpp), [include/audio_i2s.hpp](include/audio_i2s.hpp) | I2S streaming driver (BCLK=38 LRCLK=39 DIN=40). `startI2SStreaming` / `pushI2SSamples` / `stopI2SStreaming` + writer task that drains a PSRAM StreamBuffer to DMA |
| Network | [src/network.cpp](src/network.cpp), [include/network.hpp](include/network.hpp) | WiFi, HTTP client, friend list API |
| Entry point | [src/main.cpp](src/main.cpp) | Hardware init sequence, boot screen |
| Panel driver | [lib/Panel_PCBA5981/](lib/Panel_PCBA5981/) | Low-level LT7680A + ST7701S driver |
| Display config | [include/LGFX_ESP32_PCBA5981_GT911.hpp](include/LGFX_ESP32_PCBA5981_GT911.hpp) | LovyanGFX config for primary hardware rev |

---

## Key facts for agents

- Standards and expectations for FriendBox are not set in stone, please prompt for further input if things should go in another direction.
- Drawing canvas is **480×480 pixels**, stored as **4-bit palette indices**, packed 2-per-byte → 115,200 bytes per sketch.
- **.fbox v4** files are the canonical format. 512-byte header (with 256-byte description field), frame size table, per-frame chunks of `[type 'I'/'P'/'S'][N-byte IMA ADPCM block][video RLE]`, optional trailing keyframe offset table, CRC32 over [64..EOF]. Audio is **interleaved per frame** (one ADPCM block per video frame, ~919 samples at 22050 Hz / 24 fps) so it streams from any source at any file size — no PSRAM-full requirement. Skip-frame marker ('S') for held frames carries audio but no video bytes. Spec in [docs/design-docs/fbox-codec.md](docs/design-docs/fbox-codec.md); encoder is `FriendBox-Server/fbox.py`. v3 files are not readable — re-encode via `migrate_v4.py` on the server.
- Frames are expected to be offloaded to the LT7680's SDRAM. It is capable of having up to ~72 slots available for our use when using 4bpp frame indices as indicated in `docs/LT7680.pdf`.
- LovyanGFX (similar to TFT-eSPI) handles the core functions for the display. If needed and if possible, new helper functions should be implemented as extensions to already existing functions in LGFX. For example, button.drawButton() could be extended to draw using the LT7680's 2D graphics engine, reducing CPU time on the ESP32.
- Canvas helper functions (loading frame data to a specific slot, pulling frame data from a specific slot, switching display to draw from a different slot) should all be abstracted away with extensions to LGFX. Code in `/src` shouldn't have to deal with writing to registers.
- Animation playback is tri-task: producer on core 0 reads chunks, decodes ADPCM → pushes PCM to a PSRAM StreamBuffer, RLE-decodes video into PSRAM slots; consumer (main task, core 1) streams slots to LT7680 paced by `vTaskDelayUntil`; I2S writer (core 1, prio 3) drains the StreamBuffer to the DAC. Source-agnostic via the `FboxSource` interface — SD and HTTP share the same playback core. Skip frames bypass the SPI burst. See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md#iocpp--iohpp) for the pipeline diagram and invariants.
- **Pin-init gotcha:** I2S DAC pins (GPIO 38/39/40) MUST be `pinMode(OUTPUT) + digitalWrite(LOW)` in `setup()` before anything else. Floating pins let the external DAC's input network oscillate and corrupt PSRAM, causing crashes in seemingly unrelated code paths (IDLE0 task-WDT walks, loopTask stack canary). See [src/main.cpp](src/main.cpp).
- **No second f_open during playback.** A second SD file handle opened mid-playback consistently tripped IDLE0 memory corruption we never root-caused. v4's interleaved layout means one f_open suffices; don't reintroduce pre-decode-via-second-handle.

---

## Where to look next

- **Design decisions and core beliefs:** [docs/design-docs/index.md](docs/design-docs/index.md)
- **Hardware details:** [docs/HARDWARE.md](docs/HARDWARE.md)
- **Active work and execution plans:** [docs/exec-plans/active/](docs/exec-plans/active/)
- **Tech debt:** [docs/exec-plans/tech-debt-tracker.md](docs/exec-plans/tech-debt-tracker.md)
- **Feature specs:** [docs/product-specs/index.md](docs/product-specs/index.md)
