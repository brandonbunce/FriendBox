# FBOX Codec

**Status:** Active
**Version:** 4
**Applies to:** `include/io.hpp`, `src/io.cpp`, `src/audio.cpp`, `src/audio_i2s.cpp`, `FriendBox-Server/fbox.py`

---

## What changed in v4

v3 packed audio as one trailing ADPCM section after the last video frame. That worked for files small enough to load into PSRAM whole, but synced playback for larger files required a pre-decode pass that opened a second SD handle — which consistently corrupted memory in a way we never root-caused (IDLE0 walked a freed task-WDT entry, see "Abandoned approaches" below). v4 restructures the file so audio rides alongside video as one IMA ADPCM block per frame; the producer reads them in stream order and there's never a need to seek or re-open. Synced audio works at any file size with no PSRAM-full requirement.

Bundled in the same release:
- **Skip-frame marker ('S')** — a P-frame identical to the previous frame emits only its audio block plus a 1-byte type marker. Saves SPI bandwidth on held frames and shrinks long animation files (up to **−90%** on sketches with long held intervals, measured during migration).
- **Keyframe offset table** — optional trailing index of (frame_idx → byte_offset) for every I-frame, enabling future seek/scrub.
- **Header description field** — 256-byte null-terminated UTF-8 string for human-readable metadata.

v3 files are not readable on v4 clients. The server's `migrate_v4.py` script re-encodes them in place (writes `*.v3.bak` alongside originals).

---

## Format overview

### Header (512 bytes, little-endian)

| Offset | Size | Type | Field | Notes |
|---|---|---|---|---|
| 0 | 4 | char[4] | `magic` | `"FBOX"` |
| 4 | 1 | uint8 | `version` | `4` |
| 5 | 1 | uint8 | `kind` | `0`=sketch, `1`=animation |
| 6 | 2 | uint16 | `frame_count` | Number of frames |
| 8 | 2 | uint16 | `fps` | Playback FPS |
| 10 | 2 | uint16 | `width` | Usually 480 (variable for stickers) |
| 12 | 2 | uint16 | `height` | Usually 480 (variable for stickers) |
| 14 | 4 | uint32 | `created` | Unix timestamp |
| 18 | 32 | char[32] | `username` | UUID hex, null-padded |
| 50 | 4 | uint32 | `expected_file_size` | Total file bytes; cheap pre-CRC truncation check |
| 54 | 2 | uint16 | `audio_sample_rate` | Hz (0 if no audio) |
| 56 | 1 | uint8 | `audio_channels` | `1`=mono (0 if no audio) |
| 57 | 1 | uint8 | *(reserved)* | Alignment, zeroed |
| 58 | 2 | uint16 | `audio_samples_per_frame` | = `sample_rate / fps` (0 if no audio) |
| 60 | 4 | uint32 | `crc32` | CRC32 over bytes [64..EOF]; `0`=skip |
| 64 | 2 | uint16 | `keyframe_count` | `0` if no keyframe table |
| 66 | 4 | uint32 | `keyframe_table_offset` | Absolute byte offset (0 if absent) |
| 70 | 2 | bytes | *(reserved)* | Alignment, zeroed |
| 72 | 256 | char[256] | `description` | Null-terminated UTF-8, ≤ 255 chars |
| 328 | 184 | bytes | *(reserved)* | Zeroed; future fields |

### File layout

```
[0..511]                    Header
[512..]                     Frame size table: frame_count × uint32 LE (chunk size, see below)
[512 + frame_count×4..]     Per-frame chunks (concatenated)
[keyframe_table_offset..]   Optional keyframe table (absent if keyframe_count == 0)
```

### Per-frame chunk

```
[0]               type byte: 0x49 'I' | 0x50 'P' | 0x53 'S'
[1 .. 1+N-1]      IMA ADPCM block (N bytes; omitted entirely when audio_samples_per_frame == 0)
[1+N ..]          Video RLE payload (empty if type == 'S')
```

