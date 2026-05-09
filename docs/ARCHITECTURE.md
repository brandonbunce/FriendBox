# Architecture

FriendBox firmware is organized into five logical layers. Each layer depends only on layers below it.

```
┌─────────────────────────────────────────────┐
│                 Application                  │
│  src/ui/ — ui_core dispatcher + per-screen   │
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

### src/ui/ — UI layer

The UI is a registry-dispatched state machine. Each `screen_id_t` value maps to a row in the `screens[]` table (`ui_core.cpp`); the table holds function pointers for that screen's lifecycle hooks. The dispatcher in `ui_core.cpp` is the only place that knows about screen transitions; per-screen files only implement their own behavior.

**`ScreenHandlers` (`include/ui_core.hpp`)** — one row per screen:

| Field | Purpose |
|---|---|
| `name` | Debug-printable screen name (also surfaces via `getUIContextName`) |
| `implemented` | `false` → `changeScreenContext` logs critical and aborts the transition |
| `preservePriorUI` | `true` → on entry, hide buttons but don't destroy them (used by transient overlays like `SCREEN_SYSTEM_MESSAGE`) |
| `init()` | Build LGFX_Buttons. Called lazily, dedupe-guarded by `checkIfUIIsInitialized` |
| `onEnter()` | Pre-draw side effects on every transition into the screen — e.g. reset page, clear dropdown |
| `draw()` | Paint the screen |
| `handleTouch()` | Per-frame touch dispatch, called from `handleTouchUIUpdate` |
| `activeSubcontext()` | Optional. Returns the screen's currently active subcontext (open dropdown, sort mode, etc.). `cleanupUIOutOfContext` queries this to decide which buttons to hide. See "Subcontexts" below. |

Any hook may be `nullptr` to skip that phase.

**`changeScreenContext(target)` flow:**
1. If `target` is unimplemented → log and return.
2. Cleanup prior buttons. `cleanupUIOutOfContext(destroy)` is called with `destroy = false` if `preservePriorUI`, transitioning within the canvas family (`SCREEN_CANVAS` ↔ `SCREEN_CANVAS_MENU`), or re-entering the same screen; otherwise `destroy = true`.
3. `currentScreen = target`.
4. `init()` (skipped if already initialized) → `onEnter()` → `draw()`.

**Per-frame loop:** `handleTouchUIUpdate()` → `screens[currentScreen].handleTouch()`.

**Adding a new screen:** create `src/ui/ui_screen_<name>.cpp/.hpp` exposing whichever hooks the screen needs, `#include` the header from `ui_core.cpp`, and add one row to `screens[]`.

**Subcontexts** — for sub-states within a screen (open dropdowns, sort modes, confirm dialogs, etc.):

`UIButton::subcontext` is a screen-private `int`. **Convention:** `0` = always visible on this screen; any nonzero value means the button is only visible when the screen's `activeSubcontext()` returns the same value.

`cleanupUIOutOfContext` filters buttons using:

```cpp
int activeSub = screens[currentScreen].activeSubcontext
                    ? screens[currentScreen].activeSubcontext()
                    : 0;
// hide button if (button.screenContext != currentScreen)
//             || (button.subcontext != 0 && button.subcontext != activeSub)
```

`ui_core` never knows the type behind the int. Each screen module:

1. Defines its own enum (e.g. `dropdown_id_t` in `ui_screen_canvas_menu.hpp`) with the `_NONE = 0` value.
2. Holds the current value as a file-static variable (e.g. `static dropdown_id_t currentDropdown`).
3. Casts to `int` when assigning `UIButton::subcontext` and when implementing `activeSubcontext()`.
4. Registers `activeSubcontext` in its row of `screens[]`.

Cross-screen references are not allowed — a screen's subcontext type is private to that screen's module. New subcontext shapes (sort modes for SEND, confirm dialogs, etc.) ship as new enums in their owning screen's header without touching `ui_core`.

Worked example — `ui_screen_canvas_menu`:

