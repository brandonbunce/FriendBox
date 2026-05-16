#include "io.hpp"
#include "display.hpp"
#include "canvas.hpp"
#include "ui_core.hpp"
#include "audio.hpp"
#include "fbox_source.hpp"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_rom_crc.h>
#include <esp_heap_caps.h>
#include <esp_cache.h>

Preferences nvs;
SPIClass sdspi = SPIClass(HSPI);


bool initNVS()
{
  nvs.begin("Friendbox", true);
  nvs.end();
  return true;
}

bool initSD(bool forceFormat)
{
#ifdef FRIENDBOX_DEBUG_MODE
    Serial.println("INFO: Initializing SD...");
#endif
    sdspi.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    sdspi.setFrequency(40000000); // 40 MHz — explicit to avoid inheriting display bus speed
    if (!SD.begin(SD_CS, sdspi))
    {
#ifdef FRIENDBOX_DEBUG_MODE
        Serial.println("ERROR: SD mount failed! Is it connected properly?");
#endif
        return false;
    }
    return true;
}

bool initMenuButton() {
    pinMode(HALL_SENSOR_PIN, INPUT_PULLUP);
    return true;
}

void handleMenuButton(bool recheckInput)
{
    if (currentScreen == SCREEN_CANVAS || currentScreen == SCREEN_CANVAS_MENU)
    {
        static unsigned long lastPress = 0;
        static unsigned int lastButtonState = 0;
        static bool alreadyPressed = false;
        if (digitalRead(HALL_SENSOR_PIN) == LOW) /*Button Pressed*/
        {
            if (lastPress == 0)
            {
                lastPress = millis();
            }
            if (((millis() >= (lastPress + DEBOUNCE_MILLISECONDS)) & !alreadyPressed) || recheckInput)
            {
                Serial.println("Logical Press");
                changeScreenContext(SCREEN_CANVAS_MENU);
                alreadyPressed = true;
            }
            else
            {
                return;
            }
        }
        else /*Button Released*/
        {
            if (lastPress && alreadyPressed)
            {
                lastPress = 0;
                alreadyPressed = false;
                Serial.println("Logical Release.");
                if (currentScreen != SCREEN_CANVAS)
                    changeScreenContext(SCREEN_CANVAS);
            }
        }
    }
}

bool fboxReadHeader(FboxSource &src, FboxHeader &out, uint8_t *raw_out)
{
    uint8_t hdr[FBOX_HEADER_SIZE];
    size_t got = 0;
    while (got < FBOX_HEADER_SIZE) {
        int r = src.read(hdr + got, FBOX_HEADER_SIZE - got);
        if (r <= 0) return false;
        got += (size_t)r;
    }
    if (memcmp(hdr, "FBOX", 4) != 0) return false;
    if (hdr[4] != FBOX_VERSION_3) return false;

    out.version     = hdr[4];
    out.kind        = hdr[5];
    out.frame_count = (uint16_t)hdr[6]  | ((uint16_t)hdr[7]  << 8);
    out.fps         = (uint16_t)hdr[8]  | ((uint16_t)hdr[9]  << 8);
    out.width       = (uint16_t)hdr[10] | ((uint16_t)hdr[11] << 8);
    out.height      = (uint16_t)hdr[12] | ((uint16_t)hdr[13] << 8);
    out.created     = (uint32_t)hdr[14] | ((uint32_t)hdr[15] << 8)
                    | ((uint32_t)hdr[16] << 16) | ((uint32_t)hdr[17] << 24);
    memcpy(out.username, &hdr[18], 32);
    out.username[32] = '\0';
    out.audio_size  = (uint32_t)hdr[50] | ((uint32_t)hdr[51] << 8)
                    | ((uint32_t)hdr[52] << 16) | ((uint32_t)hdr[53] << 24);
    out.audio_sample_rate = (uint16_t)hdr[54] | ((uint16_t)hdr[55] << 8);
    out.audio_channels    = hdr[56];
    out.crc32 = (uint32_t)hdr[58] | ((uint32_t)hdr[59] << 8)
              | ((uint32_t)hdr[60] << 16) | ((uint32_t)hdr[61] << 24);
    if (raw_out) memcpy(raw_out, hdr, FBOX_HEADER_SIZE);
    return true;
}

