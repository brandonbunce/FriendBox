#include "display.hpp"
#include "canvas.hpp"
#include "idf_compat.hpp"
#include <Panel_PCBA5981.hpp>

LGFX tft;
uint16_t touchX, touchY, touchZ;
uint16_t lastTouchX, lastTouchY;
static unsigned long lastTouchTime = 0;

// Register baseline captured at the end of initDisplay() for the bug
// diagnostic: the FBOX horizontal-shift bug is a one-shot persistent chip
// state corruption — once tripped, it stays until ESP32 reboot. Comparing
// runtime register values against this baseline on every animation
// iteration tells us which register flipped at the moment the bug appears.
// Sized for the current snapshot count (~70 bytes); add slack.
static uint8_t  g_reg_baseline[128];
static size_t   g_reg_baseline_count = 0;
static bool     g_reg_baseline_captured = false;

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

    // Capture chip-register baseline so the per-iteration diff can detect
    // any register that gets clobbered during runtime.
    tft.startWrite();
    g_reg_baseline_count = pcba_panel()->snapshotRegisters(
        g_reg_baseline, sizeof(g_reg_baseline));
    tft.endWrite();
    if (g_reg_baseline_count == 0) {
        Serial.println("[REG BASELINE] FAILED — snapshot buffer too small");
    } else {
        g_reg_baseline_captured = true;
        Serial.printf("[REG BASELINE] captured %u regs at init:", (unsigned)g_reg_baseline_count);
        const uint8_t *addrs = lgfx::Panel_PCBA5981::snapshotRegisterAddresses();
        for (size_t i = 0; i < g_reg_baseline_count; i++) {
            Serial.printf(" %02x=%02x", addrs[i], g_reg_baseline[i]);
        }
        Serial.println();
    }

    return true;
}

void displayDiagDiffRegistersAgainstBaseline(int iter_label)
{
    if (!g_reg_baseline_captured) return;
    uint8_t current[sizeof(g_reg_baseline)];
    tft.startWrite();
    size_t n = pcba_panel()->snapshotRegisters(current, sizeof(current));
    tft.endWrite();
    if (n != g_reg_baseline_count) return;

    const uint8_t *addrs = lgfx::Panel_PCBA5981::snapshotRegisterAddresses();
    int diff_count = 0;
    for (size_t i = 0; i < n; i++) {
        if (current[i] != g_reg_baseline[i]) {
            if (diff_count == 0) {
                Serial.printf("[REG DIFF iter=%d] ", iter_label);
            }
            Serial.printf("REG[%02xh]=0x%02x(was 0x%02x) ", addrs[i], current[i], g_reg_baseline[i]);
            diff_count++;
        }
    }
    if (diff_count > 0) Serial.println();
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
    pcba_panel()->setMainImageAddress(back);
    _anim_slot_b = !_anim_slot_b;
    pcba_panel()->setCanvasAddress(LT7680_SLOT_CANVAS);
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