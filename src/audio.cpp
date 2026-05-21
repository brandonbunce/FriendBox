#include "audio.hpp"

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

void ImaAdpcmDecoder::decodeOneBlock(const uint8_t *payload, int16_t *out, uint16_t samples)
{
    int32_t pred = predictor;
    int32_t step_idx = step_index;
    uint16_t out_i = 0;

    uint16_t pairs = samples >> 1;
    for (uint16_t i = 0; i < pairs; i++) {
        uint8_t byte = payload[i];
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
    // Odd trailing sample (encoder zero-pads but the trailing nibble may still
    // exist in the payload).
    if (samples & 1) {
        uint8_t code = payload[pairs] & 0x0F;
        int32_t step = kStepTable[step_idx];
        int32_t delta = step >> 3;
        if (code & 4) delta += step;
        if (code & 2) delta += step >> 1;
        if (code & 1) delta += step >> 2;
        pred     = _clamp_i16(pred + ((code & 8) ? -delta : delta));
        step_idx = _clamp_step(step_idx + kIndexTable[code]);
        out[out_i++] = (int16_t)pred;
    }

    predictor  = (int16_t)pred;
    step_index = (int8_t)step_idx;
}
