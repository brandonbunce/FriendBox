# Tech Debt Tracker

Known structural problems that should be resolved before significant feature work. Ordered by severity.

---

## ~~[MEDIUM] ui_core.cpp screen extraction~~ — DONE

`SCREEN_SEND` → `src/ui/ui_screen_send.cpp/.hpp` ✓  
`SCREEN_FILE_BROWSER` → `src/ui/ui_screen_file_browser.cpp/.hpp` ✓  
`ui_core.cpp` now holds only the dispatcher, registry, `UIButton` plumbing, `cleanupUIOutOfContext`, `drawSketchPreview`, and `drawFriendboxLoadingScreen`. No further extraction needed unless `drawSketchPreview` / `drawFriendboxLoadingScreen` grow substantially.

---

## [MEDIUM] Audio source handoff at video EOF

**Files:** [src/io.cpp](../../src/io.cpp), [src/audio.cpp](../../src/audio.cpp), [include/fbox_source.hpp](../../include/fbox_source.hpp)

**Problem:** The producer task's persistent `FboxRleReader` chunk-buffers up to 256 bytes of look-ahead from the source. At video EOF, those buffered bytes are the first bytes of the audio section. `fboxDecodeAudio` reads from the source directly (bypassing the reader's buf), so those bytes are skipped — decoded PCM starts mid-block with wrong predictor/step_index state.

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