- **N = 4 + ceil(audio_samples_per_frame / 2)** — constant across the file, set in the header.
- `frame_sizes[i]` holds the **total chunk size** (type byte + audio + video). The producer subtracts `1 + N` to compute its video budget.
- An 'S' (skip) chunk has size `1 + N` (no video bytes). The consumer reuses the previous frame on the LT7680.
- Files with no audio (`audio_sample_rate == 0`) have `N = 0`; chunks are `[type][video]`.

### Keyframe table (optional, trailing)

`keyframe_count` × 8 bytes, each entry:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | `frame_index` (uint32 LE) |
| 4 | 4 | `byte_offset` (uint32 LE) — absolute, from file start |

Encoder writes one entry per emitted I-frame (frame 0 plus any forced or natural I-frames). Client parses it after the last video chunk and stores it for future seek/scrub. Currently unused at playback; logged as `[ANIM] keyframes: N entries parsed`.

### Frame-type details

**I-frame video payload** — RLE-encodes raw 4bpp pixel values:

| Token | Header byte | Data |
|---|---|---|
| Run (bit 7 = 1) | `0x80 \| (count−1)` — count 1–128 | 1 byte, low nibble = palette index |
| Literal (bit 7 = 0) | `count−1` — count 1–128 | ⌈count/2⌉ bytes, nibble-packed, high nibble first |

**P-frame video payload** — identical RLE encoding, but decoded nibbles represent `(current_pixel XOR previous_pixel)`. Unchanged regions produce long runs of `0x00`, compressing much better than the equivalent I-frame. The encoder picks I or P per frame, whichever is smaller. Frame 0 is always I. With `keyframe_interval > 0`, every Nth frame is forced I to bound the dependency chain (e.g. for future seek).

**S-frame ("skip")** — emitted by the encoder when frame N is byte-identical to frame N-1. Carries the audio block but no video RLE. Client reuses the prior frame on the LT7680 canvas (only `displayAnimWriteFrame` mutates display memory; skipping it preserves the previous output) and still paces via `vTaskDelayUntil`.

### IMA ADPCM block (per frame)

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | Initial predictor (int16 LE) — block-seeded; not a sample |
| 2 | 1 | Initial step_index (uint8, 0–88) |
| 3 | 1 | Reserved (0x00) |
| 4 .. N−1 | ceil(samples_per_frame / 2) | nibble-packed codes, LSB-first per byte |

Each block is **independently decodable** — `predictor` and `step_index` are re-seeded from the header on every frame. No cross-frame decoder state. A dropped or skipped frame cannot desynchronize subsequent audio.

---

## Decision: interleaved audio over trailing audio

v3 stored audio as a single ADPCM section after the last video frame. For synced playback the client had to know audio samples before video frame 0 was displayed, which required either loading the whole file into PSRAM (capped at ~7 MB free PSRAM after slot allocation) or pre-decoding the audio via a separate SD handle (memory-corruption symptom, see "Abandoned approaches").

v4 interleaves one ADPCM block per video frame:

- **Naturally synced.** Producer reads chunks in stream order: pull audio block → decode → push to I2S StreamBuffer → decode video → post slot. By the time consumer dispatches frame 0, audio for frame 0 is already in the I2S DMA (or within its ~65 ms cushion).
- **Streamable at any file size.** No PSRAM-full requirement, no `f_lseek` mid-playback. The `FboxSourceRingBuffered` PSRAM ring (2 MB default) plus SDIO 4-bit bandwidth (~4.7 MB/s effective) handles dithered 24 fps content with margin.
- **Skip-frame resilient.** Audio block is independent of frame type; 'S' frames still carry audio so silent video doesn't silence audio.
- **Slightly heavier per-frame overhead** — each frame carries a 4-byte ADPCM header. At 22050 Hz / 24 fps that's 4 / 463 ≈ 0.9% audio overhead vs v3's single block header per 1016 samples. Acceptable for the streaming win.

---

## Decision: skip-frame marker ('S')

P-frames carry an XOR delta; when the delta is uniformly zero (every pixel equal to the previous frame), the encoder emits a full RLE token stream of zero runs that's still hundreds of bytes per frame. v4 detects this case at encode time and emits a 1-byte `'S'` marker with no video RLE bytes at all.

