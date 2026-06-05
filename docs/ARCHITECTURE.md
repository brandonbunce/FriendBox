# Architecture

FriendBox firmware is organized into five logical layers. Each layer depends only on layers below it.

```
┌─────────────────────────────────────────────┐
│                 Application                  │
│  src/ui/ — ui:: builder framework + screens  │
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
- SDRAM slot allocation (`include/display.hpp`): `SLOT_CANVAS` (live sketch), `SLOT_UI` (overlay/menus), `SLOT_ANIM`/`SLOT_ANIM_B` (playback + UI enter-anim scratch), `SLOT_MENU` (playback-menu cache), and `LT7680_CGRAM_ADDR` (UCG glyph storage)
- Resolution constant: `TFT_HOR_RES` / `TFT_VER_RES` = 480

### canvas.cpp / canvas.hpp
- Defines the 16-color palette (RGB565 values, sourced from androidarts.com)
- Rainbow index array and per-color text contrast values
- Tools: pencil, fill (flood), dither, palette picker
- Color quantization: nearest-neighbor lookup for off-palette input colors

### src/ui/ — UI layer (`ui::` framework)

A screen is authored with imperative builder calls that run once on entry and populate a **retained** widget store; the framework owns layout, theming, draw, touch hit-test, and animation from there. No hand pixel-math, no per-element `setColor`/`drawButton`, no `switch(col)` touch blocks. Full rationale in [design-docs/ui-framework.md](design-docs/ui-framework.md).

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
ui::registerScreen(SCREEN_SEND, { "SCREEN_SEND", buildSend, nullptr, false,
                                  SLOT_OVERLAY_CLEAN, ANIM_SLIDE_FROM_RIGHT });
```

