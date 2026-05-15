#include "io.hpp"
#include "display.hpp"
#include "canvas.hpp"
#include "ui_core.hpp"

Preferences nvs; // https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/storage/nvs_flash.html
SPIClass sdspi = SPIClass(HSPI);


bool initNVS()
{
  nvs.begin("Friendbox", true);
  nvs.end();
  return true;
}

bool initSD(bool forceFormat)
{
#ifdef FRIENDBOX_DEBUG_MODE
    Serial.println("INFO: Initializing SD...");
#endif
    sdspi.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    sdspi.setFrequency(40000000); // 40 MHz — explicit to avoid inheriting display bus speed
    if (!SD.begin(SD_CS, sdspi))
    {
#ifdef FRIENDBOX_DEBUG_MODE
        Serial.println("ERROR: SD mount failed! Is it connected properly?");
#endif
        return false;
    }
    else
    {
        return true;
#ifdef FRIENDBOX_DEBUG_MODE
        Serial.println("INFO: SD ready!");
#endif
    }
}

bool initMenuButton() {
    pinMode(HALL_SENSOR_PIN, INPUT_PULLUP);
    return true;
}

void handleMenuButton(bool recheckInput)
{
    if (currentScreen == SCREEN_CANVAS || currentScreen == SCREEN_CANVAS_MENU)
    {
        static unsigned long lastPress = 0;
        static unsigned int lastButtonState = 0;
        static bool alreadyPressed = false;
        if (digitalRead(HALL_SENSOR_PIN) == LOW) /*Button Pressed*/
        {
            if (lastPress == 0)
            {
                lastPress = millis();
            }
            if (((millis() >= (lastPress + DEBOUNCE_MILLISECONDS)) & !alreadyPressed) || recheckInput)
            {
                Serial.println("Logical Press");
                changeScreenContext(SCREEN_CANVAS_MENU);
                alreadyPressed = true;
            }
            else
            {
                return;
            }
        }
        else /*Button Released*/
        {
            if (lastPress && alreadyPressed)
            {
                lastPress = 0;
                alreadyPressed = false;
                Serial.println("Logical Release.");
                if (currentScreen != SCREEN_CANVAS)
                    changeScreenContext(SCREEN_CANVAS);
            }
        }
    }
}

bool fboxReadHeader(File &f, FboxHeader &out)
{
    uint8_t hdr[FBOX_HEADER_SIZE];
    if ((int)f.read(hdr, FBOX_HEADER_SIZE) != FBOX_HEADER_SIZE) return false;
    if (memcmp(hdr, "FBOX", 4) != 0) return false;
    if (hdr[4] != FBOX_VERSION_3) return false;

    out.version     = hdr[4];
    out.kind        = hdr[5];
    out.frame_count = (uint16_t)hdr[6]  | ((uint16_t)hdr[7]  << 8);
    out.fps         = (uint16_t)hdr[8]  | ((uint16_t)hdr[9]  << 8);
    out.width       = (uint16_t)hdr[10] | ((uint16_t)hdr[11] << 8);
    out.height      = (uint16_t)hdr[12] | ((uint16_t)hdr[13] << 8);
    out.created     = (uint32_t)hdr[14] | ((uint32_t)hdr[15] << 8)
                    | ((uint32_t)hdr[16] << 16) | ((uint32_t)hdr[17] << 24);
    memcpy(out.username, &hdr[18], 32);
    out.username[32] = '\0';
    out.audio_size  = (uint32_t)hdr[50] | ((uint32_t)hdr[51] << 8)
                    | ((uint32_t)hdr[52] << 16) | ((uint32_t)hdr[53] << 24);
    out.audio_sample_rate = (uint16_t)hdr[54] | ((uint16_t)hdr[55] << 8);
    out.audio_channels    = hdr[56];
    out.crc32 = (uint32_t)hdr[58] | ((uint32_t)hdr[59] << 8)
              | ((uint32_t)hdr[60] << 16) | ((uint32_t)hdr[61] << 24);
    return true;
}

