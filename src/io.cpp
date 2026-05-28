#include "io.hpp"
#include "display.hpp"
#include "canvas.hpp"
#include "ui_core.hpp"
#include "audio.hpp"
#include "audio_i2s.hpp"
#include "fbox_source.hpp"
#include "idf_compat.hpp"

#include <vector>
#include <string>
#include <sys/stat.h>
#include <dirent.h>
#include <stdio.h>

#include <driver/gpio.h>
#include <driver/sdmmc_host.h>
#include <esp_vfs_fat.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_rom_crc.h>
#include <esp_heap_caps.h>
#include <esp_cache.h>
#include <esp_timer.h>

NvsStore nvs;
static sdmmc_card_t *s_sd_card = nullptr;


bool initNVS()
{
  // nvs_flash_init() already ran in app_main; this just sanity-opens the
  // namespace to confirm the "Friendbox" key store exists.
  nvs.begin("Friendbox", true);
  nvs.end();
  return true;
}

bool initSD(bool forceFormat)
{
#ifdef FRIENDBOX_DEBUG_MODE
    puts("INFO: Initializing SD (SDIO 4-bit)...");
#endif
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags        = SDMMC_HOST_FLAG_4BIT;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    host.slot         = SDMMC_HOST_SLOT_1;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk   = (gpio_num_t)SD_CLK;
    slot.cmd   = (gpio_num_t)SD_CMD;
    slot.d0    = (gpio_num_t)SD_DAT0;
    slot.d1    = (gpio_num_t)SD_DAT1;
    slot.d2    = (gpio_num_t)SD_DAT2;
    slot.d3    = (gpio_num_t)SD_DAT3;
    slot.width = 4;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = forceFormat,
        .max_files              = 5,
        .allocation_unit_size   = 16 * 1024,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };

    // Mount at "/sd" so paths like "/sd/sketches/received/..." keep working;
    // FatFS drive number "0:" is what the raw FATFS callers in fbox_source.cpp
    // expect (esp_vfs_fat_sdmmc_mount registers it as drive 0 by default).
    esp_err_t err = esp_vfs_fat_sdmmc_mount("/sd", &host, &slot, &mount_cfg, &s_sd_card);
    if (err != ESP_OK) {
#ifdef FRIENDBOX_DEBUG_MODE
        printf("ERROR: esp_vfs_fat_sdmmc_mount failed: 0x%x\n", err);
#endif
        s_sd_card = nullptr;
        return false;
    }
#ifdef FRIENDBOX_DEBUG_MODE
    if (s_sd_card) {
        uint64_t size_mb = ((uint64_t)s_sd_card->csd.capacity *
                            (uint64_t)s_sd_card->csd.sector_size) / (1024ULL * 1024ULL);
        printf("INFO: SD ready — type=%s size=%lluMB freq=%dkHz\n",
               (s_sd_card->is_mmc ? "MMC" : (s_sd_card->ocr & (1 << 30) ? "SDHC" : "SDSC")),
               (unsigned long long)size_mb,
               s_sd_card->max_freq_khz);
    }
#endif
    return true;
}

bool initMenuButton() {
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << (uint32_t)HALL_SENSOR_PIN;
    io.mode         = GPIO_MODE_INPUT;
    io.pull_up_en   = GPIO_PULLUP_ENABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type    = GPIO_INTR_DISABLE;
    gpio_config(&io);
    return true;
}

