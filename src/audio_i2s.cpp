#include "audio_i2s.hpp"
#include <driver/i2s_std.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/stream_buffer.h>

static i2s_chan_handle_t    s_tx_chan      = nullptr;
static StreamBufferHandle_t s_stream       = nullptr;
static uint8_t             *s_stream_storage = nullptr;  // PSRAM backing
static TaskHandle_t         s_writer_task  = nullptr;
static volatile bool        s_stop_writer  = false;
static volatile bool        s_writer_done  = true;
static uint32_t             s_drops        = 0;

static bool installChannel(uint32_t sample_rate)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    // 6 × 240 frames ≈ 65 ms of DMA buffer @ 22050 Hz. Absorbs writer-task
    // scheduling jitter from competing core-1 work (SPI consumer, etc.).
    chan_cfg.dma_desc_num  = 6;
    chan_cfg.dma_frame_num = 240;

    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx_chan, nullptr);
    if (err != ESP_OK) {
        printf("[I2S] i2s_new_channel err=%d\n", err);
        s_tx_chan = nullptr;
        return false;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                       I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)I2S_BCLK_PIN,
            .ws   = (gpio_num_t)I2S_LRCLK_PIN,
            .dout = (gpio_num_t)I2S_DOUT_PIN,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { false, false, false },
        },
    };

    err = i2s_channel_init_std_mode(s_tx_chan, &std_cfg);
    if (err != ESP_OK) {
        printf("[I2S] init_std_mode err=%d\n", err);
        i2s_del_channel(s_tx_chan);
        s_tx_chan = nullptr;
        return false;
    }

    err = i2s_channel_enable(s_tx_chan);
    if (err != ESP_OK) {
        printf("[I2S] enable err=%d\n", err);
        i2s_del_channel(s_tx_chan);
        s_tx_chan = nullptr;
        return false;
    }

    printf("[I2S] channel up @ %lu Hz (BCLK=%d LRCLK=%d DOUT=%d)\n",
                  (unsigned long)sample_rate, I2S_BCLK_PIN, I2S_LRCLK_PIN, I2S_DOUT_PIN);
    return true;
}

// Writer-side stereo expansion scratch in .bss so the writer task stack stays
// small. 256 mono frames → 512 stereo samples → 1024 bytes. Only the writer
// task touches it.
static const uint32_t kFrameChunk = 256;
static int16_t s_stereo_scratch[kFrameChunk * 2];

static void writerTask(void *)
{
    int16_t mono[kFrameChunk];
    while (!s_stop_writer) {
        // Drain up to kFrameChunk mono samples per iteration.
        size_t want_bytes = kFrameChunk * sizeof(int16_t);
        size_t got_bytes  = xStreamBufferReceive(s_stream, mono, want_bytes,
                                                 pdMS_TO_TICKS(100));
        if (got_bytes == 0) continue;  // re-check stop flag, then wait again

        uint32_t got_samples = got_bytes / sizeof(int16_t);
        for (uint32_t k = 0; k < got_samples; k++) {
            int16_t s = mono[k];
            s_stereo_scratch[k * 2]     = s;
            s_stereo_scratch[k * 2 + 1] = s;
        }
        size_t written = 0;
        esp_err_t err = i2s_channel_write(s_tx_chan, s_stereo_scratch,
                                          got_samples * 2 * sizeof(int16_t),
                                          &written, pdMS_TO_TICKS(500));
        if (err != ESP_OK) {
            printf("[I2S] write err=%d\n", err);
            break;
        }
    }
    s_writer_done = true;
    vTaskDelete(NULL);
}

