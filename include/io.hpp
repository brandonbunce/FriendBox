#ifndef IO_H
#define IO_H

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <Preferences.h>
#include "fbox_source.hpp"

#define SD_CS 14
#define SD_SCK 12
#define SD_MISO 47
#define SD_MOSI 48
extern Preferences nvs;

// Input (Buttons)
/** Which GPIO pin will be used as input for the hall effect button? */
#define HALL_SENSOR_PIN 21
/** How long should button be pressed before logically registering input? */
#define DEBOUNCE_MILLISECONDS 50

// FBOX format constants
#define FBOX_HEADER_SIZE 128
#define FBOX_VERSION_3   3    // XOR delta + RLE video, IMA ADPCM audio, CRC32

// Frame type bytes — first byte of each frame payload in the frame data section
#define FBOX_FRAME_I     0x49  // 'I' — intra-frame: full RLE-encoded 4bpp pixels
#define FBOX_FRAME_P     0x50  // 'P' — delta-frame:  RLE-encoded (current XOR previous)

struct FboxHeader {
    uint8_t  version;            // FBOX_VERSION_3
    uint8_t  kind;               // 0=sketch, 1=animation
    uint16_t frame_count;
    uint16_t fps;
    uint16_t width;
    uint16_t height;
    uint32_t created;            // Unix timestamp
    char     username[33];       // null-terminated, 32 chars max
    uint32_t audio_size;         // bytes of IMA ADPCM audio (0=none)
    uint16_t audio_sample_rate;  // Hz (0 if no audio)
    uint8_t  audio_channels;     // 1=mono (0 if no audio)
    uint32_t crc32;              // CRC32 over file bytes [62..EOF]; 0=skip
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

/** Parse the 128-byte FBOX header from a source positioned at byte 0.
 *  Returns false if the magic is wrong or version unsupported.
 *  If raw_out != nullptr, the raw 128 header bytes are copied there so the
 *  caller can compute the partial CRC over bytes [62..127]. */
bool fboxReadHeader(FboxSource &src, FboxHeader &out, uint8_t *raw_out = nullptr);

/** Streaming RLE decoder for FBOX v3 frame payloads (I-frames and P-frames).
 *  Returns raw 4-bit nibbles; the caller applies XOR delta for P-frames.
 *  Internal 256-byte chunk buffer reduces source-side calls ~100× versus 1-byte reads. */
struct FboxRleReader {
    FboxSource *src;
    int      pixels_left;
    bool     in_run;
    uint8_t  run_color;
    int      token_count;
    uint8_t  lit_byte;
    bool     lit_hi_valid;

    // Chunk buffer: fills from source 256 bytes at a time
    uint8_t buf[256];
    int     buf_pos;
    int     buf_fill;
    bool    err;

    void begin(FboxSource *s, int pixel_count) {
        src = s; pixels_left = pixel_count;
        in_run = false; token_count = 0; lit_hi_valid = false;
        buf_pos = 0; buf_fill = 0; err = false;
    }

    /* Reset per-frame decode state for streaming playback. Keeps the chunk
     * buffer intact so any bytes the previous frame's decode pre-read (up to
     * 256) are consumed first — without this, the over-read bytes would be
     * lost when the reader is discarded between frames, shifting every
     * subsequent frame's start offset and producing visual chaos. The encoder
     * emits one fresh RLE token stream per frame, so token state resets cleanly. */
    void beginFrame(int pixel_count) {
        pixels_left = pixel_count;
        in_run = false; token_count = 0; lit_hi_valid = false;
        err = false;
    }

    // Returns next raw byte from the source, or -1 on EOF/error.
    int read_byte() {
        if (buf_pos >= buf_fill) {
            int r = src->read(buf, sizeof(buf));
            if (r <= 0) { err = (r < 0); buf_fill = 0; return -1; }
            buf_fill = r;
            buf_pos  = 0;
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

/** Play all frames of an FBOX animation from the given source.
 *  Stops when the screen is touched. For single-frame files, behaves like
 *  loadSketchFromSD. */
PlaybackResult playFboxAnimation(FboxSource &src);

/** Convenience wrapper: open path on SD and play. */
PlaybackResult playFboxAnimationFromSD(const char *path);

#endif
