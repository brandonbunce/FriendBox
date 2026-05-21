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

#endif