bool startI2SStreaming(uint32_t sample_rate, uint16_t samples_per_frame)
{
    if (sample_rate == 0 || samples_per_frame == 0) return false;
    if (s_tx_chan) {
        puts("[I2S] startI2SStreaming: already running");
        return false;
    }
    if (!installChannel(sample_rate)) return false;

    // Buffer ≈ 12 frames of mono PCM in PSRAM. 12 frames × samples_per_frame ×
    // 2 bytes — covers ~500 ms at 24 fps so producer scheduling glitches don't
    // underrun. Stream buffer needs +1 internal byte.
    uint32_t buf_bytes = (uint32_t)samples_per_frame * 12u * 2u + 1u;
    s_stream_storage = (uint8_t *)heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM);
    if (!s_stream_storage) {
        printf("[I2S] stream storage ps_malloc %lu failed\n",
                      (unsigned long)buf_bytes);
        i2s_channel_disable(s_tx_chan);
        i2s_del_channel(s_tx_chan);
        s_tx_chan = nullptr;
        return false;
    }
    static StaticStreamBuffer_t stream_static;  // FreeRTOS control block in .bss
    s_stream = xStreamBufferCreateStatic(buf_bytes - 1, 1, s_stream_storage,
                                         &stream_static);
    if (!s_stream) {
        free(s_stream_storage); s_stream_storage = nullptr;
        i2s_channel_disable(s_tx_chan);
        i2s_del_channel(s_tx_chan);
        s_tx_chan = nullptr;
        puts("[I2S] xStreamBufferCreateStatic failed");
        return false;
    }

    s_stop_writer = false;
    s_writer_done = false;
    s_drops       = 0;

    // Pinned to core 1 alongside the SPI consumer so the producer/decoder on
    // core 0 stays unobstructed. Priority 3 (above the SD loader's 2 and the
    // main task's 1) so DMA refills win brief CPU contention.
    // 8 KB stack: i2s_channel_write + ESP-IDF logging frames are surprisingly
    // deep; 4 KB had overflowed and trampled task-WDT bookkeeping previously.
    BaseType_t ok = xTaskCreatePinnedToCore(writerTask, "i2s_wr", 8192, nullptr,
                                            3, &s_writer_task, 1);
    if (ok != pdPASS) {
        puts("[I2S] writer task spawn failed");
        vStreamBufferDelete(s_stream); s_stream = nullptr;
        free(s_stream_storage); s_stream_storage = nullptr;
        i2s_channel_disable(s_tx_chan);
        i2s_del_channel(s_tx_chan);
        s_tx_chan = nullptr;
        s_writer_done = true;
        return false;
    }
    return true;
}

bool pushI2SSamples(const int16_t *pcm, uint32_t n_samples)
{
    if (!s_stream) return false;
    if (!pcm || n_samples == 0) return false;
    size_t bytes = n_samples * sizeof(int16_t);
    size_t sent = xStreamBufferSend(s_stream, pcm, bytes, pdMS_TO_TICKS(5));
    if (sent < bytes) {
        s_drops++;
        if ((s_drops & 0x1F) == 1) {
            printf("[I2S] push timeout (%u/%u bytes, drops=%u)\n",
                          (unsigned)sent, (unsigned)bytes, s_drops);
        }
        return false;
    }
    return true;
}

void stopI2SStreaming()
{
    if (s_writer_done && !s_tx_chan) return;

    s_stop_writer = true;
    // Wake writer if it's blocked in xStreamBufferReceive.
    if (s_stream) xStreamBufferReset(s_stream);
    for (int i = 0; i < 50 && !s_writer_done; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!s_writer_done) {
        puts("[I2S] writer task did not exit within 500 ms");
    }
    s_writer_task = nullptr;

    if (s_stream)         { vStreamBufferDelete(s_stream); s_stream = nullptr; }
    if (s_stream_storage) { free(s_stream_storage); s_stream_storage = nullptr; }
    if (s_tx_chan) {
        i2s_channel_disable(s_tx_chan);
        i2s_del_channel(s_tx_chan);
        s_tx_chan = nullptr;
    }
    if (s_drops > 0) {
        printf("[I2S] total push drops: %u\n", s_drops);
    }
}