**Core pieces:**
- **Retained store** — `ui::Widget g_widgets[96]` lives in **PSRAM** (not BSS: internal SRAM is reserved for the fbox decoder's 115 KB `frame_4bpp`). Builders return a `ui::Ref` whose chained modifiers (`.sfx().color().size().sub().anim().value()`) mutate the widget.
- **Deferred layout** — builders append to an ordered op list; `endScreen()` replays it through a `Column`/`Row`/`Grid` container stack to resolve every rect (so chained `.size()` works).
- **Dispatch** — `screen_id_t` and `changeScreenContext()` are kept (so `canvas.cpp`/`io.cpp` only swapped an include). Screens are `ui::Screen` records registered via `ui::registerScreen`; widgetless screens (e.g. `SCREEN_CANVAS`) use the optional `customTick` hook to run `handleCanvasDraw`.
- **Frame tick** — `ui::tick()` (from `uiLoopTask`) runs the custom tick, dispatches touch honoring `subcontext` visibility, advances animations, and repaints only changed widgets. **Idle = zero redraws.**
- **Theming** — `ui_theme.cpp` derives `accentColor()`/`onAccentColor()` from the existing `currentDrawColorIndex`; `ui::setAccent()` re-themes the live screen.
- **SFX** — `ui_sfx.cpp` synthesizes blips at boot and plays them through a lazy/persistent UI I2S session; fbox playback calls `ui::suspendSfxSession()` around its own I2S use.
- **Animation** — `ui_anim.cpp` composes the new screen into `SLOT_ANIM` (scratch) and reveals it onto the shown slot: slide/bounce via `blitFrames`, fade via `blitFramesAlpha` (BTE opacity).

**SDRAM slot model.** Menus draw to `SLOT_UI` and leave the sketch in `SLOT_CANVAS` untouched, so closing a menu is a single `setMainImageAddress` flip. `ui::Screen.slotMode`:

| Mode | Behavior | Used by |
|---|---|---|
| `SLOT_DIRECT` | Draw onto `SLOT_CANVAS`; never cleared on entry | `SCREEN_CANVAS` |
| `SLOT_OVERLAY_CLEAN` | Fresh menu on `SLOT_UI` over a bg fill; sketch preserved | `SEND`, `FILE_BROWSER`, `CANVAS_MENU` |
| `SLOT_OVERLAY_SNAPSHOT` | Copy sketch into `SLOT_UI`, draw over it | (available) |

Slot addresses are in `include/display.hpp` (`LT7680_SLOT_CANVAS/_UI/_ANIM/_ANIM_B/_MENU`, plus `LT7680_CGRAM_ADDR` for glyph storage).

**Subcontexts** — a widget's `subcontext` int gates visibility: `0` = always visible; nonzero means visible only when `ui::activeSubcontext()` matches. `ui::setSubcontext(n)` opens/closes a sub-state (dropdowns, confirm states) and repaints.

**Glyphs + boot splash** — `lt_assets.*` programs embedded UCG glyphs into LT7680 flash, loads them into CGRAM (written one 64-byte row at a time — see the design doc's "CGRAM write gotcha"), and renders them with the hardware character engine (`drawChar` / `ui::glyphCentered`); a host-driven splash shows at boot. See [design-docs/ui-framework.md](design-docs/ui-framework.md).

**Modules:**

| Module | Owns |
|---|---|
| `include/ui.hpp`, `ui.cpp` | Public API, store, deferred layout, dispatch, `tick()`, touch, salvaged `drawFriendboxLoadingScreen`/`sketchPreview` |
| `ui_widgets.cpp` | Per-type draw + hit-test (button/label/slider/checkbox/list) |
| `ui_anim.cpp`, `ui_theme.cpp`, `ui_sfx.cpp` | Animation, accent theming, procedural SFX |
| `ui_screens.cpp` | The FriendBox screens + `registerFriendboxScreens()` |
| `lt_assets.*`, `lt_glyph_data.cpp`, `lt_splash_data.cpp` | Flash glyph/splash assets, CGRAM loader, glyph render |

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
                  │  fbox_dec task  │  ──slot──→   │  displayAnim*      │
                  │  RLE+XOR+CLUT   │              │  writeRawFrame8bpp │
                  │  (priority 2)   │              │  waitVSync, flip   │
                  └─────────────────┘              ├────────────────────┤
                          ↑                       │  fbox_ld task      │
                          │                       │  (priority 2)      │
  FboxSource ──→ FboxSourceCrc ──→ FboxSourceRingBuffered ──→ PSRAM ring
  (SD/HTTP/PSRAM)                       (loader runs here ──────────────┘
                                         on core 1, NOT core 0)
                          ↑                                  ↓
                          └───── sem_free  ←── 3-slot ring ──┘
                                 sem_ready (PSRAM, 32-byte aligned)
```

Core placement matters: **decoder on core 0, SPI consumer + ring loader on core 1**. Wi-Fi tasks live on core 0 (ESP-IDF default) and used to preempt the loader for ~120 ms at a time when the loader was also on core 0 — that was the cause of "ring stalls" that capped fps at ~23.4 even after every other optimization. With the loader on core 1, SDIO's interrupt-driven I/O lets it co-exist cleanly with the SPI consumer (consumer is mostly blocked on SPI DMA, so the loader gets CPU when it has bytes to push). Both `playFboxAnimationFromSDBuffered` on a fully-dithered file and the all-static animation hit **24+ fps** with margin in this layout.

Key invariants:
- **One persistent `FboxRleReader` across all frames.** Its 4 KB chunk buffer pre-reads next-frame bytes; a per-frame reader would discard those and shift every frame's start offset, scrambling output and forcing an early EOF. Use `rle.beginFrame(pixel_count)` between frames to reset token state while preserving the buffer.
- **Slot buffers are 32-byte aligned via `heap_caps_aligned_alloc(32, ..., MALLOC_CAP_SPIRAM)`.** Required so `esp_cache_msync(buf, n, ESP_CACHE_MSYNC_FLAG_DIR_C2M)` (called by the producer before signalling each slot ready) actually flushes the cache instead of failing the alignment check and silently no-op'ing. Without the writeback, core 1's DMA reads stale physical PSRAM.
- **Producer (`fbox_dec`) task pinned to core 0**, priority 2, 16 KB stack, `vTaskDelay(1)` per frame. Without the yield, IDLE0 never runs and the task watchdog trips after 5 s of decoding.
- **Loader (`fbox_ld`) task pinned to core 1**, priority 2, 8 KB stack. Decisively: not core 0 — see the pipeline diagram note above. Under SDIO this avoids both decoder contention on core 0 and Wi-Fi-task preemption.
- **Consumer pacing via `vTaskDelayUntil`.** Holds an absolute wake time across iterations; the older `do { handleTouch; vTaskDelay(2) } while (millis() < until)` form overshot the deadline by up to one tick per frame.
- **`frame_4bpp` lives in internal SRAM, not PSRAM.** Allocated via `heap_caps_calloc(..., MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)`. CPU-only buffer (no DMA) hit ~3× per pixel in the decode hot path; internal SRAM ~1–3 cycles per access vs PSRAM ~10–30. 115,200 bytes.
- **`FboxSourceSD` uses FATFS direct (`f_open`/`f_read`)**, skipping VFS + POSIX. Combined with the ring buffer (which is what the playback default uses) this trims ~150–250 µs of per-call overhead from the loader. The direct-SD path is slower this way (FATFS's `f_read` has less read-ahead than Arduino `fs::File`), so the convenience helper `playFboxAnimationFromSDBuffered` is the right entry point for animation playback.
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

**Current performance.** Both mostly-static and fully-dithered 480×480 content play at **24+ fps with margin**. The remaining wall is the LT7680 SPI burst (~30 ms/frame at 80 MHz = 33 fps theoretical ceiling). Full perf history and the full sequence of changes that got us here in [docs/exec-plans/tech-debt-tracker.md](exec-plans/tech-debt-tracker.md).

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
GT911 polled in uiLoopTask (handleTouch → touchX/Y/Z globals)
  → ui::tick()
    → screen customTick (e.g. handleCanvasDraw on SCREEN_CANVAS)
    → touch dispatch over the retained widget store (subcontext-filtered hit-test)
      → widget callback (changeScreenContext, file load, setDrawColor, slider value, …)
        → playSfx(widget.sfx) + repaint only the changed widget
    → advance enter-animation (BTE reveal from SLOT_ANIM), if any
```