- **Client cost**: zero. Consumer just skips `displayAnimWriteFrame` for SKIP slots and counts the frame as drawn.
- **File-size win**: substantial for any content with long held frames. Migration of 37 v3 files showed −0.5% to −90% size deltas (median −5%; the −90% cases were short loops with extended held intervals).
- **SPI bandwidth**: a SKIP frame skips the LT7680 SPI burst (~30 ms at 24 fps) entirely. Frees core 1 to do other work or just idle.

The encoder still always emits an audio block for SKIP frames so audio sample alignment stays exact.

---

## Decision: keyframe offset table

Future seek/scrub needs O(1) lookup of "the nearest I-frame to time T." Walking the frame_size_table at file open + scanning each chunk's type byte would work but requires reading every frame's type byte before any seek. A trailing offset table replaces that with one `seek + read keyframe_count × 8 bytes`.

- **Server**: tracks every emitted I-frame's byte offset during pack, writes the table at file end, patches `keyframe_table_offset` into the header at finalize.
- **Client**: parses the table after the last frame (so it naturally flows through `FboxSourceCrc`) and stores it on a `std::vector<KeyframeEntry>` on `playFboxAnimation`'s stack frame. Currently only logged; playback ignores it. Seek/scrub will read it once it lands.
- **Cost**: 8 bytes per I-frame. Typical short animation has 5–30 I-frames → 40–240 bytes. Negligible.

`keyframe_interval > 0` in the encoder forces an I-frame every N frames regardless of P-frame size win, bounding the maximum dependency chain length for seek.

---

## Decision: 512-byte header

v3's 128-byte header was tight enough that adding a description and keyframe pointers would have required overlapping reserved bytes. Growing to 512 bytes:

- Adds `description[256]`, `keyframe_count`, `keyframe_table_offset`, `audio_samples_per_frame`, `expected_file_size`.
- Leaves 184 bytes reserved for future fields without forcing another header bump.
- Costs one extra SDIO sector read on file open. The SDIO ISR is interrupt-driven; the actual data transfer is one ~50 µs DMA cycle. Imperceptible.

**Client gotcha:** The full 512 bytes must NOT live on `playFboxAnimation`'s stack. Earlier revisions kept `uint8_t raw_hdr[FBOX_HEADER_SIZE]` on the stack frame; combined with other locals and a downstream `Serial.printf → vsnprintf` call, it pushed loopTask into its 256-byte canary band. `fboxReadHeader` now heap-allocates the buffer internally, computes the CRC seed there, returns only the 4-byte seed via `uint32_t *crc_seed_out`. See [`src/io.cpp:106`](../../src/io.cpp#L106).

---

## Decision: CRC32 checksum

A CRC32 covering bytes [64..EOF] is stored at header offset [60..63]. The covered range starts after the CRC field itself so the field is not included in its own checksum.

- **Verification**: `zlib.crc32(data[64:]) & 0xFFFFFFFF == header.crc32`. Value `0` means no checksum (skip).
- **ESP32**: `esp_rom_crc32_le()` provides hardware-accelerated CRC32. Producer naturally streams every byte through `FboxSourceCrc` during playback — header[64..511] is folded in by `fboxReadHeader`, frame size table + per-frame chunks + keyframe table are folded by the producer's reads. No separate verification pass needed.
- **expected_file_size pre-check**: a cheaper truncation check happens before any decode work. If the source reports a known size (SD), `fboxReadHeader` rejects the file early on mismatch.
- **Server**: computed in `fbox.pack()`, verified in `fbox.unpack()`.

---

## Decision: 4-bit / 16-color palette

Unchanged from v1/v2/v3. A 480×480 frame is exactly 115,200 bytes uncompressed — a constant every layer can hard-code. The palette from androidarts.com gives a consistent aesthetic across all users. Expanding to 8-bit would double the uncompressed frame size and break every fixed-size invariant in the system without a deliberate redesign.

---

## ESP32 decode recipe

### Sketch (single frame)