void handleMenuButton(bool recheckInput)
{
    if (currentScreen == SCREEN_CANVAS || currentScreen == SCREEN_CANVAS_MENU)
    {
        static unsigned long lastPress = 0;
        static unsigned int lastButtonState = 0;
        static bool alreadyPressed = false;
        if (gpio_get_level((gpio_num_t)HALL_SENSOR_PIN) == 0) /*Button Pressed*/
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

bool fboxReadHeader(FboxSource &src, FboxHeader &out, uint32_t *crc_seed_out)
{
    // Heap-allocated buffer: 512 bytes on the loopTask stack pushed it into
    // the canary band when combined with the rest of playFboxAnimation's
    // locals + a printf-driven vsnprintf frame downstream.
    uint8_t *hdr = (uint8_t *)heap_caps_malloc(FBOX_HEADER_SIZE,
                                               MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!hdr) {
        Serial.println("[FBOX] header buffer alloc failed");
        return false;
    }

    size_t got = 0;
    while (got < FBOX_HEADER_SIZE) {
        int r = src.read(hdr + got, FBOX_HEADER_SIZE - got);
        if (r <= 0) { free(hdr); return false; }
        got += (size_t)r;
    }
    if (memcmp(hdr, "FBOX", 4) != 0) {
        Serial.println("[FBOX] bad magic");
        free(hdr); return false;
    }
    if (hdr[4] != FBOX_VERSION) {
        Serial.printf("[FBOX] unsupported version %u (expected %u)\n",
                      (unsigned)hdr[4], (unsigned)FBOX_VERSION);
        free(hdr); return false;
    }

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
    out.expected_file_size  = (uint32_t)hdr[50] | ((uint32_t)hdr[51] << 8)
                            | ((uint32_t)hdr[52] << 16) | ((uint32_t)hdr[53] << 24);
    out.audio_sample_rate   = (uint16_t)hdr[54] | ((uint16_t)hdr[55] << 8);
    out.audio_channels      = hdr[56];
    // hdr[57] reserved
    out.audio_samples_per_frame = (uint16_t)hdr[58] | ((uint16_t)hdr[59] << 8);
    out.crc32 = (uint32_t)hdr[60] | ((uint32_t)hdr[61] << 8)
              | ((uint32_t)hdr[62] << 16) | ((uint32_t)hdr[63] << 24);
    out.keyframe_count        = (uint16_t)hdr[64] | ((uint16_t)hdr[65] << 8);
    out.keyframe_table_offset = (uint32_t)hdr[66] | ((uint32_t)hdr[67] << 8)
                              | ((uint32_t)hdr[68] << 16) | ((uint32_t)hdr[69] << 24);
    // hdr[70..71] reserved alignment
    memcpy(out.description, &hdr[72], 256);
    out.description[255] = '\0';

    if (crc_seed_out) {
        *crc_seed_out = esp_rom_crc32_le(0, hdr + 64, FBOX_HEADER_SIZE - 64);
    }
    free(hdr);

    uint32_t src_size = src.size();
    if (src_size != 0 && out.expected_file_size != 0 &&
        src_size != out.expected_file_size)
    {
        Serial.printf("[FBOX] file-size mismatch: got %lu, header expects %lu\n",
                      (unsigned long)src_size, (unsigned long)out.expected_file_size);
        // Streaming sources report size=0; only fail if size is known.
        return false;
    }

    if (out.audio_sample_rate > 0 && out.fps > 0) {
        uint16_t expected = (uint16_t)(out.audio_sample_rate / out.fps);
        if (out.audio_samples_per_frame != expected) {
            Serial.printf("[FBOX] warn: audio_samples_per_frame=%u, expected %u from rate/fps\n",
                          out.audio_samples_per_frame, expected);
        }
    }
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

    // v4 per-frame layout: [type][audio block][video]. Skip the audio block
    // for sketch import — audio doesn't apply to single-frame canvas loads.
    if (hdr.audio_samples_per_frame > 0) {
        uint32_t audio_bytes = 4u + ((uint32_t)hdr.audio_samples_per_frame + 1u) / 2u;
        while (audio_bytes > 0) {
            int r = src.read(skip_buf, audio_bytes > sizeof(skip_buf) ? sizeof(skip_buf) : audio_bytes);
            if (r <= 0) { Serial.println("loadSketchFromSD: skip audio failed"); return; }
            audio_bytes -= (uint32_t)r;
        }
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
    SLOT_SKIP   = 4,  // v4: 'S' frame — consumer reuses previous frame on LT7680
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

            // Leading half-byte: if i is odd, consume one source byte for two
            // pixels — HIGH nibble lands on the odd-i pixel (LOW nib of
            // f4[i>>1]), LOW nibble on the following even-i pixel (HIGH nib of
            // f4[(i+1)>>1]). i advances by 2 and STAYS ODD; the bulk-tail and
            // trailing-single below have odd-i variants for that reason. (An
            // earlier version of this comment claimed it "realigns" to even.
            // It doesn't — and the bulk/trailing fast paths used to silently
            // misaddress f4 nibbles when reached at odd i, corrupting one
            // adjacent pixel per source byte. That manifested as accumulating
            // noise on dithered P-frame content between I-frame resets.)
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
                // Tail: any remaining bytes through the 2-pixel path. Two
                // shapes — even-i is the fast path (1 f4 byte per source
                // byte, single 16-bit fb store); odd-i has to straddle two
                // f4 bytes (LOW of f4[i>>1] + HIGH of f4[i>>1 + 1]) but the
                // fb store is still byte-addressable so it stays cheap.
                if (is_pf) {
                    if (i & 1) {
                        for (; k < take; k++) {
                            uint8_t s    = src_ptr[k];
                            uint8_t s_hi = (uint8_t)((s >> 4) & 0x0F);
                            uint8_t s_lo = (uint8_t)(s & 0x0F);
                            uint8_t b0   = f4[i >> 1];
                            uint8_t nl   = (uint8_t)((b0 & 0x0F) ^ s_hi);
                            f4[i >> 1]   = (uint8_t)((b0 & 0xF0) | nl);
                            fb[i]        = clut_lut[nl];
                            i++;
                            uint8_t b1   = f4[i >> 1];
                            uint8_t nh   = (uint8_t)(((b1 >> 4) & 0x0F) ^ s_lo);
                            f4[i >> 1]   = (uint8_t)((nh << 4) | (b1 & 0x0F));
                            fb[i]        = clut_lut[nh];
                            i++;
                        }
                    } else {
                        for (; k < take; k++) {
                            uint8_t new_byte = (uint8_t)(f4[i >> 1] ^ src_ptr[k]);
                            f4[i >> 1] = new_byte;
                            *(uint16_t *)(fb + i) = clut_pair[new_byte];
                            i += 2;
                        }
                    }
                } else {
                    if (i & 1) {
                        for (; k < take; k++) {
                            uint8_t s    = src_ptr[k];
                            uint8_t s_hi = (uint8_t)((s >> 4) & 0x0F);
                            uint8_t s_lo = (uint8_t)(s & 0x0F);
                            uint8_t b0   = f4[i >> 1];
                            f4[i >> 1]   = (uint8_t)((b0 & 0xF0) | s_hi);
                            fb[i]        = clut_lut[s_hi];
                            i++;
                            uint8_t b1   = f4[i >> 1];
                            f4[i >> 1]   = (uint8_t)((s_lo << 4) | (b1 & 0x0F));
                            fb[i]        = clut_lut[s_lo];
                            i++;
                        }
                    } else {
                        for (; k < take; k++) {
                            uint8_t new_byte = src_ptr[k];
                            f4[i >> 1] = new_byte;
                            *(uint16_t *)(fb + i) = clut_pair[new_byte];
                            i += 2;
                        }
                    }
                }
                rle.buf_pos += take;
                remaining   -= (uint32_t)(take << 1);
            }

            // Trailing single pixel (only the high nibble of one source byte;
            // the low nibble is the encoder's odd-count padding). Lands on
            // HIGH nib of f4[i>>1] when i is even, LOW nib when i is odd —
            // odd i happens when the literal started odd and bulk advanced
            // by an odd number of pixel-pairs.
            if (remaining > 0) {
                int b = rle.read_byte();
                if (b < 0) { *err_under = rle.err; return false; }
                uint8_t hi = (uint8_t)((b >> 4) & 0x0F);
                uint8_t prev = f4[i >> 1];
                if (i & 1) {
                    uint8_t pn = (uint8_t)(prev & 0x0F);
                    uint8_t cn = is_pf ? (uint8_t)(hi ^ pn) : hi;
                    f4[i >> 1] = (uint8_t)((prev & 0xF0) | cn);
                    fb[i] = clut_lut[cn];
                } else {
                    uint8_t pn = (uint8_t)((prev >> 4) & 0x0F);
                    uint8_t cn = is_pf ? (uint8_t)(hi ^ pn) : hi;
                    f4[i >> 1] = (uint8_t)((cn << 4) | (prev & 0x0F));
                    fb[i] = clut_lut[cn];
                }
                i++; remaining--;
            }
        }
    }
    return true;
}

