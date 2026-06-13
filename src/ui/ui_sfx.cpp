// Procedural UI sound effects. Short blips are synthesized once at boot into
// small PSRAM buffers (no SD assets) and pushed through a lazy, persistent UI
// I2S session. fbox playback owns the I2S channel exclusively (startI2SStreaming
// refuses a second install), so playback suspends the UI session around itself.
#include "ui.hpp"
#include "audio_i2s.hpp"
#include "mem.hpp"
#include "idf_compat.hpp"

#include <math.h>
#include <string.h>
#include <esp_heap_caps.h>
#include <stdio.h>

namespace ui {

// UI session format. Buffer = UI_SPF * 12 frames * 2 bytes ≈ 12 KB ≈ 278 ms of
// cushion at 22050 Hz — comfortably larger than any single blip.
static const uint32_t UI_RATE = 22050;
static const uint16_t UI_SPF  = 512;

struct Clip { int16_t *pcm; uint32_t n; };
static Clip s_clips[SFX_COUNT] = {};
static bool s_synthDone   = false;
static bool s_sessionUp   = false;
static uint32_t s_lastPushMs = 0;     // when the last blip was queued

// Tear the I2S session down this long after the final blip. An idle session
// underruns the I2S DMA, which loops its last buffer into a continuous buzz;
// closing it returns the DAC to silence. Comfortably longer than the longest
// clip (180 ms) plus the ~278 ms buffer cushion so audio fully drains first.
static const uint32_t SFX_IDLE_STOP_MS = 500;

// One eased blip generator. waveform: 0 = sine, 1 = square. f0->f1 sweeps the
// frequency across the clip; a linear-decay envelope avoids clicks at the tail.
static Clip synth(uint32_t ms, float f0, float f1, int wave, int amp)
{
    uint32_t n = (UI_RATE * ms) / 1000u;
    int16_t *buf = (int16_t *)heap_caps_malloc(n * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!buf) return { nullptr, 0 };

    double phase = 0.0;
    for (uint32_t i = 0; i < n; i++) {
        double t = (double)i / (double)n;                  // 0..1
        double f = f0 + (f1 - f0) * t;
        phase += 2.0 * M_PI * f / (double)UI_RATE;
        double s = (wave == 1) ? (sin(phase) >= 0.0 ? 1.0 : -1.0) : sin(phase);
        double env = 1.0 - t;                              // linear decay
        buf[i] = (int16_t)(s * env * (double)amp);
    }
    return { buf, n };
}

// Eviction hook: free the ~25 KB of PSRAM clips and release the I2S session
// (its DMA descriptors are internal RAM). playSfx() re-synthesizes lazily.
static void sfxEvict(void *)
{
    suspendSfxSession();
    for (int i = 0; i < SFX_COUNT; i++) {
        if (s_clips[i].pcm) heap_caps_free(s_clips[i].pcm);
        s_clips[i] = { nullptr, 0 };
    }
    s_synthDone = false;
}

/* synthesize UI blips (lazy I2S session opens on first playSfx)*/
void initSfx()
{
    if (s_synthDone) return;
    s_clips[SFX_TAP]     = synth(45,  1200, 1200, 1, 7000);   // crisp click
    s_clips[SFX_CONFIRM] = synth(120, 700,  1400, 0, 8000);   // rising chirp
    s_clips[SFX_ERROR]   = synth(180, 220,  170,  1, 7000);   // low falling buzz
    s_clips[SFX_OPEN]    = synth(110, 500,  1100, 0, 7000);   // open sweep up
    s_clips[SFX_CLOSE]   = synth(110, 1100, 500,  0, 7000);   // close sweep down
    s_synthDone = true;

    static bool s_registered = false;   // register the evictable exactly once
    if (!s_registered) {
        memRegisterEvictable("sfx", sfxEvict, nullptr, /*priority=*/10);
        s_registered = true;
    }
}

void playSfx(SfxId id)
{
    if (id <= SFX_NONE || id >= SFX_COUNT) return;
    if (!s_synthDone) initSfx();        // re-synth if the clips were evicted
    const Clip &c = s_clips[id];
    if (!c.pcm || c.n == 0) return;

    // Lazily (re)start the UI session. If install fails the channel is owned by
    // fbox playback right now — drop the blip rather than fight for it.
    if (!s_sessionUp) {
        if (!startI2SStreaming(UI_RATE, UI_SPF)) return;
        s_sessionUp = true;
    }
    pushI2SSamples(c.pcm, c.n);
    s_lastPushMs = millis();
    // The I2S channel is configured auto_clear_after_cb (see audio_i2s.cpp), so
    // once the clip drains the DMA emits silence rather than looping the tail;
    // sfxTick() then closes the idle session.
}

void sfxTick()
{
    // Close the lazily-opened UI session once it has been idle, so an empty
    // StreamBuffer never leaves the DAC looping a stale DMA buffer.
    if (s_sessionUp && (millis() - s_lastPushMs) > SFX_IDLE_STOP_MS)
        suspendSfxSession();
}

void suspendSfxSession()
{
    if (!s_sessionUp) return;
    stopI2SStreaming();
    s_sessionUp = false;
}

void resumeSfxSession()
{
    // Lazy: the next playSfx() restarts the session. Nothing to do here beyond
    // ensuring we don't hold a stale "up" flag if playback tore the channel down.
    s_sessionUp = false;
}

} // namespace ui
