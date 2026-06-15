#include "display.hpp"
#include "canvas.hpp"
#include "idf_compat.hpp"
#include "nvs_store.hpp"
#include <Panel_PCBA5981.hpp>

LGFX tft;
uint16_t touchX, touchY, touchZ;
uint16_t lastTouchX, lastTouchY;
static unsigned long lastTouchTime = 0;

static lgfx::Panel_PCBA5981* pcba_panel()
{
    return static_cast<lgfx::Panel_PCBA5981*>(tft.getPanel());
}

bool initDisplay()
{
    if (!tft.init())
    {
        Serial.println("[DISPLAY] tft.init() FAILED");
        return false;
    }
    Serial.println("[DISPLAY] tft.init() OK");
    tft.setRotation(3); // This option enables suffering. Don't forget to account for coordinate translation!
    tft.setBrightness(255);
    tft.setColorDepth(8);
    //tft.setFont(&DejaVu9);
    return true;
}

//============================================================================
// Backlight brightness (LT7680 internal PWM) — live/commit pattern copied
// from setI2SVolumeLive/commitI2SVolume in audio_i2s.cpp; see display.hpp.
//============================================================================

extern NvsStore nvs;   // defined in io.cpp
static const char *BRT_NVS_NAMESPACE = "Friendbox";
static const char *BRT_NVS_KEY       = "brt_pct";

static uint8_t s_brightness_pct           = 100;
static uint8_t s_brightness_pct_persisted = 100;

static void apply_brightness_pct(uint8_t pct)
{
    if (pct > 100) pct = 100;
    // Floor the duty at ~5% (13/255) so the slider's bottom end dims the
    // screen without blacking it out — the menu must stay findable.
    uint32_t level = (uint32_t)pct * 255u / 100u;
    if (level < 13) level = 13;
    tft.setBrightness((uint8_t)level);
    s_brightness_pct = pct;
}

static void persist_brightness_pct(uint8_t pct)
{
    if (pct == s_brightness_pct_persisted) return;
    if (nvs.begin(BRT_NVS_NAMESPACE, /*read_only=*/false)) {
        nvs.putUInt(BRT_NVS_KEY, (uint32_t)pct);
        nvs.end();
        s_brightness_pct_persisted = pct;
    }
}

void setDisplayBrightnessLive(uint8_t pct)
{
    if (pct > 100) pct = 100;
    if (pct == s_brightness_pct) return;
    apply_brightness_pct(pct);           // PWM registers only; no flash I/O
}

void commitDisplayBrightness(void)
{
    persist_brightness_pct(s_brightness_pct);
}

void setDisplayBrightness(uint8_t pct)
{
    if (pct > 100) pct = 100;
    if (pct == s_brightness_pct) return;
    apply_brightness_pct(pct);
    persist_brightness_pct(pct);
}

uint8_t getDisplayBrightness(void)
{
    return s_brightness_pct;
}

/* Self-explanatory, retrieve brightness from NVS and then apply*/
void loadDisplayBrightnessFromNVS(void)
{
    uint8_t pct = 100;   // default if no saved value or NVS unavailable
    if (nvs.begin(BRT_NVS_NAMESPACE, /*read_only=*/true)) {
        uint32_t stored = nvs.getUInt(BRT_NVS_KEY, 100);
        if (stored > 100) stored = 100;
        pct = (uint8_t)stored;
        nvs.end();
    }
    apply_brightness_pct(pct);
    s_brightness_pct_persisted = pct;
    Serial.printf("[DISPLAY] brightness restored from NVS: %u%%\n", (unsigned)pct);
}

void displayWriteScanline(int x, int y, int w, const uint16_t* data)
{
    pcba_panel()->writeRawPixels(x, y, w, data);
}

void displayFrameBegin() { tft.startWrite(); }
void displayFrameEnd()   { tft.endWrite();   }

static bool _anim_slot_b = false;

void displayAnimFrameBegin()
{
    uint32_t back = _anim_slot_b ? LT7680_SLOT_ANIM_B : LT7680_SLOT_ANIM;
    tft.startWrite();
    pcba_panel()->setCanvasAddress(back);
}

void displayAnimWriteFrame(const uint8_t* clut8)
{
    pcba_panel()->writeRawFrame8bpp(clut8, (uint32_t)TFT_HOR_RES * TFT_VER_RES);
}

void displayAnimCanvasToMenuCache()
{
    pcba_panel()->setCanvasAddress(LT7680_SLOT_MENU);
}

