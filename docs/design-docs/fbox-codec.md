# FBOX Codec

**Status:** Active  
**Version:** 3  
**Applies to:** `include/io.hpp`, `src/io.cpp`, `FriendBox-Server/fbox.py`

---

## Format overview

FBOX is the binary format for all FriendBox sketches and animations. It is defined in the Python server (`fbox.py`) and decoded on-device in `io.cpp`. v3 is the only supported version; v1/v2 files can be converted with `migrate_v3.py`.

### Header (128 bytes, little-endian)

| Offset | Size | Type | Field | Notes |
|---|---|---|---|---|
| 0 | 4 | char[4] | `magic` | `"FBOX"` |
| 4 | 1 | uint8 | `version` | `3` |
| 5 | 1 | uint8 | `kind` | `0`=sketch, `1`=animation |
| 6 | 2 | uint16 | `frame_count` | Number of frames |
| 8 | 2 | uint16 | `fps` | Playback FPS |
| 10 | 2 | uint16 | `width` | Always 480 (variable for stickers) |
| 12 | 2 | uint16 | `height` | Always 480 (variable for stickers) |
| 14 | 4 | uint32 | `created` | Unix timestamp |
| 18 | 32 | char[32] | `username` | UUID hex, null-padded |
| 50 | 4 | uint32 | `audio_size` | Bytes of IMA ADPCM audio (0=none) |
| 54 | 2 | uint16 | `audio_sample_rate` | Hz (0 if no audio) |
| 56 | 1 | uint8 | `audio_channels` | `1`=mono (0 if no audio) |
| 57 | 1 | uint8 | *(reserved)* | Zeroed |
| 58 | 4 | uint32 | `crc32` | CRC32 over bytes [62..EOF]; `0`=skip |
| 62 | 66 | bytes | *(reserved)* | Zeroed |

### File layout

```
[0..127]               Header
[128..]                Frame size table: frame_count × uint32 LE
[128 + frame_count×4]  Frame data — concatenated, each prefixed by a type byte
[after frames]         IMA ADPCM audio (if audio_size > 0)
```

### Frame data

Each frame entry in the frame data section:

```
[0]    type byte: 0x49 ('I') = intra-frame | 0x50 ('P') = delta-frame
[1..]  RLE-encoded payload
```

The frame size table entry for each frame includes the type byte, so `frame_sizes[i]` bytes cover both.

**I-frame payload** — encodes raw 4bpp pixel values:

| Token | Header byte | Data |
|---|---|---|
| Run (bit 7 = 1) | `0x80 \| (count−1)` — count 1–128 | 1 byte, low nibble = palette index |
| Literal (bit 7 = 0) | `count−1` — count 1–128 | ⌈count/2⌉ bytes, nibble-packed, high nibble first |

**P-frame payload** — identical RLE encoding, but the decoded nibbles represent `(current_pixel XOR previous_pixel)` for each pixel. Unchanged regions produce long runs of `0x00`, compressing much better than the equivalent I-frame. The encoder chooses whichever encoding (I or P) is smaller for each frame. Frame 0 is always an I-frame.

---

## Decision: XOR delta frames over pure per-frame RLE

v2 RLE encoded every frame independently. v3 adds XOR delta (P-frames) on top of the same RLE token format:

- **Temporal redundancy**: static backgrounds and slow-moving content XOR to `0x00`, which RLE-encodes as a single 2-byte run token regardless of region size. A frame with 80% static background can compress to a fraction of the equivalent I-frame.
- **Decoder cost**: negligible. One extra read + XOR per pixel in the decode loop. The `frame_4bpp` buffer (115,200 bytes PSRAM) stores the previous frame's nibble-packed pixels for XOR. Both I and P frames share a single decode path; the only branch is `curr_nib = (P-frame) ? raw ^ prev_nib : raw`.
- **Dithered images**: worst-case for RLE (alternating pixels). Using 128-pixel literal tokens, overhead is < 1% versus raw nibble-packed. XOR delta still helps for unchanged regions even in dithered animations.
- **Frame 0 invariant**: frame 0 is always an I-frame. When a looping animation restarts, the decoder re-enters at frame 0 (always I), so `frame_4bpp` is fully overwritten before any P-frame depends on it.

---

## Decision: IMA ADPCM over Opus/WebM

v2 stored Opus audio in a WebM container. The ESP32 has no hardware Opus decoder and software decode is too heavy for real-time playback alongside animation. v3 uses IMA ADPCM:

- **Decoder**: ~30 lines of C, two integer lookup tables, no division or floats. Runs easily on a second core with I2S DMA output.
- **Compression**: 4 bits/sample → 4:1 over 16-bit PCM. At 22,050 Hz mono: ~11 KB/s. A 30-second clip = ~330 KB.
- **Block format** (512 bytes per block):

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | Initial predictor (int16 LE) — encoder state, not a sample itself |
| 2 | 1 | Initial step_index (uint8, 0–88) |
| 3 | 1 | Reserved (0x00) |
| 4..511 | 508 | 1016 nibbles, LSB-first per byte |

- **Server encoding**: `fbox.adpcm_encode(pcm_bytes)` — accepts raw 16-bit LE PCM from ffmpeg (`-f s16le -ar 11025 -ac 1`).
- **ESP32 decode**: not yet wired to I2S (speaker hardware planned). `audio_size`, `audio_sample_rate`, and `audio_channels` are parsed from the header and available in `FboxHeader` when the audio subsystem is implemented.

---

## Decision: CRC32 checksum

A CRC32 covering bytes [62..EOF] is stored at header offset [58..61]. The covered range starts after the CRC field itself so the field is not included in its own checksum.

