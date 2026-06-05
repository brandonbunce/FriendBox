// Procedural UI sound effects. Short blips are synthesized once at boot into
// small PSRAM buffers (no SD assets) and pushed through a lazy, persistent UI
// I2S session. fbox playback owns the I2S channel exclusively (startI2SStreaming
// refuses a second install), so playback suspends the UI session around itself.
#include "ui.hpp"
#include "audio_i2s.hpp"

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

void initSfx()
{
    if (s_synthDone) return;
    s_clips[SFX_TAP]     = synth(45,  1200, 1200, 1, 7000);   // crisp click
    s_clips[SFX_CONFIRM] = synth(120, 700,  1400, 0, 8000);   // rising chirp
    s_clips[SFX_ERROR]   = synth(180, 220,  170,  1, 7000);   // low falling buzz
    s_clips[SFX_OPEN]    = synth(110, 500,  1100, 0, 7000);   // open sweep up
    s_clips[SFX_CLOSE]   = synth(110, 1100, 500,  0, 7000);   // close sweep down
    s_synthDone = true;
}

void playSfx(SfxId id)
{
    if (id <= SFX_NONE || id >= SFX_COUNT) return;
    const Clip &c = s_clips[id];
    if (!c.pcm || c.n == 0) return;

    // Lazily (re)start the UI session. If install fails the channel is owned by
    // fbox playback right now — drop the blip rather than fight for it.
    if (!s_sessionUp) {
        if (!startI2SStreaming(UI_RATE, UI_SPF)) return;
        s_sessionUp = true;
    }
    pushI2SSamples(c.pcm, c.n);
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