void displayAnimBlitMenuToBack(int x, int y, int w, int h)
{
    uint32_t back = _anim_slot_b ? LT7680_SLOT_ANIM_B : LT7680_SLOT_ANIM;
    pcba_panel()->blitFrames(LT7680_SLOT_MENU, (uint16_t)x, (uint16_t)y,
                              back, (uint16_t)x, (uint16_t)y,
                              (uint16_t)w, (uint16_t)h);
}

void displayAnimFrameEnd()
{
    uint32_t back = _anim_slot_b ? LT7680_SLOT_ANIM_B : LT7680_SLOT_ANIM;
    // Wait for VBlank BEFORE re-pointing MISA so the 4-byte register update at
    // REG[20h..23h] lands while the scanout isn't actively sampling MISA. The
    // original order (setMainImageAddress → waitVSync) wrote MISA at a random
    // scan position; suspected cause of an intermittent stable horizontal
    // shift where one of the four MISA bytes appeared to latch mid-scan.
    pcba_panel()->waitVSync();
    // Datasheet §5 Figure 5-4: reassert every scanout-side register the chip
    // samples (MIW, MWULX, MWULY, MPWCTR, CIW, AW) inside the VBlank window
    // before MISA. Cheap insurance against the persistent horizontal-shift
    // bug — if a stray transient flips one of these registers, the next frame
    // resets it instead of carrying the corruption until reboot.
    pcba_panel()->reassertScanoutConfig(TFT_HOR_RES, TFT_VER_RES);
    pcba_panel()->setMainImageAddress(back);
    _anim_slot_b = !_anim_slot_b;
    pcba_panel()->setCanvasAddress(LT7680_SLOT_CANVAS);
    tft.endWrite();
}

void displayPresentSlot(uint32_t slot)
{
    // Glitch-free page flip for callers that compose a complete frame into an
    // off-screen slot and then want it shown atomically (e.g. the home-screen
    // background cross-fade). Same VBlank-before-MISA ordering and scanout
    // re-assert as displayAnimFrameEnd() — see the notes there.
    tft.startWrite();
    pcba_panel()->waitVSync();
    pcba_panel()->reassertScanoutConfig(TFT_HOR_RES, TFT_VER_RES);
    pcba_panel()->setMainImageAddress(slot);
    tft.endWrite();
}

// BTE fill threshold: runs shorter than this are batched with adjacent literals
// for a single raw write. Break-even is ~5 pixels; 8 gives comfortable margin.
static constexpr int BTE_RUN_THRESHOLD = 8;

void displayWriteScanlineSpanned(int y, const uint16_t* pixels, int w)
{
    auto* panel = pcba_panel();
    int x = 0;
    while (x < w) {
        // Measure run of same-color pixels starting at x
        uint16_t col = pixels[x];
        int run_end = x + 1;
        while (run_end < w && pixels[run_end] == col) run_end++;
        int run_len = run_end - x;

        if (run_len >= BTE_RUN_THRESHOLD) {
            // BTE hardware fill — no per-pixel SPI traffic
            panel->writeFillRectPreclipped(x, y, run_len, 1, (uint32_t)col);
            x = run_end;
        } else {
            // Collect consecutive pixels up to the next BTE-worthy run
            int lit_end = run_end;
            while (lit_end < w) {
                uint16_t c2 = pixels[lit_end];
                int r2 = lit_end + 1;
                while (r2 < w && pixels[r2] == c2) r2++;
                if (r2 - lit_end >= BTE_RUN_THRESHOLD) break;
                lit_end = r2;
            }
            // Write the whole literal segment in one raw burst
            panel->writeRawPixels(x, y, lit_end - x, pixels + x);
            x = lit_end;
        }
    }
}

//============================================================================
// Serial Flash animation API
//============================================================================

uint32_t displayFlashReadJEDECID(void)
{
    tft.startWrite();
    uint32_t id = pcba_panel()->flashReadJEDECID();
    tft.endWrite();
    return id;
}

void displayFlashErase(uint32_t start_addr, uint32_t len)
{
    uint32_t addr = start_addr & ~0xFFFu;  // align down to 4 KB sector
    uint32_t end  = start_addr + len;
    Serial.printf("[FLASH] Erase range 0x%06X–0x%06X (%lu sectors)\n",
                  addr, end, (end - addr + 0xFFF) / 0x1000);
    tft.startWrite();
    while (addr < end) {
        pcba_panel()->flashEraseSector(addr);
        addr += 0x1000;
    }
    tft.endWrite();
}

static inline uint8_t _rgb565_to_clut8(uint16_t c)
{
    return (uint8_t)(((c >> 13) & 0x07) << 5 |
                     ((c >>  8) & 0x07) << 2 |
                     ((c >>  3) & 0x03));
}

