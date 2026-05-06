# Tech Debt Tracker

Known structural problems that should be resolved before significant feature work. Ordered by severity.

---

## [CRITICAL] ui.cpp monolith

**File:** [src/ui.cpp](../../src/ui.cpp) (~1,300 lines)

**Problem:** All screens, buttons, dropdowns, and UI state live in a single file. Adding new screens increases coupling and makes reasoning about state transitions harder.

**Impact:** Every new UI feature (Home screen, Settings, Registration, Send history) makes this worse.

**Desired state:** Each screen is an independent module with a clear interface for entering, rendering, and exiting. A central dispatcher handles screen transitions.

**Plan:** See [active/ui-refactor.md](active/ui-refactor.md) when that plan is written.

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
