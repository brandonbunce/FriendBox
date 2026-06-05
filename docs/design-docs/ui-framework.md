# UI Framework

The `ui::` framework (`include/ui.hpp`, `src/ui/`) replaced the legacy
`UIButton` / `ScreenHandlers` monolith. This doc records *why* it is shaped the
way it is. For a how-to, the header doc-comments in `include/ui.hpp` are the
reference.

## Problem

The legacy UI was a registry-dispatched state machine with a clean dispatcher
but heavy per-screen boilerplate: every screen hand-positioned `UIButton`s with
hardcoded pixel math, re-themed and redrew each button by hand
(`setTextColor`/`setFillColor`/`drawButton` repeated per element), and dispatched
touch through large `switch(col)` blocks. ~190 lines to draw 10 buttons. There
were no sliders/checkboxes, no animation, no overlay/keyboard system, no SFX, and
the accent color was copy-pasted from `draw_color_palette[currentDrawColorIndex]`
everywhere.

## Decisions

### Imperative builder API over a *retained* store
A screen is authored with a few builder calls that run once on entry:

```cpp
static void buildSend() {
    ui::beginScreen();
    ui::beginColumn(16, 12);
        ui::label("Send to");
        ui::list(s_friends, &s_sendPage, 4, sendPickFriend).sfx(ui::SFX_CONFIRM);
        ui::beginRow(10);
            ui::button("Canvas", sendToCanvasMenu).sfx(ui::SFX_CLOSE).size(150, 48);
            ui::button("Refresh", sendRefresh);
        ui::endRow();
    ui::endColumn();
    ui::endScreen();
}
```

The builders populate a **retained** widget store; the framework owns layout,
theming, draw, touch hit-test, and animation from there.

- **Alternative rejected — immediate mode** (rebuild + repaint every frame). Fatal
  on this panel: a full 480×480 SPI burst is ~30–46 ms, so per-frame full repaints
  are impossible. Retained mode lets idle screens issue **zero** redraws; only the
  widgets that change repaint.

### Builder helpers, not a declarative tree
Authoring is imperative (`ui::button(...)`), returning a `ui::Ref` handle that
chains modifiers (`.sfx().color().size().sub().anim().value()`). Chosen over a
nested declarative tree for familiarity and to match the codebase's C-ish style.

### Deferred layout
Builders don't place widgets immediately — they append the widget plus an entry
to an ordered **op list**. `endScreen()` replays the ops through a container
stack (`Column`/`Row`/`Grid`) and resolves every rect. This is what makes chained
modifiers like `.size()` work: the modifier mutates the widget *before* layout runs.

### Store lives in PSRAM, not BSS
`g_widgets[96]` + the op list are allocated in PSRAM (`heap_caps_calloc(...,
MALLOC_CAP_SPIRAM)`) on first use, not as static BSS. Internal SRAM is scarce —
the fbox decoder needs a contiguous 115 KB internal `frame_4bpp` buffer, and ~9 KB
of UI BSS was enough to push that allocation into OOM. UI access is not hot, so
PSRAM is the right home. (See the "frame_4bpp lives in internal SRAM" invariant in
ARCHITECTURE.md — the two allocations compete.)

### Slot model — three routing modes
The LT7680 scans out from any SDRAM slot, so menus draw to `SLOT_UI` and leave the
sketch in `SLOT_CANVAS` untouched; closing a menu is one address flip.
`ui::Screen.slotMode`:

| Mode | Behavior |
|---|---|
| `SLOT_DIRECT` | Draw straight onto `SLOT_CANVAS` (the live sketch). Used by `SCREEN_CANVAS`. Never cleared on entry. |
| `SLOT_OVERLAY_CLEAN` | Draw a fresh menu onto `SLOT_UI` over a background fill; `SLOT_CANVAS` is untouched so the sketch restores on exit. Used by `SEND`, `FILE_BROWSER`, `CANVAS_MENU`. |
| `SLOT_OVERLAY_SNAPSHOT` | Copy the sketch into `SLOT_UI`, then draw over it (menu composited on top of the live sketch). |

### Frame tick
`ui::tick()` (called from `uiLoopTask`) runs the current screen's optional
`customTick` (e.g. `handleCanvasDraw` for the widgetless paint screen), dispatches
touch to the retained widgets honoring `subcontext` visibility, advances
animations, and repaints only changed widgets. A callback that calls
`changeScreenContext` rebuilds the store, so the dispatch loop bails out on a
screen change mid-iteration.

### Theming
`src/ui/ui_theme.cpp` is the single source of truth: the accent is the existing
`currentDrawColorIndex` into the 16-color palette; `ui::accentColor()` /
`onAccentColor()` derive fill/text. `ui::setAccent(i)` re-themes and repaints the
live screen. Deletes the scattered `draw_color_palette[currentDrawColorIndex]`
copy-paste.