- **Verification**: `zlib.crc32(data[62:]) & 0xFFFFFFFF == header.crc32`. Value `0` means no checksum (skip).
- **ESP32**: `esp_rom_crc32_le()` provides hardware-accelerated CRC32. Verification is best done once after download; `playFboxAnimation` does not re-verify on every play.
- **Server**: computed in `fbox.pack()`, verified in `fbox.unpack()`.

---

## Decision: 4-bit / 16-color palette

Unchanged from v1/v2. A 480×480 frame is exactly 115,200 bytes uncompressed — a constant every layer can hard-code. The palette from androidarts.com gives a consistent aesthetic across all users. Expanding to 8-bit would double the uncompressed frame size and break every fixed-size invariant in the system without a deliberate redesign.

---

## ESP32 decode recipe

### Sketch (single frame)

```
1. FboxSourceSD src(path)            — sequential reader; no seek()
2. fboxReadHeader(src, hdr)          — reads and validates 128 bytes
3. consume frame_count × 4 bytes      — skip frame size table sequentially
4. src.read(&type, 1)                — must be FBOX_FRAME_I (0x49)
5. FboxRleReader rle; rle.begin(&src, width * height)
6. For each row y:
     for each pixel x: pxLine[x] = draw_color_palette[rle.next()]
     displayWriteScanlineSpanned(y, pxLine, width)
```

### Animation (multi-frame loop)

Playback is dual-core. The producer task on core 0 decodes frames into a 3-slot PSRAM ring; the consumer (app-main on core 1) drains slots to the LT7680. Source-agnostic via the `FboxSource` interface — works the same for SD, HTTP, or PSRAM-resident input. See [`docs/ARCHITECTURE.md`](../ARCHITECTURE.md#iocpp--iohpp) for the full pipeline diagram.

Decode logic (per slot):

```
Setup once:
  CrcSource = FboxSourceCrc(user_source, crc_seed_over_header[62..127])
  Read frame size table into PSRAM (also fed into CRC).
  Allocate in PSRAM:
    frame_4bpp[pixel_count / 2]  — nibble-packed previous-frame baseline
    slot[3].frame_buf[pixel_count] — 8bpp CLUT indices, 32-byte aligned
  rle = FboxRleReader bound to CrcSource — ONE instance for the whole stream

For each frame fi in 0..frame_count-1:
  frame_type = rle.read_byte()        — FBOX_FRAME_I or FBOX_FRAME_P
  rle.beginFrame(pixel_count)         — resets token state, KEEPS chunk buffer
  for i in 0..pixel_count-1:
    raw      = rle.next()             — 0–15 from RLE stream
    prev_nib = nibble i from frame_4bpp
    curr_nib = (P-frame) ? raw ^ prev_nib : raw
    write curr_nib → frame_4bpp[i]
    slot.frame_buf[i] = clut_lut[curr_nib]
  esp_cache_msync(slot.frame_buf, pixel_count, ESP_CACHE_MSYNC_FLAG_DIR_C2M)
  xSemaphoreGive(sem_ready)
  vTaskDelay(1)                       — feed WDT

After last frame:
  Verify FboxSourceCrc.crc() against header crc32 (0 = skip).
  Decode any audio section into PSRAM PCM via fboxDecodeAudio(src, hdr, &audio).
```

**Critical invariants** — see [`docs/ARCHITECTURE.md`](../ARCHITECTURE.md#iocpp--iohpp) for full rationale:

- Use **one persistent `FboxRleReader`** across all frames. Discarding it per-frame loses up to 255 pre-read next-frame bytes and shifts every subsequent frame's start offset → scrambled output and premature EOF.
- Allocate `frame_buf` with `heap_caps_aligned_alloc(32, ..., MALLOC_CAP_SPIRAM)`. Cache writeback is mandatory and only fires on 32-byte-aligned addresses.
- Producer task at priority 2 with `vTaskDelay(1)` per frame so IDLE0 stays alive and the task watchdog doesn't trip when the SPI bus is the bottleneck.

---

## Memory budget (animation playback)

| Buffer | Size | Location |
|---|---|---|
| `frame_sizes[]` | `frame_count × 4` bytes | PSRAM (`ps_malloc`) |
| `frame_4bpp` | 115,200 bytes | PSRAM (`ps_calloc`) |
| `slot[i].frame_buf` × 3 | 230,400 bytes each | PSRAM, 32-byte aligned (`heap_caps_aligned_alloc`) |
| `FboxRleReader.buf` | 256 bytes | task stack |
| `FboxAudio.pcm` (optional) | ≤ `FBOX_AUDIO_PSRAM_CAP` = 1 MB | PSRAM (`ps_malloc`) |

Total PSRAM during playback: ~960 KB for video, up to ~1.96 MB with audio. Well within 8 MB.

---

## Audio

Audio is stored as IMA ADPCM blocks after the last frame. `fboxDecodeAudio` decodes the trailing section into a PSRAM `int16` PCM buffer (`FboxAudio`). The decoder is in [`src/audio.cpp`](../../src/audio.cpp) and ports the step/index tables verbatim from `fbox.py`.

I2S output is not yet wired up — decoded PCM sits in PSRAM until speaker hardware lands. There is a known byte-handoff bug: bytes still inside the producer's `FboxRleReader` chunk buffer at video EOF are the first bytes of the audio section, so direct reads from the source skip them and decoded PCM starts mid-block. Tracked in [`docs/exec-plans/tech-debt-tracker.md`](../exec-plans/tech-debt-tracker.md). Will be fixed alongside the I2S work via a shared `FboxSourceBuffered`.

When implementation lands:
- Read one 512-byte block from the (buffered) source
- Decode 1016 samples per block using `ImaAdpcmDecoder`
- Output decoded int16 samples to I2S DMA
- Advance to next block; repeat until `audio_size` bytes consumed
