# Core Beliefs

Guiding constraints for working in this codebase. These are not style preferences — they reflect real hardware limits and past decisions.

---

## Memory is the primary constraint

The ESP32 has limited heap. The LT7680A SDRAM holds framebuffers, not arbitrary data. Every allocation decision should justify itself against this constraint. Prefer stack, static arrays, and fixed-size buffers over heap allocations in hot paths.

## Stable animations are a must.

18fps animations must be possible with even the most intense .fbox file.

## The palette is intentional, not a limitation

16 colors (4-bit palette indices, packed 2-per-byte) was chosen deliberately. It keeps sketch file size at a fixed 115,200 bytes, enables fast SD read/write, and gives the device a distinct aesthetic. Do not expand the palette without a deliberate design decision.

## The panel driver is a black box

`lib/Panel_PCBA5981/` implements a custom LovyanGFX panel interface for the LT7680A controller. It is stable, hardware-tested, and not touched during normal feature work. If display behavior changes unexpectedly, investigate calling code before touching the driver.

## Touch input is queued, not interrupt-driven at the UI layer

The touch queue in `display.cpp` decouples hardware timing from UI logic. UI code must only read from the queue — never access the touch controller directly.

## SD card is the primary persistence layer

NVS stores only small preferences (palette selection, etc.). Sketches live on SD. Network sync of sketches is aspirational — do not design features assuming reliable cloud storage.