void loadSketchFromSD(const char *path)
{
    FboxSourceSD src(path);
    if (!src.ok()) { Serial.printf("loadSketchFromSD: cannot open %s\n", path); return; }

    FboxHeader hdr;
    if (!fboxReadHeader(src, hdr)) {
        Serial.println("loadSketchFromSD: invalid FBOX header");
        return;
    }
    if (hdr.width != TFT_HOR_RES || hdr.height != TFT_VER_RES) {
        Serial.printf("loadSketchFromSD: unexpected size %dx%d\n", hdr.width, hdr.height);
        return;
    }

    // Skip frame size table — sketch is single-frame I-frame at the start of data.
    uint32_t table_bytes = (uint32_t)hdr.frame_count * 4;
    uint8_t skip_buf[256];
    while (table_bytes > 0) {
        int r = src.read(skip_buf, table_bytes > sizeof(skip_buf) ? sizeof(skip_buf) : table_bytes);
        if (r <= 0) { Serial.println("loadSketchFromSD: skip table failed"); return; }
        table_bytes -= (uint32_t)r;
    }

    uint8_t frame_type;
    if (src.read(&frame_type, 1) != 1 || frame_type != FBOX_FRAME_I) {
        Serial.println("loadSketchFromSD: frame 0 is not an I-frame");
        return;
    }

    uint16_t pxLine[TFT_HOR_RES];
    displayFrameBegin();

    FboxRleReader rle;
    rle.begin(&src, hdr.width * hdr.height);
    bool ok = true;
    for (int y = 0; y < hdr.height && ok; y++) {
        for (int x = 0; x < hdr.width; x++) {
            int idx = rle.next();
            if (idx < 0) { ok = false; break; }
            pxLine[x] = draw_color_palette[idx];
        }
        if (ok) displayWriteScanlineSpanned(y, pxLine, hdr.width);
    }

    displayFrameEnd();
    Serial.printf("loadSketchFromSD: loaded '%s'\n", path);
}

// ── playFboxAnimation: dual-core producer/consumer pipeline ──────────────────
//
//  Producer (core 0): pulls bytes from a CRC-wrapped FboxSource, RLE-decodes,
//    applies XOR for P-frames, CLUT-maps into one of N PSRAM frame_buf slots,
//    signals ready.
//  Consumer (this task, core 1 / app-main): waits for a ready slot, writes it
//    to LT7680 SDRAM via the existing displayAnim* helpers, releases the slot.
//
//  Ring of 3 slots hides the SPI burst behind the next decode. With only 2
//  slots the producer would stall whenever the consumer is mid-burst on the
//  one ready frame.

