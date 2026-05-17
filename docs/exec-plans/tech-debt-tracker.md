# Tech Debt Tracker

Known structural problems that should be resolved before significant feature work. Ordered by severity.

---

## ~~[MEDIUM] ui_core.cpp screen extraction~~ — DONE

`SCREEN_SEND` → `src/ui/ui_screen_send.cpp/.hpp` ✓  
`SCREEN_FILE_BROWSER` → `src/ui/ui_screen_file_browser.cpp/.hpp` ✓  
`ui_core.cpp` now holds only the dispatcher, registry, `UIButton` plumbing, `cleanupUIOutOfContext`, `drawSketchPreview`, and `drawFriendboxLoadingScreen`. No further extraction needed unless `drawSketchPreview` / `drawFriendboxLoadingScreen` grow substantially.

---

## ~~[HIGH] SD bandwidth in SPI mode caps complex animations below 24 fps — plan to migrate to SD_MMC 4-bit~~ — **DONE**

Migrated to SDIO 4-bit at 40 MHz. Pin assignments (final) in [include/io.hpp](../../include/io.hpp): `CLK=13, CMD=2, DAT0=48, DAT1=47, DAT2=1, DAT3=14`. **CMD and DAT2 were moved off GPIO 35 and 36 because those are reserved for the octal PSRAM data lines on N16R8 modules (SPIIO6/SPIIO7)** — any SDIO peripheral driving 35/36 corrupts PSRAM and silently crashes the firmware. See [docs/HARDWARE.md](../HARDWARE.md) for the full pin reservation list and [[project-octal-psram-pin-reservation]].

Result (162-frame dithered test, post-migration):

| Metric | SD over SPI | SDIO 4-bit | Change |
|---|---:|---:|---|
| `refill_us/call` (4 KB) | ~2977 µs | **849 µs** | 3.5× faster |
| `refill_avg/frame` | ~90 ms | **26 ms** | 3.5× faster |
| `decode_avg` | ~114 ms | **51 ms** | 2.2× faster |
| Dithered fps | 8.7 | **19.1** | 2.2× faster |

Static / mostly-static content (the original 493-frame test) already hit 24 fps on SPI and continues to. Worst-case dithered is now within ~5 fps of the 24 fps target; the remaining gap is producer-side CPU + sequential SD reads.

**Open work to close the last 5 fps on dithered content** (deferred, lower priority since the wall is no longer hardware):

- Decoder CPU is now the binding stage at ~25 ms (the other ~26 ms of producer time is sequential SD reads). Vectorising the literal path (Xtensa SIMD intrinsics) and/or unrolling the per-byte loop should cut that further.
- `playFboxAnimationFromSDBuffered` is now roughly a wash with the direct path post-SDIO (18.7 vs 19.1 fps) — the ring's parallelism gain is small once SD is fast, and the loader-task overhead negates it. Keep the buffered helper available for HTTP source / mixed sources, but the SD playback default can move back to direct.
- One-line LGFX bump in [LGFX_ESP32_PCBA5981_GT911.hpp:35](../../include/LGFX_ESP32_PCBA5981_GT911.hpp#L35): `cfg.freq_write = 80000000` (user already applied this; SPI burst is now ~30 ms / frame = 33 fps panel-side ceiling).

**Files:** [src/io.cpp](../../src/io.cpp), [include/io.hpp](../../include/io.hpp), [docs/HARDWARE.md](../HARDWARE.md), [include/fbox_source.hpp](../../include/fbox_source.hpp), [src/fbox_source.cpp](../../src/fbox_source.cpp)

**Problem:** Fully dithered / literal-heavy 480×480 frames at 24 fps demand ~2.76 MB/s of compressed source data. SD over SPI on this board tops out at ~1.3 MB/s effective (40 MHz SPI, 4 KB chunks, FATFS + Arduino-ESP32 SD library overhead). Even with the `FboxSourceRingBuffered` async loader feeding a 2 MB PSRAM ring on core 0, the loader physically can't fill faster than SD delivers — so dithered playback stalls when the ring drains.

Current observed numbers on the worst-case dithered 162-frame test (after every CPU-side optimization in this branch):

| Stage | Value | Note |
|---|---:|---|
| `decode_avg` (from PSRAM ring) | ~39 ms/frame | CPU work alone, not the wall |
| `refill_us/call` (loader → SD) | ~3 ms/4 KB | Per-call SD+FATFS overhead is irreducible at this layer |
| Loader sustained throughput | ~1.3 MB/s | Hard wall; SD over SPI on this card |
| Required throughput @ 24 fps | ~2.76 MB/s | 115 KB/frame × 24 |
| Effective playback | ~9 fps | ring drains, async loader can't refill in time |

The companion ceiling: LT7680 SPI write at 40 MHz = 46 ms / 230 KB frame = 21 fps theoretical regardless of source. So even infinite SD bandwidth caps dithered playback at ~21 fps until LT7680 SPI is pushed higher.

**Tried and merged in this branch (none of these break the wall — they put us *at* it):**
- `FboxSourceRingBuffered` with PSRAM ring + async SD loader task ([include/fbox_source.hpp](../../include/fbox_source.hpp)).
- Token-driven decoder ([src/io.cpp](../../src/io.cpp) `decodeFrameTokens`).
- `frame_4bpp` in internal SRAM.
- `SD.begin(..., 40000000)` (3rd arg is bus clock, defaults to 4 MHz silently — see [[project-sd-begin-frequency]]).
- 4 KB `rle.buf` (was 256 B).
- Loader pinned to core 0 to avoid preempting the consumer on core 1.
- `vTaskDelay(1)` per chunk in the loader to keep IDLE0 alive.

**Planned fix (deferred): migrate SD from SPI to SD_MMC 4-bit mode.**

ESP32-S3 supports SDIO. SD_MMC at 40 MHz in 4-bit mode delivers ~10 MB/s effective in practice — comfortably above the ~2.76 MB/s dithered demand. Combined with bumping LT7680 SPI to 80 MHz this puts 24 fps within reach for *any* content complexity.

**Implementation outline when we get back to it:**
1. **Hardware audit:** confirm SD card slot wiring on PCBA5981 exposes CMD/CLK + D0/D1/D2/D3 (not just CS/MISO/MOSI/SCK). [docs/HARDWARE.md](../HARDWARE.md) currently documents an SPI-only pinout (CS=14, SCK=12, MISO=47, MOSI=48). A rework may be required; if D1–D3 aren't wired, 1-bit SD_MMC is still ~2× faster than SPI mode and may be enough.
2. **Driver swap:** replace Arduino `SD` (over `SPIClass`) with `SD_MMC` from `esp32-arduino`'s SD_MMC library, or call into ESP-IDF's `sdmmc_host_*` directly for finer control.
3. **`FboxSourceSD` refactor:** the public abstraction stays the same — only the underlying `File::readBytes` call changes. Likely a one-screen diff.
4. **Validation:** re-run the same dithered test. Expected: `refill_us/call` drops from ~3 ms to <1 ms, ring buffer doesn't drain, decode is the only meaningful cost, `wait_avg` returns to ~0, 24 fps achieved.
5. **Re-test LT7680 SPI at 80 MHz** in tandem (`LGFX_ESP32_PCBA5981_GT911.hpp:35` `cfg.freq_write`); without that, SPI write becomes the wall at 21 fps even with fast SD.

**Interim mitigation (no firmware change needed today):**
- Keep encoded animations small enough to comfortably preload to PSRAM (~6 MB cap with current playback PSRAM use).
- Treat the `fps` field in the FBOX header as a *ceiling*, not a guarantee — UI may communicate "complex animation, playing at best-effort rate."
- A `playFboxAnimationFromSDPreloaded(path)` helper would slot in cleanly (uses existing `FboxSourcePSRAM`); deferred along with the SDIO work since the right default behavior is "preload if size fits, else ring-buffered" and that decision is best made once we know the post-SDIO numbers.

**Related memories:** [[project-animation-pipeline]], [[project-sd-begin-frequency]], [[project-fbox-decoder-profiling]].

---

## [MEDIUM] Audio source handoff at video EOF

**Files:** [src/io.cpp](../../src/io.cpp), [src/audio.cpp](../../src/audio.cpp), [include/fbox_source.hpp](../../include/fbox_source.hpp)

**Problem:** The producer task's persistent `FboxRleReader` chunk-buffers up to 4 KB of look-ahead from the source. At video EOF, those buffered bytes are the first bytes of the audio section. `fboxDecodeAudio` reads from the source directly (bypassing the reader's buf), so those bytes are skipped — decoded PCM starts mid-block with wrong predictor/step_index state.

