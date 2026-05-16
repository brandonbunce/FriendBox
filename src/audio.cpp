#include "audio.hpp"
#include <esp_heap_caps.h>

// IMA ADPCM tables — ported verbatim from FriendBox-Server/fbox.py
static const int16_t kStepTable[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37,
    41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173,
    190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
    6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289,
    16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

static const int8_t kIndexTable[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8
};

static inline int16_t _clamp_i16(int32_t v)
{
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

static inline int8_t _clamp_step(int32_t v)
{
    if (v > 88) return 88;
    if (v <  0) return  0;
    return (int8_t)v;
}

void ImaAdpcmDecoder::resetFromBlockHeader(const uint8_t *hdr4)
{
    predictor  = (int16_t)((uint16_t)hdr4[0] | ((uint16_t)hdr4[1] << 8));
    step_index = _clamp_step((int8_t)hdr4[2]);
    // hdr4[3] reserved
}

void ImaAdpcmDecoder::decodePayload(const uint8_t *payload508, int16_t *out)
{
    int32_t pred = predictor;
    int32_t step_idx = step_index;
    uint32_t out_i = 0;

    for (int i = 0; i < FBOX_ADPCM_SAMPLES_PER_BLOCK / 2; i++) {
        uint8_t byte = payload508[i];
        // LSB-first nibble order per fbox spec
        uint8_t codes[2] = { (uint8_t)(byte & 0x0F), (uint8_t)(byte >> 4) };
        for (int k = 0; k < 2; k++) {
            uint8_t code = codes[k];
            int32_t step = kStepTable[step_idx];
            int32_t delta = step >> 3;
            if (code & 4) delta += step;
            if (code & 2) delta += step >> 1;
            if (code & 1) delta += step >> 2;
            pred     = _clamp_i16(pred + ((code & 8) ? -delta : delta));
            step_idx = _clamp_step(step_idx + kIndexTable[code]);
            out[out_i++] = (int16_t)pred;
        }
    }
    predictor  = (int16_t)pred;
    step_index = (int8_t)step_idx;
}

bool fboxDecodeAudio(FboxSource &src, const FboxHeader &hdr, FboxAudio &out)
{
    out.pcm = nullptr;
    out.sample_rate = hdr.audio_sample_rate;
    out.sample_count = 0;

    if (hdr.audio_size == 0) return true;
    if (hdr.audio_size % FBOX_ADPCM_BLOCK_SIZE != 0) {
        Serial.printf("[AUDIO] bad audio_size %lu (not multiple of %d)\n",
                      (unsigned long)hdr.audio_size, FBOX_ADPCM_BLOCK_SIZE);
        return false;
    }

    uint32_t n_blocks = hdr.audio_size / FBOX_ADPCM_BLOCK_SIZE;
    uint32_t n_samples = n_blocks * FBOX_ADPCM_SAMPLES_PER_BLOCK;
    uint32_t pcm_bytes = n_samples * 2;

    if (pcm_bytes > FBOX_AUDIO_PSRAM_CAP) {
        Serial.printf("[AUDIO] %lu PCM bytes exceeds cap %u — skipping decode\n",
                      (unsigned long)pcm_bytes, FBOX_AUDIO_PSRAM_CAP);
        // Still consume the bytes so the cursor ends at EOF.
        uint8_t scratch[512];
        uint32_t remaining = hdr.audio_size;
        while (remaining > 0) {
            int chunk = src.read(scratch, remaining > sizeof(scratch) ? sizeof(scratch) : remaining);
            if (chunk <= 0) break;
            remaining -= (uint32_t)chunk;
        }
        return false;
    }

    out.pcm = (int16_t *)ps_malloc(pcm_bytes);
    if (!out.pcm) {
        Serial.printf("[AUDIO] ps_malloc %lu bytes failed\n", (unsigned long)pcm_bytes);
        return false;
    }

    ImaAdpcmDecoder dec;
    uint8_t block[FBOX_ADPCM_BLOCK_SIZE];
    int16_t *out_ptr = out.pcm;

    for (uint32_t bi = 0; bi < n_blocks; bi++) {
        uint32_t read_total = 0;
        while (read_total < FBOX_ADPCM_BLOCK_SIZE) {
            int r = src.read(block + read_total, FBOX_ADPCM_BLOCK_SIZE - read_total);
            if (r <= 0) {
                Serial.printf("[AUDIO] short read at block %lu (%lu/%d)\n",
                              (unsigned long)bi, (unsigned long)read_total, FBOX_ADPCM_BLOCK_SIZE);
                free(out.pcm); out.pcm = nullptr;
                return false;
            }
            read_total += (uint32_t)r;
        }
        dec.resetFromBlockHeader(block);
        dec.decodePayload(block + 4, out_ptr);
        out_ptr += FBOX_ADPCM_SAMPLES_PER_BLOCK;
    }

    out.sample_count = n_samples;
    Serial.printf("[AUDIO] decoded %lu samples @ %u Hz (%lu bytes PCM)\n",
                  (unsigned long)n_samples, hdr.audio_sample_rate, (unsigned long)pcm_bytes);
    return true;
}

void fboxAudioFree(FboxAudio &a)
{
    if (a.pcm) { free(a.pcm); a.pcm = nullptr; }
    a.sample_count = 0;
}
