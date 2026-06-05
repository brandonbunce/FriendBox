# Tech Debt Tracker

Known structural problems that should be resolved before significant feature work. Ordered by severity.

---

## ~~[MEDIUM] ui_core.cpp screen extraction~~ — SUPERSEDED by the `ui::` rewrite

The legacy `ui_core` / `UIButton` / `ScreenHandlers` monolith was fully replaced by the `ui::` builder framework (retained store, deferred layout, declarative screen records). `ui_core.cpp/hpp` and the `ui_screen_*` files were deleted. See [design-docs/ui-framework.md](../design-docs/ui-framework.md).

---

## ~~[HIGH] SD bandwidth in SPI mode caps complex animations below 24 fps — plan to migrate to SD_MMC 4-bit~~ — **DONE — 24 fps achieved on worst-case dithered content**

Final state: **24.4 fps on the 162-frame fully-dithered test file**, with Wi-Fi connected and active throughout. The bottleneck is now panel SPI (~30 ms/frame at 80 MHz) and we hit 24 fps consistently with margin.

**Final configuration:**

- **SDIO 4-bit at 40 MHz** via `SD_MMC` (`SDMMC_FREQ_HIGHSPEED`). Pin assignments in [include/io.hpp](../../include/io.hpp): `CLK=13, CMD=2, DAT0=48, DAT1=47, DAT2=1, DAT3=14`. **CMD and DAT2 are deliberately off GPIO 35/36** because those are the octal PSRAM data lines (SPIIO6/SPIIO7) on N16R8 modules — any SDIO peripheral driving 35/36 corrupts PSRAM and silently resets the chip. See [docs/HARDWARE.md](../HARDWARE.md) for the full pin reservation list and [[project-octal-psram-pin-reservation]].
- **`FboxSourceSD` uses FATFS direct (`f_open`/`f_read`)** rather than the Arduino `fs::File` wrapper. Skips VFS + POSIX overhead in the buffered playback path (the direct-SD path is slower this way, but buffered is the only path the UI uses for animation playback).
- **`FboxSourceRingBuffered` loader pinned to core 1**, not core 0. This is the single decisive change that crossed 24 fps. Under SD-over-SPI (polling-mode driver) this previously inflated the consumer cycle by ~66 ms/frame; under SDIO (interrupt-driven) the loader spends most of each refill blocked on DMA so it doesn't fight the SPI consumer for core 1 cycles. Wi-Fi tasks live on core 0 and no longer preempt the loader.
- **Decoder pinned to core 0**, priority 2, `vTaskDelay(1)` per frame, `frame_4bpp` in internal SRAM.
- **Consumer pacing via `vTaskDelayUntil`**, absolute wake target → no per-frame overshoot.
- **LT7680 SPI at 80 MHz** ([include/LGFX_ESP32_PCBA5981_GT911.hpp:35](../../include/LGFX_ESP32_PCBA5981_GT911.hpp#L35)) → 23 ms SPI burst + ~6 ms vsync/MISA = ~30 ms `spi_avg`.

**Final numbers on the worst-case dithered 162-frame test:**

| Metric | SD-over-SPI (start) | Final | Total change |
|---|---:|---:|---|
| `refill_us/call` (4 KB) | ~9612 µs (4 MHz SD default!) | **413 µs** (PSRAM ring read) | 23× |
| `refill_avg/frame` | ~273 ms | **12.6 ms** | 21× |
| `decode_avg` | ~343 ms | **39 ms** | 8.8× |
| Dithered fps | **3.4** | **24.4** | 7.2× |

The journey, frame-by-frame:

| Step | Change | Dithered fps |
|---|---|---:|
| 0 | Baseline (SD over SPI @ default 4 MHz, per-pixel decode) | 3.4 |
| 1 | `SD.begin(..., 40000000)` (3rd arg is bus clock) | 8.7 |
| 2 | Token-driven decoder (`decodeFrameTokens`) + clut_pair table + 4-pixel/32-bit unroll | 9.2 |
| 3 | `frame_4bpp` moved to internal SRAM | 9.2 |
| 4 | Larger `rle.buf` (256 B → 4 KB) | 8.7* |
| 5 | `FboxSourceRingBuffered` (async PSRAM ring, loader on core 0) | 19.1 |
| 6 | **SDIO 4-bit migration** (largest single jump after pin-conflict fix) | 19.1 |
| 7 | 80 MHz LT7680 SPI | (already applied, lifted panel ceiling) |
| 8 | FATFS direct (`f_open`/`f_read`, skip VFS) | 23.3 |
| 9 | `vTaskDelayUntil` consumer pacing | 23.4 |
| 10 | **Loader pinned to core 1** | **24.4** |

*Step 4 was a transient regression before the ring buffer landed; with the ring it became the right tuning.

**Things tried that didn't pan out (kept for future reference):**

- 16 KB loader chunks — widened the ring-empty window per cycle; net regression.
- 8-pixel/32-bit decoder unroll — within measurement noise; reverted for readability.
- Pausing Wi-Fi during playback — not needed once loader was on core 1.

The buffered path (`playFboxAnimationFromSDBuffered`) is the canonical SD playback API. Direct-SD (`playFboxAnimationFromSD`) is intentionally slower since FATFS direct's `f_read` is unbuffered — keep using it only for the one-shot sketch loader (`loadSketchFromSD`) where per-frame perf doesn't matter.

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

## [LOW] ST7701S init bus cannot be shared with the LT7680 SPI bus

**Files:** [include/LGFX_ESP32_PCBA5981_GT911.hpp](../../include/LGFX_ESP32_PCBA5981_GT911.hpp), [lib/Panel_PCBA5981/Panel_PCBA5981.cpp](../../lib/Panel_PCBA5981/Panel_PCBA5981.cpp)

**Context:** The ST7701S panel config is bit-banged on dedicated GPIOs 9/11/10 (CS/CLK/DIN). The LT7680 host SPI bus uses GPIOs 4/7/6/5 (CS/SCLK/MOSI/MISO). These are two physically separate buses on the BuyDisplay ER-TFT040-3 FFC, routed to different ESP32 pads on the PCBA5981. Sharing CLK/DIN between the two chips would free GPIOs 11 and 10 for other use.

**What was tried:**

1. Hardware rework: cut the original GPIO 11 → LCD_CLK and GPIO 10 → LCD_DIN traces and patched so that GPIO 7 drives both the LT7680 SCL pad and the ST7701S CLK pad (and GPIO 6 drives both LT7680 SDI and ST7701S DIN). GPIO 9 stays as the dedicated ST7701S CS, GPIO 4 stays as the dedicated LT7680 CS.
2. Firmware refactor in `Panel_PCBA5981::init`: inlined `Panel_Device::init` so the ST7701S bit-bang happens *before* `_bus->init()` claims the SPI peripheral on pins 7/6. This guarantees the bit-bang sees pristine GPIO. Both LT7680 CS and ST7701S CS are correctly gated so the chips deselect each other during their respective transfers. This refactor is still in the tree — it's a clean change that works in both pin configs.

**Result:** Panel stays black with shared pins. Boot log diagnostic:

```
Shared pins:                          Dedicated pins:
[PANEL] Initial STSR=0xC1             [PANEL] Initial STSR=0x55
[PANEL] Normal-op OK STSR=0xA0        [PANEL] Normal-op OK STSR=0x55
[PANEL] _wait_sdram_ready TIMEOUT     [PANEL] SDRAM ready (status=0x55, 0ms)
```

`0x55` is the LT7680's expected normal-op value; `0xC1`/`0xA0` are corrupted but non-random reads — almost certainly the LT7680 receiving a garbled register-address on the shared SCLK/MOSI lines and returning the contents of a *different* register over the (still clean, dedicated) SDO pin. Dropping `freq_write`/`freq_read` to 10 MHz did not recover (1 MHz not tested). The most likely root cause is signal integrity on the now-stubbed trace — the rerouted segment turns each clock/data line into a T-topology with two pad loads plus an unterminated stub.

**Why deferred:**

- The board already works fine on dedicated 9/11/10.
- GPIOs 11 and 10 are not currently load-bearing for any planned feature.
- Making sharing work would require either a board re-spin with proper terminated routing, or a much lower SPI clock — and 80 MHz `freq_write` is the LT7680-side ceiling that the 24 fps animation pipeline budget depends on ([above](#high-sd-bandwidth-in-spi-mode-caps-complex-animations-below-24-fps--plan-to-migrate-to-sd_mmc-4-bit)). Lower SPI = no 24 fps.

**If revisited:** test at 1 MHz to confirm the failure mode is purely SI (vs. e.g. a bad bridge joint); a multimeter continuity check from GPIO 7 → LCD_CLK pad would rule out the trivial hardware-fault explanation before any further firmware work. If SI is confirmed, a board re-spin with proper bus routing (or a 33–50 Ω series termination at the stub) is the only realistic path forward.

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
