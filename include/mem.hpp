// FriendBox memory manager.
//
// The ESP32-S3 has ~320 KB of internal DIRAM and 8 MB of external PSRAM. The one
// large *internal* allocation the firmware needs is the 115 KB per-pixel decode
// scratch for .fbox playback (frame_4bpp in io.cpp) — internal because PSRAM is
// 10-30x slower per access and would blow the per-frame decode budget. By the
// time playback runs, WiFi/lwIP/TLS and task stacks have fragmented internal RAM
// enough that a *contiguous* 115 KB block is often unavailable, so the alloc
// fails (PlaybackResult::OOM).
//
// This module provides three things:
//   1. Diagnostics  — memReport(): per-pool free + largest-contiguous block.
//   2. Reserved pool — grab the playback scratch buffer at boot while internal
//                      RAM is still pristine, and lend it to playback forever.
//                      Guarantees the fast internal decode path can't OOM.
//   3. Eviction registry — subsystems register evict callbacks that memReclaim()
//                      runs to free reclaimable memory before big allocations or
//                      under pressure. The reusable "clear unneeded assets" hook.
#ifndef MEM_HPP
#define MEM_HPP

#include <stddef.h>

// One-time init (clears the eviction registry). Call once, very early in
// app_main before WiFi comes up.
void memInit();

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------
// Log free bytes and the largest free (contiguous) block for INTERNAL and
// SPIRAM, tagged so boot/post-wifi/playback samples are distinguishable.
void memReport(const char *tag);

// ---------------------------------------------------------------------------
// Reserved playback scratch pool
// ---------------------------------------------------------------------------
// Reserve the playback internals once (idempotent), while internal RAM is still
// pristine/contiguous at boot: the 115 KB decode buffer AND the 16 KB fbox_dec
// producer task stack. Both are reused across every playback and never freed.
// Returns true if the decode buffer was reserved. Call right after memInit().
bool   memReservePlaybackScratch();
// The pinned internal scratch buffer, or nullptr if the reservation failed.
void  *memPlaybackScratch();
// Size of the scratch buffer in bytes (PIXEL_COUNT >> 1 = 115200).
size_t memPlaybackScratchSize();

// Reserved producer-task resources, so the per-playback fbox_dec task is
// guaranteed to spawn (via xTaskCreateStatic*) even when the heap is fragmented.
// memProducerStack() is nullptr if the reservation failed (caller falls back to
// a dynamic xTaskCreate). memProducerStackWords() is the depth in StackType_t
// units; memProducerTCB() points at StaticTask_t storage. Returned as void* so
// this header stays free of the FreeRTOS include.
void  *memProducerStack();
size_t memProducerStackWords();
void  *memProducerTCB();

// ---------------------------------------------------------------------------
// Eviction registry ("clear unneeded assets")
// ---------------------------------------------------------------------------
typedef void (*MemEvictFn)(void *ctx);

// Register a subsystem that can free reclaimable memory on demand. Lower
// `priority` is evicted first (least important). `name` is for logging. Safe to
// call once per subsystem at its init.
void   memRegisterEvictable(const char *name, MemEvictFn evict, void *ctx, int priority);

// Run every registered evict callback (low priority first). Returns the number
// of INTERNAL bytes reclaimed (measured around the calls).
size_t memReclaim();

#endif // MEM_HPP