```
1. FboxSourceSD src(path)            — sequential reader; no seek()
2. fboxReadHeader(src, hdr)          — reads and validates 512 bytes
3. consume frame_count × 4 bytes      — skip frame size table sequentially
4. src.read(&type, 1)                — must be FBOX_FRAME_I (0x49)
5. skip N audio bytes (= 4 + ceil(samples_per_frame/2))  — sketches usually have no audio
6. FboxRleReader rle; rle.begin(&src, width * height)
7. For each row y:
     for each pixel x: pxLine[x] = draw_color_palette[rle.next()]
     displayWriteScanlineSpanned(y, pxLine, width)
```

See [`loadSketchFromSD`](../../src/io.cpp).

### Animation (multi-frame loop)

Playback is **tri-task**:
- **Producer** (core 0, prio 2, 16 KB stack, `fbox_dec`): reads chunks, decodes audio + video, posts ready slots.
- **Consumer** (app-main on core 1): drains ready slots to LT7680 SDRAM, paced by `vTaskDelayUntil`. Skip slots bypass SPI.
- **I2S writer** (core 1, prio 3, 8 KB stack, `i2s_wr`): drains a PSRAM StreamBuffer of mono PCM, expands to stereo, feeds `i2s_channel_write` DMA.
- **Ring loader** (core 1, prio 2, 8 KB stack, `fbox_ld`): pumps SDIO into the 2 MB PSRAM ring inside `FboxSourceRingBuffered`.