void loadSketchFromSD(const char *path)
{
    File f = SD.open(path, FILE_READ);
    if (!f) { Serial.printf("loadSketchFromSD: cannot open %s\n", path); return; }

    FboxHeader hdr;
    if (!fboxReadHeader(f, hdr)) {
        Serial.println("loadSketchFromSD: invalid FBOX header");
        f.close(); return;
    }
    if (hdr.width != TFT_HOR_RES || hdr.height != TFT_VER_RES) {
        Serial.printf("loadSketchFromSD: unexpected size %dx%d\n", hdr.width, hdr.height);
        f.close(); return;
    }

    // Seek past the frame size table to the first frame
    if (!f.seek(FBOX_HEADER_SIZE + (uint32_t)hdr.frame_count * 4)) {
        Serial.println("loadSketchFromSD: seek failed");
        f.close(); return;
    }

    // Frame 0 must be an I-frame
    uint8_t frame_type;
    if (f.read(&frame_type, 1) != 1 || frame_type != FBOX_FRAME_I) {
        Serial.println("loadSketchFromSD: frame 0 is not an I-frame");
        f.close(); return;
    }

    uint16_t pxLine[TFT_HOR_RES];
    displayFrameBegin();

    FboxRleReader rle;
    rle.begin(&f, hdr.width * hdr.height);
    bool ok = true;
    for (int y = 0; y < hdr.height && ok; y++) {
        for (int x = 0; x < hdr.width; x++) {
            int idx = rle.next();
            if (idx < 0) { ok = false; break; }
            pxLine[x] = draw_color_palette[idx];
        }
        if (ok) displayWriteScanlineSpanned(y, pxLine, hdr.width);
    }

    displayFrameEnd();
    f.close();
    Serial.printf("loadSketchFromSD: loaded '%s'\n", path);
}

/*
 * playFboxAnimation — full pipeline from SD file to LT7680 display.
 *
 * PIPELINE OVERVIEW
 * -----------------
 * 1. FILE & HEADER
 *    Open the .fbox file from SD. Read and validate the 128-byte header
 *    (magic, version, frame count, fps, dimensions). Require dimensions
 *    to match TFT_HOR_RES × TFT_VER_RES (480×480).
 *
 * 2. FRAME SIZE TABLE
 *    Immediately after the header: frame_count × uint32 LE entries, each
 *    giving the byte length of the corresponding frame (type byte + RLE
 *    payload). data_base = FBOX_HEADER_SIZE + frame_count × 4 marks where
 *    the first frame's type byte lives. Used for absolute seeks; the RLE
 *    reader's file position after decoding is ignored.
 *
 * 3. PSRAM BUFFERS
 *    frame_4bpp (115,200 B, zeroed): nibble-packed previous/current frame.
 *      Even pixel i lives in the high nibble of frame_4bpp[i/2]; odd in low.
 *      Zeroed at allocation so the first I-frame XOR baseline is clean.
 *    frame_buf  (230,400 B): one 8bpp RGB332 CLUT index per pixel, in raster
 *      order, ready to DMA-blast to LT7680 SDRAM.
 *    clut_lut[16]: pre-computed map from 4bpp palette index → RGB332 byte,
 *      matching the fixed 256-entry RGB332 CLUT programmed into the LT7680.
 *
 * 4. DECODE LOOP (per frame)
 *    Seek absolutely to frame_offset (start of frame fi's type byte).
 *    Read 1 type byte: 'I' (0x49) = intra, 'P' (0x50) = XOR delta.
 *    Arm FboxRleReader with pixel_count = 480×480 = 230,400.
 *      The reader buffers 256 bytes at a time from SD to reduce SPI overhead.
 *      Run tokens:     [0x80|len-1][color_byte] → len identical nibbles.
 *      Literal tokens: [len-1][⌈len/2⌉ nibble-packed bytes] → len nibbles,
 *                      high nibble first, low nibble padding on odd counts.
 *    For each pixel i (0 → 229,999):
 *      raw      = rle.next()                   ← 4-bit value from stream
 *      prev_nib = frame_4bpp[i/2] hi (even i) or lo (odd i) nibble
 *      curr_nib = (P-frame) ? raw ^ prev_nib : raw
 *      Store curr_nib back into frame_4bpp[i/2] for the next P-frame.
 *      frame_buf[i] = clut_lut[curr_nib]       ← RGB332 CLUT index
 *    Advance frame_offset by frame_sizes[fi] (includes the type byte).
 *
 * 5. DISPLAY WRITE
 *    displayAnimFrameBegin:  startWrite() + setCanvasAddress(LT7680_SLOT_ANIM).
 *    displayAnimWriteFrame:  writeRawFrame8bpp — temporarily sets REG[02h] to
 *      natural order (L→R, T→B), arms the active window (0,0)–(479,479),
 *      selects MRWDP (REG[04h]), then CS-held DMA-blasts [0x80]+230,400 bytes
 *      directly into LT7680_SLOT_ANIM SDRAM in one burst.
 *    displayAnimFrameEnd:    waitVSync() then page-flip MISA → LT7680_SLOT_ANIM
 *      so the display hardware scans the freshly written frame on the next
 *      active scan period.
 *
 * KNOWN ISSUE — SINGLE-BUFFER TEARING
 *    After the first frame's flip, MISA = LT7680_SLOT_ANIM permanently and
 *    every subsequent write targets the same slot the display is actively
 *    scanning. At 80 MHz SPI the write takes ~23 ms; a 60 Hz display scan
 *    takes ~16.7 ms. The scan races ahead of the write, so bottom rows are
 *    read by the display before being updated — they show the previous frame.
 *    On the very first playback the unwritten bottom rows contain uninitialised
 *    SDRAM, which appears as garbage.
 *    Fix: ping-pong between LT7680_SLOT_ANIM and LT7680_SLOT_ANIM_B, as
 *    displayFlashPlayFrame() already does.
 */