struct KeyframeEntry {
    uint32_t frame_index;
    uint32_t byte_offset;
};

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
    // v4: per-frame chunk sizes (from frame_size_table). Read once before producer
    // task starts. Owned by playFboxAnimation; producer reads only.
    const uint32_t     *frame_sizes;
    // v4: ADPCM block size derived from hdr->audio_samples_per_frame. 0 = no audio.
    uint32_t            audio_block_bytes;
    // v4: scratch buffers — kept off the producer task stack to avoid blowing
    // the 16 KB budget. Owned by playFboxAnimation; producer uses but doesn't free.
    uint8_t            *audio_block_scratch;   // audio_block_bytes (PSRAM)
    int16_t            *pcm_scratch;           // hdr->audio_samples_per_frame samples (PSRAM)
    // v4: keyframe table parsed at file end (after last frame). 0 = no table.
    std::vector<KeyframeEntry> *keyframes;
    // Profiling totals (µs)
    uint64_t            t_decode_us;    // per-pixel decode loop only
    uint64_t            t_msync_us;     // esp_cache_msync
    uint64_t            t_wait_free_us; // blocked on sem_free
    uint64_t            t_refill_us;    // src->read calls during decode (SD/HTTP/PSRAM)
    uint64_t            t_adpcm_us;     // ADPCM block decode time
    uint32_t            n_refills;      // count of src->read calls
    uint32_t            n_frames;       // number of frames measured
    uint32_t            n_skip_frames;  // count of 'S' frames seen
};

} // namespace (close so the playback menu state is reachable from
  //            playFboxAnimationFromSD's loop wrapper below)

// ── Playback menu overlay ───────────────────────────────────────────────────
// Tap-on-animation pops up a centered menu: 4 stacked main buttons (Stop /
// Pause / Loop / Restart) plus a horizontal row of 5 volume presets
// (0/25/50/75/100). Drawn on top of each animation frame on the BACK ANIM
// buffer after the SPI burst but before the page flip — so it lands on
// whichever buffer is about to be scanned out, regardless of the page-flip
// alternation. While paused, the consumer holds its last non-SKIP slot and
// re-blits it every frame so the menu stays composited on top.
//
// State persists across playFboxAnimation calls — `loop_enabled` survives a
// Restart, and any subsequent call from playFboxAnimationFromSD honors it.
// Volume lives in audio_i2s.cpp and also persists across calls. Per-call state
// (menu_open, paused, stop_requested, restart_requested) is reset at the top
// of playFboxAnimation.

enum PlaybackMenuButton : int {
    PB_BTN_STOP    = 0,
    PB_BTN_PAUSE   = 1,
    PB_BTN_LOOP    = 2,
    PB_BTN_RESTART = 3,
    PB_BTN_COUNT   = 4,
};

struct PlaybackController {
    bool menu_open;
    bool paused;
    bool loop_enabled;        // persists across calls; default on
    bool stop_requested;      // one-shot: consumer returns USER_CANCELLED
    bool restart_requested;   // one-shot: consumer returns USER_RESTART
};

static PlaybackController g_pbc = {
    /* menu_open        = */ false,
    /* paused           = */ false,
    /* loop_enabled     = */ true,
    /* stop_requested   = */ false,
    /* restart_requested= */ false,
};

constexpr int PB_MENU_X  = 100;
constexpr int PB_MENU_Y  = 60;
constexpr int PB_MENU_W  = 280;
constexpr int PB_MENU_H  = 360;
constexpr int PB_BTN_X   = 120;
constexpr int PB_BTN_W   = 240;
constexpr int PB_BTN_H   = 50;
constexpr int PB_BTN_GAP = 8;
constexpr int PB_BTN_Y0  = 70;

constexpr int     PB_VOL_COUNT             = 5;
constexpr uint8_t PB_VOL_PCT[PB_VOL_COUNT] = {0, 25, 50, 75, 100};
constexpr int     PB_VOL_GAP               = 4;
constexpr int     PB_VOL_H                 = 50;
// 5 buttons share PB_BTN_W: each is (W - 4 gaps) / 5 = 44 px.
constexpr int     PB_VOL_W                 = (PB_BTN_W - (PB_VOL_COUNT - 1) * PB_VOL_GAP) / PB_VOL_COUNT;
// Placed below the 4 main buttons with a slightly bigger gap as a separator.
constexpr int     PB_VOL_Y                 = PB_BTN_Y0 + PB_BTN_COUNT * (PB_BTN_H + PB_BTN_GAP) + PB_BTN_GAP;

// Hit-test result encoding:
//   PB_HIT_OUTSIDE → tap was outside the menu rect (caller dismisses)
//   PB_HIT_NONE    → tap inside the menu but not on any button (no-op)
//   0..3           → main button index (PB_BTN_*)
//   PB_HIT_VOL0+i  → volume preset index i (indexes PB_VOL_PCT)
constexpr int PB_HIT_OUTSIDE = -2;
constexpr int PB_HIT_NONE    = -1;
constexpr int PB_HIT_VOL0    = 10;

static inline int pb_btn_y(int idx) { return PB_BTN_Y0 + idx * (PB_BTN_H + PB_BTN_GAP); }
static inline int pb_vol_x(int idx) { return PB_BTN_X  + idx * (PB_VOL_W + PB_VOL_GAP); }

static bool pb_rect_contains(int rx, int ry, int rw, int rh, int x, int y)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static int pb_hit_test(int x, int y)
{
    if (!pb_rect_contains(PB_MENU_X, PB_MENU_Y, PB_MENU_W, PB_MENU_H, x, y)) return PB_HIT_OUTSIDE;
    for (int i = 0; i < PB_BTN_COUNT; i++) {
        if (pb_rect_contains(PB_BTN_X, pb_btn_y(i), PB_BTN_W, PB_BTN_H, x, y)) return i;
    }
    for (int i = 0; i < PB_VOL_COUNT; i++) {
        if (pb_rect_contains(pb_vol_x(i), PB_VOL_Y, PB_VOL_W, PB_VOL_H, x, y)) return PB_HIT_VOL0 + i;
    }
    return PB_HIT_NONE;
}

