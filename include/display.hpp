#ifndef DISPLAY_HPP
#define DISPLAY_HPP

#include <stdint.h>
#include <LovyanGFX.h>
#include "LGFX_ESP32_PCBA5981_GT911.hpp"

#define TFT_HOR_RES 480
#define TFT_VER_RES 480

// LT7680 SDRAM frame slot addresses. Canvas is 8bpp (1 byte per pixel),
// so each full-screen slot is TFT_HOR_RES * TFT_VER_RES * 1 bytes.
//   SLOT_CANVAS — canonical drawing surface (the user's sketch).
//   SLOT_UI     — overlay backing store; CANVAS_MENU snapshots SLOT_CANVAS
//                 here, then paints UI buttons over the snapshot. Switching
//                 the panel's main-image address back to SLOT_CANVAS makes
//                 the overlay disappear instantly with no fillRect needed.
// Both addresses must be 4-byte aligned (TFT_HOR_RES already is).
#define LT7680_FRAME_BYTES ((uint32_t)TFT_HOR_RES * TFT_VER_RES * 1)
#define LT7680_SLOT_CANVAS 0u
#define LT7680_SLOT_UI     LT7680_FRAME_BYTES
// Two SDRAM slots for ping-pong animation double-buffering. DMA writes to the
// back slot while the front slot is displayed, then page-flips — eliminating
// scanlines caused by writing into the active frame. The slots alternate each
// displayFlashPlayFrame() call.
// Four slots = 4 * 460,800 = ~1.84 MB; well within the 16 MB SDRAM.
#define LT7680_SLOT_ANIM   (LT7680_FRAME_BYTES * 2u)
#define LT7680_SLOT_ANIM_B (LT7680_FRAME_BYTES * 3u)

extern LGFX tft;
/* Tracks touch location in current tick- If Z>0 then touching. */
extern uint16_t touchX, touchY, touchZ;
/* Tracks touch location in previous tick. Does not reset when releasing
touch, so check touchZ to avoid stale numbers.*/
extern u_int16_t lastTouchX, lastTouchY;

// Functions
bool initDisplay();
/** Read touch input as it becomes available and write to global variables.*/
void handleTouch();

/** Write one scanline of raw RGB565 pixels directly to the display,
 *  bypassing the LovyanGFX pixelcopy machinery. Safe to call without an
 *  active tft.startWrite() — manages its own SPI transaction. */
void displayWriteScanline(int x, int y, int w, const uint16_t* data);

/** Begin a multi-scanline display transaction. Call before a sequence of
 *  displayWriteScanlineSpanned() calls, then end with displayFrameEnd(). */
void displayFrameBegin();
void displayFrameEnd();

/** Double-buffered animation frame helpers for SD-decoded frames.
 *  displayAnimFrameBegin() redirects subsequent draw calls to the back ANIM
 *  slot (not the displayed one).
 *  displayAnimWriteFrame() streams a pre-decoded 8bpp CLUT-index buffer to
 *  the canvas in one CS-held DMA burst — no per-pixel CS toggling.
 *  displayAnimFrameEnd() waits for VSync, flips the display to the back slot,
 *  toggles the ping-pong, and restores the canvas to SLOT_CANVAS. */
void displayAnimFrameBegin();
void displayAnimWriteFrame(const uint8_t* clut8);
void displayAnimFrameEnd();

/** Write one scanline using BTE hardware fills for runs of ≥5 same-color
 *  pixels and raw SPI writes for mixed segments. Must be called between
 *  displayFrameBegin() / displayFrameEnd(). Significantly faster than
 *  displayWriteScanline() for palette-indexed content with long runs. */
void displayWriteScanlineSpanned(int y, const uint16_t* pixels, int w);

// ---- Serial Flash animation API -----------------------------------------

/** Read and log the flash chip JEDEC ID. Returns 0xFFFFFF if no response.
 *  Call once at startup to confirm the flash is reachable. */
uint32_t displayFlashReadJEDECID(void);

/** Erase all 4 KB sectors covering [start_addr, start_addr+len).
 *  start_addr is rounded down to the nearest 4 KB boundary. */
void displayFlashErase(uint32_t start_addr, uint32_t len);

/** Write one animation frame to Serial Flash.
 *  frame_idx : zero-based index; frames are packed sequentially from address 0.
 *  w, h      : frame dimensions in pixels (must match playback dimensions).
 *  pixels    : RGB565 data, w*h uint16_t values, row-major.
 *  Erases the required sectors before programming. */
void displayFlashWriteFrame(uint16_t frame_idx, uint16_t w, uint16_t h,
                             const uint16_t* pixels);

/** DMA one frame from Serial Flash → LT7680_SLOT_ANIM, then page-flip the
 *  display to show it.  The drawing canvas (SLOT_CANVAS) is unaffected.
 *  frame_idx / w / h must match what was passed to displayFlashWriteFrame. */
void displayFlashPlayFrame(uint16_t frame_idx, uint16_t w, uint16_t h);

#endif