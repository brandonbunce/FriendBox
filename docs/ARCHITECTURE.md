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

| Module | Owns |
|---|---|
| `ui_core.cpp/hpp` | Dispatcher, registry, `UIButton` plumbing, `cleanupUIOutOfContext`, `drawSketchPreview`, `drawFriendboxLoadingScreen`, `useCanvasSlot` |
| `ui_screen_canvas_menu.cpp/hpp` | Dropdown layout, action/color/tool/save/load buttons, dropdown render |
| `ui_screen_send.cpp/hpp` | Address book buttons, friend list pagination, send trigger |
| `ui_screen_file_browser.cpp/hpp` | File list buttons, pagination, file selection → `loadSketchFromSD` |

### io.cpp / io.hpp

**SD hardware:** SDIO 4-bit @ 40 MHz via the `SD_MMC` library. Pin assignments in [include/io.hpp](../include/io.hpp): `CLK=13, CMD=2, DAT0=48, DAT1=47, DAT2=1, DAT3=14`. `initSD()` calls `SD_MMC.setPins(...)` then `SD_MMC.begin("/sd", false, false, SDMMC_FREQ_HIGHSPEED, 5)`. Effective throughput ~4.7 MB/s vs ~1.3 MB/s on the previous SPI path — sufficient to play fully-dithered 480×480 content at ~19 fps where SPI mode was stuck at ~9 fps. **Do not put SDIO (or any peripheral) on GPIO 26–37 on N16R8 modules — those are the internal flash/octal-PSRAM bus** and any reassignment silently corrupts PSRAM. See [HARDWARE.md](HARDWARE.md#reserved-pins-on-n16r8-modules--do-not-reuse).

**NVS:** namespace `"Friendbox"`, stores small user preferences (last slot, etc.).

**Hall effect sensor:** GPIO 15, 50 ms debounce, opens the canvas menu.

**FBOX codec** — the canonical binary format for all sketches and animations. See [design-docs/fbox-codec.md](design-docs/fbox-codec.md) for the format spec. Playback is decoupled from the byte source via the `FboxSource` interface (see `fbox_source.hpp`/`.cpp`):

| Source impl | Use |
|---|---|
| `FboxSourceSD` | Read from a path on SD card. Sequential, no `seek()`. |
| `FboxSourcePSRAM` | Read from an in-PSRAM buffer (entire file pre-loaded). |
| `FboxSourceHTTP` | Stream from `HTTPClient::getStreamPtr()`, with read timeout. |
| `FboxSourceRingBuffered` | Wraps any source with an async loader task (core 0, priority 2) feeding a PSRAM-backed `xStreamBuffer`. Reads on the playback path hit PSRAM speed; the loader runs in parallel with the decoder and SPI burst. Read blocks if the ring drains (visible as a momentary playback pause). Loader yields `vTaskDelay(1)` per chunk so IDLE0 stays alive. Use this whenever SD throughput is below content demand. |
| `FboxSourceCrc` | Wraps any source; folds every byte into a running `esp_rom_crc32_le` accumulator. The playback core composes this on top of the caller's source. |

The public API in `io.hpp`:

| Function | Purpose |
|---|---|
| `fboxReadHeader(FboxSource &src, FboxHeader &out, uint8_t *raw_out)` | Parse 128-byte header, validate magic and version. `raw_out` captures the bytes so the caller can compute the CRC seed over [62..127]. |
| `loadSketchFromSD(const char *path)` | Open .fbox, decode frame 0, blit to LT7680 canvas slot. |
| `playFboxAnimation(FboxSource &src)` | Decode and display all frames at the file's FPS. Returns a `PlaybackResult` so callers can fall back across sources. |
| `playFboxAnimationFromSD(const char *path)` | Convenience wrapper that constructs `FboxSourceSD`. Direct SD playback — fast for low-bitrate (mostly-static) content, slow for dithered content. |
| `playFboxAnimationFromSDBuffered(const char *path, uint32_t ring_bytes)` | Wraps SD source with `FboxSourceRingBuffered`. Async loader prefetches into a PSRAM ring (default 2 MB) so reads on the playback path hit PSRAM speed. Recommended whenever PSRAM headroom allows. Emits `[ANIM] ring stalls=N stall_time=Tms` so callers can see when the ring drained. |

`PlaybackResult` enum: `OK`, `READ_UNDERRUN`, `DECODE_ERROR`, `CRC_MISMATCH`, `USER_CANCELLED`, `OOM`. Intended use: UI tries `FboxSourceHTTP` first; on `READ_UNDERRUN` it caches the file to SD and replays via `playFboxAnimationFromSD()`.

**Animation playback pipeline** (`playFboxAnimation` in `io.cpp`):

```
                       core 0                          core 1 (app-main)
                  ┌─────────────────┐              ┌────────────────────┐
  FboxSource ──→  │  fbox_dec task  │  ──slot──→   │  displayAnim*      │
  (SD/HTTP/PSRAM) │  RLE+XOR+CLUT    │              │  writeRawFrame8bpp │
   wrapped by     │  one frame/slot │              │  waitVSync, flip   │
   FboxSourceCrc  └─────────────────┘              └────────────────────┘
                          ↑                                  ↓
                          └───── sem_free  ←── 3-slot ring ──┘
                                 sem_ready (PSRAM, 32-byte aligned)
```

Key invariants:
- **One persistent `FboxRleReader` across all frames.** Its 4 KB chunk buffer pre-reads next-frame bytes; a per-frame reader would discard those and shift every frame's start offset, scrambling output and forcing an early EOF. Use `rle.beginFrame(pixel_count)` between frames to reset token state while preserving the buffer.
- **Slot buffers are 32-byte aligned via `heap_caps_aligned_alloc(32, ..., MALLOC_CAP_SPIRAM)`.** Required so `esp_cache_msync(buf, n, ESP_CACHE_MSYNC_FLAG_DIR_C2M)` (called by the producer before signalling each slot ready) actually flushes the cache instead of failing the alignment check and silently no-op'ing. Without the writeback, core 1's DMA reads stale physical PSRAM.
- **Producer task yields one tick per frame (`vTaskDelay(1)`).** Pinned to core 0 at priority 2, 16 KB stack (≈ 8 KB of which is the reader's chunk buffer + locals). Otherwise CPU-bound; without the yield, IDLE0 never runs and the task watchdog trips after 5 s of decoding when SPI is the bottleneck.
- **`frame_4bpp` lives in internal SRAM, not PSRAM.** Allocated via `heap_caps_calloc(..., MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)`. CPU-only buffer (no DMA) hit ~3× per pixel in the decode hot path; internal SRAM ~1–3 cycles per access vs PSRAM ~10–30. 115,200 bytes.
- **CRC32** is accumulated by `FboxSourceCrc` over header [62..127] + frame table + frame data + audio. Verified at end of playback when `hdr.crc32 != 0`.

**Token-driven decoder.** The per-pixel decode loop is in `decodeFrameTokens()` in `io.cpp`. It dispatches on RLE token type instead of iterating pixel-by-pixel:
- **I-frame RUN** → `memset` on both `fb` and `f4`.
- **P-frame RUN with raw=0** ("no change" — dominates motion-sparse animation deltas) → rebuild `fb` from existing `f4` nibbles via `clut_pair[]`; `f4` untouched.
- **P-frame RUN with raw≠0** → XOR a packed `pair_xor` byte into `f4`, write `fb` via `clut_pair[]`.
- **LITERAL** → pull bytes directly off `rle.buf + buf_pos` in tight inner loops (one `is_pf`-hoisted variant for I-frames, one for P), 2 pixels per source byte with no nibble masking. One aligned 16-bit `fb` store per pair.

`clut_pair[256]` is a precomputed lookup keyed by source byte → `(clut_lut[hi_nib]) | (clut_lut[lo_nib] << 8)` little-endian. Lets the decoder write `fb` in 16-bit increments matching the natural source-byte stride.

**Profiling output.** The summary lines at end of `playFboxAnimation` expose:
```
[ANIM] done: drawn=N/M elapsed=Tms fps=F spi_avg=Sms wait_avg=Wms
[ANIM] producer: decode_avg=Dms msync_avg=Mus wait_free_avg=Wus refill_avg=Rus (n=N)
[ANIM] producer: refills/frame=K.K refill_us/call=C
[ANIM] ring stalls=N stall_time=Tms        ← only with playFboxAnimationFromSDBuffered
```
- `spi_avg` = consumer-side SPI burst + vsync + flip per frame.
- `wait_avg` = consumer time blocked on the producer (large → decode is the bottleneck).
- `decode_avg` = producer-side total (decode + source I/O).
- `refill_avg` = portion of `decode_avg` spent in `src->read()`. Large → SD/HTTP is the wall; small → CPU work in the decoder is the wall. When the ring-buffered source is in use, this measures *PSRAM ring* reads (sub-ms) — for the underlying SD bandwidth, look at `stall_time` instead.
- `refill_us/call` = per-`File::readBytes` cost; sanity-check against SD bus speed.
- `stalls` / `stall_time` = receive-side waits >1 ms inside `FboxSourceRingBuffered`. Non-zero means the ring drained at least once because SD couldn't keep up with content demand.

**Current performance ceiling and the SD wall.** Mostly-static content plays at 24 fps cleanly (SPI is the wall, ~30 ms burst). Fully dithered content stalls because SD over SPI delivers ~1.3 MB/s while dithered demand is ~2.76 MB/s. This is the binding constraint, documented in detail in [docs/exec-plans/tech-debt-tracker.md](exec-plans/tech-debt-tracker.md) — the planned fix is migrating from SD-over-SPI to SD_MMC 4-bit mode (after a hardware audit of the SD slot wiring). Don't expect arbitrary-complexity 24 fps until that lands.

**Audio:** IMA ADPCM decode lives in `audio.cpp` / `audio.hpp`. `fboxDecodeAudio` reads the trailing audio section from the source and produces an in-PSRAM int16 PCM buffer (`FboxAudio`). Capped at `FBOX_AUDIO_PSRAM_CAP` = 1 MB (~23 s at 22050 Hz mono). No I2S output yet — speaker hardware is planned. Known handoff bug: bytes still buffered inside the producer's `FboxRleReader` at video EOF are the first bytes of the audio section, so `fboxDecodeAudio` reading the source directly skips them and the decoded PCM starts mid-block. Documented in the `playFboxAnimation` source. Fix when audio output lands: route audio reads through a shared buffered source.

**File layout on SD:**

```
/sketches/saved/     — user-saved sketches (.fbox files)
/sketches/received/  — sketches downloaded from the server
```

### network.cpp / network.hpp

WiFi connection with hostname `"friendbox"`.

| Function | Status | Notes |
|---|---|---|
| `networkGetFriends()` | Implemented | HTTP GET `/get/friends` → ArduinoJson → `std::vector<std::string>` |
| `networkSendCanvas()` | Implemented | Reads LT7680 SDRAM line-by-line, packs to 4-bit, POST to `/sketches/upload` |
| `networkDownloadFbox(sketch_id, dest_path)` | Implemented | HTTPS GET from `friendbox.chocolatedonut.dev/api/download/sketch/{id}`, streams body to SD file via 512-byte chunks; yields watchdog each iteration |
| `networkSendFramebuffer()` | Stub | Not yet implemented |
| `networkReceiveFramebuffer()` | Stub | Not yet implemented |

`networkDownloadFbox` uses `WiFiClientSecure` with `setInsecure()` — certificate validation is skipped because this targets the project's own server. A cached file on SD is not re-downloaded.

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
