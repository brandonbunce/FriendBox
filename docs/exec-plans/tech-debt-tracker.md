# Tech Debt Tracker

Known structural problems that should be resolved before significant feature work. Ordered by severity.

---

## ~~[MEDIUM] ui_core.cpp screen extraction~~ — DONE

`SCREEN_SEND` → `src/ui/ui_screen_send.cpp/.hpp` ✓  
`SCREEN_FILE_BROWSER` → `src/ui/ui_screen_file_browser.cpp/.hpp` ✓  
`ui_core.cpp` now holds only the dispatcher, registry, `UIButton` plumbing, `cleanupUIOutOfContext`, `drawSketchPreview`, and `drawFriendboxLoadingScreen`. No further extraction needed unless `drawSketchPreview` / `drawFriendboxLoadingScreen` grow substantially.

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
