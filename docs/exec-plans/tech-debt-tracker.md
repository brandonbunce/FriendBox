# Tech Debt Tracker

Known structural problems that should be resolved before significant feature work. Ordered by severity.

---

## [MEDIUM] ui_core.cpp screen extraction (in progress)

**Files:** [src/ui/ui_core.cpp](../../src/ui/ui_core.cpp), [src/ui/ui_screen_canvas_menu.cpp](../../src/ui/ui_screen_canvas_menu.cpp)

**Status:** The dispatcher and registry are in place — `ScreenHandlers` table in `ui_core.cpp`, indexed by `screen_id_t`, with `init` / `onEnter` / `draw` / `handleTouch` hooks per screen. `SCREEN_CANVAS_MENU` is fully extracted to its own module. See [docs/ARCHITECTURE.md](../ARCHITECTURE.md) for the dispatch model.

**Remaining work:**
- Extract `SCREEN_SEND` to `src/ui/ui_screen_send.cpp/.hpp` (init, draw, handleTouch are currently file-static helpers in `ui_core.cpp`).
- Extract `SCREEN_FILE_BROWSER` to `src/ui/ui_screen_file_browser.cpp/.hpp` (same situation).
- Move the SEND/FILE_BROWSER button arrays out of `ui_core.cpp` once their owning modules exist.
- Once that's done, `ui_core.cpp` should hold only the dispatcher, `UIButton` plumbing, `cleanupUIOutOfContext`, and the registry table.

**Pattern to follow:** mirror `ui_screen_canvas_menu` — header declares the four hooks, `.cpp` keeps button arrays/labels `static`, and `ui_core.cpp` includes the header and references the hooks from one row in `screens[]`.

---

## [HIGH] Network send/receive not implemented

**File:** [src/network.cpp](../../src/network.cpp)

**Problem:** `networkSendFramebuffer()` and `networkReceiveFramebuffer()` are stubs. The backend API contract is undefined.

**Impact:** Core feature (sending drawings to friends) is blocked.

**Desired state:** Defined API contract in a product spec; implemented and tested send/receive flow.

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
