# Animation Playback

**Status:** Playback pipeline complete, hitting the 24fps perf target on dithered content. End-to-end receive UX (server push → auto-play) still pending.

**Implemented:** [playFboxAnimation](../../src/io.cpp), [playFboxAnimationFromSD](../../src/io.cpp), [networkDownloadFbox](../../src/network.cpp), [FboxSourceSD / FboxSourceRingBuffered / FboxSourceHTTP / FboxSourceCrc](../../src/fbox_source.cpp), [startI2SStreaming / pushI2SSamples](../../src/audio_i2s.cpp).

---

## User story

A FriendBox receives an animation (a multi-frame .fbox file) from the server and plays it back full-screen at the correct frame rate with synchronized audio. The user can dismiss the animation by touching the screen and return to the drawing canvas.

---

## UI flow

```
Boot
  └─ initFriendbox() completes
       └─ playSketchFromServer("1776835153465")
            ├─ [if not cached] Loading screen "Downloading sketch..."
            │       └─ networkDownloadFbox → /sd/sketches/received/{id}.fbox
            └─ playFboxAnimationFromSD(/sd/sketches/received/{id}.fbox)
                 ├─ Frames play full-screen, paced by file FPS
                 ├─ Audio streams to I2S DAC in lockstep
                 └─ Touch anywhere → exit → SCREEN_CANVAS
```

---

## Implementation

### Format

.fbox **v4** — 512-byte header, per-frame chunks of `[type 'I'/'P'/'S'][N-byte IMA ADPCM block][video RLE]`, optional trailing keyframe offset table, CRC32 over [64..EOF]. Audio is interleaved per video frame (one ADPCM block per frame, ~919 samples at 22050 Hz / 24 fps) so a frame can stream from any source at any file size. Full spec in [fbox-codec.md](../design-docs/fbox-codec.md). Encoder is `FriendBox-Server/fbox.py`; legacy v3 files are not readable — re-encode via `migrate_v4.py` on the server.

### Source abstraction

[FboxSource](../../include/fbox_source.hpp) is the sequential byte-source interface playback consumes — `read(dst, n)` + `reset()` + `size()`. Three concrete sources share the same playback core:

| Source | Backing | Where it matters |
|---|---|---|
| `FboxSourceSD` | FATFS `f_open` / `f_read` at drive `0:` | Cached or pre-downloaded sketches. Bypasses VFS + POSIX wrappers (~150–250 µs saved per refill). |
| `FboxSourceHTTP` | `esp_http_client_open` / `esp_http_client_read` | Direct streaming from server (no SD round-trip). HTTPS via the mbedTLS certificate bundle. |
| `FboxSourceRingBuffered` | wraps another source; loader task fills a 2 MB PSRAM stream buffer | Decouples decoder demand from source latency. Default for SD playback. |

`FboxSourceCrc` is a non-buffering wrapper that folds every byte into a CRC32 accumulator — `playFboxAnimation` wraps its source with it and checks against `header.crc32` at EOF.

### Three-task pipeline (dual-core)

```
       CPU 0                          CPU 1
  ┌──────────────────┐         ┌──────────────────┐
  │ producer task    │ slots   │ consumer         │
  │ "fbox_dec" p=2   │────────▶│ (main task) p=1  │
  │                  │ ready/  │                  │
  │ - read chunk     │ free    │ - SPI burst      │
  │ - decode RLE     │ sems    │   into LT7680    │
  │ - msync slot     │         │ - waitVSync      │
  │ - push I2S PCM   │ stream  │ - vTaskDelayUntil│
  └──────────────────┘ buffer  │   (frame pacing) │
           │                   └──────────────────┘
           ▼                            │
                                        ▼
                              ┌──────────────────┐
                              │ i2s_wr task p=3  │
                              │ (CPU 1)          │
                              │ drains PCM →     │
                              │ I2S DMA          │
                              └──────────────────┘

Optional: fbox_ld task (CPU 1, p=2) — pulls source bytes into
the FboxSourceRingBuffered PSRAM ring ahead of the decoder.
```