void playFboxAnimation(const char *path)
{
    File f = SD.open(path, FILE_READ);
    if (!f) { Serial.printf("playFboxAnimation: cannot open %s\n", path); return; }

    FboxHeader hdr;
    if (!fboxReadHeader(f, hdr)) {
        Serial.println("playFboxAnimation: invalid FBOX header");
        f.close(); return;
    }
    if (hdr.frame_count == 0 || hdr.width != TFT_HOR_RES || hdr.height != TFT_VER_RES) {
        Serial.printf("playFboxAnimation: bad header (frames=%d, %dx%d)\n",
                      hdr.frame_count, hdr.width, hdr.height);
        f.close(); return;
    }

    const int      pixel_count = hdr.width * hdr.height;
    const uint32_t frame_ms    = hdr.fps > 0 ? 1000u / hdr.fps : 100u;

    /* Step 2: read frame size table (frame_count × uint32 LE at byte 128).
       data_base is the absolute file offset of the first frame's type byte. */
    uint32_t *frame_sizes = (uint32_t *)malloc((uint32_t)hdr.frame_count * 4);
    if (!frame_sizes) { f.close(); return; }
    {
        uint8_t sb[4];
        for (uint16_t i = 0; i < hdr.frame_count; i++) {
            f.read(sb, 4);
            frame_sizes[i] = (uint32_t)sb[0] | ((uint32_t)sb[1] << 8)
                           | ((uint32_t)sb[2] << 16) | ((uint32_t)sb[3] << 24);
        }
    }
    const uint32_t data_base = FBOX_HEADER_SIZE + (uint32_t)hdr.frame_count * 4;

    Serial.printf("playFboxAnimation: %d frames @ %d fps filesize=%lu\n",
                  hdr.frame_count, hdr.fps, (uint32_t)f.size());

    /* Step 3: pre-compute 4bpp palette → 8bpp RGB332 CLUT index lookup.
       Matches the fixed 256-entry RGB332 CLUT in the LT7680 (_init_clut_rgb332). */
    uint8_t clut_lut[16];
    for (int i = 0; i < 16; i++) {
        uint16_t c = draw_color_palette[i];
        clut_lut[i] = (uint8_t)(((c >> 13) & 7) << 5 |
                                 ((c >>  8) & 7) << 2 |
                                 ((c >>  3) & 3));
    }

    /* Step 3 (buffers):
       frame_4bpp — nibble-packed previous frame for P-frame XOR; zeroed so
                    the I-frame baseline starts clean.
       frame_buf  — 8bpp CLUT indices for the current frame, DMA'd to SDRAM. */
    uint8_t *frame_4bpp = (uint8_t *)ps_calloc((uint32_t)pixel_count >> 1, 1);
    uint8_t *frame_buf  = (uint8_t *)ps_malloc((uint32_t)pixel_count);
    if (!frame_4bpp || !frame_buf) {
        Serial.println("playFboxAnimation: ps_malloc failed");
        free(frame_4bpp); free(frame_buf); free(frame_sizes); f.close(); return;
    }

    bool running = true;

    while (running) {
        uint32_t frame_offset = data_base;

        for (uint16_t fi = 0; fi < hdr.frame_count && running; fi++) {
            uint32_t t0 = millis();

            /* Step 4: seek to frame fi's type byte, then decode pixel_count pixels. */
            f.seek(frame_offset);

            uint8_t frame_type;
            if (f.read(&frame_type, 1) != 1) break;

            Serial.printf("[ANIM] Frame %u  type=%c  offset=0x%06X  size=%lu\n",
                          fi, (char)frame_type, frame_offset, frame_sizes[fi]);

            bool ok = true;
            FboxRleReader rle;
            rle.begin(&f, pixel_count);

            int i = 0;
            for (; i < pixel_count && ok; i++) {
                int raw = rle.next();
                if (raw < 0) {
                    Serial.printf("[ANIM] Frame %u: rle.next()=-1 at pixel %d / %d  file_pos=%u\n",
                                  fi, i, pixel_count, (unsigned)f.position());
                    ok = false; break;
                }

                // For I-frames curr_nib = raw directly.
                // For P-frames curr_nib = raw XOR previous nibble from frame_4bpp.
                uint8_t prev_byte = frame_4bpp[i >> 1];
                uint8_t prev_nib  = (i & 1) ? (prev_byte & 0x0F) : ((prev_byte >> 4) & 0x0F);
                uint8_t curr_nib  = (frame_type == FBOX_FRAME_P)
                                        ? ((uint8_t)raw ^ prev_nib)
                                        : (uint8_t)raw;

                // Write updated nibble back to frame_4bpp for next P-frame
                if (i & 1)
                    frame_4bpp[i >> 1] = (prev_byte & 0xF0) | curr_nib;
                else
                    frame_4bpp[i >> 1] = (prev_byte & 0x0F) | (curr_nib << 4);

                frame_buf[i] = clut_lut[curr_nib];
            }

            Serial.printf("[ANIM] Frame %u: decoded %d / %d pixels  ok=%d\n",
                          fi, i, pixel_count, (int)ok);

            frame_offset += frame_sizes[fi];

            /* Step 5: DMA frame_buf to LT7680_SLOT_ANIM, wait for vsync, page-flip. */
            if (ok) {
                displayAnimFrameBegin();
                displayAnimWriteFrame(frame_buf);
                displayAnimFrameEnd();
                Serial.printf("[ANIM] Frame %u written\n", fi);
            } else {
                Serial.printf("[ANIM] Frame %u SKIPPED (decode error)\n", fi);
            }

            uint32_t elapsed = millis() - t0;
            uint32_t wait    = elapsed < frame_ms ? frame_ms - elapsed : 0;
            uint32_t until   = millis() + wait;
            while (millis() < until) {
                handleTouch();
                if (touchZ > 0) { running = false; break; }
                delay(5);
            }
        }
        if (hdr.frame_count == 1) break;
    }

    free(frame_buf);
    free(frame_4bpp);
    free(frame_sizes);
    f.close();
    Serial.println("playFboxAnimation: done");
    changeScreenContext(SCREEN_CANVAS);
}

std::vector<std::string> sdGetFboxFiles()
{
    std::vector<std::string> fileNames;
    File root = SD.open("/sketches/saved");
    if (root)
    {
        File entry;
        while (entry = root.openNextFile())
        {
            if (!entry.isDirectory())
            {
                Serial.println("Found file: " + String(entry.name()));
                fileNames.push_back(entry.name());
            }
            entry.close();
        }
        root.close();
    }
    return fileNames;
}