Source-agnostic via the `FboxSource` interface — works the same for SD or HTTP. See [`docs/ARCHITECTURE.md`](../ARCHITECTURE.md#iocpp--iohpp) for the full pipeline diagram, token-decoder details, and profiling output.

Decode logic (per frame):

```
Setup once:
  fboxReadHeader(src, hdr, &crc_seed)         — also validates expected_file_size
  CrcSource = FboxSourceCrc(src, crc_seed)
  Read frame_sizes[frame_count] from CrcSource into PSRAM.
  Allocate:
    frame_4bpp[pixel_count / 2]               — INTERNAL SRAM (heap_caps_calloc, MALLOC_CAP_INTERNAL)
    slot[3].frame_buf[pixel_count]            — PSRAM, 32-byte aligned (heap_caps_aligned_alloc)
    audio_block_scratch[N]                    — internal RAM (N ≤ ~470 bytes), if audio
    pcm_scratch[samples_per_frame]            — internal RAM (~1.8 KB), if audio
  Build clut_lut[16] and clut_pair[256] (source-byte → 16-bit fb-pair lookup).
  rle = FboxRleReader bound to CrcSource — ONE instance for the whole stream (4 KB chunk buffer).
  startI2SStreaming(audio_sample_rate, audio_samples_per_frame), if audio.
  Spawn producer task on core 0.

For each frame fi in 0..frame_count-1 (producer):
  1. Read 1 type byte via rle.read_byte() (unbounded — drains any leftover rle.buf bytes from
     the previous frame's audio block read).
  2. If audio enabled: read N audio bytes — drain rle.buf first, then src direct into
     audio_block_scratch. Decode via ImaAdpcmDecoder::decodeOneBlock into pcm_scratch.
     pushI2SSamples(pcm_scratch, samples_per_frame)  — 5 ms timeout, log+drop on timeout.
  3. video_budget = frame_sizes[fi] - 1 - N
  4. If type == FBOX_FRAME_S:
       slot.state = SLOT_SKIP
       (no msync, no decode)
     Else (I or P):
       rle.beginFrame(pixel_count, video_budget_remaining_on_src)
       decodeFrameTokens(rle, is_pf, fb, f4, clut_lut, clut_pair, pixel_count)
       esp_cache_msync(slot.frame_buf, pixel_count, ESP_CACHE_MSYNC_FLAG_DIR_C2M)
       slot.state = SLOT_OK
  5. xSemaphoreGive(sem_ready)
  6. vTaskDelay(1)                            — feed WDT

After last frame (producer):
  If keyframe_count > 0:
    Parse keyframe table via crc_src into ProducerCtx-resident std::vector<KeyframeEntry>.
    (Bytes naturally flow through CRC.)
  Signal SLOT_EOF; set ran_to_eof.

After consumer exits (main task):
  stopI2SStreaming()                          — drains and joins writer.
  Verify FboxSourceCrc.crc() against header crc32 (0 = skip).
```

**Critical invariants** — see [`docs/ARCHITECTURE.md`](../ARCHITECTURE.md#iocpp--iohpp) for full rationale:

- Use **one persistent bounded `FboxRleReader`** across all frames. `beginFrame(pixel_count, video_budget)` resets token state and clamps refill so the reader cannot consume into the next chunk's audio block. Earlier per-frame reader instantiation lost up to ~4 KB of pre-read bytes and scrambled output.
- Allocate `frame_buf` with `heap_caps_aligned_alloc(32, ..., MALLOC_CAP_SPIRAM)`. Cache writeback is mandatory and only fires on 32-byte-aligned addresses.
- Allocate `frame_4bpp` in **internal SRAM**, not PSRAM. Hot read/write per pixel; PSRAM access here cuts decode throughput in half.
- Allocate `audio_block_scratch` + `pcm_scratch` on the **heap**, not the producer's stack. Stack allocations of these sizes have tripped task stack canaries on the loopTask (downstream printf path) and IDLE0 (WDT walk on adjacent corruption).
- Producer task at priority 2 with `vTaskDelay(1)` per frame so IDLE0 stays alive and the task watchdog doesn't trip when the SPI bus is the bottleneck.
- SDIO 4-bit init via `SD_MMC.begin(...)` at `SDMMC_FREQ_HIGHSPEED`. The historical SPI driver and its 4 MHz default are dead code.
- I2S DAC pins (GPIO 38/39/40) **MUST** be `pinMode(OUTPUT) + digitalWrite(LOW)` in `setup()` before anything else runs. Floating pins corrupt PSRAM via the external DAC's input network oscillating.
- **No second `f_open` mid-playback.** Period. See "Abandoned approaches."

---

## Abandoned approaches — do not revive

**Pre-decoding audio from a second SD file handle.** v3 stored audio as a trailing section; a transitional v3.5 client opened a second `FboxSourceSD`, seeked to `file_size - audio_size`, and decoded into a PSRAM PCM buffer before video began. This consistently tripped a memory-corruption symptom that surfaced as IDLE0 walking the task-WDT subscriber list and faulting on a freed entry. Root cause never identified. Symptom only manifested when SDIO was active on core 1 while core 0 was idle — keeping the producer task busy on core 0 masked it, which is why the v4 design works (all SDIO activity overlaps the producer). If you ever consider re-opening the SD file mid-playback for any reason, expect this class of crash to come back.

**Whole-file PSRAM load** (`loadFboxFileToPSRAM`, transitional). Worked but capped playable size at ~7 MB free PSRAM after slot/frame_4bpp allocation. v4 streams; the function is gone. Don't bring it back to "speed up small files" — `FboxSourceRingBuffered` is fast enough and uniform across sizes.

**Buffer-mode I2S (`startI2SPlayback(pcm, count)`).** v3-era API for playing back a single PCM buffer. Replaced in v4 by the streaming `startI2SStreaming` / `pushI2SSamples` / `stopI2SStreaming` trio. Don't reintroduce — there's no v4 code path that produces a single PCM blob.

---

## Memory budget (animation playback)

| Buffer | Size | Location |
|---|---|---|
| `frame_sizes[]` | `frame_count × 4` bytes | PSRAM (`ps_malloc`) |
| `frame_4bpp` | 115,200 bytes | **internal SRAM** (`heap_caps_calloc(..., MALLOC_CAP_INTERNAL)`) |
| `slot[i].frame_buf` × 3 | 230,400 bytes each | PSRAM, 32-byte aligned (`heap_caps_aligned_alloc`) |
| `FboxSourceRingBuffered._storage` | 2,097,152 bytes (default) | PSRAM (`ps_malloc`) |
| I2S StreamBuffer storage | `samples_per_frame × 24` bytes ≈ 22 KB | PSRAM (`heap_caps_malloc(MALLOC_CAP_SPIRAM)`) |
| `FboxRleReader.buf` | 4096 bytes | producer task stack (16 KB stack) |
| `clut_lut[16]` + `clut_pair[256]` | 16 + 512 bytes | `playFboxAnimation` stack |
| `audio_block_scratch` | 4 + ⌈samples_per_frame/2⌉ ≈ 470 bytes | internal RAM heap |
| `pcm_scratch` | `samples_per_frame × 2` ≈ 1.8 KB | internal RAM heap |

PSRAM during playback: slot ring (~690 KB) + RingBuffered storage (2 MB) + I2S StreamBuffer (~22 KB) ≈ **2.7 MB**. Internal SRAM: frame_4bpp (~115 KB) + ADPCM scratches (~2.3 KB) on top of firmware static use and runtime heap. Well within 8 MB PSRAM and 327 KB internal SRAM.

---

## Performance ceiling and content complexity

Post-SDIO + ring-buffered decoder, **all content shapes hit 24+ fps with headroom**. The binding constraint is the LT7680 SPI burst at 80 MHz (~30 ms/frame ≈ 33 fps theoretical), not source bandwidth.

| Content shape | Source bytes/frame | Source MB/s @ 24 fps | Headroom (SDIO ~4.7 MB/s) | Playback |
|---|---:|---:|---|---|
| White background + sparse motion | ~1–5 KB | ~0.1 MB/s | 40× | 24 fps, SPI-bound |
| Mixed content / typical sketches | ~10–40 KB | ~0.5 MB/s | 10× | 24 fps, SPI-bound |
| Heavy motion, partial dither | ~40–80 KB | ~1.4 MB/s | 3× | 24 fps, SPI-bound |
| Fully dithered, no compression headroom | ~115 KB + ~470 B audio | ~2.8 MB/s | 1.7× | **24.4 fps**, measured |

`FboxSourceRingBuffered` (the default SD playback path, see [ARCHITECTURE.md](../ARCHITECTURE.md#iocpp--iohpp)) pipelines SDIO with decode + SPI across both cores. Audio adds ~11 KB/s on top of video — invisible. Path validated to 24 fps on worst-case dithered content; mostly-static content is comfortably SPI-bound and additionally benefits from skip-frame compression.

Per-frame decode budget on core 0:
- RLE + XOR (`decodeFrameTokens`): ~10 ms typical, ~20 ms worst case (pure-literal).
- ADPCM block decode (`decodeOneBlock`): ~1 ms (~919 samples × ~1 µs each).
- Total: ~11 ms typical, 21 ms worst case. Producer slack is ~30 ms/frame (while consumer is doing SPI on core 1). Comfortable margin.

---

## Audio (I2S)

I2S output wiring on PCBA5981: **BCLK=GPIO38, LRCLK=GPIO39, DIN=GPIO40**. Driver in [`src/audio_i2s.cpp`](../../src/audio_i2s.cpp) using the ESP-IDF `driver/i2s_std.h` channel API at 16-bit stereo. Mono ADPCM source is expanded to L/R in the writer task. DMA = 6 × 240 frames ≈ 65 ms cushion at 22050 Hz.

Streaming model:
- Producer decodes one ADPCM block per video frame via `ImaAdpcmDecoder::decodeOneBlock`.
- Producer calls `pushI2SSamples(pcm, samples_per_frame)` per frame — non-blocking with 5 ms timeout, drops on timeout (audio dropout beats video stall).
- Writer task drains the PSRAM-backed StreamBuffer in 256-sample chunks, expands to stereo via a static scratch, calls `i2s_channel_write` with a 500 ms timeout.
- StreamBuffer size ≈ `samples_per_frame × 12 × 2 bytes` ≈ 22 KB at 22050 Hz / 24 fps. Holds ~12 frames of cushion.

API:
- `bool startI2SStreaming(uint32_t sample_rate, uint16_t samples_per_frame)` — installs channel, allocates buffer, spawns writer task.
- `bool pushI2SSamples(const int16_t *pcm, uint32_t n_samples)` — producer pushes per frame.
- `void stopI2SStreaming()` — drains, joins writer, tears down everything.

Sync at frame 0 is naturally ≤65 ms — the DMA cushion absorbs the difference between writer task spawn and the consumer's first `displayAnimWriteFrame`. No explicit pre-buffer phase required.