namespace {

constexpr int RING_SLOTS = 3;
constexpr uint32_t PIXEL_COUNT = (uint32_t)TFT_HOR_RES * TFT_VER_RES;  // 230,400

enum SlotState : uint8_t {
    SLOT_OK     = 0,
    SLOT_EOF    = 1,
    SLOT_ERR    = 2,
    SLOT_UNDER  = 3,
};

struct Slot {
    uint8_t  *frame_buf;
    uint8_t   state;
    uint16_t  frame_idx;
};

struct ProducerCtx {
    FboxSource         *src;            // CRC-wrapped source
    const FboxHeader   *hdr;
    const uint8_t      *clut_lut;
    uint8_t            *frame_4bpp;     // PSRAM, PIXEL_COUNT/2 bytes — XOR baseline
    Slot               *slots;
    SemaphoreHandle_t   sem_free;
    SemaphoreHandle_t   sem_ready;
    volatile bool      *cancel;
    bool                ran_to_eof;
    TaskHandle_t        task;
};

void producerTask(void *param)
{
    ProducerCtx *ctx = (ProducerCtx *)param;
    int slot_idx = 0;
    bool error = false;
    uint8_t err_state = SLOT_ERR;

    // Single persistent reader across all frames. Its 256-byte chunk buffer
    // carries pre-read next-frame bytes from one iteration to the next.
    FboxRleReader rle;
    rle.begin(ctx->src, 0);

    for (uint16_t fi = 0; fi < ctx->hdr->frame_count && !error; fi++) {
        if (*ctx->cancel) break;

        while (xSemaphoreTake(ctx->sem_free, pdMS_TO_TICKS(50)) != pdTRUE) {
            if (*ctx->cancel) goto producer_exit;
        }

        Slot &s = ctx->slots[slot_idx];
        s.frame_idx = fi;
        s.state     = SLOT_OK;

        // Type byte comes from the chunk-buffered reader — using src->read
        // directly here would bypass the buffer and lose any pre-read bytes
        // sitting in rle.buf, re-introducing the offset drift the persistent
        // reader is meant to prevent.
        int type_int = rle.read_byte();
        if (type_int < 0) { error = true; err_state = rle.err ? SLOT_UNDER : SLOT_ERR; }
        uint8_t frame_type = (uint8_t)type_int;

        if (!error) {
            bool is_pf  = (frame_type == FBOX_FRAME_P);
            uint8_t *fb = s.frame_buf;
            uint8_t *f4 = ctx->frame_4bpp;

            rle.beginFrame((int)PIXEL_COUNT);

            for (uint32_t i = 0; i < PIXEL_COUNT; i++) {
                int raw = rle.next();
                if (raw < 0) {
                    error    = true;
                    err_state = rle.err ? SLOT_UNDER : SLOT_ERR;
                    break;
                }
                uint8_t prev_byte = f4[i >> 1];
                uint8_t prev_nib  = (i & 1) ? (prev_byte & 0x0F) : ((prev_byte >> 4) & 0x0F);
                uint8_t curr_nib  = is_pf ? ((uint8_t)raw ^ prev_nib) : (uint8_t)raw;
                if (i & 1)
                    f4[i >> 1] = (prev_byte & 0xF0) | curr_nib;
                else
                    f4[i >> 1] = (prev_byte & 0x0F) | (curr_nib << 4);
                fb[i] = ctx->clut_lut[curr_nib];
            }
        }

        if (error) {
            Serial.printf("[ANIM] producer error at frame %u: state=%u\n",
                          fi, err_state);
            s.state = err_state;
            xSemaphoreGive(ctx->sem_ready);
            goto producer_exit;
        }

        // ESP32-S3 cache coherency: this task (core 0) wrote frame_buf via
        // the PSRAM cache. The consumer (core 1) will trigger a DMA read of
        // physical PSRAM that bypasses the cache. Without an explicit
        // writeback the DMA sees stale memory and the displayed frame is a
        // mosaic of new + old data. esp_cache_msync flushes cache lines to
        // PSRAM. Direction C2M (CPU→memory) is the default, called out for
        // clarity. Must complete before xSemaphoreGive so the consumer
        // sees flushed data when it takes the slot.
        esp_cache_msync(s.frame_buf, PIXEL_COUNT, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

        xSemaphoreGive(ctx->sem_ready);
        slot_idx = (slot_idx + 1) % RING_SLOTS;

        // Yield 1 tick per frame so IDLE0 can run and pet the task watchdog.
        // Producer at priority 2 never naturally yields to priority-0 IDLE,
        // and the decode loop is CPU-bound (no system calls between frames),
        // so without this the WDT trips after 5 s of uninterrupted decoding.
        vTaskDelay(1);
    }

    if (!error && !*ctx->cancel) {
        // Signal clean EOF
        if (xSemaphoreTake(ctx->sem_free, pdMS_TO_TICKS(500)) == pdTRUE) {
            ctx->slots[slot_idx].state = SLOT_EOF;
            xSemaphoreGive(ctx->sem_ready);
        }
        ctx->ran_to_eof = true;
    }

producer_exit:
    vTaskDelete(NULL);
}

} // namespace

PlaybackResult playFboxAnimation(FboxSource &src)
{
    uint8_t raw_hdr[FBOX_HEADER_SIZE];
    FboxHeader hdr;
    if (!fboxReadHeader(src, hdr, raw_hdr)) {
        Serial.println("playFboxAnimation: invalid FBOX header");
        return PlaybackResult::DECODE_ERROR;
    }
    if (hdr.frame_count == 0 || hdr.width != TFT_HOR_RES || hdr.height != TFT_VER_RES) {
        Serial.printf("playFboxAnimation: bad header (frames=%d, %dx%d)\n",
                      hdr.frame_count, hdr.width, hdr.height);
        return PlaybackResult::DECODE_ERROR;
    }

    const uint32_t frame_ms = hdr.fps > 0 ? 1000u / hdr.fps : 100u;

    // CRC seed = header bytes [62..127] folded in. All later bytes go through
    // FboxSourceCrc which accumulates as the producer reads.
    uint32_t crc_seed = esp_rom_crc32_le(0, raw_hdr + 62, FBOX_HEADER_SIZE - 62);
    FboxSourceCrc crc_src(&src, crc_seed);

    // Frame size table — read and discard payload; we don't need it for
    // sequential playback. Folding it into CRC happens automatically via crc_src.
    {
        uint32_t table_bytes = (uint32_t)hdr.frame_count * 4;
        uint8_t buf[256];
        while (table_bytes > 0) {
            size_t take = table_bytes > sizeof(buf) ? sizeof(buf) : table_bytes;
            int r = crc_src.read(buf, take);
            if (r <= 0) return PlaybackResult::READ_UNDERRUN;
            table_bytes -= (uint32_t)r;
        }
    }

    uint8_t clut_lut[16];
    for (int i = 0; i < 16; i++) {
        uint16_t c = draw_color_palette[i];
        clut_lut[i] = (uint8_t)((((c >> 13) & 7) << 5) |
                                (((c >>  8) & 7) << 2) |
                                 ((c >>  3) & 3));
    }

    // Memory layout:
    //   frame_4bpp — CPU-only XOR baseline, hot read+write per pixel. Kept in
    //     INTERNAL SRAM (115,200 bytes) so the producer's hottest loop hits
    //     1–3 cycle accesses instead of 10–30 cycle PSRAM accesses. Halves the
    //     decode time and unblocks the consumer.
    //   slot[i].frame_buf — DMA'd by the consumer's SPI engine. Must live in
    //     PSRAM (3 × 230,400 = 690 KB doesn't fit in 327 KB internal) and must
    //     be 32-byte aligned for esp_cache_msync. PIXEL_COUNT is already a
    //     multiple of 32, so heap_caps_aligned_alloc covers both ends.
    uint8_t *frame_4bpp = (uint8_t *)heap_caps_calloc(PIXEL_COUNT >> 1, 1, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!frame_4bpp) {
        Serial.printf("[ANIM] internal alloc for frame_4bpp (%lu B) failed\n",
                      (unsigned long)(PIXEL_COUNT >> 1));
        return PlaybackResult::OOM;
    }
    Slot slots[RING_SLOTS] = {};
    for (int i = 0; i < RING_SLOTS; i++) {
        slots[i].frame_buf = (uint8_t *)heap_caps_aligned_alloc(32, PIXEL_COUNT, MALLOC_CAP_SPIRAM);
        if (!slots[i].frame_buf) {
            for (int j = 0; j < i; j++) free(slots[j].frame_buf);
            free(frame_4bpp);
            return PlaybackResult::OOM;
        }
    }

    SemaphoreHandle_t sem_free  = xSemaphoreCreateCounting(RING_SLOTS, RING_SLOTS);
    SemaphoreHandle_t sem_ready = xSemaphoreCreateCounting(RING_SLOTS, 0);
    volatile bool cancel = false;

    ProducerCtx pctx = {};
    pctx.src         = &crc_src;
    pctx.hdr         = &hdr;
    pctx.clut_lut    = clut_lut;
    pctx.frame_4bpp  = frame_4bpp;
    pctx.slots       = slots;
    pctx.sem_free    = sem_free;
    pctx.sem_ready   = sem_ready;
    pctx.cancel      = &cancel;
    pctx.ran_to_eof  = false;

    xTaskCreatePinnedToCore(producerTask, "fbox_dec", 8192, &pctx, 2, &pctx.task, 0);

    Serial.printf("[ANIM] play %u frames @ %u fps\n", hdr.frame_count, hdr.fps);
    uint32_t t_start     = millis();
    uint32_t t_spi_total = 0;
    uint32_t t_wait_total = 0;

    PlaybackResult result = PlaybackResult::OK;
    bool consumer_running = true;
    int slot_idx = 0;
    uint16_t frames_drawn = 0;

    while (consumer_running) {
        uint32_t t_wait_start = millis();
        if (xSemaphoreTake(sem_ready, pdMS_TO_TICKS(2000)) != pdTRUE) {
            Serial.println("[ANIM] consumer timeout waiting for slot");
            result = PlaybackResult::READ_UNDERRUN;
            break;
        }
        t_wait_total += millis() - t_wait_start;

        Slot &s = slots[slot_idx];

        if (s.state == SLOT_EOF) {
            Serial.printf("[ANIM] consumer exit: SLOT_EOF at frame %u (drawn=%u)\n",
                          s.frame_idx, frames_drawn);
            xSemaphoreGive(sem_free);
            break;
        }
        if (s.state == SLOT_UNDER) {
            Serial.printf("[ANIM] consumer exit: SLOT_UNDER at frame %u (drawn=%u)\n",
                          s.frame_idx, frames_drawn);
            result = PlaybackResult::READ_UNDERRUN;
            xSemaphoreGive(sem_free);
            break;
        }
        if (s.state == SLOT_ERR) {
            Serial.printf("[ANIM] consumer exit: SLOT_ERR at frame %u (drawn=%u)\n",
                          s.frame_idx, frames_drawn);
            result = PlaybackResult::DECODE_ERROR;
            xSemaphoreGive(sem_free);
            break;
        }

        uint32_t t_spi = millis();
        displayAnimFrameBegin();
        displayAnimWriteFrame(s.frame_buf);
        displayAnimFrameEnd();
        t_spi_total += millis() - t_spi;
        frames_drawn++;

        xSemaphoreGive(sem_free);
        slot_idx = (slot_idx + 1) % RING_SLOTS;

        // Pace + poll touch. Touch poll runs every ~2ms during the pacing window.
        uint32_t elapsed = millis() - t_spi;
        uint32_t wait    = elapsed < frame_ms ? frame_ms - elapsed : 0;
        uint32_t until   = millis() + wait;
        do {
            handleTouch();
            if (touchZ > 0) {
                Serial.printf("[ANIM] consumer exit: USER_CANCELLED at frame_idx=%u drawn=%u\n",
                              s.frame_idx, frames_drawn);
                result = PlaybackResult::USER_CANCELLED;
                consumer_running = false;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(2));
        } while (millis() < until);
    }

    // Tear down producer
    cancel = true;
    // Drain any pending sem_free posts so the producer can wake and exit.
    for (int i = 0; i < RING_SLOTS + 1; i++) xSemaphoreGive(sem_free);
    for (int i = 0; i < 200; i++) {
        if (eTaskGetState(pctx.task) == eDeleted) break;
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    uint32_t elapsed_ms = millis() - t_start;
    Serial.printf("[ANIM] done: drawn=%u/%u elapsed=%lums fps=%.1f spi_avg=%lums wait_avg=%lums\n",
                  frames_drawn, hdr.frame_count, (unsigned long)elapsed_ms,
                  elapsed_ms ? (frames_drawn * 1000.0f / elapsed_ms) : 0.0f,
                  frames_drawn ? (unsigned long)(t_spi_total / frames_drawn) : 0ul,
                  frames_drawn ? (unsigned long)(t_wait_total / frames_drawn) : 0ul);

    // Audio decode (buffer-only) — only valid if the producer reached EOF, since
    // otherwise the source cursor isn't at the audio section.
    FboxAudio audio = {};
    bool audio_attempted = false;
    if (pctx.ran_to_eof && hdr.audio_size > 0) {
        // KNOWN LIMITATION: any bytes still buffered inside the producer's
        // persistent FboxRleReader at video EOF are the first bytes of the
        // audio section. Audio decode reads from crc_src directly, so those
        // bytes are skipped → wrong predictor/step_index seed → decoded PCM
        // is garbage. Fine for now because we have no I2S DAC hardware, but
        // when audio playback lands we need a Buffered source shared by RLE
        // and audio so the handoff is clean.
        audio_attempted = true;
        fboxDecodeAudio(crc_src, hdr, audio);
    }

    // CRC32 verification (header value 0 = skip)
    if (hdr.crc32 != 0 && pctx.ran_to_eof &&
        (hdr.audio_size == 0 || audio_attempted))
    {
        uint32_t got = crc_src.crc();
        if (got != hdr.crc32) {
            Serial.printf("[ANIM] CRC mismatch: got %08lx expected %08lx\n",
                          (unsigned long)got, (unsigned long)hdr.crc32);
            if (result == PlaybackResult::OK) result = PlaybackResult::CRC_MISMATCH;
        } else {
            Serial.printf("[ANIM] CRC OK (%08lx)\n", (unsigned long)got);
        }
    }

    fboxAudioFree(audio);
    for (int i = 0; i < RING_SLOTS; i++) free(slots[i].frame_buf);
    free(frame_4bpp);
    vSemaphoreDelete(sem_free);
    vSemaphoreDelete(sem_ready);

    changeScreenContext(SCREEN_CANVAS);
    return result;
}

PlaybackResult playFboxAnimationFromSD(const char *path)
{
    FboxSourceSD src(path);
    if (!src.ok()) {
        Serial.printf("playFboxAnimationFromSD: cannot open %s\n", path);
        return PlaybackResult::READ_UNDERRUN;
    }
    return playFboxAnimation(src);
}

std::vector<std::string> sdGetFboxFiles()
{
    std::vector<std::string> fileNames;
    File root = SD.open("/sketches/saved");
    if (root)
    {
        File entry;
        while (entry = root.openNextFile())
        {
            if (!entry.isDirectory())
            {
                Serial.println("Found file: " + String(entry.name()));
                fileNames.push_back(entry.name());
            }
            entry.close();
        }
        root.close();
    }
    return fileNames;
}
