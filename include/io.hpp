#ifndef IO_H
#define IO_H

#include <stdint.h>
#include <vector>
#include <string>
#include <esp_timer.h>
#include "fbox_source.hpp"
#include "nvs_store.hpp"

// SDIO 4-bit pinout (replaces the previous SPI-mode wiring).
// Effective bandwidth ~10 MB/s vs ~1.3 MB/s on SPI — see
// docs/exec-plans/tech-debt-tracker.md and project-sd-spi-bandwidth-wall.
#define SD_DAT0 48
#define SD_DAT1 47
#define SD_DAT2 2
#define SD_DAT3 14
#define SD_CLK 13
#define SD_CMD 1

extern NvsStore nvs;

// Input (Buttons)
/** Which GPIO pin will be used as input for the hall effect button? */
#define HALL_SENSOR_PIN 21
/** How long should button be pressed before logically registering input? */
#define DEBOUNCE_MILLISECONDS 50

// FBOX format constants — v4: interleaved audio per frame, skip-frame markers,
// trailing keyframe offset table. Header grew from 128 to 512 bytes; per-frame
// chunk = [1 byte type 'I'/'P'/'S'][N-byte ADPCM block (optional)][video RLE].
#define FBOX_HEADER_SIZE 512
#define FBOX_VERSION     4

// Frame type bytes — first byte of each frame chunk.
#define FBOX_FRAME_I     0x49  // 'I' — intra: full RLE-encoded 4bpp pixels
#define FBOX_FRAME_P     0x50  // 'P' — delta: RLE-encoded (current XOR previous)
#define FBOX_FRAME_S     0x53  // 'S' — skip: no video bytes; reuse previous frame

struct FboxHeader {
    uint8_t  version;                  // FBOX_VERSION
    uint8_t  kind;                     // 0=sketch, 1=animation
    uint16_t frame_count;
    uint16_t fps;
    uint16_t width;
    uint16_t height;
    uint32_t created;                  // Unix timestamp
    char     username[33];             // null-terminated, 32 chars max
    uint32_t expected_file_size;       // total file bytes; cheap truncation check
    uint16_t audio_sample_rate;        // Hz (0 if no audio)
    uint8_t  audio_channels;           // 1=mono (0 if no audio)
    uint16_t audio_samples_per_frame;  // = sample_rate / fps (0 if no audio)
    uint32_t crc32;                    // CRC32 over file bytes [64..EOF]; 0=skip
    uint16_t keyframe_count;           // 0 if no keyframe table
    uint32_t keyframe_table_offset;    // absolute file offset; 0 if absent
    char     description[256];         // null-terminated, 255 chars max
};

/* Result of a playback attempt. Callers use this to decide whether to retry,
 * fall back to a different source, or surface an error in the UI. */
enum class PlaybackResult {
    OK,                // played to EOF, no errors
    READ_UNDERRUN,     // source returned -1 mid-stream (network died, SD ejected)
    DECODE_ERROR,      // bad header, bad RLE, dimensions mismatch
    CRC_MISMATCH,      // file CRC32 didn't match header value
    USER_CANCELLED,    // touch input aborted playback
    OOM                // ps_malloc failed
};

// Functions
bool initSD(bool forceFormat = false);
bool initNVS();
/* Prepare ESP32 to receive inputs from button / hall effect. */
bool initMenuButton();
/**
 * Handle pressing of hardware button, will implement as hall effect sensor later
 * recheckInput will register another logical press even if button is being held.
 * @param recheckInput Should we register another input in the event the button is still being held?
 */
void handleMenuButton(bool recheckInput);
void saveImageToSD(int slot);
void loadImageFromSD(int slot);
std::vector<std::string> sdGetFboxFiles();

/** Parse the 512-byte FBOX v4 header from a source positioned at byte 0.
 *  Returns false on bad magic, wrong version, or file-size mismatch.
 *  If crc_seed_out != nullptr, writes the CRC32 accumulator seed computed
 *  from header bytes [64..511] — this is the value to pass to FboxSourceCrc
 *  so the full-file CRC covers the header. The raw header bytes themselves
 *  are not exposed (they used to be, but the 512-byte stack allocation pushed
 *  loopTask into its canary band on the playback path). */
bool fboxReadHeader(FboxSource &src, FboxHeader &out, uint32_t *crc_seed_out = nullptr);

/** Streaming RLE decoder for FBOX v4 video frame payloads (I-frames and P-frames).
 *  Returns raw 4-bit nibbles; the caller applies XOR delta for P-frames.
 *  Internal 4 KB chunk buffer reduces source-side calls ~100× versus 1-byte reads.
 *
 *  v4 bounded-read mode: each per-frame call to `beginFrame` accepts a `byte_budget`.
 *  The chunk-buffer refill clamps to remaining budget so the reader cannot
 *  consume bytes belonging to the next frame's audio chunk. Pass -1 for unbounded
 *  (legacy). */
