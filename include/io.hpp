#ifndef IO_H
#define IO_H

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <Preferences.h>

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

/** Parse the 128-byte FBOX header from an open file positioned at byte 0.
 *  Returns false if the magic is wrong or version unsupported. */
bool fboxReadHeader(File &f, FboxHeader &out);

/** Streaming RLE decoder for FBOX v3 frame payloads (I-frames and P-frames).
 *  Returns raw 4-bit nibbles; the caller applies XOR delta for P-frames.
 *  Internal 256-byte chunk buffer reduces SD SPI calls ~100× versus 1-byte reads. */
struct FboxRleReader {
    File    *f;
    int      pixels_left;
    bool     in_run;
    uint8_t  run_color;
    int      token_count;
    uint8_t  lit_byte;
    bool     lit_hi_valid;

    // Chunk buffer: fills from SD 256 bytes at a time
    uint8_t buf[256];
    int     buf_pos;
    int     buf_fill;

    void begin(File *f_, int pixel_count) {
        f = f_; pixels_left = pixel_count;
        in_run = false; token_count = 0; lit_hi_valid = false;
        buf_pos = 0; buf_fill = 0;
    }

    // Returns next raw byte from the file, or -1 on EOF/error.
    int read_byte() {
        if (buf_pos >= buf_fill) {
            buf_fill = (int)f->readBytes((char *)buf, sizeof(buf));
            buf_pos  = 0;
            if (buf_fill <= 0) return -1;
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

/** Play all frames of an FBOX animation from SD in a loop.
 *  Stops when the screen is touched. For single-frame files, behaves like loadSketchFromSD. */
void playFboxAnimation(const char *path);

#endif