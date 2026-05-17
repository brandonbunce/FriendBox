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
#include <esp_timer.h>

Preferences nvs;


bool initNVS()
{
  nvs.begin("Friendbox", true);
  nvs.end();
  return true;
}

bool initSD(bool forceFormat)
{
#ifdef FRIENDBOX_DEBUG_MODE
    Serial.println("INFO: Initializing SD (SDIO 4-bit)...");
#endif
    // SDIO 4-bit mode at 40 MHz. ~10 MB/s effective vs ~1.3 MB/s on the
    // previous SD-over-SPI path. setPins must be called before begin.
    if (!SD_MMC.setPins(SD_CLK, SD_CMD, SD_DAT0, SD_DAT1, SD_DAT2, SD_DAT3)) {
#ifdef FRIENDBOX_DEBUG_MODE
        Serial.println("ERROR: SD_MMC.setPins failed");
#endif
        return false;
    }
    // mode1bit=false → 4-bit. format_if_mount_failed=false. Frequency
    // SDMMC_FREQ_HIGHSPEED = 40 MHz. mountpoint "/sd" matches the historical
    // path layout (e.g. "/sketches/received/...").
    if (!SD_MMC.begin("/sd", /*mode1bit=*/false, /*format_if_mount_failed=*/false,
                      SDMMC_FREQ_HIGHSPEED, /*maxOpenFiles=*/5)) {
#ifdef FRIENDBOX_DEBUG_MODE
        Serial.println("ERROR: SD_MMC.begin failed");
#endif
        return false;
    }
#ifdef FRIENDBOX_DEBUG_MODE
    sdcard_type_t ct = SD_MMC.cardType();
    const char *type_s = (ct == CARD_NONE)  ? "NONE"
                       : (ct == CARD_MMC)   ? "MMC"
                       : (ct == CARD_SD)    ? "SDSC"
                       : (ct == CARD_SDHC)  ? "SDHC"
                                            : "UNKNOWN";
    Serial.printf("INFO: SD_MMC ready — type=%s size=%lluMB freq=%dkHz\n",
                  type_s, SD_MMC.cardSize() / (1024 * 1024), SDMMC_FREQ_HIGHSPEED);
#endif
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

// Token-driven frame decoder. Replaces the per-pixel loop with per-RLE-token
// dispatch so RUN tokens hit memset/bulk fast paths instead of paying the
// rle.next() + nibble shuffle + writeback cost 230,400 times per frame. On
// motion-sparse content (large unchanged backgrounds compress to RUN tokens
// with raw=0) this cuts decode from ~100 ms to ~20–30 ms.
//
// clut_pair[byte] = clut_lut[hi_nib] | (clut_lut[lo_nib] << 8). One 16-bit
// store per source byte lands two CLUT-mapped pixels in fb at once — halves
// the PSRAM write count vs two 8-bit stores. Little-endian layout means low
// byte = fb[i], high byte = fb[i+1].
//
// Returns true on success, false on read/decode error (sets *err_under to
// true if the underlying source returned -1, false on malformed RLE).
static bool decodeFrameTokens(FboxRleReader &rle, bool is_pf,
                              uint8_t *fb, uint8_t *f4,
                              const uint8_t *clut_lut,
                              const uint16_t *clut_pair,
                              uint32_t pixel_count, bool *err_under)
{
    uint32_t i = 0;
    *err_under = false;
    while (i < pixel_count) {
        int h = rle.read_byte();
        if (h < 0) { *err_under = rle.err; return false; }
        uint32_t count = (uint32_t)(h & 0x7F) + 1u;
        if (i + count > pixel_count) return false;  // malformed: token overflows frame

        if (h & 0x80) {
            // ─── RUN ───────────────────────────────────────────────────────
            int rb = rle.read_byte();
            if (rb < 0) { *err_under = rle.err; return false; }
            uint8_t raw = (uint8_t)rb & 0x0F;

            if (!is_pf) {
                // I-frame run: memset both buffers.
                uint8_t clut_byte = clut_lut[raw];
                memset(fb + i, clut_byte, count);

                // f4 holds 2 nibbles/byte; handle leading odd-pixel + bulk + trailing odd.
                uint32_t s = i, n = count;
                if (s & 1) {
                    f4[s >> 1] = (f4[s >> 1] & 0xF0) | raw;
                    s++; n--;
                }
                if (n >> 1) memset(f4 + (s >> 1), (uint8_t)((raw << 4) | raw), n >> 1);
                if (n & 1)  f4[(s + (n & ~1u)) >> 1] = (uint8_t)((raw << 4) | (f4[(s + (n & ~1u)) >> 1] & 0x0F));
                i += count;
            } else if (raw == 0) {
                // P-frame "no change" run: f4 stays; rebuild fb from existing
                // f4 nibbles. 4-pixel chunk via uint16 f4 load, 2-pixel tail
                // via byte load.
                uint32_t end = i + count;
                if (i & 1) {
                    fb[i] = clut_lut[f4[i >> 1] & 0x0F];
                    i++;
                }
                while ((i & 3) && i + 1 < end) {
                    *(uint16_t *)(fb + i) = clut_pair[f4[i >> 1]];
                    i += 2;
                }
                while (i + 3 < end) {
                    uint16_t two_f4 = *(uint16_t *)(f4 + (i >> 1));
                    uint32_t lo = clut_pair[(uint8_t)two_f4];
                    uint32_t hi = clut_pair[(uint8_t)(two_f4 >> 8)];
                    *(uint32_t *)(fb + i) = lo | (hi << 16);
                    i += 4;
                }
                while (i + 1 < end) {
                    *(uint16_t *)(fb + i) = clut_pair[f4[i >> 1]];
                    i += 2;
                }
                if (i < end) {
                    fb[i] = clut_lut[(f4[i >> 1] >> 4) & 0x0F];
                    i++;
                }
            } else {
                // P-frame run with non-zero delta: XOR raw into each f4 nibble.
                uint8_t pair_xor = (uint8_t)((raw << 4) | raw);
                uint32_t end = i + count;
                if (i & 1) {
                    uint8_t b  = f4[i >> 1];
                    uint8_t nb = (b & 0x0F) ^ raw;
                    f4[i >> 1] = (b & 0xF0) | nb;
                    fb[i] = clut_lut[nb];
                    i++;
                }
                while (i + 1 < end) {
                    uint8_t b  = f4[i >> 1] ^ pair_xor;
                    f4[i >> 1] = b;
                    *(uint16_t *)(fb + i) = clut_pair[b];
                    i += 2;
                }
                if (i < end) {
                    uint8_t b  = f4[i >> 1];
                    uint8_t nb = ((b >> 4) & 0x0F) ^ raw;
                    f4[i >> 1] = (nb << 4) | (b & 0x0F);
                    fb[i] = clut_lut[nb];
                    i++;
                }
            }
        } else {
            // ─── LITERAL ───────────────────────────────────────────────────
            // count nibbles packed high-first into ceil(count/2) source bytes.
            // Fast path: when i is even and ≥2 pixels remain, each source byte
            // maps 1:1 to one f4 byte (XOR-equal for P-frames, overwrite for
            // I-frames), and the two nibbles decode to fb[i] and fb[i+1] via
            // CLUT lookup with NO nibble masking. ~3–4× faster than scalar.
            uint32_t remaining = count;

            // Leading half-byte: if i is odd, consume the high nibble of one
            // source byte to realign, then the low nibble (now at even i).
            if ((i & 1) && remaining > 0) {
                int b = rle.read_byte();
                if (b < 0) { *err_under = rle.err; return false; }
                uint8_t hi = (uint8_t)((b >> 4) & 0x0F);
                uint8_t lo = (uint8_t)(b & 0x0F);
                // hi at odd i  → low nibble of f4[i>>1]
                {
                    uint8_t prev = f4[i >> 1];
                    uint8_t pn   = prev & 0x0F;
                    uint8_t cn   = is_pf ? (uint8_t)(hi ^ pn) : hi;
                    f4[i >> 1] = (uint8_t)((prev & 0xF0) | cn);
                    fb[i] = clut_lut[cn];
                    i++; remaining--;
                }
                if (remaining > 0) {
                    // lo at even i → high nibble of f4[i>>1]
                    uint8_t prev = f4[i >> 1];
                    uint8_t pn   = (uint8_t)((prev >> 4) & 0x0F);
                    uint8_t cn   = is_pf ? (uint8_t)(lo ^ pn) : lo;
                    f4[i >> 1] = (uint8_t)((cn << 4) | (prev & 0x0F));
                    fb[i] = clut_lut[cn];
                    i++; remaining--;
                }
            }

            // Aligned bulk: 2 pixels per source byte. Pull bytes directly
            // from rle.buf in chunks to avoid per-byte function-call overhead.
            //
            // Fast path: when i is 4-aligned and ≥2 source bytes are buffered,
            // process 2 source bytes (4 pixels) per iteration — one 32-bit
            // fb store + one 16-bit f4 update covers four pixels with no
            // nibble masking. Cuts the inner-loop trip count in half over the
            // 2-pixel path; observed ~30 % decoder-CPU saving on dithered.
            while (remaining >= 2) {
                if (rle.buf_pos >= rle.buf_fill) {
                    uint64_t rt = esp_timer_get_time();
                    int r = rle.src->read(rle.buf, sizeof(rle.buf));
                    rle.refill_t_us += esp_timer_get_time() - rt;
                    rle.refill_n++;
                    if (r <= 0) { *err_under = (r < 0); rle.buf_fill = 0; return false; }
                    rle.buf_fill = r; rle.buf_pos = 0;
                }
                int available    = rle.buf_fill - rle.buf_pos;
                int pair_pixels  = (int)(remaining >> 1);
                int take         = available < pair_pixels ? available : pair_pixels;
                const uint8_t *src_ptr = rle.buf + rle.buf_pos;

                int k = 0;
                // 4-pixel aligned inner loop. Requires i % 4 == 0; if i is
                // 2 mod 4 after the leading-half realign, do one 2-pixel
                // iteration first to align (handled below outside this if).
                // (Tried an 8-pixel/uint32 unroll; was within noise, no win.)
                if ((i & 3) == 0 && take >= 2) {
                    int quads = take >> 1;
                    if (is_pf) {
                        for (int q = 0; q < quads; q++) {
                            uint8_t s0 = src_ptr[k];
                            uint8_t s1 = src_ptr[k + 1];
                            uint8_t f0 = (uint8_t)(f4[(i >> 1)]     ^ s0);
                            uint8_t f1 = (uint8_t)(f4[(i >> 1) + 1] ^ s1);
                            *(uint16_t *)(f4 + (i >> 1)) =
                                (uint16_t)f0 | ((uint16_t)f1 << 8);
                            uint32_t lo = clut_pair[f0];
                            uint32_t hi = clut_pair[f1];
                            *(uint32_t *)(fb + i) = lo | (hi << 16);
                            i += 4;
                            k += 2;
                        }
                    } else {
                        for (int q = 0; q < quads; q++) {
                            uint8_t s0 = src_ptr[k];
                            uint8_t s1 = src_ptr[k + 1];
                            *(uint16_t *)(f4 + (i >> 1)) =
                                (uint16_t)s0 | ((uint16_t)s1 << 8);
                            uint32_t lo = clut_pair[s0];
                            uint32_t hi = clut_pair[s1];
                            *(uint32_t *)(fb + i) = lo | (hi << 16);
                            i += 4;
                            k += 2;
                        }
                    }
                }
                // Tail: any remaining bytes through the 2-pixel path.
                if (is_pf) {
                    for (; k < take; k++) {
                        uint8_t new_byte = (uint8_t)(f4[i >> 1] ^ src_ptr[k]);
                        f4[i >> 1] = new_byte;
                        *(uint16_t *)(fb + i) = clut_pair[new_byte];
                        i += 2;
                    }
                } else {
                    for (; k < take; k++) {
                        uint8_t new_byte = src_ptr[k];
                        f4[i >> 1] = new_byte;
                        *(uint16_t *)(fb + i) = clut_pair[new_byte];
                        i += 2;
                    }
                }
                rle.buf_pos += take;
                remaining   -= (uint32_t)(take << 1);
            }

            // Trailing single pixel (only the high nibble of one source byte).
            if (remaining > 0) {
                int b = rle.read_byte();
                if (b < 0) { *err_under = rle.err; return false; }
                uint8_t hi = (uint8_t)((b >> 4) & 0x0F);
                uint8_t prev = f4[i >> 1];
                uint8_t pn   = (uint8_t)((prev >> 4) & 0x0F);
                uint8_t cn   = is_pf ? (uint8_t)(hi ^ pn) : hi;
                f4[i >> 1] = (uint8_t)((cn << 4) | (prev & 0x0F));
                fb[i] = clut_lut[cn];
                i++; remaining--;
            }
        }
    }
    return true;
}

struct ProducerCtx {
    FboxSource         *src;            // CRC-wrapped source
    const FboxHeader   *hdr;
    const uint8_t      *clut_lut;
    const uint16_t     *clut_pair;      // 256-entry pair lookup, hot in decode
    uint8_t            *frame_4bpp;     // INTERNAL SRAM, PIXEL_COUNT/2 bytes — XOR baseline
    Slot               *slots;
    SemaphoreHandle_t   sem_free;
    SemaphoreHandle_t   sem_ready;
    volatile bool      *cancel;
    bool                ran_to_eof;
    TaskHandle_t        task;
    // Profiling totals (µs)
    uint64_t            t_decode_us;    // per-pixel decode loop only
    uint64_t            t_msync_us;     // esp_cache_msync
    uint64_t            t_wait_free_us; // blocked on sem_free
    uint64_t            t_refill_us;    // src->read calls during decode (SD/HTTP/PSRAM)
    uint32_t            n_refills;      // count of src->read calls
    uint32_t            n_frames;       // number of frames measured
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

        uint64_t t_wait_start = esp_timer_get_time();
        while (xSemaphoreTake(ctx->sem_free, pdMS_TO_TICKS(50)) != pdTRUE) {
            if (*ctx->cancel) goto producer_exit;
        }
        ctx->t_wait_free_us += esp_timer_get_time() - t_wait_start;

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

            // Reader keeps its chunk buffer; we still need to reset the
            // per-frame token state in case a previous frame ended mid-token
            // due to truncation (it shouldn't in well-formed files, but the
            // reset is cheap and defensive).
            rle.beginFrame((int)PIXEL_COUNT);

            uint64_t rt_before = rle.refill_t_us;
            uint32_t rn_before = rle.refill_n;
            uint64_t t_decode_start = esp_timer_get_time();
            bool err_under = false;
            bool dec_ok = decodeFrameTokens(rle, is_pf, fb, f4, ctx->clut_lut,
                                            ctx->clut_pair, PIXEL_COUNT, &err_under);
            ctx->t_decode_us += esp_timer_get_time() - t_decode_start;
            ctx->t_refill_us += rle.refill_t_us - rt_before;
            ctx->n_refills   += rle.refill_n    - rn_before;
            ctx->n_frames++;

            if (!dec_ok) {
                error     = true;
                err_state = err_under ? SLOT_UNDER : SLOT_ERR;
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
        uint64_t t_msync_start = esp_timer_get_time();
        esp_cache_msync(s.frame_buf, PIXEL_COUNT, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        ctx->t_msync_us += esp_timer_get_time() - t_msync_start;

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

    uint8_t  clut_lut[16];
    uint16_t clut_pair[256];
    for (int i = 0; i < 16; i++) {
        uint16_t c = draw_color_palette[i];
        clut_lut[i] = (uint8_t)((((c >> 13) & 7) << 5) |
                                (((c >>  8) & 7) << 2) |
                                 ((c >>  3) & 3));
    }
    // Precompute every 4bpp→8bpp nibble pair. clut_pair[byte] packs the two
    // mapped pixels into a little-endian uint16 so the decoder can do one
    // aligned 16-bit store to fb per source byte instead of two 8-bit stores.
    for (int b = 0; b < 256; b++) {
        clut_pair[b] = (uint16_t)clut_lut[(b >> 4) & 0x0F]
                     | ((uint16_t)clut_lut[b & 0x0F] << 8);
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
    pctx.clut_pair   = clut_pair;
    pctx.frame_4bpp  = frame_4bpp;
    pctx.slots       = slots;
    pctx.sem_free    = sem_free;
    pctx.sem_ready   = sem_ready;
    pctx.cancel      = &cancel;
    pctx.ran_to_eof  = false;
    pctx.t_decode_us = 0;
    pctx.t_msync_us  = 0;
    pctx.t_wait_free_us = 0;
    pctx.t_refill_us = 0;
    pctx.n_refills   = 0;
    pctx.n_frames    = 0;

    // Stack 16 KB: the producer's FboxRleReader local (≈4 KB buf) + decode
    // helpers + FreeRTOS overhead won't fit comfortably in the default 8 KB.
    xTaskCreatePinnedToCore(producerTask, "fbox_dec", 16384, &pctx, 2, &pctx.task, 0);

    Serial.printf("[ANIM] play %u frames @ %u fps\n", hdr.frame_count, hdr.fps);
    uint32_t t_start     = millis();
    uint32_t t_spi_total = 0;
    uint32_t t_wait_total = 0;

    PlaybackResult result = PlaybackResult::OK;
    bool consumer_running = true;
    int slot_idx = 0;
    uint16_t frames_drawn = 0;
    TickType_t prev_wake = xTaskGetTickCount();

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

        // Touch poll once per frame (latency ≤ frame_ms, fine at 24 fps).
        handleTouch();
        if (touchZ > 0) {
            Serial.printf("[ANIM] consumer exit: USER_CANCELLED at frame_idx=%u drawn=%u\n",
                          s.frame_idx, frames_drawn);
            result = PlaybackResult::USER_CANCELLED;
            consumer_running = false;
            break;
        }

        // Precise pacing: vTaskDelayUntil holds an absolute wake time so any
        // per-iteration overshoot is absorbed instead of accumulated. The old
        // do-while + vTaskDelay(2) loop overshot `until` by up to one tick per
        // frame → ~1 ms/frame slip vs the 24 fps target.
        vTaskDelayUntil(&prev_wake, pdMS_TO_TICKS(frame_ms));
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
    if (pctx.n_frames) {
        uint64_t refill_total = pctx.t_refill_us;
        Serial.printf("[ANIM] producer: decode_avg=%llums msync_avg=%lluus wait_free_avg=%lluus refill_avg=%lluus (n=%u)\n",
                      pctx.t_decode_us  / pctx.n_frames / 1000,
                      pctx.t_msync_us   / pctx.n_frames,
                      pctx.t_wait_free_us / pctx.n_frames,
                      refill_total / pctx.n_frames,
                      pctx.n_frames);
        Serial.printf("[ANIM] producer: refills/frame=%.1f refill_us/call=%llu\n",
                      pctx.n_refills * 1.0f / pctx.n_frames,
                      pctx.n_refills ? (refill_total / pctx.n_refills) : 0);
    }

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

PlaybackResult playFboxAnimationFromSDBuffered(const char *path, uint32_t ring_bytes)
{
    FboxSourceSD sd_src(path);
    if (!sd_src.ok()) {
        Serial.printf("playFboxAnimationFromSDBuffered: cannot open %s\n", path);
        return PlaybackResult::READ_UNDERRUN;
    }
    FboxSourceRingBuffered buf_src(&sd_src, ring_bytes);
    if (!buf_src.ok()) {
        Serial.println("playFboxAnimationFromSDBuffered: ring alloc failed, falling back to direct SD");
        return playFboxAnimation(sd_src);
    }
    Serial.printf("[ANIM] ring buffer: %lu KB PSRAM, async SD loader on core 1\n",
                  (unsigned long)(ring_bytes / 1024));
    PlaybackResult result = playFboxAnimation(buf_src);
    Serial.printf("[ANIM] ring stalls=%u stall_time=%llums\n",
                  buf_src.stallCount(), buf_src.stallTimeUs() / 1000);
    return result;
}

std::vector<std::string> sdGetFboxFiles()
{
    std::vector<std::string> fileNames;
    File root = SD_MMC.open("/sketches/saved");
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