**Producer** ([io.cpp:`producerTask`](../../src/io.cpp)): reads one frame chunk via the source, decodes the ADPCM block and pushes PCM to the I2S stream buffer, then RLE-decodes the video into a 4-bit internal-SRAM scratch, applies XOR delta for P-frames, expands to 8bpp into the slot's PSRAM frame_buf, and finally `esp_cache_msync(... DIR_C2M)` to make the cache lines visible to the consumer's DMA on the other core.

**Consumer** (the main task running `playFboxAnimation`): waits on `sem_ready`, calls `displayAnimFrameBegin/Write/End` (a single CS-held SPI burst of 230,400 bytes from PSRAM → LT7680 SDRAM, then `waitVSync` and page-flip), polls touch, and uses `vTaskDelayUntil(prev_wake, frame_ms)` for precise pacing.

**I2S writer** ([audio_i2s.cpp:`writerTask`](../../src/audio_i2s.cpp)): drains the PSRAM stream buffer 256 mono samples at a time, expands mono → stereo, writes to the I2S DMA channel.

### Ring slots and synchronization

| Resource | Count / size | Allocation |
|---|---|---|
| Video slots (`frame_buf`) | 3 × 230,400 B = ~690 KB | `heap_caps_aligned_alloc(32, …, MALLOC_CAP_SPIRAM \| MALLOC_CAP_DMA)` |
| 4bpp XOR baseline | 115,200 B | `MALLOC_CAP_INTERNAL` — touched per-pixel in decode, internal SRAM keeps the hot loop fast |
| Audio block scratch | per-frame size | `MALLOC_CAP_INTERNAL` |
| PCM scratch | `samples_per_frame × 2 B` | `MALLOC_CAP_INTERNAL` |
| I2S stream buffer | ~12 frames of audio cushion | `MALLOC_CAP_SPIRAM` |
| HTTP/SD ring buffer (optional) | 2 MB default | `MALLOC_CAP_SPIRAM` |

Producer/consumer hand-off uses two FreeRTOS counting semaphores (`sem_free` initialised to 3, `sem_ready` initialised to 0). Each frame is one `slot_idx = (slot_idx + 1) % 3` step. Slot states: `SLOT_OK`, `SLOT_SKIP` (consumer reuses previous frame, no SPI burst), `SLOT_EOF`, `SLOT_UNDER` (source underrun), `SLOT_ERR` (decode error).

### Performance discipline

A handful of non-obvious requirements that the pipeline depends on — each one was a regression that cost us frames:

- **Producer on CPU 0, consumer on CPU 1.** Both on the same core serialised them to a single buffer's worth of throughput regardless of `RING_SLOTS`. Pinned via `xTaskCreatePinnedToCore` for the producer and `CONFIG_ESP_MAIN_TASK_AFFINITY_CPU1=y` in [sdkconfig.defaults](../../sdkconfig.defaults) for the consumer (the main task). Wrong pinning = 14.8 fps; correct = 24.4 fps.
- **Video slots need `MALLOC_CAP_DMA`.** The IDF spi_master driver falls back to a bounce-buffer through internal RAM when handed a plain-SPIRAM buffer. Same alloc surfaces this for the SPI consumer's burst.
- **`esp_cache_msync` after every decode.** PSRAM is cached. Producer writes go into the data cache; without the C2M flush the consumer's DMA on the other core sees stale lines. Slots must be 32-byte aligned for this to succeed (the cache line size).
- **PSRAM not used as XIP target.** `CONFIG_SPIRAM_FETCH_INSTRUCTIONS` / `CONFIG_SPIRAM_RODATA` are deliberately off — mapping code or rodata into PSRAM puts CPU instruction fetches on the same octal bus the SPI DMA is reading frames from.
- **Producer must `vTaskDelay(1)` per frame** — feeds IDLE0 / task-WDT. Same applies to the `FboxSourceRingBuffered` loader on SD. The original SD-over-SPI workaround imposed ~30 ms of forced delay per frame; on SDIO the interrupt-driven driver yields naturally, so `vTaskDelay(1)` is enough.
- **One SD file handle during playback.** Opening a second handle mid-playback consistently tripped IDLE0 memory corruption. v4's interleaved layout means one handle suffices.
- **I2S DAC pins held LOW before init.** GPIO 38/39/40 must be `gpio_set_direction(OUTPUT)` + `gpio_set_level(0)` at the top of `app_main` before anything else — floating pins let the external DAC oscillate and corrupt PSRAM. See [src/main.cpp:`preInitI2SDacPins`](../../src/main.cpp).