static void pb_handle_tap(PlaybackController &c, int x, int y)
{
    int hit = pb_hit_test(x, y);
    if (hit == PB_HIT_OUTSIDE) { c.menu_open = false; return; }
    if (hit >= PB_HIT_VOL0 && hit < PB_HIT_VOL0 + PB_VOL_COUNT) {
        setI2SVolume(PB_VOL_PCT[hit - PB_HIT_VOL0]);
        return;
    }
    switch (hit) {
        case PB_BTN_STOP:    c.stop_requested    = true; break;
        case PB_BTN_PAUSE:   c.paused            = !c.paused; break;
        case PB_BTN_LOOP:    c.loop_enabled      = !c.loop_enabled; break;
        case PB_BTN_RESTART: c.restart_requested = true; break;
        default:             break;  // PB_HIT_NONE or anything else: no-op
    }
}

// Snapshot of the state that affects what the menu LOOKS like (paused
// label, loop highlight, volume highlight). Used to skip the expensive
// re-render path when nothing changed. menu_open is NOT part of the cache
// signature — opening/closing the menu doesn't change pixels, it only
// toggles whether we composite at all.
struct PlaybackMenuCache {
    bool    valid;
    bool    paused;
    bool    loop_enabled;
    uint8_t volume_pct;
};
static PlaybackMenuCache g_pbc_cache = { /*valid=*/false, false, false, 0 };

static bool pb_cache_matches(const PlaybackController &c)
{
    return g_pbc_cache.valid
        && g_pbc_cache.paused       == c.paused
        && g_pbc_cache.loop_enabled == c.loop_enabled
        && g_pbc_cache.volume_pct   == getI2SVolume();
}

// Render the menu chrome + all buttons into LT7680_SLOT_MENU. The 20+
// rounded-rect GPU kicks and text-rendering SPI traffic land in this slot
// once per state change, then per-frame compose is a single BTE blit. The
// caller is responsible for ensuring this is invoked inside an active
// startWrite() (i.e. between displayAnimFrameBegin and displayAnimFrameEnd)
// — switching canvas to SLOT_MENU here does NOT need to be restored, since
// the next displayAnimFrameBegin re-points it at the back ANIM slot anyway.
static void pb_render_menu_to_cache(const PlaybackController &c)
{
    displayAnimCanvasToMenuCache();

    tft.fillRoundRectGPU(PB_MENU_X-2, PB_MENU_Y-2, PB_MENU_W+4, PB_MENU_H+4, 10, 0xFFFF);  // white border
    tft.fillRoundRectGPU(PB_MENU_X,   PB_MENU_Y,   PB_MENU_W,   PB_MENU_H,   10, 0x0000);  // black fill

    const char *labels[PB_BTN_COUNT];
    labels[PB_BTN_STOP]    = "Stop";
    labels[PB_BTN_PAUSE]   = c.paused        ? "Resume" : "Pause";
    labels[PB_BTN_LOOP]    = c.loop_enabled  ? "Loop: On" : "Loop: Off";
    labels[PB_BTN_RESTART] = "Restart";

    tft.setTextSize(2);
    for (int i = 0; i < PB_BTN_COUNT; i++) {
        int by = pb_btn_y(i);
        uint16_t fill   = (i == PB_BTN_LOOP && c.loop_enabled) ? 0x07E0 /*green*/ : 0x4208 /*dark gray*/;
        uint16_t border = 0xFFFF;
        tft.fillRoundRectGPU(PB_BTN_X-1, by-1, PB_BTN_W+2, PB_BTN_H+2, 10, border);
        tft.fillRoundRectGPU(PB_BTN_X,   by,   PB_BTN_W,   PB_BTN_H,   10, fill);
        tft.setTextColor(0xFFFF, fill);
        tft.drawCenterString(labels[i], PB_BTN_X + PB_BTN_W / 2, by + PB_BTN_H / 2 - 8);
    }

    const uint8_t cur_pct = getI2SVolume();
    tft.setTextSize(1);
    for (int i = 0; i < PB_VOL_COUNT; i++) {
        int bx = pb_vol_x(i);
        uint16_t fill   = (PB_VOL_PCT[i] == cur_pct) ? 0x07E0 /*green*/ : 0x4208 /*dark gray*/;
        uint16_t border = 0xFFFF;
        tft.fillRoundRectGPU(bx-1, PB_VOL_Y-1, PB_VOL_W+2, PB_VOL_H+2, 8, border);
        tft.fillRoundRectGPU(bx,   PB_VOL_Y,   PB_VOL_W,   PB_VOL_H,   8, fill);
        tft.setTextColor(0xFFFF, fill);
        char label[8];
        snprintf(label, sizeof(label), "%u", (unsigned)PB_VOL_PCT[i]);
        tft.drawCenterString(label, bx + PB_VOL_W / 2, PB_VOL_Y + PB_VOL_H / 2 - 4);
    }

    g_pbc_cache.valid        = true;
    g_pbc_cache.paused       = c.paused;
    g_pbc_cache.loop_enabled = c.loop_enabled;
    g_pbc_cache.volume_pct   = cur_pct;
}

// Composite the cached menu onto the current back ANIM buffer. Refreshes
// the cache first iff the state-affecting fields changed.
static void pb_draw_menu(const PlaybackController &c)
{
    if (!pb_cache_matches(c)) pb_render_menu_to_cache(c);
    displayAnimBlitMenuToBack(PB_MENU_X - 2, PB_MENU_Y - 2,
                              PB_MENU_W + 4, PB_MENU_H + 4);
}

