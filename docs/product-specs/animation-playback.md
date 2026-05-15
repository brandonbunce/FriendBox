# Animation Playback

**Status:** In progress  
**Implemented:** `playFboxAnimation` (io.cpp), `networkDownloadFbox` (network.cpp)

---

## User story

A FriendBox receives an animation (a multi-frame .fbox file) from the server and plays it back full-screen at the correct frame rate. The user can dismiss the animation by touching the screen and return to the drawing canvas.

---

## UI flow

```
Boot
  └─ initFriendbox() completes
       └─ playSketchFromServer("1776835153465")
            ├─ [if not cached] Loading screen "Downloading sketch..."
            │       └─ networkDownloadFbox → /sketches/received/{id}.fbox
            └─ playFboxAnimation(/sketches/received/{id}.fbox)
                 ├─ Frames play full-screen at file FPS
                 ├─ Loops continuously
                 └─ Touch anywhere → exit → SCREEN_CANVAS
```

---

## API contract

**Endpoint:** `GET https://friendbox.chocolatedonut.dev/api/download/sketch/{sketch_id}`

**Response:** `application/octet-stream` — raw .fbox v3 file body. See [fbox-codec.md](../design-docs/fbox-codec.md) for format.

**Error cases:**
- HTTP 404 → file not found on server; show error loading screen, return to canvas.
- HTTP non-200 → treat as download failure; do not write partial file to SD.
- Connection timeout (60 s) → same as non-200.

---

## Edge cases

| Case | Behavior |
|---|---|
| File already on SD | Skip download; play immediately |
| `frame_count == 0` | Log error, return without displaying anything |
| `width` or `height` ≠ 480 | Log error, return — device only supports 480×480 |
| `ps_malloc` fails (frame_4bpp or frame_buf) | Log error, return |
| Single-frame file | Display once, return (no loop) |
| WiFi not connected | Download attempt fails; if file cached, play from SD |
| SD not mounted | Both download and playback fail with Serial error |
| Touch during download | Not currently interruptible — loading screen blocks |
| CRC mismatch on download | Not yet verified on-device; server verifies on pack |

---

## Done criteria

- [ ] Animation with ≥ 2 frames from the server plays at the correct FPS
- [ ] Loop is seamless (no visible gap between last and first frame)
- [ ] Touching the screen during playback exits to the canvas
- [ ] Second boot does not re-download (file cached on SD)
- [ ] v3 files (I-frames and P-frames) decode correctly
- [ ] Device does not reset (watchdog) during download of a large file
- [ ] Audio plays via I2S in sync with frames (pending speaker hardware)

---

## Known limitations

- Touch is not interruptible during the download phase (blocking HTTP stream).
- Audio bytes in the .fbox are parsed but not played — speaker and I2S driver not yet implemented.
- The sketch ID is currently hard-coded in `main.cpp`. A proper receive flow (server push notification → download → play) is not yet designed.
- CRC32 is not verified on-device after download. `networkDownloadFbox` could accumulate the CRC while streaming and compare against the header value once complete.