### Per-frame profiling

Each `[ANIM] done:` line in the monitor prints (`spi_avg`, `wait_avg`, `skips`) for the consumer and (`decode_avg`, `msync_avg`, `wait_free_avg`, `refill_avg`, `adpcm_avg`, `refills/frame`, `refill_us/call`) for the producer. Interpretation guide in the [project-fbox-decoder-profiling](../../memory/project-fbox-decoder-profiling.md) memory note — tells you which lever to pull next when something regresses.

Current numbers for the reference 24 fps dithered animation: `fps=24.4`, `spi_avg=30 ms`, `wait_free_avg=3.4 ms`, `decode_avg=35 ms`, ring buffer `stalls=1 / 2 ms total`.

---

## API contract

**Endpoint:** `GET https://friendbox.chocolatedonut.dev/api/download/sketch/{sketch_id}`

**Response:** `application/octet-stream` — raw .fbox v4 file body. See [fbox-codec.md](../design-docs/fbox-codec.md) for format.

**Transport:** `esp_http_client` over HTTPS, certificate bundle (`esp_crt_bundle_attach`) validates the server cert against the IDF-provided CA bundle.

**Error cases:**
- HTTP 404 → file not found on server; show error loading screen, return to canvas.
- HTTP non-2xx → treat as download failure; partial file on SD is `unlink`'d.
- Connection timeout (60 s) → same as non-2xx.
- TLS handshake failure → returns with the same error path.

---

## Edge cases

| Case | Behavior |
|---|---|
| File already on SD | Skip download (`stat` returns 0); play immediately |
| `frame_count == 0` | Header validation fails (`fboxReadHeader` returns false) → caller surfaces error |
| `width` or `height` ≠ 480 | Validation fails — device only supports 480×480 |
| Slot allocation fails | `playFboxAnimation` returns `PlaybackResult::OOM` |
| Single-frame file | Display once, return (no loop) |
| WiFi not connected | Download attempt fails fast; if file cached, play from SD |
| SD not mounted | Both download and playback fail with logged error |
| Touch during playback | `handleTouch` polled once per frame → `PlaybackResult::USER_CANCELLED` |
| Touch during download | Not currently interruptible — `esp_http_client_perform` blocks |
| Source underrun mid-stream | Producer flags `SLOT_UNDER`; consumer returns `READ_UNDERRUN` |
| Decode error (bad RLE / type) | `SLOT_ERR` → consumer returns `DECODE_ERROR` |
| CRC32 mismatch at EOF | Logged; result still surfaces playback status, but `[ANIM] CRC OK` line shows whether it matched |

---

## Done criteria

- [x] Animation with ≥ 2 frames from the server plays at the correct FPS (24 fps on the dithered reference)
- [x] Touching the screen during playback exits to the canvas
- [x] Second boot does not re-download (file cached on SD)
- [x] v4 files (I-frames, P-frames, skip frames) decode correctly
- [x] Device does not reset (watchdog) during download or playback of a large file
- [x] Audio plays via I2S in sync with frames
- [x] CRC32 verified end-to-end against the header value
- [ ] Loop seamlessly between last and first frame (current behaviour: plays once, returns)
- [ ] Server push → auto-play receive flow (sketch ID is still hard-coded in [src/main.cpp](../../src/main.cpp))

---

## Known limitations

- Touch is not interruptible during the download phase (`esp_http_client_perform` is blocking).
- The sketch ID is currently hard-coded in `main.cpp`. A proper receive flow (server push notification → download → play) is not yet designed.
- `FboxSourceHTTP` is implemented but not yet wired through the UI — downloads always land on SD first and then play via `FboxSourceSD`. Streaming directly from HTTP would skip the SD round-trip but adds risk of stalls on flaky WiFi.
- Looping playback requires `FboxSource::reset()` on the ring-buffered wrapper, which is currently a no-op (returns false). Single-shot only today.