void displayFlashWriteFrame(uint16_t frame_idx, uint16_t w, uint16_t h,
                             const uint16_t* pixels)
{
    uint32_t n_pixels   = (uint32_t)w * h;
    uint32_t frame_bytes = n_pixels;              // 8bpp: 1 byte per pixel
    uint32_t start_addr  = (uint32_t)frame_idx * frame_bytes;

    Serial.printf("[FLASH] Write frame %u  addr=0x%06X  %u×%u  %lu bytes\n",
                  frame_idx, start_addr, w, h, frame_bytes);

    displayFlashErase(start_addr, frame_bytes);

    tft.startWrite();
    uint8_t page_buf[256];
    uint32_t pix_done = 0;
    while (pix_done < n_pixels) {
        uint16_t chunk = (uint16_t)((n_pixels - pix_done) > 256
                                     ? 256
                                     : (n_pixels - pix_done));
        for (uint16_t i = 0; i < chunk; i++)
            page_buf[i] = _rgb565_to_clut8(pixels[pix_done + i]);
        pcba_panel()->flashPageProgram(start_addr + pix_done, page_buf, chunk);
        pix_done += chunk;
    }
    tft.endWrite();

    Serial.printf("[FLASH] Frame %u write complete\n", frame_idx);
}

void displayFlashPlayFrame(uint16_t frame_idx, uint16_t w, uint16_t h)
{
    uint32_t frame_bytes = (uint32_t)w * h;       // 8bpp: 1 byte per pixel
    uint32_t flash_addr  = (uint32_t)frame_idx * frame_bytes;

    // Ping-pong: DMA into whichever slot is not currently displayed,
    // then page-flip. Prevents scanlines from writing into the active frame.
    static bool use_b = false;
    uint32_t back = use_b ? LT7680_SLOT_ANIM_B : LT7680_SLOT_ANIM;

    Serial.printf("[ANIM] Play frame %u  flash=0x%06X  back=0x%08X\n",
                  frame_idx, flash_addr, back);

    tft.startWrite();
    pcba_panel()->dmaFlashBlock(flash_addr, w, back, 0, 0, w, h);
    pcba_panel()->waitVSync();
    pcba_panel()->setMainImageAddress(back);
    tft.endWrite();

    use_b = !use_b;
}

void displayFlashRead(uint32_t addr, uint8_t* buf, uint32_t len)
{
    tft.startWrite();
    // flashReadBytes takes uint16_t len; chunk for larger reads.
    uint32_t done = 0;
    while (done < len) {
        uint16_t chunk = (uint16_t)((len - done) > 4096 ? 4096 : (len - done));
        pcba_panel()->flashReadBytes(addr + done, buf + done, chunk);
        done += chunk;
    }
    tft.endWrite();
}

void displayFlashProgram(uint32_t addr, const uint8_t* data, uint32_t len)
{
    tft.startWrite();
    uint32_t done = 0;
    while (done < len) {
        // Programs must not cross a 256-byte page boundary; clamp each chunk to
        // the remaining bytes in the current page.
        uint32_t page_left = 256 - ((addr + done) & 0xFF);
        uint16_t chunk = (uint16_t)((len - done) < page_left ? (len - done) : page_left);
        pcba_panel()->flashPageProgram(addr + done, data + done, chunk);
        done += chunk;
    }
    tft.endWrite();
}

void displayDmaFlashToCanvas(uint32_t flash_addr, uint16_t w, uint16_t h,
                             uint32_t canvas_addr, uint16_t dst_x, uint16_t dst_y)
{
    tft.startWrite();
    pcba_panel()->dmaFlashBlock(flash_addr, w, canvas_addr, dst_x, dst_y, w, h);
    tft.endWrite();
}

void handleTouch()
{
    uint16_t localTouchX, localTouchY;
    if (tft.getTouch(&localTouchX, &localTouchY) && (localTouchX >= 0 && localTouchX < TFT_HOR_RES &&
                                                     localTouchY >= 0 && localTouchY < TFT_VER_RES))
    {
        /*Serial.print("Touch - X: ");
        Serial.print(localTouchX);
        Serial.print(" Y: ");
        Serial.println(localTouchY);*/

        /* Place last touch coordinate into lastX/lastY for tracking movement changes. */
        lastTouchX = touchX;
        lastTouchY = touchY;

        /* Update current touch coordinate to newest input. */
        touchX = localTouchX;
        touchY = localTouchY;

        /* Mark as touching. */
        touchZ = 1;
    }
    else
    {
        /* Stash the last time we were touching the display.*/
        lastTouchTime = millis();

        /* Mark as no longer touching the display. */
        touchZ = 0;
    }
}