struct FboxRleReader {
    FboxSource *src;
    int      pixels_left;
    bool     in_run;
    uint8_t  run_color;
    int      token_count;
    uint8_t  lit_byte;
    bool     lit_hi_valid;

    // Chunk buffer: fills from source N bytes at a time. Sized large enough
    // (4 KB) that even literal-heavy frames (≈115 KB source/frame for full
    // dithered content) refill < 30× per frame instead of ≈450×.
    uint8_t buf[4096];
    int     buf_pos;
    int     buf_fill;
    bool    err;

    // Per-frame byte budget. Decremented by each refill from src->read.
    // -1 = unbounded. Refill clamps to min(sizeof(buf), budget_left).
    int32_t budget_left;

    // Refill profiling: cumulative time and count of src->read calls.
    uint64_t refill_t_us;
    uint32_t refill_n;

    void begin(FboxSource *s, int pixel_count) {
        src = s; pixels_left = pixel_count;
        in_run = false; token_count = 0; lit_hi_valid = false;
        buf_pos = 0; buf_fill = 0; err = false;
        budget_left = -1;
        refill_t_us = 0; refill_n = 0;
    }

    /* Reset per-frame decode state for streaming playback. Keeps the chunk
     * buffer intact so any bytes the previous frame's decode pre-read (up to
     * 4 KB) are consumed first.
     *
     * byte_budget: -1 = unbounded. ≥0 = maximum bytes this frame's video
     * payload may consume from src. Refill clamps. Bytes already in `buf` at
     * call time count toward the budget (caller must include them in the
     * budget value — typically buf_fill - buf_pos remain from previous frame). */
    void beginFrame(int pixel_count, int32_t byte_budget = -1) {
        pixels_left = pixel_count;
        in_run = false; token_count = 0; lit_hi_valid = false;
        err = false;
        budget_left = byte_budget;
    }

    // Returns next raw byte from the source, or -1 on EOF/error.
    int read_byte() {
        if (buf_pos >= buf_fill) {
            if (budget_left == 0) { err = false; return -1; }   // frame boundary
            size_t want = sizeof(buf);
            if (budget_left > 0 && (int32_t)want > budget_left) want = (size_t)budget_left;
            uint64_t t = esp_timer_get_time();
            int r = src->read(buf, want);
            refill_t_us += esp_timer_get_time() - t;
            refill_n++;
            if (r <= 0) { err = (r < 0); buf_fill = 0; return -1; }
            buf_fill = r;
            buf_pos  = 0;
            if (budget_left > 0) budget_left -= r;
        }
        return buf[buf_pos++];
    }

    // Returns palette index 0-15, or -1 on error / end of frame.
    int next() {
        if (pixels_left <= 0) return -1;
        if (token_count == 0) {
            int h = read_byte(); if (h < 0) return -1;
            if (h & 0x80) {
                in_run = true; token_count = (h & 0x7F) + 1;
                int b  = read_byte(); if (b < 0) return -1;
                run_color = b & 0x0F;
            } else {
                in_run = false; token_count = (h & 0x7F) + 1; lit_hi_valid = false;
            }
        }
        int px;
        if (in_run) {
            px = run_color; token_count--;
        } else if (!lit_hi_valid) {
            int b = read_byte(); if (b < 0) return -1;
            lit_byte = (uint8_t)b; px = (b >> 4) & 0x0F;
            token_count--;
            lit_hi_valid = (token_count > 0);
        } else {
            px = lit_byte & 0x0F; lit_hi_valid = false; token_count--;
        }
        pixels_left--;
        return px;
    }
};

/** Open an FBOX file from SD, decode frame 0, and blit it into the LT7680 canvas slot. */
void loadSketchFromSD(const char *path);

/** Play all frames of an FBOX v4 animation from the given source.
 *  Stops when the screen is touched. Audio is interleaved per-frame and
 *  streamed to I2S in real time — no caller-side audio handling. */
PlaybackResult playFboxAnimation(FboxSource &src);

/** Convenience wrapper: open path on SD, wrap with the PSRAM ring buffer, and
 *  play. The ring loader pulls from SD into PSRAM while the producer/consumer
 *  drain it. Single entry point — replaces the v3-era FromSDFull/Buffered split.
 *  ring_bytes defaults to 2 MB. */
PlaybackResult playFboxAnimationFromSD(const char *path,
                                       uint32_t ring_bytes = 2u * 1024u * 1024u);

#endif
