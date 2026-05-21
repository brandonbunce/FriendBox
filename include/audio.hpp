#ifndef AUDIO_HPP
#define AUDIO_HPP

#include <Arduino.h>

/* IMA ADPCM block decoder. One block = 4-byte header + ceil(samples/2) nibble
 * payload bytes. Each block re-seeds state from its header, so blocks decode
 * independently — important for v4 since blocks are scattered one-per-frame
 * across the file and a single missed frame must not desynchronize audio. */
struct ImaAdpcmDecoder {
    int16_t predictor;
    int8_t  step_index;

    /* Re-seed from a 4-byte block header (predictor LE u16, step_index u8, reserved u8). */
    void resetFromBlockHeader(const uint8_t *hdr4);

    /* Decode `samples` nibbles from `payload` (= ceil(samples/2) bytes) into `out`.
     * Caller must call `resetFromBlockHeader` immediately before this call. */
    void decodeOneBlock(const uint8_t *payload, int16_t *out, uint16_t samples);
};

#endif