```cpp
// ui_screen_canvas_menu.hpp
typedef enum { DROPDOWN_NONE = 0, DROPDOWN_MENU, DROPDOWN_TOOLS, DROPDOWN_SAVE, DROPDOWN_LOAD } dropdown_id_t;
int activeSubcontextScreenCanvasMenu();

// ui_screen_canvas_menu.cpp
static dropdown_id_t currentDropdown = DROPDOWN_NONE;
int activeSubcontextScreenCanvasMenu() { return (int)currentDropdown; }
// ...later in init...
SCREEN_CANVAS_MENU_TOOL_BUTTON[col].subcontext = (int)DROPDOWN_TOOLS;
```

**SDRAM slot model — overlay screens**

The LT7680 holds multiple full-screen frames in SDRAM and can scan out from any of them. Two slots are reserved for the UI layer (addresses defined in `include/display.hpp`):

| Slot | Address | Purpose |
|---|---|---|
| `LT7680_SLOT_CANVAS` | 0 | Canonical drawing surface. The user's sketch lives here untouched. |
| `LT7680_SLOT_UI` | `LT7680_FRAME_BYTES` | Overlay backing store. Snapshot target for screens that need to paint *on top of* the canvas without destroying it. |

`SCREEN_CANVAS_MENU` uses the slot pair to overlay seamlessly on `SCREEN_CANVAS`:

1. `onEnterScreenCanvasMenu` — `tft.blitFrames(SLOT_CANVAS → SLOT_UI)` (BTE-accelerated, no SPI per pixel), then aims both `setCanvasAddress` and `setMainImageAddress` at `SLOT_UI`.
2. `drawScreenCanvasMenu` paints buttons onto `SLOT_UI`, leaving `SLOT_CANVAS` untouched.
3. On exit (entering `SCREEN_CANVAS`), `onEnterScreenCanvas` calls `useCanvasSlot()` which switches both addresses back to `SLOT_CANVAS`. The overlay disappears in one register write — no fillRect, no redraw of the canvas.

Other screens (`SEND`, `FILE_BROWSER`) call `useCanvasSlot()` in their `onEnter` to defensively snap back to slot 0 when arriving from a slot-1 screen. Their `draw` functions repaint the entire screen, so any leftover content on the active slot is irrelevant — but keeping the address consistent avoids subtle bugs in future code that assumes "draws land on slot 0."

`SCREEN_SYSTEM_MESSAGE` intentionally leaves the slot pair alone (no `onEnter`) so transient overlays like `drawFriendboxLoadingScreen` paint over whatever is currently displayed. This means a loading screen triggered from `SCREEN_CANVAS` would scribble on `SLOT_CANVAS` — currently safe because no flow does that (loading screens are only triggered from CANVAS_MENU dropdowns and from SEND), but worth knowing if you add a new caller.

**Other UI primitives:**
- `UIButton`: position + `screenContext` + `subcontext` + fill color; wraps `LGFX_Button`. Action modes: `ACT_ON_PRESS`, `ACT_ON_HOVER_AND_RELEASE`, `ACT_ON_RELEASE`.
- `UIList`: paginated friend and file lists.

**Current modules:**
- `ui_core.cpp/hpp` — dispatcher, registry, button handling, cleanup, `drawScreenSend` / `drawScreenFileBrowser` / `drawSketchPreview` / `drawFriendboxLoadingScreen`. SEND and FILE_BROWSER hook implementations are still file-static here pending extraction.
- `ui_screen_canvas_menu.cpp/hpp` — extracted screen module: dropdown layout constants, action/color/menu/tool/save/load buttons, dropdown render logic.

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
    → ui_core.cpp::handleTouchUIUpdate reads touchX/Y/Z
      → screens[currentScreen].handleTouch() dispatch
        → per-screen UIButton hit-test
          → action callback (draw, menu open, file load, etc.)
            → tft.drawPixel / tft.fillRect / SDRAM slot restore
```
