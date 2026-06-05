#ifndef AUDIO_I2S_HPP
#define AUDIO_I2S_HPP

#include <stdint.h>

// PCBA5981 I2S DAC wiring
#define I2S_BCLK_PIN  38
#define I2S_LRCLK_PIN 39
#define I2S_DOUT_PIN  40

/* Streaming I2S API for v4 playback. Producer task pushes one frame's worth of
 * mono PCM per video frame via pushI2SSamples(); writer task drains the stream
 * buffer, expands mono→stereo, and writes to the I2S TX DMA. */

/* Install the I2S TX channel for 16-bit stereo PCM at sample_rate, allocate
 * a streaming PSRAM buffer sized for ~12 frames of cushion, and spawn the
 * writer task on core 1 prio 3. Returns false on init failure. */
bool startI2SStreaming(uint32_t sample_rate, uint16_t samples_per_frame);

/* Push n_samples of mono int16 PCM into the stream buffer. Non-blocking with
 * a 5 ms timeout — better to drop a frame of audio than to stall the video
 * producer past its 30 ms decode budget. Returns true on full enqueue.
 * Returns false (and logs) on timeout/drop. */
bool pushI2SSamples(const int16_t *pcm, uint32_t n_samples);

/* Signal the writer task to drain and exit. Tears down the stream buffer and
 * I2S channel. No-op if not running. */
void stopI2SStreaming();

/* Software volume control. NS4168 is a fixed-gain Class-D amp (no digital
 * volume input), so attenuation must happen in PCM before I2S. Scaling lives
 * in the writer task's mono→stereo expansion — single chokepoint, takes
 * effect within ~12 ms of a setter call (one writer chunk).
 *
 * pct: 0 = mute, 100 = unity gain. Internally stored as a Q15 multiplier.
 * Linear curve: perceived loudness drops sharply below ~50%. Swap to a log
 * curve if a UI volume slider feels too binary.
 *
 * Persistence: setI2SVolume commits the value to NVS (key "i2s_vol" in the
 * "Friendbox" namespace). Call loadI2SVolumeFromNVS() once at boot to apply
 * the saved value before audio playback. Default if no saved value: 100. */
void    setI2SVolume(uint8_t pct);
uint8_t getI2SVolume(void);
void    loadI2SVolumeFromNVS(void);

/* Live volume change WITHOUT touching NVS — for continuous UI drags (a volume
 * slider sweep fires every frame; an NVS/flash write per frame stalls playback
 * and wears the flash). Apply with setI2SVolumeLive() during the drag, then
 * call commitI2SVolume() once on release to persist the final value. commit is
 * a no-op if nothing changed since the last persisted value. */
void    setI2SVolumeLive(uint8_t pct);
void    commitI2SVolume(void);

#endif
