# Architecture

FriendBox firmware is organized into five logical layers. Each layer depends only on layers below it.

```
┌─────────────────────────────────────────────┐
│                 Application                  │
│  ui.cpp — screen state machine, menus        │
├─────────────────────────────────────────────┤
│                Feature modules               │
│  canvas.cpp      network.cpp    io.cpp        │
│  (drawing tools) (WiFi / HTTP)  (SD / NVS)   │
├─────────────────────────────────────────────┤
│              Display subsystem               │
│  display.cpp — touch queue, SDRAM slots      │
│  LGFX_ESP32_PCBA5981_GT911.hpp — LovyanGFX  │
├─────────────────────────────────────────────┤
│           Custom panel driver                │
│  lib/Panel_PCBA5981/ — LT7680A + ST7701S    │
├─────────────────────────────────────────────┤
│         ESP32 / Arduino framework            │
└─────────────────────────────────────────────┘
```

## Module responsibilities

### main.cpp
Boot sequencer. Initializes hardware in order: display → SD → canvas → touch → WiFi. Renders a loading screen with progress feedback. Defines `SW_VERSION` and `DEBUG_MODE`.

### display.cpp / display.hpp
- Owns the global `tft` (LovyanGFX) object
- Manages a 10-item touch input queue (`touchX`, `touchY`, `touchZ`)
- SDRAM slot allocation: slot 0 = live canvas framebuffer, slot 1 = UI backing store
- Resolution constant: `DISPLAY_WIDTH` / `DISPLAY_HEIGHT` = 480

### canvas.cpp / canvas.hpp
- Defines the 16-color palette (RGB565 values, sourced from androidarts.com)
- Rainbow index array and per-color text contrast values
- Tools: pencil, fill (flood), dither, palette picker
- Color quantization: nearest-neighbor lookup for off-palette input colors

### ui.cpp / ui.hpp
- Central state machine: `currentScreen` enum drives rendering and input handling
- Screens: `SCREEN_CANVAS`, `SCREEN_SEND`, `SCREEN_FILE_BROWSER`, `SCREEN_SYSTEM_MESSAGE`, and others
- `UIButton` struct tracks position, context, fill color, and action mode (`ACT_ON_PRESS`, `ACT_ON_HOVER_AND_RELEASE`, `ACT_ON_RELEASE`)
- `UIList` manages paginated friend and file lists
- **This module is the primary refactor target** — see [exec-plans/tech-debt-tracker.md](exec-plans/tech-debt-tracker.md)

### io.cpp / io.hpp
- SD card over SPI: CS=12, SCK=16, MISO=21, MOSI=33
- Sketch file format: 115,200 bytes, 4-bit palette indices packed 2-per-byte for 230,400 pixels
- NVS namespace `"Friendbox"` stores user preferences
- Hall effect sensor: GPIO 15, 50 ms debounce, opens main menu

### network.cpp / network.hpp
- WiFi connection with hostname `"friendbox"`
- `networkGetFriends()`: HTTP GET → ArduinoJson parse → friend list
- `networkSendFramebuffer()` / `networkReceiveFramebuffer()`: stubs, not yet implemented

### lib/Panel_PCBA5981/
Low-level driver for the LT7680A graphics accelerator driving the ST7701S MIPI panel. Implements the LovyanGFX panel interface. Not modified during normal feature work.

## Hardware revisions

| Revision | Display IC | Touch IC | Config header |
|---|---|---|---|
| PCBA5981 (primary) | LT7680A + ST7701S | GT911 (capacitive) | `LGFX_ESP32_PCBA5981_GT911.hpp` |
| Older variant | ST7796S | XPT2046 (resistive) | `LGFX_ESP32_ST7796S_XPT2046.hpp` |

`platformio.ini` targets the PCBA5981 revision by default.

## Data flow: touch → render

```
GT911 hardware interrupt
  → display.cpp touch queue (ring buffer, 10 items)
    → ui.cpp poll loop reads touchX/Y/Z
      → UIButton hit-test
        → action callback (draw, menu open, file load, etc.)
          → tft.drawPixel / tft.fillRect / SDRAM slot restore
```