### SFX
`src/ui/ui_sfx.cpp` synthesizes short blips procedurally at boot (square/sine +
envelope into small PSRAM buffers — no SD assets) and plays them through a **lazy,
persistent** UI I2S session. fbox playback owns the I2S channel exclusively
(`startI2SStreaming` refuses a second install), so playback calls
`ui::suspendSfxSession()` before grabbing I2S; the next `playSfx()` lazily
restarts the UI session.

### Animations — GPU-driven enter reveals
`src/ui/ui_anim.cpp` composes the new screen into `SLOT_ANIM` (scratch), then
reveals it onto the shown slot with a short stepped BTE sequence:
- **slide / bounce** → `blitFrames` at an eased offset (bounce = ease-out-back).
- **fade** → `blitFramesAlpha` (LT7680 BTE *Memory Copy with Opacity*, Picture
  Mode, alpha level in REG[B5h]). NB: the canvas is 8bpp **RGB332**, so alpha is
  blended in that space; if a fade looks wrong on hardware the fallback is a
  dither-mask stepped reveal. Continuous per-widget tweens are stubbed for a later
  phase (idle = zero redraws).

## GPU / driver additions (`lib/Panel_PCBA5981`)
The BTE/GDE was underused (`blitFrames` only did plain memory-copy). Added:
- **`blitFramesAlpha`** — BTE opacity op (the fade primitive).
- **`cgramSetStart` / `cgramWrite` / `drawChar`** — the user-defined-character
  (UCG) engine: glyphs in CGRAM rendered by the hardware text engine, recolorable
  via FG/BG. See "Glyphs + boot splash" below.

Still on the table (not yet built): chroma-key BTE for overlay/keyboard
compositing; geometry-engine lines/triangles for widget chrome.

## Glyphs + boot splash (`lt_assets.*`, `lt_glyph_data.cpp`, `lt_splash_data.cpp`)
Symbols instead of software text, and a defined startup display:
- **Glyphs** are 32×32 "full-width" UCG dot-matrices (128 B each; codes ≥0x8000,
  CGRAM index = `code & 0x7FFF`), embedded in firmware (`tools/make_glyphs.py` →
  `src/lt_glyph_data.cpp`), programmed into the LT7680's external serial flash once
  (marker-guarded `ltAssetsInit()`), then loaded into CGRAM and rendered with
  `drawChar`/`ui::glyphCentered`. Native size 32×32; `enlarge` 1–4 scales to 128×128.
- **Boot splash** is host-driven: shown immediately after panel init. The LT7680
  §10.1 Power-on Display MCU was rejected — it can only write LT7680 registers and
  cannot bring up the ESP-bit-banged ST7701S panel. Default is a GPU wordmark;
  `tools/png_to_fbox8.py` embeds a real RGB332 bitmap DMA'd from flash.

> **CGRAM write gotcha (learned the hard way):** the glyph blob is written to
> CGRAM **one 64-byte row at a time** (`Panel_PCBA5981::_cgram_write_row`). A
> single multi-row block write at a relocated canvas (`CVSSA = CGRAM_ADDR`,
> width 64) silently corrupts — a 1-row (64-byte) write round-trips perfectly,
> but a 12-row write does not. Per-row writes (each a proven 64×1 block write)
> are reliable. The linear-mode memory port was also tried first and did **not**
> honor the linear address for the `[0x80]` stream — avoid it for CGRAM.

## Consequences
- New screens are a single `build()` of builder calls — no pixel math, no touch
  `switch` blocks, no manual theming.
- The widget set (Button/Label/Slider/Checkbox/List) and layout containers are the
  foundation; overlay/system-message, keyboard, and a video-player overlay are
  designed-for but deferred.
- The slot/anim model assumes `SLOT_ANIM` is free during UI navigation (true — it's
  only used by fbox playback, which isn't running in menus).

## Files
| File | Owns |
|---|---|
| `include/ui.hpp` | Public builder API, `Widget`/`Ref`/`Screen`, enums |
| `src/ui/ui.cpp` | Store, deferred layout, dispatch, `tick()`, touch, salvaged `drawFriendboxLoadingScreen`/`sketchPreview` |
| `src/ui/ui_widgets.cpp` | Per-type draw + hit-test |
| `src/ui/ui_anim.cpp` | Enter-reveal animations |
| `src/ui/ui_theme.cpp` | Accent theming, `glyphCentered` |
| `src/ui/ui_sfx.cpp` | Procedural SFX + UI I2S session |
| `src/ui/ui_screens.cpp` | The FriendBox screens (CANVAS, CANVAS_MENU, SEND, FILE_BROWSER, SYSTEM_MESSAGE) |
| `include/lt_assets.hpp`, `src/lt_assets.cpp` | Flash asset map, one-shot writer, CGRAM loader, splash, glyph render |