namespace {

// Read exactly `n` bytes from src into dst. Returns true on full read.
static bool readExact(FboxSource *src, uint8_t *dst, uint32_t n)
{
    uint32_t got = 0;
    while (got < n) {
        int r = src->read(dst + got, n - got);
        if (r <= 0) return false;
        got += (uint32_t)r;
    }
    return true;
}

void producerTask(void *param)
{
    ProducerCtx *ctx = (ProducerCtx *)param;
    int slot_idx = 0;
    bool error = false;
    uint8_t err_state = SLOT_ERR;

    // Persistent reader across video chunks. Bounded per-frame via
    // beginFrame(video_budget) so it never consumes into the next frame's
    // audio block. The buffer's pre-read still helps within a single video
    // chunk for literal-heavy content.
    FboxRleReader rle;
    rle.begin(ctx->src, 0);

    const uint32_t audio_bytes = ctx->audio_block_bytes;
    const uint16_t samples_per_frame = ctx->hdr->audio_samples_per_frame;

    ImaAdpcmDecoder dec;

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

        // Determine this frame's total chunk size from the table.
        uint32_t chunk_total = ctx->frame_sizes[fi];
        if (chunk_total < 1 + audio_bytes) {
            Serial.printf("[ANIM] frame %u chunk too small (%lu < %lu)\n",
                          fi, (unsigned long)chunk_total,
                          (unsigned long)(1 + audio_bytes));
            error = true; err_state = SLOT_ERR;
        }

        // ── Read frame type byte ─────────────────────────────────────────
        // Read via the rle reader's buffer because previous frame may have
        // pre-read bytes into it (within its budget — but at frame boundary
        // budget went to 0 so refills stopped; any remaining buf_pos<buf_fill
        // bytes belong to THIS frame and must be consumed first).
        int type_int = -1;
        if (!error) {
            // Temporarily allow reader to drain any remaining buffered bytes
            // by setting an unbounded budget for the read_byte call. Since
            // the previous beginFrame budget should have been exact, this
            // matters only for the very first frame.
            rle.budget_left = -1;
            type_int = rle.read_byte();
            if (type_int < 0) { error = true; err_state = rle.err ? SLOT_UNDER : SLOT_ERR; }
        }
        uint8_t frame_type = error ? 0 : (uint8_t)type_int;

        // ── Read & decode audio block ────────────────────────────────────
        if (!error && audio_bytes > 0) {
            // Audio bytes come direct from src (not the rle buffer) — we want
            // them in a contiguous scratch for the ADPCM decoder. But any
            // bytes already in rle.buf belong to THIS frame's chunk, so we
            // must drain them first.
            uint32_t got = 0;
            // Drain from rle buffer first.
            while (got < audio_bytes && rle.buf_pos < rle.buf_fill) {
                ctx->audio_block_scratch[got++] = rle.buf[rle.buf_pos++];
            }
            // Then from src directly.
            if (got < audio_bytes) {
                if (!readExact(ctx->src, ctx->audio_block_scratch + got, audio_bytes - got)) {
                    error = true; err_state = SLOT_UNDER;
                }
            }

            if (!error) {
                uint64_t t_a = esp_timer_get_time();
                dec.resetFromBlockHeader(ctx->audio_block_scratch);
                dec.decodeOneBlock(ctx->audio_block_scratch + 4,
                                   ctx->pcm_scratch, samples_per_frame);
                ctx->t_adpcm_us += esp_timer_get_time() - t_a;
                pushI2SSamples(ctx->pcm_scratch, samples_per_frame);
            }
        }

        // ── Video ─────────────────────────────────────────────────────────
        uint32_t video_bytes = chunk_total - 1 - audio_bytes;

        if (!error && frame_type == FBOX_FRAME_S) {
            // Skip frame: no video bytes, consumer reuses previous slot.
            if (video_bytes != 0) {
                Serial.printf("[ANIM] frame %u: SKIP but video_bytes=%lu\n",
                              fi, (unsigned long)video_bytes);
            }
            s.state = SLOT_SKIP;
            ctx->n_skip_frames++;
            // No msync needed; consumer doesn't read frame_buf for SKIP.
        }
        else if (!error && (frame_type == FBOX_FRAME_I || frame_type == FBOX_FRAME_P)) {
            bool is_pf  = (frame_type == FBOX_FRAME_P);
            uint8_t *fb = s.frame_buf;
            uint8_t *f4 = ctx->frame_4bpp;

            // Bound the reader to this frame's video budget. Count buffered
            // bytes already in rle.buf (they came from src->read and belong
            // to this frame's chunk).
            int32_t in_buf = rle.buf_fill - rle.buf_pos;
            int32_t still_on_src = (int32_t)video_bytes - in_buf;
            if (still_on_src < 0) still_on_src = 0;
            rle.beginFrame((int)PIXEL_COUNT, still_on_src);

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
            } else {
                // ESP32-S3 cache coherency — flush PSRAM cache lines so the
                // consumer's SPI DMA on core 1 sees the just-written frame.
                uint64_t t_msync_start = esp_timer_get_time();
                esp_cache_msync(s.frame_buf, PIXEL_COUNT, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
                ctx->t_msync_us += esp_timer_get_time() - t_msync_start;

                // EXPERIMENT: Xtensa memw memory barrier. CPU-level barrier
                // that prevents instruction reordering across it. Unlikely
                // to help if the bug is in the external PSRAM controller's
                // write queue (memw doesn't drain that), but cheap to verify.
                __asm__ __volatile__ ("memw" ::: "memory");
            }
        }
        else if (!error) {
            Serial.printf("[ANIM] frame %u: unknown type 0x%02x\n", fi, frame_type);
            error = true; err_state = SLOT_ERR;
        }

        if (error) {
            Serial.printf("[ANIM] producer error at frame %u: state=%u\n",
                          fi, err_state);
            s.state = err_state;
            xSemaphoreGive(ctx->sem_ready);
            goto producer_exit;
        }

        xSemaphoreGive(ctx->sem_ready);
        slot_idx = (slot_idx + 1) % RING_SLOTS;

        // Yield 1 tick per frame so IDLE0 can run and pet the task watchdog.
        vTaskDelay(1);
    }

