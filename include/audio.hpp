#ifndef AUDIO_HPP
#define AUDIO_HPP

#include <Arduino.h>
#include "fbox_source.hpp"
#include "io.hpp"

#define FBOX_ADPCM_BLOCK_SIZE        512
#define FBOX_ADPCM_SAMPLES_PER_BLOCK 1016
#define FBOX_AUDIO_PSRAM_CAP         (1u * 1024u * 1024u)  // 1 MB PCM cap (≈ 23 s @ 22050 Hz mono)

/* IMA ADPCM block decoder. One instance per stream — state persists across
 * blocks because each block header re-seeds it. Spec ported verbatim from
 * FriendBox-Server/fbox.py. */
struct ImaAdpcmDecoder {
    int16_t predictor;
    int8_t  step_index;

    void resetFromBlockHeader(const uint8_t *hdr4);
    /* Decode the 508-byte sample payload of one block into out_1016 int16s.
     * payload must point at byte 4 of the block (after the 4-byte header). */
    void decodePayload(const uint8_t *payload508, int16_t *out_1016);
};

struct FboxAudio {
    int16_t *pcm;          // ps_malloc'd, mono int16 LE; nullptr if no audio
    uint32_t sample_rate;  // Hz
    uint32_t sample_count; // count of int16 samples in pcm
};

/* Decode the trailing IMA ADPCM section of an FBOX file into PSRAM PCM.
 *
 * The source's cursor must already be positioned at the first byte of the
 * audio section (i.e. immediately after the last video frame). hdr provides
 * audio_size / sample_rate / channels.
 *
 * Returns true on success (out.pcm valid) or true with out.pcm=nullptr if
 * the file has no audio. Returns false if PSRAM allocation failed or audio
 * exceeds FBOX_AUDIO_PSRAM_CAP — in which case the function reads-and-discards
 * the audio bytes so the source cursor still ends at EOF. */
bool fboxDecodeAudio(FboxSource &src, const FboxHeader &hdr, FboxAudio &out);

/* Free the PSRAM PCM buffer. Safe to call on a zero-initialised FboxAudio. */
void fboxAudioFree(FboxAudio &a);

#endif