**Workaround in place:** Documented inline in `playFboxAnimation`; not currently user-visible because we have no I2S DAC hardware and audio decode is buffer-only.

**Desired state:** Introduce a shared `FboxSourceBuffered` wrapper that both `FboxRleReader` and `fboxDecodeAudio` consume bytes from, so the handoff is byte-exact. Land this with audio output hardware.

---

## [MEDIUM] LT7680 bottom-strip artifact (cause unknown)

**File:** [lib/Panel_PCBA5981/Panel_PCBA5981.cpp](../../lib/Panel_PCBA5981/Panel_PCBA5981.cpp)

**Problem:** Animation frames sometimes show a stale strip at the bottom of the panel. Reproduces at 40 MHz and at 80 MHz SPI, so it isn't the FIFO-drain story the legacy comments in `writeRawFrame8bpp` and `LGFX_ESP32_PCBA5981_GT911.hpp` claim. The previously-claimed "50 MHz max" datasheet ceiling does not have evidence behind it.

**Desired state:** Root-cause investigation. Candidates worth checking: MISA-vs-VSync write ordering (animation path writes MISA before `waitVSync`; flash-DMA path writes after — the second order is what the existing project memory describes as correct), SDRAM refresh setting, or a panel timing edge case.

---

## [HIGH] Friend-to-friend send/receive not implemented

**File:** [src/network.cpp](../../src/network.cpp)

**Problem:** `networkSendFramebuffer()` and `networkReceiveFramebuffer()` are stubs. The peer-to-peer API contract (sending a sketch to a specific friend, receiving a push notification) is undefined.

**Partial progress:** `networkSendCanvas()` (upload to server) is implemented. `networkDownloadFbox()` (download by sketch ID) is implemented and used by animation playback.

**Impact:** Core feature (sending drawings to a specific friend) is still blocked.

**Desired state:** Defined API contract in a product spec; implemented and tested send/receive flow with friend addressing.

---

## [MEDIUM] No hardware abstraction for GPIO

**Files:** [include/io.hpp](../../include/io.hpp), [src/io.cpp](../../src/io.cpp)

**Problem:** Pin assignments are `#define` constants scattered across headers. Two hardware revisions (PCBA5981 and ST7796S) differ in wiring but share some source files.

**Desired state:** Board-specific pin maps isolated to the LGFX config headers; source files reference logical names only.

---

## [LOW] Test directory is empty

**Directory:** [test/](../../test/)

**Problem:** No unit or integration tests exist. PlatformIO test runner is available but unused.

**Desired state:** At minimum, tests for color quantization, palette lookup, and file format encode/decode.