    if (!error && !*ctx->cancel) {
        // Drain the trailing keyframe table through the source so its bytes
        // flow through FboxSourceCrc and contribute to the file CRC32. Parse
        // entries into ctx->keyframes — playback ignores the table for now,
        // but future seek/scrub will use it.
        if (ctx->hdr->keyframe_count > 0 && ctx->keyframes) {
            ctx->keyframes->reserve(ctx->hdr->keyframe_count);
            uint8_t entry[8];
            uint8_t pending_n = 0;

            auto consume_byte = [&](uint8_t b) {
                entry[pending_n++] = b;
                if (pending_n == 8) {
                    KeyframeEntry e;
                    e.frame_index = (uint32_t)entry[0] | ((uint32_t)entry[1] << 8)
                                  | ((uint32_t)entry[2] << 16) | ((uint32_t)entry[3] << 24);
                    e.byte_offset = (uint32_t)entry[4] | ((uint32_t)entry[5] << 8)
                                  | ((uint32_t)entry[6] << 16) | ((uint32_t)entry[7] << 24);
                    ctx->keyframes->push_back(e);
                    pending_n = 0;
                }
            };

            // Drain bytes still in rle.buf first (they came from src through
            // crc_src so CRC is already accumulated).
            while (rle.buf_pos < rle.buf_fill &&
                   ctx->keyframes->size() < ctx->hdr->keyframe_count)
            {
                consume_byte(rle.buf[rle.buf_pos++]);
            }
            // Then pull the rest from src directly.
            uint8_t scratch[64];
            while (ctx->keyframes->size() < ctx->hdr->keyframe_count) {
                uint32_t remaining = (ctx->hdr->keyframe_count - ctx->keyframes->size()) * 8u
                                   - pending_n;
                if (remaining == 0) break;
                uint32_t want = remaining > sizeof(scratch) ? sizeof(scratch) : remaining;
                if (!readExact(ctx->src, scratch, want)) break;
                for (uint32_t i = 0; i < want; i++) consume_byte(scratch[i]);
            }
            Serial.printf("[ANIM] keyframes: %u entries parsed\n",
                          (unsigned)ctx->keyframes->size());
        }

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
    FboxHeader hdr;
    uint32_t crc_seed = 0;
    if (!fboxReadHeader(src, hdr, &crc_seed)) {
        Serial.println("playFboxAnimation: invalid FBOX header");
        return PlaybackResult::DECODE_ERROR;
    }
    if (hdr.frame_count == 0 || hdr.width != TFT_HOR_RES || hdr.height != TFT_VER_RES) {
        Serial.printf("playFboxAnimation: bad header (frames=%d, %dx%d)\n",
                      hdr.frame_count, hdr.width, hdr.height);
        return PlaybackResult::DECODE_ERROR;
    }
    if (hdr.description[0]) Serial.printf("[ANIM] \"%s\"\n", hdr.description);

    const uint32_t frame_ms = hdr.fps > 0 ? 1000u / hdr.fps : 100u;

    // CRC seed (header bytes [64..511]) was computed by fboxReadHeader. All
    // subsequent bytes go through FboxSourceCrc which accumulates as the
    // producer reads.
    FboxSourceCrc crc_src(&src, crc_seed);

    // Read frame size table into PSRAM — producer needs per-frame chunk sizes
    // to compute video budget and detect chunk boundaries.
    uint32_t *frame_sizes = (uint32_t *)heap_caps_malloc(hdr.frame_count * 4u, MALLOC_CAP_SPIRAM);
    if (!frame_sizes) {
        Serial.printf("[ANIM] frame_sizes ps_malloc (%lu B) failed\n",
                      (unsigned long)(hdr.frame_count * 4u));
        return PlaybackResult::OOM;
    }
    {
        // Read raw LE u32s into a temp byte buffer, decode into frame_sizes.
        // Done in 256-byte chunks; CRC accumulates automatically through crc_src.
        uint8_t buf[256];
        uint32_t remaining = hdr.frame_count * 4u;
        uint32_t out_idx   = 0;
        uint8_t  pending[4]; uint8_t pending_n = 0;
        while (remaining > 0) {
            size_t take = remaining > sizeof(buf) ? sizeof(buf) : remaining;
            int r = crc_src.read(buf, take);
            if (r <= 0) { free(frame_sizes); return PlaybackResult::READ_UNDERRUN; }
            for (int i = 0; i < r; i++) {
                pending[pending_n++] = buf[i];
                if (pending_n == 4) {
                    frame_sizes[out_idx++] =
                        (uint32_t)pending[0] | ((uint32_t)pending[1] << 8) |
                        ((uint32_t)pending[2] << 16) | ((uint32_t)pending[3] << 24);
                    pending_n = 0;
                }
            }
            remaining -= (uint32_t)r;
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
        free(frame_sizes);
        return PlaybackResult::OOM;
    }
    Slot slots[RING_SLOTS] = {};
    for (int i = 0; i < RING_SLOTS; i++) {
        // MALLOC_CAP_DMA is required for the SPI2 driver to read the slot
        // directly via DMA. Without it, IDF 5.x's spi_master falls back to
        // bouncing the buffer through internal RAM in chunks — adds ~11ms
        // per 230 KB frame, the gap between theoretical (23ms @ 80 MHz)
        // and the observed 34ms spi_avg.
        slots[i].frame_buf = (uint8_t *)heap_caps_aligned_alloc(
            32, PIXEL_COUNT, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
        if (!slots[i].frame_buf) {
            for (int j = 0; j < i; j++) free(slots[j].frame_buf);
            free(frame_4bpp);
            free(frame_sizes);
            return PlaybackResult::OOM;
        }
    }

    // v4 audio scratches — kept off the producer task stack. PSRAM (not
    // internal): low-fps / high-sample-rate files push pcm_scratch into the
    // tens of KB (e.g. fps=1 @ 22050 Hz → samples_per_frame=22050 →
    // pcm_scratch=44 KB), which OOMs internal heap after frame_4bpp (115 KB)
    // and the producer/loader task stacks. Both buffers are touched only
    // once per frame (ADPCM decode in/out + pushI2SSamples memcpy into a
    // stream buffer); no DMA, no per-pixel hot loop, so PSRAM latency is
    // negligible at the producer's frame cadence.
    const uint32_t audio_block_bytes = (hdr.audio_samples_per_frame > 0)
        ? (4u + ((uint32_t)hdr.audio_samples_per_frame + 1u) / 2u)
        : 0u;
    uint8_t  *audio_block_scratch = nullptr;
    int16_t  *pcm_scratch         = nullptr;
    if (audio_block_bytes > 0) {
        audio_block_scratch = (uint8_t *)heap_caps_malloc(audio_block_bytes,
                                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        pcm_scratch = (int16_t *)heap_caps_malloc(hdr.audio_samples_per_frame * sizeof(int16_t),
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!audio_block_scratch || !pcm_scratch) {
            Serial.println("[ANIM] audio scratch alloc failed");
            if (audio_block_scratch) free(audio_block_scratch);
            if (pcm_scratch) free(pcm_scratch);
            for (int i = 0; i < RING_SLOTS; i++) free(slots[i].frame_buf);
            free(frame_4bpp);
            free(frame_sizes);
            return PlaybackResult::OOM;
        }
    }
    std::vector<KeyframeEntry> keyframes;

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
    pctx.frame_sizes = frame_sizes;
    pctx.audio_block_bytes   = audio_block_bytes;
    pctx.audio_block_scratch = audio_block_scratch;
    pctx.pcm_scratch         = pcm_scratch;
    pctx.keyframes   = &keyframes;
    pctx.t_decode_us = 0;
    pctx.t_msync_us  = 0;
    pctx.t_wait_free_us = 0;
    pctx.t_refill_us = 0;
    pctx.t_adpcm_us  = 0;
    pctx.n_refills   = 0;
    pctx.n_frames    = 0;
    pctx.n_skip_frames = 0;

    // v4 audio: init I2S streaming BEFORE spawning the producer so the
    // producer's first pushI2SSamples lands in a valid stream buffer.
    // Pre-buffer phase: producer fills ~12 frames of cushion in the stream
    // buffer naturally — for the first few frames it pushes audio with no
    // I2S consumer running yet (writer task spawned but DMA hasn't started
    // draining audio until its first write). Audio start vs frame 0 is
    // within the ~65 ms DMA cushion either way.
    bool audio_streaming = false;
    if (hdr.audio_sample_rate > 0 && hdr.audio_samples_per_frame > 0) {
        audio_streaming = startI2SStreaming(hdr.audio_sample_rate,
                                            hdr.audio_samples_per_frame);
    }

    // Stack 16 KB: producer's FboxRleReader local (≈4 KB buf) + decode helpers
    // + ADPCM decoder + FreeRTOS overhead.
    xTaskCreatePinnedToCore(producerTask, "fbox_dec", 16384, &pctx, 2, &pctx.task, 0);

    Serial.printf("[ANIM] play %u frames @ %u fps (audio=%s, samples/frame=%u)\n",
                  hdr.frame_count, hdr.fps,
                  audio_streaming ? "on" : "off",
                  hdr.audio_samples_per_frame);
    uint32_t t_start     = millis();
    uint32_t t_spi_total = 0;
    uint32_t t_wait_total = 0;

    PlaybackResult result = PlaybackResult::OK;
    bool consumer_running = true;
    int slot_idx = 0;
    uint16_t frames_drawn = 0;
    TickType_t prev_wake = xTaskGetTickCount();

    // Reset per-call playback-menu state. loop_enabled persists.
    g_pbc.menu_open         = false;
    g_pbc.paused            = false;
    g_pbc.stop_requested    = false;
    g_pbc.restart_requested = false;

    // held_slot is the most recent non-SKIP slot whose sem_free we have NOT
    // released. While paused, we re-blit this slot's frame_buf every frame so
    // the displayed image stays consistent through page flips. Holding one
    // slot leaves the producer 2 of 3 to work with — fine for forward
    // progress, and producer naturally blocks on sem_free once paused.
    Slot *held_slot = nullptr;
    bool  last_touch_active = (touchZ > 0);  // seed so existing touch doesn't fire on entry

    while (consumer_running) {
        // ── PAUSED branch ─────────────────────────────────────────────────
        // Reuse the held slot, redraw + menu, pace and poll. Don't take a
        // new slot from sem_ready; producer will fill and block on sem_free.
        if (g_pbc.paused && held_slot) {
            displayAnimFrameBegin();
            displayAnimWriteFrame(held_slot->frame_buf);
            if (g_pbc.menu_open) pb_draw_menu(g_pbc);
            displayAnimFrameEnd();

            handleTouch();
            bool cur = (touchZ > 0);
            if (cur && !last_touch_active) {
                if (!g_pbc.menu_open) g_pbc.menu_open = true;
                else                  pb_handle_tap(g_pbc, touchX, touchY);
            }
            last_touch_active = cur;

            if (g_pbc.stop_requested) {
                Serial.printf("[ANIM] consumer exit: USER_CANCELLED (paused, drawn=%u)\n",
                              frames_drawn);
                result = PlaybackResult::USER_CANCELLED;
                consumer_running = false;
                break;
            }
            if (g_pbc.restart_requested) {
                Serial.printf("[ANIM] consumer exit: USER_RESTART (paused, drawn=%u)\n",
                              frames_drawn);
                result = PlaybackResult::USER_RESTART;
                consumer_running = false;
                break;
            }
            vTaskDelayUntil(&prev_wake, pdMS_TO_TICKS(frame_ms));
            continue;
        }

        // ── NOT PAUSED: advance pipeline ─────────────────────────────────
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

        if (s.state == SLOT_SKIP) {
            // SKIP: no blit (LT7680 retains the previous frame). Release
            // immediately — SKIP slots are never the held slot.
            frames_drawn++;
            xSemaphoreGive(sem_free);
            slot_idx = (slot_idx + 1) % RING_SLOTS;
        } else {
            uint32_t t_spi = millis();
            displayAnimFrameBegin();
            displayAnimWriteFrame(s.frame_buf);
            if (g_pbc.menu_open) pb_draw_menu(g_pbc);
            displayAnimFrameEnd();
            t_spi_total += millis() - t_spi;
            frames_drawn++;

            // Release the previously held slot (if any), then become the
            // new held slot. We don't release &s until the next non-SKIP
            // blit (or until consumer exits).
            if (held_slot) xSemaphoreGive(sem_free);
            held_slot = &s;
            slot_idx = (slot_idx + 1) % RING_SLOTS;
        }

        // Touch + menu dispatch.
        handleTouch();
        bool cur = (touchZ > 0);
        if (cur && !last_touch_active) {
            if (!g_pbc.menu_open) g_pbc.menu_open = true;
            else                  pb_handle_tap(g_pbc, touchX, touchY);
        }
        last_touch_active = cur;

        if (g_pbc.stop_requested) {
            Serial.printf("[ANIM] consumer exit: USER_CANCELLED at frame_idx=%u drawn=%u\n",
                          s.frame_idx, frames_drawn);
            result = PlaybackResult::USER_CANCELLED;
            consumer_running = false;
            break;
        }
        if (g_pbc.restart_requested) {
            Serial.printf("[ANIM] consumer exit: USER_RESTART at frame_idx=%u drawn=%u\n",
                          s.frame_idx, frames_drawn);
            result = PlaybackResult::USER_RESTART;
            consumer_running = false;
            break;
        }

        // Precise pacing.
        vTaskDelayUntil(&prev_wake, pdMS_TO_TICKS(frame_ms));
    }

    // Release the held slot before tearing down semaphores.
    if (held_slot) { xSemaphoreGive(sem_free); held_slot = nullptr; }

    // Tear down producer
    cancel = true;
    // Drain any pending sem_free posts so the producer can wake and exit.
    for (int i = 0; i < RING_SLOTS + 1; i++) xSemaphoreGive(sem_free);
    for (int i = 0; i < 200; i++) {
        if (eTaskGetState(pctx.task) == eDeleted) break;
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    uint32_t elapsed_ms = millis() - t_start;
    Serial.printf("[ANIM] done: drawn=%u/%u elapsed=%lums fps=%.1f spi_avg=%lums wait_avg=%lums skips=%u\n",
                  frames_drawn, hdr.frame_count, (unsigned long)elapsed_ms,
                  elapsed_ms ? (frames_drawn * 1000.0f / elapsed_ms) : 0.0f,
                  frames_drawn ? (unsigned long)(t_spi_total / frames_drawn) : 0ul,
                  frames_drawn ? (unsigned long)(t_wait_total / frames_drawn) : 0ul,
                  pctx.n_skip_frames);
    if (pctx.n_frames) {
        uint64_t refill_total = pctx.t_refill_us;
        Serial.printf("[ANIM] producer: decode_avg=%llums msync_avg=%lluus wait_free_avg=%lluus refill_avg=%lluus adpcm_avg=%lluus (n=%u)\n",
                      pctx.t_decode_us  / pctx.n_frames / 1000,
                      pctx.t_msync_us   / pctx.n_frames,
                      pctx.t_wait_free_us / pctx.n_frames,
                      refill_total / pctx.n_frames,
                      pctx.t_adpcm_us / pctx.n_frames,
                      pctx.n_frames);
        Serial.printf("[ANIM] producer: refills/frame=%.1f refill_us/call=%llu\n",
                      pctx.n_refills * 1.0f / pctx.n_frames,
                      pctx.n_refills ? (refill_total / pctx.n_refills) : 0);
    }

    // Stop I2S streaming before freeing scratches the producer pushed from.
    // Producer has already exited at this point (joined above), so no in-flight
    // pushI2SSamples can race the teardown.
    if (audio_streaming) stopI2SStreaming();

    // CRC32 verification (header value 0 = skip). Producer naturally drained
    // every byte (header → frame_table → frames → keyframe table) through crc_src,
    // so the accumulator covers the whole file.
    if (hdr.crc32 != 0 && pctx.ran_to_eof) {
        uint32_t got = crc_src.crc();
        if (got != hdr.crc32) {
            Serial.printf("[ANIM] CRC mismatch: got %08lx expected %08lx\n",
                          (unsigned long)got, (unsigned long)hdr.crc32);
            if (result == PlaybackResult::OK) result = PlaybackResult::CRC_MISMATCH;
        } else {
            Serial.printf("[ANIM] CRC OK (%08lx)\n", (unsigned long)got);
        }
    }

    for (int i = 0; i < RING_SLOTS; i++) free(slots[i].frame_buf);
    if (audio_block_scratch) free(audio_block_scratch);
    if (pcm_scratch) free(pcm_scratch);
    free(frame_4bpp);
    free(frame_sizes);
    vSemaphoreDelete(sem_free);
    vSemaphoreDelete(sem_ready);

    changeScreenContext(SCREEN_CANVAS);
    return result;
}

PlaybackResult playFboxAnimationFromSD(const char *path, uint32_t ring_bytes)
{
    FboxSourceSD sd_src(path);
    if (!sd_src.ok()) {
        Serial.printf("playFboxAnimationFromSD: cannot open %s\n", path);
        return PlaybackResult::READ_UNDERRUN;
    }

    // Loop control:
    //   OK            → rewind & loop iff the playback menu's Loop toggle is on.
    //   USER_RESTART  → always rewind & loop (user explicitly asked).
    //   USER_CANCELLED → break (user hit Stop).
    //   anything else → break (don't spin on a broken file or a stuck loader).
    auto loop_play = [](FboxSource &src, const char *label) {
        PlaybackResult result;
        uint32_t iter = 0;
        while (true) {
            // Per-iteration diagnostics for the horizontal-shift-bug hunt.
            // (1) Heap log to spot leaks / DMA-cap fragmentation across iterations.
            // (2) Register diff against init-time baseline: the FIRST iteration
            //     that shows ANY diff is the smoking gun for the persistent
            //     chip-state corruption we're chasing.
            Serial.printf("[HEAP iter=%lu] internal_free=%u spiram_free=%u largest_dma=%u\n",
                          (unsigned long)iter,
                          (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                          (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
            displayDiagDiffRegistersAgainstBaseline((int)iter);

            result = playFboxAnimation(src);
            Serial.printf("[ANIM] %s loop iter=%lu result=%d (loop=%d)\n",
                          label, (unsigned long)iter, (int)result,
                          g_pbc.loop_enabled ? 1 : 0);
            bool should_rewind = (result == PlaybackResult::USER_RESTART) ||
                                 (result == PlaybackResult::OK && g_pbc.loop_enabled);
            if (!should_rewind) break;
            if (!src.reset()) {
                Serial.printf("[ANIM] %s loop: source reset failed; stopping\n", label);
                break;
            }
            iter++;
        }
        return result;
    };

    FboxSourceRingBuffered buf_src(&sd_src, ring_bytes);
    if (!buf_src.ok()) {
        Serial.println("playFboxAnimationFromSD: ring alloc failed; using direct SD source");
        return loop_play(sd_src, "sd");
    }
    Serial.printf("[ANIM] ring buffer: %lu KB PSRAM, async SD loader on core 1\n",
                  (unsigned long)(ring_bytes / 1024));
    PlaybackResult result = loop_play(buf_src, "ring");
    Serial.printf("[ANIM] ring stalls=%u stall_time=%llums (cumulative across loops)\n",
                  buf_src.stallCount(), buf_src.stallTimeUs() / 1000);
    return result;
}

std::vector<std::string> sdGetFboxFiles()
{
    std::vector<std::string> fileNames;
    DIR *dir = opendir("/sd/sketches/saved");
    if (!dir) return fileNames;
    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_type == DT_REG) {
            printf("Found file: %s\n", entry->d_name);
            fileNames.emplace_back(entry->d_name);
        }
    }
    closedir(dir);
    return fileNames;
}
