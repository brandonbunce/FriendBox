/*----------------------------------------------------------------------------/
  Lovyan GFX panel driver for the FriendBox PCBA5981 hardware revision.
  See Panel_PCBA5981.hpp for protocol notes.
/----------------------------------------------------------------------------*/

#include "Panel_PCBA5981.hpp"

#include "arduino_compat.h"

#include <lgfx/v1/Bus.hpp>
#include <lgfx/v1/platforms/common.hpp>
#include <lgfx/v1/misc/pixelcopy.hpp>

namespace lgfx
{
 inline namespace v1
 {
//----------------------------------------------------------------------------

// SDRAM Auto Refresh Interval (REG[E3h-E2h]). Datasheet §13 Table 13-7
// recommends 0x061A for LT7680A-R/B-R (MCLK=100 MHz, 4K rows, Tref=64ms).
// Earlier value 486 (0x01E6) assumed 60 MHz MCLK + 8K rows — neither of
// which match our PLL config (N=100, R=5, OD=1 → 100 MHz MCLK) or the
// SDRAR setting (REG[E0]=0x29 → 4K rows). Refreshing 3.2x too often eats
// SDRAM bandwidth and starves host writes during the per-frame burst,
// dropping bytes destined for the last SDRAM rows.
//   sdram_itv = (64000000 / 8192) / (1000 / 60) - 2  = 486
static constexpr uint16_t SDRAM_REFRESH_INTERVAL = 0x061A;

static constexpr uint32_t CANVAS_BASE_ADDR = 0;

//============================================================================
// Low-level SPI bus helpers
//============================================================================

void Panel_PCBA5981::_cmd16(uint16_t word)
{
    _bus->wait();
    cs_control(true);
    cs_control(false);
    _bus->writeCommand(word, 16);
}

void Panel_PCBA5981::_write_reg(uint8_t reg, uint8_t data)
{
    _flg_memorywrite = false;

    _bus->wait();
    cs_control(true);
    cs_control(false);
    _bus->writeCommand((uint32_t)reg << 8, 16);            // [0x00][reg]

    _bus->wait();
    cs_control(true);
    cs_control(false);
    _bus->writeCommand(((uint32_t)data << 8) | 0x80, 16);  // [0x80][data]
}

void Panel_PCBA5981::_write_reg16(uint8_t reg_lo, uint16_t value)
{
    _write_reg(reg_lo,     (uint8_t)(value      ));
    _write_reg(reg_lo + 1, (uint8_t)(value >>  8));
}

void Panel_PCBA5981::_write_reg32(uint8_t reg_lsb, uint32_t value)
{
    _write_reg(reg_lsb,     (uint8_t)(value      ));
    _write_reg(reg_lsb + 1, (uint8_t)(value >>  8));
    _write_reg(reg_lsb + 2, (uint8_t)(value >> 16));
    _write_reg(reg_lsb + 3, (uint8_t)(value >> 24));
}

uint8_t Panel_PCBA5981::_read_status(void)
{
    _bus->wait();
    cs_control(true);
    cs_control(false);
    _bus->writeCommand(0x40, 8);
    _bus->beginRead(0);
    uint8_t s = (uint8_t)_bus->readData(8);
    _bus->endRead();
    cs_control(true);
    return s;
}

bool Panel_PCBA5981::_wait_busy(uint32_t timeout_ms)
{
    auto t = millis();
    while (_read_status() & 0x08)
    {
        if (millis() - t > timeout_ms) return false;
    }
    cs_control(false);
    return true;
}

void Panel_PCBA5981::_wait_sdram_ready(void)
{
    auto t = millis();
    uint8_t s;
    while (((s = _read_status()) & 0x04) == 0x00)
    {
        if (millis() - t > 500)
        {
            Serial.printf("[PANEL] _wait_sdram_ready TIMEOUT status=0x%02X\n", s);
            cs_control(false);
            return;
        }
    }
    Serial.printf("[PANEL] SDRAM ready (status=0x%02X, %lums)\n", s, millis() - t);
    cs_control(false);
}

// Read one byte from the LT7680 memory port: [0xC0][_data_].
// CS toggled around the full 16-bit transaction; one byte returned per call.
uint8_t Panel_PCBA5981::_read_byte(void)
{
    _bus->wait();
    cs_control(true);
    cs_control(false);
    _bus->writeCommand(0xC0, 8);
    _bus->beginRead(0);
    uint8_t b = (uint8_t)_bus->readData(8);
    _bus->endRead();
    cs_control(true);
    return b;
}

//============================================================================
// Mid-level drawing helpers
//============================================================================

void Panel_PCBA5981::_set_active_window(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    _write_reg16(0x56, x);                  // Active Window upper-left X
    _write_reg16(0x58, y);                  // Active Window upper-left Y
    _write_reg16(0x5A, w);                  // Active Window width
    _write_reg16(0x5C, h);                  // Active Window height
    _write_reg16(0x5F, x);                  // Graphic write cursor X
    _write_reg16(0x61, y);                  // Graphic write cursor Y
}

void Panel_PCBA5981::_start_memorywrite(void)
{
    if (_flg_memorywrite) return;

    _set_active_window(_win_xs, _win_ys,
                       _win_xe - _win_xs + 1,
                       _win_ye - _win_ys + 1);

    // Select MRWDP (REG[04h]); subsequent [0x80][byte] frames write into SDRAM.
    _bus->wait();
    cs_control(true);
    cs_control(false);
    _bus->writeCommand((uint32_t)0x04 << 8, 16);
    _bus->wait();
    cs_control(true);

    _flg_memorywrite = true;
}

// Configure the read window and prime REG[04h] for sequential reads.
// REG[03h] bits[1:0] = 00b selects the Image buffer (Display RAM) for both
// reads and writes; direction is implicit in the next port op (read = [0xC0],
// write = [0x80]). The first read after switching to the read port is dummy
// per datasheet section 13.4 - we discard it here.
void Panel_PCBA5981::_start_memoryread(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    _flg_memorywrite = false;

    _set_active_window(x, y, w, h);

    _write_reg(0x03, 0x00);  // ICR: graphic mode, target = Image buffer

    // Select MRWDP (REG[04h]) so subsequent [0xC0] reads pull pixel data.
    _bus->wait();
    cs_control(true);
    cs_control(false);
    _bus->writeCommand((uint32_t)0x04 << 8, 16);
    _bus->wait();
    cs_control(true);

    (void)_read_byte();  // discard dummy first byte
}

// RGB565 → RGB332 palette index: top-3 R, top-3 G, top-2 B.
static inline uint8_t rgb565_to_clut8(uint16_t c)
{
    return (uint8_t)(((c >> 13) & 0x07) << 5 |   // R[7:5]
                     ((c >>  8) & 0x07) << 2 |   // G[4:2]
                     ((c >>  3) & 0x03));          // B[1:0]
}

void Panel_PCBA5981::_write_pixel16(uint16_t color)
{
    // Canvas is 8bpp: send one palette-index byte per pixel.
    uint8_t idx = rgb565_to_clut8(color);
    _cmd16((uint16_t)((idx << 8) | 0x80));
}

void Panel_PCBA5981::_set_forecolor(uint32_t rawcolor)
{
    uint16_t c = (uint16_t)rawcolor;
    _write_reg(0xD2, (uint8_t)(c >> 8));
    _write_reg(0xD3, (uint8_t)(c >> 3));
    _write_reg(0xD4, (uint8_t)(c << 3));
}

// Program a fixed 256-entry RGB332 palette into the chip CLUT.
// Entry i maps to R = i[7:5] expanded to 8 bits, G = i[4:2], B = i[1:0].
// REG[CEh] PCLUT_SA: start index. REG[CFh] PCLUT_D: R then G then B, auto-increments.
void Panel_PCBA5981::_init_clut_rgb332(void)
{
    _write_reg(0xCE, 0x00);   // start at palette entry 0
    for (int i = 0; i < 256; i++) {
        uint8_t r = (uint8_t)(((i >> 5) & 0x07) * 255 / 7);
        uint8_t g = (uint8_t)(((i >> 2) & 0x07) * 255 / 7);
        uint8_t b = (uint8_t)(( i       & 0x03) * 255 / 3);
        _write_reg(0xCF, r);
        _write_reg(0xCF, g);
        _write_reg(0xCF, b);
    }
}

//============================================================================
// Transaction management
//============================================================================

void Panel_PCBA5981::beginTransaction(void) { begin_transaction(); }
void Panel_PCBA5981::endTransaction(void)   { end_transaction();   }

void Panel_PCBA5981::begin_transaction(void)
{
    if (_in_transaction) return;
    _in_transaction  = true;
    _flg_memorywrite = false;
    _bus->beginTransaction();
}

void Panel_PCBA5981::end_transaction(void)
{
    if (!_in_transaction) return;
    _in_transaction = false;
    _bus->wait();
    cs_control(true);
    _bus->endTransaction();
}

//============================================================================
// Multi-frame SDRAM addressing
//============================================================================

// LT7680 §13.4 — VSYNC interrupt lives in INTEN/INTF, bit 4 (0x10):
//   REG[0Bh] INTEN: bit4 = VSYNC Time Base Interrupt Enable
//   REG[0Ch] INTF : bit4 = VSYNC flag (read=status, write-1-to-clear)
// (Earlier code used REG[F0h]/[F1h] bit3 — that's the RA8876 map and does
//  nothing on the LT7680. The wait silently timed out every frame, so
//  setMainImageAddress() landed at a random point in the scan cycle and
//  produced the bottom-strip tearing artifact.)
void Panel_PCBA5981::waitVSync(uint32_t timeout_ms)
{
    bool tr = _in_transaction;
    if (!tr) begin_transaction();

    uint8_t inten = _read_reg_byte(0x0B);
    _write_reg(0x0B, inten | 0x10);   // enable VSYNC interrupt
    _write_reg(0x0C, 0x10);            // clear any stale VSYNC flag

    auto t = millis();
    while (!(_read_reg_byte(0x0C) & 0x10)) {
        if (millis() - t > timeout_ms) break;
    }

    _write_reg(0x0C, 0x10);            // clear the flag
    _write_reg(0x0B, inten);           // restore prior INTEN

    if (!tr) end_transaction();
}

void Panel_PCBA5981::setMainImageAddress(uint32_t addr)
{
    bool tr = _in_transaction;
    if (!tr) begin_transaction();
    _write_reg32(0x20, addr);   // MISA: Main Image Start Address
    if (!tr) end_transaction();
}

uint32_t Panel_PCBA5981::readMainImageAddress(void)
{
    bool tr = _in_transaction;
    if (!tr) begin_transaction();
    uint32_t v = (uint32_t)_read_reg_byte(0x20)
               | ((uint32_t)_read_reg_byte(0x21) <<  8)
               | ((uint32_t)_read_reg_byte(0x22) << 16)
               | ((uint32_t)_read_reg_byte(0x23) << 24);
    if (!tr) end_transaction();
    return v;
}

uint16_t Panel_PCBA5981::readMainImageWidth(void)
{
    bool tr = _in_transaction;
    if (!tr) begin_transaction();
    uint16_t v = (uint16_t)_read_reg_byte(0x24)
               | ((uint16_t)_read_reg_byte(0x25) << 8);
    if (!tr) end_transaction();
    return v;
}

uint16_t Panel_PCBA5981::readCanvasImageWidth(void)
{
    bool tr = _in_transaction;
    if (!tr) begin_transaction();
    uint16_t v = (uint16_t)_read_reg_byte(0x54)
               | ((uint16_t)_read_reg_byte(0x55) << 8);
    if (!tr) end_transaction();
    return v;
}

uint16_t Panel_PCBA5981::readMainWindowUpperLeftX(void)
{
    bool tr = _in_transaction;
    if (!tr) begin_transaction();
    uint16_t v = (uint16_t)_read_reg_byte(0x26)
               | ((uint16_t)_read_reg_byte(0x27) << 8);
    if (!tr) end_transaction();
    return v;
}

uint16_t Panel_PCBA5981::readMainWindowUpperLeftY(void)
{
    bool tr = _in_transaction;
    if (!tr) begin_transaction();
    uint16_t v = (uint16_t)_read_reg_byte(0x28)
               | ((uint16_t)_read_reg_byte(0x29) << 8);
    if (!tr) end_transaction();
    return v;
}

void Panel_PCBA5981::reassertScanoutConfig(uint16_t width, uint16_t height)
{
    bool tr = _in_transaction;
    if (!tr) begin_transaction();
    // Datasheet §5 Figure 5-4 — every scanout-side register the chip samples
    // to display the main window. Rewriting these per page-flip means a stray
    // clobber to any one of them only corrupts a single frame instead of
    // becoming a persistent post-reset-only artefact (see horizontal-shift bug).
    _write_reg16(0x24, width);     // MIW   (Figure 5-4 step 2)
    _write_reg16(0x26, 0);         // MWULX (step 3)
    _write_reg16(0x28, 0);         // MWULY (step 4)
    _write_reg(0x10, 0x00);        // MPWCTR main-window color depth = 8bpp (step 5)
    _write_reg16(0x54, width);     // CIW   (canvas, write-side)
    _set_active_window(0, 0, width, height);
    _flg_memorywrite = false;
    if (!tr) end_transaction();
}

void Panel_PCBA5981::setCanvasAddress(uint32_t addr)
{
    bool tr = _in_transaction;
    if (!tr) begin_transaction();
    _write_reg32(0x50, addr);   // CVSSA: Canvas Start Address
    _canvas_addr = addr;        // mirror so BTE source/dest registers follow
    _flg_memorywrite = false;   // force active-window reload on next access
    if (!tr) end_transaction();
}

//============================================================================
// Backlight PWM (datasheet §9 Pulse Width Modulation, registers §13.7)
//============================================================================
// PWM timer clock = CCLK / (PSCLR+1). CCLK = 100 MHz (pll_core N=100, R=5,
// OD=1), so PSCLR=4 → 20 MHz timer clock. With a 10-bit period (TCNTB=1023)
// the PWM output runs at ~19.5 kHz — above the audible range so the backlight
// boost inductor can't whine, and far above flicker perception.
static constexpr uint8_t  PWM_PRESCALE = 4;
static constexpr uint16_t PWM_PERIOD   = 1023;   // TCNTB; duty resolution 1/1024

// One-time PWM setup, called at the end of init(). The ER-PCBA5981 board
// datasheet doesn't document whether the J2 "internal PWM" jump point routes
// PWM[0] or PWM[1] to the backlight circuit, so both timers are programmed
// identically and both pins output their timer — whichever one the board
// uses, it gets the right waveform. The other pin is unconnected on CON1.
void Panel_PCBA5981::_init_backlight_pwm(void)
{
    _write_reg(0x84, PWM_PRESCALE);    // PSCLR: prescaler
    // PMUXR: Timer-0/1 divisors = /1 (bits[7:4]=0000b),
    //        PWM[1] = Timer-1 output (bits[3:2]=10b),
    //        PWM[0] = Timer-0 output (bits[1:0]=10b).
    _write_reg(0x85, 0x0A);
    _write_reg16(0x8A, PWM_PERIOD);    // TCNTB0: Timer-0 period
    _write_reg16(0x8E, PWM_PERIOD);    // TCNTB1: Timer-1 period
    _write_reg16(0x88, PWM_PERIOD);    // TCMPB0: full duty (backlight on)
    _write_reg16(0x8C, PWM_PERIOD);    // TCMPB1: full duty
    // PCFGR: Timer-1 auto-reload + start (bits 5,4), Timer-0 auto-reload +
    // start (bits 1,0). Inverters off, dead zone off.
    _write_reg(0x86, 0x33);
}

// Output is high while the down-counter is <= TCMPB (inverter off), so
// duty_high = (TCMPB+1)/(TCNTB+1). brightness 255 maps to TCMPB=TCNTB =
// always-high; brightness 0 maps to TCMPB=0 which still spends one timer
// tick high per cycle (1/1024 duty) — visually off. Auto-reload latches the
// new compare value at the next cycle boundary, so updates are glitch-free.
void Panel_PCBA5981::setBrightness(uint8_t brightness)
{
    bool tr = _in_transaction;
    if (!tr) begin_transaction();
    uint16_t duty = (uint16_t)(((uint32_t)brightness * PWM_PERIOD + 127) / 255);
    _write_reg16(0x88, duty);   // TCMPB0
    _write_reg16(0x8C, duty);   // TCMPB1
    if (!tr) end_transaction();
}

// Filled rectangle via the Geometric Drawing Engine (datasheet pg. 145).
// Programs the rectangle endpoints into REG[68h-6Fh], the foreground colour
// into REG[D2h-D4h], then sets REG[76h] = bit7|bit6|bits[5:4]=10b which is
// "start | fill | square". Polls STSR bit3 for completion.
void Panel_PCBA5981::fillRectGPU(uint16_t x1, uint16_t y1,
                                        uint16_t x2, uint16_t y2,
                                        uint16_t rgb565)
{
    bool tr = _in_transaction;
    if (!tr) begin_transaction();

    // The GDE clips its output against the Active Window. LovyanGFX pixel-write
    // paths shrink the AW to a per-scanline strip via _start_memorywrite(), so
    // unless we restore it here the engine renders into a sliver and looks
    // like a no-op. Reference: ER-TFT040-3 demo always calls
    // Active_Window_XY/WH(0,0,W,H) before any DrawSquare_Fill/DrawCircle_Fill.
    _set_active_window(0, 0, timing.h_display, timing.v_display);

    // Foreground colour - REG[D2h] R[7:3] in bits[7:3], REG[D3h] G[7:2] in
    // bits[7:2], REG[D4h] B[7:3] in bits[7:3]. _set_forecolor handles the
    // bit-packing for an RGB565 value.
    _set_forecolor(rgb565);

    // Start (top-left) and end (bottom-right) points.
    _write_reg16(0x68, x1);   // DLHSR : start X
    _write_reg16(0x6A, y1);   // DLVSR : start Y
    _write_reg16(0x6C, x2);   // DLHER : end   X
    _write_reg16(0x6E, y2);   // DLVER : end   Y

    // DCR1 = 0xE0:
    //   bit7=1          start
    //   bit6=1          fill
    //   bits[5:4]=10b   draw square
    // (Earlier 0xD0 selected bits[5:4]=01 = curve, which silently did nothing.)
    _write_reg(0x76, 0xE0);

    _wait_busy();
    _flg_memorywrite = false;

    if (!tr) end_transaction();
}

// Filled circle via the Geometric Drawing Engine (datasheet pg. 145).
// REG[7Bh/7Ch] = centre X, REG[7Dh/7Eh] = centre Y (both 13-bit).
// REG[77h/78h] = major (X) radius, REG[79h/7Ah] = minor (Y) radius - set
// equal for a true circle. REG[76h] DCR1:
//   bit7=1   start
//   bit6=1   fill
//   bits[5:4]=00b   draw circle / ellipse
// => kick value 0xC0. Polls STSR bit3 for completion via _wait_busy().
void Panel_PCBA5981::fillCircleGPU(uint16_t cx, uint16_t cy,
                                          uint16_t r, uint16_t rgb565)
{
    bool tr = _in_transaction;
    if (!tr) begin_transaction();

    // GDE output is clipped to the Active Window; LovyanGFX pixel paths leave
    // the AW pinned to a single scanline strip. Restore full canvas before
    // the kick (matches the reference driver's pattern around DrawCircle_Fill).
    _set_active_window(0, 0, timing.h_display, timing.v_display);

    _set_forecolor(rgb565);

    _write_reg16(0x7B, cx);   // ELL_X0 : centre X
    _write_reg16(0x7D, cy);   // ELL_Y0 : centre Y
    _write_reg16(0x77, r);    // ELL_A  : major (X) radius
    _write_reg16(0x79, r);    // ELL_B  : minor (Y) radius

    _write_reg(0x76, 0xC0);   // DCR1: start | fill | circle/ellipse

    _wait_busy();
    _flg_memorywrite = false;

    if (!tr) end_transaction();
}

// Rounded-rect via the Geometric Drawing Engine (datasheet §6.6, pg. 50).
// Start/stop endpoints go into REG[68h-6Fh] (same as plain rect); corner X/Y
// radii go into REG[77h-7Ah] (same as circle/ellipse axes); foreground colour
// goes into REG[D2h-D4h]. REG[76h] DCR1 kick:
//   bit7=1          start
//   bit6=fill       1 = filled, 0 = outline
//   bits[5:4]=11b   rounded-rectangle
// => 0xF0 for fill, 0xB0 for outline. Datasheet notes 1/2: the rectangle must
// be wider than 2*rx+1 and taller than 2*ry+1; LovyanGFX's r = min(w,h)>>2
// (used by LGFX_Button) trivially satisfies that.
void Panel_PCBA5981::_round_rect_kick(uint16_t x1, uint16_t y1,
                                      uint16_t x2, uint16_t y2,
                                      uint16_t rx, uint16_t ry,
                                      uint16_t rgb565, bool fill)
{
    bool tr = _in_transaction;
    if (!tr) begin_transaction();

    // GDE clips against the Active Window; LovyanGFX pixel paths leave it
    // pinned to a scanline strip. Restore full canvas before the kick.
    _set_active_window(0, 0, timing.h_display, timing.v_display);

    _set_forecolor(rgb565);

    _write_reg16(0x68, x1);   // DLHSR : start X
    _write_reg16(0x6A, y1);   // DLVSR : start Y
    _write_reg16(0x6C, x2);   // DLHER : end   X
    _write_reg16(0x6E, y2);   // DLVER : end   Y

    _write_reg16(0x77, rx);   // ELL_A : X-axis (long) corner radius
    _write_reg16(0x79, ry);   // ELL_B : Y-axis (short) corner radius

    _write_reg(0x76, fill ? 0xF0 : 0xB0);

    _wait_busy();
    _flg_memorywrite = false;

    if (!tr) end_transaction();
}

void Panel_PCBA5981::fillRoundRectGPU(uint16_t x1, uint16_t y1,
                                      uint16_t x2, uint16_t y2,
                                      uint16_t rx, uint16_t ry,
                                      uint16_t rgb565)
{
    _round_rect_kick(x1, y1, x2, y2, rx, ry, rgb565, true);
}

void Panel_PCBA5981::drawRoundRectGPU(uint16_t x1, uint16_t y1,
                                      uint16_t x2, uint16_t y2,
                                      uint16_t rx, uint16_t ry,
                                      uint16_t rgb565)
{
    _round_rect_kick(x1, y1, x2, y2, rx, ry, rgb565, false);
}

void Panel_PCBA5981::blitFrames(uint32_t src_addr, uint16_t src_x, uint16_t src_y,
                                  uint32_t dst_addr, uint16_t dst_x, uint16_t dst_y,
                                  uint16_t w, uint16_t h)
{
    if (w == 0 || h == 0) return;

    bool tr = _in_transaction;
    if (!tr) begin_transaction();

    // Memory copy with ROP=copy, op=positive direction memory copy.
    _write_reg(0x91, 0xC2);
    // S0 = 8bpp, dest = 8bpp.
    _write_reg(0x92, 0x00);

    // S0 (source).
    _write_reg32(0x93, src_addr);
    _write_reg16(0x97, (uint16_t)timing.h_display);
    _write_reg16(0x99, src_x);
    _write_reg16(0x9B, src_y);

    // Destination.
    _write_reg32(0xA7, dst_addr);
    _write_reg16(0xAB, (uint16_t)timing.h_display);
    _write_reg16(0xAD, dst_x);
    _write_reg16(0xAF, dst_y);

    _write_reg16(0xB1, w);
    _write_reg16(0xB3, h);

    _write_reg(0x90, 0x10);   // BTE start
    _wait_busy();

    _flg_memorywrite = false;

    if (!tr) end_transaction();
}

// BTE Memory Copy with Opacity (Picture Mode), datasheet §7.6.9 / Table 7-1
// op code 1010b. Blends two SDRAM regions with a single whole-bitmap alpha:
//     DT = (S0 * alpha) + (S1 * (1 - alpha))
// where alpha32 is the REG[B5h] level (0..31 -> 0..31/32). For a UI fade-in,
// pass S0 = overlay slot (e.g. SLOT_UI), S1 = dst = the background canvas, and
// ramp alpha32 0 -> 31. All three regions share the panel width (8bpp).
// NOTE: in 8bpp *index* mode the blend is over palette indices, not RGB, so the
// visual result is only a true cross-fade if the palette is arranged for it;
// the UI layer falls back to a dither reveal if this looks wrong on hardware.
void Panel_PCBA5981::blitFramesAlpha(uint32_t s0_addr, uint16_t s0_x, uint16_t s0_y,
                                     uint32_t s1_addr, uint16_t s1_x, uint16_t s1_y,
                                     uint32_t dst_addr, uint16_t dst_x, uint16_t dst_y,
                                     uint16_t w, uint16_t h, uint8_t alpha32)
{
    if (w == 0 || h == 0) return;
    if (alpha32 > 31) alpha32 = 31;

    bool tr = _in_transaction;
    if (!tr) begin_transaction();

    // op = Memory Copy with opacity (1010b); 8bpp start bit (high nibble 0).
    _write_reg(0x91, 0x0A);
    // S0 = 8bpp, S1 = 8bpp, dest = 8bpp.
    _write_reg(0x92, 0x00);
    // Picture-mode whole-bitmap alpha level.
    _write_reg(0xB5, (uint8_t)(alpha32 & 0x3F));

    // S0 (overlay).
    _write_reg32(0x93, s0_addr);
    _write_reg16(0x97, (uint16_t)timing.h_display);
    _write_reg16(0x99, s0_x);
    _write_reg16(0x9B, s0_y);

    // S1 (background).
    _write_reg32(0x9D, s1_addr);
    _write_reg16(0xA1, (uint16_t)timing.h_display);
    _write_reg16(0xA3, s1_x);
    _write_reg16(0xA5, s1_y);

    // Destination.
    _write_reg32(0xA7, dst_addr);
    _write_reg16(0xAB, (uint16_t)timing.h_display);
    _write_reg16(0xAD, dst_x);
    _write_reg16(0xAF, dst_y);

    _write_reg16(0xB1, w);
    _write_reg16(0xB3, h);

    _write_reg(0x90, 0x10);   // BTE start
    _wait_busy();

    _flg_memorywrite = false;

    if (!tr) end_transaction();
}

//============================================================================
// ST7701S bit-bang init
//============================================================================

namespace {

// One byte of 9-bit serial: leading bit selects command(0) or data(1),
// followed by 8 data bits MSB-first, all clocked on the rising edge.
// :: qualifications force Arduino's global pinMode/digitalWrite over the
// LovyanGFX lgfx::v1 overloads that are visible from inside this namespace.
inline void st7701s_send(int pin_cs, int pin_clk, int pin_din,
                         uint8_t value, bool is_data)
{
    ::digitalWrite(pin_cs, LOW);

    ::digitalWrite(pin_clk, LOW);
    ::digitalWrite(pin_din, is_data ? HIGH : LOW);
    ::digitalWrite(pin_clk, HIGH);

    for (int n = 0; n < 8; ++n)
    {
        ::digitalWrite(pin_clk, LOW);
        ::digitalWrite(pin_din, (value & 0x80) ? HIGH : LOW);
        ::digitalWrite(pin_clk, HIGH);
        value <<= 1;
    }

    ::digitalWrite(pin_cs, HIGH);
}

inline void st7701s_cmd (int cs, int clk, int din, uint8_t v) { st7701s_send(cs, clk, din, v, false); }
inline void st7701s_data(int cs, int clk, int din, uint8_t v) { st7701s_send(cs, clk, din, v, true ); }

} // namespace

void Panel_PCBA5981::_st7701s_init_sequence(void)
{
    const int cs  = st7701s_pins.pin_cs;
    const int clk = st7701s_pins.pin_clk;
    const int din = st7701s_pins.pin_din;

    ::pinMode(cs,  OUTPUT);
    ::pinMode(clk, OUTPUT);
    ::pinMode(din, OUTPUT);
    ::digitalWrite(cs,  HIGH);
    ::digitalWrite(clk, LOW);
    ::digitalWrite(din, LOW);

    // ---- Command 2 BK0 ----
    st7701s_cmd (cs, clk, din, 0xFF);
    st7701s_data(cs, clk, din, 0x77); st7701s_data(cs, clk, din, 0x01);
    st7701s_data(cs, clk, din, 0x00); st7701s_data(cs, clk, din, 0x00);
    st7701s_data(cs, clk, din, 0x13);

    st7701s_cmd (cs, clk, din, 0xEF); st7701s_data(cs, clk, din, 0x08);

    st7701s_cmd (cs, clk, din, 0xFF);
    st7701s_data(cs, clk, din, 0x77); st7701s_data(cs, clk, din, 0x01);
    st7701s_data(cs, clk, din, 0x00); st7701s_data(cs, clk, din, 0x00);
    st7701s_data(cs, clk, din, 0x10);

    st7701s_cmd (cs, clk, din, 0xC0); st7701s_data(cs, clk, din, 0x3B); st7701s_data(cs, clk, din, 0x00);
    st7701s_cmd (cs, clk, din, 0xC1); st7701s_data(cs, clk, din, 0x0D); st7701s_data(cs, clk, din, 0x02);
    st7701s_cmd (cs, clk, din, 0xC2); st7701s_data(cs, clk, din, 0x21); st7701s_data(cs, clk, din, 0x08);
    st7701s_cmd (cs, clk, din, 0xCD); st7701s_data(cs, clk, din, 0x08);

    static const uint8_t b0_gamma[] = {
        0x00, 0x11, 0x18, 0x0E, 0x11, 0x06, 0x07, 0x08,
        0x07, 0x22, 0x04, 0x12, 0x0F, 0xAA, 0x31, 0x18,
    };
    st7701s_cmd(cs, clk, din, 0xB0);
    for (uint8_t v : b0_gamma) st7701s_data(cs, clk, din, v);

    static const uint8_t b1_gamma[] = {
        0x00, 0x11, 0x19, 0x0E, 0x12, 0x07, 0x08, 0x08,
        0x08, 0x22, 0x04, 0x11, 0x11, 0xA9, 0x32, 0x18,
    };
    st7701s_cmd(cs, clk, din, 0xB1);
    for (uint8_t v : b1_gamma) st7701s_data(cs, clk, din, v);

    // ---- Command 2 BK1 ----
    st7701s_cmd (cs, clk, din, 0xFF);
    st7701s_data(cs, clk, din, 0x77); st7701s_data(cs, clk, din, 0x01);
    st7701s_data(cs, clk, din, 0x00); st7701s_data(cs, clk, din, 0x00);
    st7701s_data(cs, clk, din, 0x11);

    st7701s_cmd (cs, clk, din, 0xB0); st7701s_data(cs, clk, din, 0x60);
    st7701s_cmd (cs, clk, din, 0xB1); st7701s_data(cs, clk, din, 0x30);
    st7701s_cmd (cs, clk, din, 0xB2); st7701s_data(cs, clk, din, 0x87);
    st7701s_cmd (cs, clk, din, 0xB3); st7701s_data(cs, clk, din, 0x80);
    st7701s_cmd (cs, clk, din, 0xB5); st7701s_data(cs, clk, din, 0x49);
    st7701s_cmd (cs, clk, din, 0xB7); st7701s_data(cs, clk, din, 0x85);
    st7701s_cmd (cs, clk, din, 0xB8); st7701s_data(cs, clk, din, 0x21);
    st7701s_cmd (cs, clk, din, 0xC1); st7701s_data(cs, clk, din, 0x78);
    st7701s_cmd (cs, clk, din, 0xC2); st7701s_data(cs, clk, din, 0x78);
    delay(20);

    static const uint8_t e0_seq[] = { 0x00, 0x1B, 0x02 };
    st7701s_cmd(cs, clk, din, 0xE0);
    for (uint8_t v : e0_seq) st7701s_data(cs, clk, din, v);

    static const uint8_t e1_seq[] = {
        0x08, 0xA0, 0x00, 0x00, 0x07, 0xA0, 0x00, 0x00,
        0x00, 0x44, 0x44,
    };
    st7701s_cmd(cs, clk, din, 0xE1);
    for (uint8_t v : e1_seq) st7701s_data(cs, clk, din, v);

    static const uint8_t e2_seq[] = {
        0x11, 0x11, 0x44, 0x44, 0xED, 0xA0, 0x00, 0x00,
        0xEC, 0xA0, 0x00, 0x00,
    };
    st7701s_cmd(cs, clk, din, 0xE2);
    for (uint8_t v : e2_seq) st7701s_data(cs, clk, din, v);
// 1776835153465.fbox
    static const uint8_t e3_seq[] = { 0x00, 0x00, 0x11, 0x11 };
    st7701s_cmd(cs, clk, din, 0xE3);
    for (uint8_t v : e3_seq) st7701s_data(cs, clk, din, v);

    st7701s_cmd(cs, clk, din, 0xE4); st7701s_data(cs, clk, din, 0x44); st7701s_data(cs, clk, din, 0x44);

    static const uint8_t e5_seq[] = {
        0x0A, 0xE9, 0xD8, 0xA0, 0x0C, 0xEB, 0xD8, 0xA0,
        0x0E, 0xED, 0xD8, 0xA0, 0x10, 0xEF, 0xD8, 0xA0,
    };
    st7701s_cmd(cs, clk, din, 0xE5);
    for (uint8_t v : e5_seq) st7701s_data(cs, clk, din, v);

    static const uint8_t e6_seq[] = { 0x00, 0x00, 0x11, 0x11 };
    st7701s_cmd(cs, clk, din, 0xE6);
    for (uint8_t v : e6_seq) st7701s_data(cs, clk, din, v);

    st7701s_cmd(cs, clk, din, 0xE7); st7701s_data(cs, clk, din, 0x44); st7701s_data(cs, clk, din, 0x44);

    static const uint8_t e8_seq[] = {
        0x09, 0xE8, 0xD8, 0xA0, 0x0B, 0xEA, 0xD8, 0xA0,
        0x0D, 0xEC, 0xD8, 0xA0, 0x0F, 0xEE, 0xD8, 0xA0,
    };
    st7701s_cmd(cs, clk, din, 0xE8);
    for (uint8_t v : e8_seq) st7701s_data(cs, clk, din, v);

    static const uint8_t eb_seq[] = { 0x02, 0x00, 0xE4, 0xE4, 0x88, 0x00, 0x40 };
    st7701s_cmd(cs, clk, din, 0xEB);
    for (uint8_t v : eb_seq) st7701s_data(cs, clk, din, v);

    st7701s_cmd(cs, clk, din, 0xEC); st7701s_data(cs, clk, din, 0x3C); st7701s_data(cs, clk, din, 0x00);

    static const uint8_t ed_seq[] = {
        0xAB, 0x89, 0x76, 0x54, 0x02, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0x20, 0x45, 0x67, 0x98, 0xBA,
    };
    st7701s_cmd(cs, clk, din, 0xED);
    for (uint8_t v : ed_seq) st7701s_data(cs, clk, din, v);

    static const uint8_t ef_seq[] = { 0x10, 0x0D, 0x04, 0x08, 0x3F, 0x1F };
    st7701s_cmd(cs, clk, din, 0xEF);
    for (uint8_t v : ef_seq) st7701s_data(cs, clk, din, v);

    // ---- Command 2 BK0 again, set RGB666 pixel format ----
    st7701s_cmd (cs, clk, din, 0xFF);
    st7701s_data(cs, clk, din, 0x77); st7701s_data(cs, clk, din, 0x01);
    st7701s_data(cs, clk, din, 0x00); st7701s_data(cs, clk, din, 0x00);
    st7701s_data(cs, clk, din, 0x00);

    st7701s_cmd (cs, clk, din, 0x3A); st7701s_data(cs, clk, din, 0x66);  // 18-bit RGB666

    // ---- Command 2 BK3, vendor magic ----
    st7701s_cmd (cs, clk, din, 0xFF);
    st7701s_data(cs, clk, din, 0x77); st7701s_data(cs, clk, din, 0x01);
    st7701s_data(cs, clk, din, 0x00); st7701s_data(cs, clk, din, 0x00);
    st7701s_data(cs, clk, din, 0x13);

    st7701s_cmd (cs, clk, din, 0xE8); st7701s_data(cs, clk, din, 0x00); st7701s_data(cs, clk, din, 0x0E);

    // ---- Sleep Out ----
    st7701s_cmd (cs, clk, din, 0xFF);
    st7701s_data(cs, clk, din, 0x77); st7701s_data(cs, clk, din, 0x01);
    st7701s_data(cs, clk, din, 0x00); st7701s_data(cs, clk, din, 0x00);
    st7701s_data(cs, clk, din, 0x00);

    st7701s_cmd (cs, clk, din, 0x11);
    delay(120);

    // ---- Vendor magic 2 ----
    st7701s_cmd (cs, clk, din, 0xFF);
    st7701s_data(cs, clk, din, 0x77); st7701s_data(cs, clk, din, 0x01);
    st7701s_data(cs, clk, din, 0x00); st7701s_data(cs, clk, din, 0x00);
    st7701s_data(cs, clk, din, 0x13);

    st7701s_cmd (cs, clk, din, 0xE8); st7701s_data(cs, clk, din, 0x00); st7701s_data(cs, clk, din, 0x0C);
    delay(10);
    st7701s_cmd (cs, clk, din, 0xE8); st7701s_data(cs, clk, din, 0x00); st7701s_data(cs, clk, din, 0x00);

    st7701s_cmd (cs, clk, din, 0xFF);
    st7701s_data(cs, clk, din, 0x77); st7701s_data(cs, clk, din, 0x01);
    st7701s_data(cs, clk, din, 0x00); st7701s_data(cs, clk, din, 0x00);
    st7701s_data(cs, clk, din, 0x00);

    st7701s_cmd (cs, clk, din, 0x36); st7701s_data(cs, clk, din, 0x00);
    st7701s_cmd (cs, clk, din, 0x3A); st7701s_data(cs, clk, din, 0x66);

    st7701s_cmd (cs, clk, din, 0x21);   // INVON
    st7701s_cmd (cs, clk, din, 0x29);   // DISPON
    delay(20);
}

//============================================================================
// Initialisation
//============================================================================

bool Panel_PCBA5981::init(bool use_reset)
{
    // Inlined Panel_Device::init() so we can bit-bang the ST7701S panel
    // config bus while pins 7/6 are still pristine GPIO. On the PCBA5981
    // those pins double as the LT7680 SPI bus (LCM_SCL/LCM_SDI bridged to
    // LCD_CLK/LCD_DIN at the panel), so once _bus->init() claims them for
    // the SPI peripheral the bit-bang can no longer drive the trace.
    init_rst();
    init_cs();
    if (use_reset)
    {
        rst_control(false);
        delay(8);
        rst_control(true);
        delay(64);
    }

    if (st7701s_pins.pin_cs >= 0 && st7701s_pins.pin_clk >= 0 && st7701s_pins.pin_din >= 0)
    {
        _st7701s_init_sequence();
    }

    _bus->init();

    startWrite(true);

    // ---- Wait for normal-operation mode (STSR bit1 == 0) ----
    {
        uint8_t s0 = _read_status();
        Serial.printf("[PANEL] Initial STSR=0x%02X\n", s0);
        auto t = millis();
        while (_read_status() & 0x02)
        {
            if (millis() - t > 500)
            {
                Serial.printf("[PANEL] Normal-op wait TIMEOUT STSR=0x%02X\n", _read_status());
                endWrite(); return false;
            }
        }
        Serial.printf("[PANEL] Normal-op OK STSR=0x%02X\n", _read_status());
    }

    // ---- PLL ----
    auto pll_reg1 = [](const pll_t& p) -> uint8_t {
        return (uint8_t)((p.OD << 6) | ((p.R & 0x1F) << 1) | ((p.N >> 8) & 0x01));
    };
    _write_reg(0x05, pll_reg1(pll_pixel));
    _write_reg(0x07, pll_reg1(pll_mem));
    _write_reg(0x09, pll_reg1(pll_core));
    _write_reg(0x06, (uint8_t)(pll_pixel.N & 0xFF));
    _write_reg(0x08, (uint8_t)(pll_mem.N   & 0xFF));
    _write_reg(0x0A, (uint8_t)(pll_core.N  & 0xFF));

    _write_reg(0x00, 0x80);   // SRR: lock PLLs
    delay(1);

    // ---- SDRAM ----
    _write_reg(0xE0, 0x29);
    _write_reg(0xE1, 0x03);
    _write_reg16(0xE2, SDRAM_REFRESH_INTERVAL);
    _write_reg(0xE4, 0x01);
    _wait_sdram_ready();
    delay(1);

    // CCR REG[01h]: bit7 = system-OK sanity bit (matches the
    // System_Check_Temp() handshake in the BuyDisplay reference; the
    // chip expects this bit set for normal operation), bit3 = 18-bit
    // TFT output, bit0 = 16-bit host bus.
    _write_reg(0x01, 0x89);
    _write_reg(0x02, 0x40);   // MACR: direct write, LR->TB read & write
                              // (bit[6]=1 is don't-care in the 0xb direct-write encoding)
    _write_reg(0x03, 0x00);   // ICR: graphic mode, write to SDRAM

    {
        uint8_t dpcr = 0;
        if (timing.pclk_rising) dpcr |= 0x80;
        _write_reg(0x12, dpcr);   // DPCR (display still off)
    }

    {
        uint8_t pcsr = 0;
        if (timing.hsync_active_high) pcsr |= 0x80;
        if (timing.vsync_active_high) pcsr |= 0x40;
        if (!timing.de_active_high)   pcsr |= 0x20;
        _write_reg(0x13, pcsr);
    }

    // ---- Horizontal timing ----
    // REG[14h]/[15h]: HDWR/HDWFTR  - active width
    // REG[16h]/[17h]: HNDR/HNDFTR  - back porch only (matches reference
    //                                 driver; HFP and HSYNC are encoded
    //                                 separately in REG[18h]/[19h])
    // REG[18h]:        HSTR        - HSYNC start = front porch
    // REG[19h]:        HPWR        - HSYNC pulse width
    {
        _write_reg(0x14, (uint8_t)((timing.h_display    / 8) - 1));
        _write_reg(0x15, (uint8_t)( timing.h_display    % 8));
        _write_reg(0x16, (uint8_t)((timing.h_back_porch / 8) - 1));
        _write_reg(0x17, (uint8_t)( timing.h_back_porch % 8));
        _write_reg(0x18, (timing.h_front_porch >= 8)
                         ? (uint8_t)((timing.h_front_porch / 8) - 1)
                         : 0);
        _write_reg(0x19, (timing.h_sync_width >= 8)
                         ? (uint8_t)((timing.h_sync_width / 8) - 1)
                         : (uint8_t)(timing.h_sync_width - 1));
    }

    // ---- Vertical timing ----
    {
        uint16_t vdhr = timing.v_display - 1;
        _write_reg(0x1A, (uint8_t)(vdhr      ));
        _write_reg(0x1B, (uint8_t)(vdhr >>  8));
        uint16_t vndr = timing.v_back_porch - 1;
        _write_reg(0x1C, (uint8_t)(vndr      ));
        _write_reg(0x1D, (uint8_t)(vndr >>  8));
        _write_reg(0x1E, (uint8_t)(timing.v_front_porch - 1));
        _write_reg(0x1F, (uint8_t)(timing.v_sync_width  - 1));
    }

    _write_reg(0x10, 0x00);   // MPWCTR: main window 8bpp (bits[3:2]=00b)

    // ---- Main image / canvas ----
    _write_reg32(0x20, CANVAS_BASE_ADDR);
    _write_reg16(0x24, (uint16_t)timing.h_display);
    _write_reg16(0x26, 0);
    _write_reg16(0x28, 0);

    _write_reg32(0x50, CANVAS_BASE_ADDR);
    _write_reg16(0x54, (uint16_t)timing.h_display);

    _write_reg(0x5E, 0x00);   // AW_COLOR: XY mode, 8bpp

    _init_clut_rgb332();

    _win_xs = 0;  _win_ys = 0;
    _win_xe = (uint16_t)(timing.h_display - 1);
    _win_ye = (uint16_t)(timing.v_display - 1);
    _set_active_window(0, 0, timing.h_display, timing.v_display);

    // ---- LT7680 Display ON ----
    // (ST7701S was already initialised before _bus->init() at the top of
    // this function — see the inlined Panel_Device::init() above.)
    {
        uint8_t dpcr = 0x40;                  // bit6 = display ON
        if (timing.pclk_rising) dpcr |= 0x80;
        _write_reg(0x12, dpcr);
    }

    // ---- Backlight PWM ----
    // With the board strapped for internal PWM (J1 open / J2 short) the
    // backlight stays dark until these timers run, so start them at full
    // duty here. Harmless when strapped for external control.
    _init_backlight_pwm();

    endWrite();

    _latestcolor = ~0u;
    return true;
}

//============================================================================
// Colour depth / rotation
//============================================================================

color_depth_t Panel_PCBA5981::setColorDepth(color_depth_t depth)
{
    _write_depth = rgb565_nonswapped;
    _read_depth  = rgb565_nonswapped;
    return rgb565_nonswapped;
}

void Panel_PCBA5981::setRotation(uint_fast8_t r)
{
    r &= 7;
    _rotation = r;
    _internal_rotation = ((r + _cfg.offset_rotation) & 3)
                       | ((r & 4) ^ (_cfg.offset_rotation & 4));

    _width  = _cfg.panel_width;
    _height = _cfg.panel_height;
    if (_internal_rotation & 1) std::swap(_width, _height);

    _xs = _xe = _ys = _ye = INT16_MAX;
    _flg_memorywrite = false;

    // REG[02h] bits[2:1] write direction:
    //   00 = L->R, T->B  (0deg)
    //   11 = R->L, B->T  (180deg)
    // 90/270 require axis swap which the LT7680 write-direction field
    // does not support natively; image will be transposed.
    uint8_t wdir = 0;
    switch (_internal_rotation & 3)
    {
    case 0: wdir = 0x00; break;
    case 1: wdir = 0x00; break;
    case 2: wdir = 0x06; break;
    case 3: wdir = 0x06; break;
    }

    _reg02 = (uint8_t)(0x40 | wdir);
    _write_reg(0x02, _reg02);

    _latestcolor = ~0u;
}

//============================================================================
// Command / data pass-through
//============================================================================

void Panel_PCBA5981::writeCommand(uint32_t data, uint_fast8_t bit_length)
{
    _flg_memorywrite = false;
    _bus->wait();
    cs_control(true);
    cs_control(false);
    _bus->writeCommand(data, bit_length);
}

void Panel_PCBA5981::writeData(uint32_t data, uint_fast8_t bit_length)
{
    _bus->wait();
    cs_control(true);
    cs_control(false);
    _bus->writeData(data, bit_length);
}

//============================================================================
// Busy / wait
//============================================================================

void Panel_PCBA5981::waitDisplay(void) { _wait_busy(); }
bool Panel_PCBA5981::displayBusy(void) { return (_read_status() & 0x08) != 0; }

//============================================================================
// Window management
//============================================================================

void Panel_PCBA5981::setWindow(uint_fast16_t xs, uint_fast16_t ys,
                                uint_fast16_t xe, uint_fast16_t ye)
{
    _win_xs = (uint16_t)xs;
    _win_ys = (uint16_t)ys;
    _win_xe = (uint16_t)xe;
    _win_ye = (uint16_t)ye;
    _xs = xs;  _xe = xe;
    _ys = ys;  _ye = ye;
    _flg_memorywrite = false;
}

//============================================================================
// Pixel and rectangle drawing
//============================================================================

void Panel_PCBA5981::drawPixelPreclipped(uint_fast16_t x, uint_fast16_t y, uint32_t rawcolor)
{
    bool tr = _in_transaction;
    if (!tr) begin_transaction();
    setWindow(x, y, x, y);
    _start_memorywrite();
    _write_pixel16((uint16_t)rawcolor);
    if (!tr) end_transaction();
}

void Panel_PCBA5981::writeFillRectPreclipped(uint_fast16_t x, uint_fast16_t y,
                                              uint_fast16_t w, uint_fast16_t h,
                                              uint32_t rawcolor)
{
    if (w == 0 || h == 0) return;

    if (h == 1 && w <= 4)
    {
        setWindow(x, y, x + w - 1, y);
        _start_memorywrite();
        for (uint_fast16_t i = 0; i < w; i++) _write_pixel16((uint16_t)rawcolor);
        return;
    }

    // BTE Solid Fill (operation 0x0C in REG[91h] low nibble).
    // Destination address tracks the active canvas (CVSSA mirror) so that
    // fills land in whatever slot setCanvasAddress() most recently selected.
    _set_forecolor(rawcolor);
    _write_reg(0x91, 0xCC);
    _write_reg(0x92, 0x00);   // dest 8bpp

    _write_reg32(0xA7, _canvas_addr);
    _write_reg16(0xAB, (uint16_t)timing.h_display);
    _write_reg16(0xAD, (uint16_t)x);
    _write_reg16(0xAF, (uint16_t)y);
    _write_reg16(0xB1, (uint16_t)w);
    _write_reg16(0xB3, (uint16_t)h);

    _write_reg(0x90, 0x10);   // BTE enable
    _wait_busy();

    _latestcolor = rawcolor;
    _flg_memorywrite = false;
}

void Panel_PCBA5981::writeBlock(uint32_t rawcolor, uint32_t len)
{
    _start_memorywrite();
    uint16_t c = (uint16_t)rawcolor;
    while (len--) _write_pixel16(c);
}

void Panel_PCBA5981::writeImage(uint_fast16_t x, uint_fast16_t y,
                                 uint_fast16_t w, uint_fast16_t h,
                                 pixelcopy_t* param, bool use_dma)
{
    auto src_x = param->src_x;

    if (param->transp == pixelcopy_t::NON_TRANSP)
    {
        setWindow(x, y, x + w - 1, y + h - 1);
        _start_memorywrite();

        do
        {
            for (uint_fast16_t i = 0; i < w; i++)
            {
                uint32_t pix = 0;
                param->fp_copy(&pix, i, i + 1, param);
                _write_pixel16((uint16_t)pix);
            }
            param->src_x = src_x;
            param->src_y++;
        } while (--h);
    }
    else
    {
        uint32_t hend = y + h;
        do
        {
            uint32_t i = 0;
            while (w != (i = param->fp_skip(i, w, param)))
            {
                uint32_t len = param->fp_copy(nullptr, 0, w - i, param);
                setWindow(x + i, y, x + i + len - 1, y);
                _start_memorywrite();
                for (uint32_t j = 0; j < len; j++)
                {
                    uint32_t pix = 0;
                    param->fp_copy(&pix, 0, 1, param);
                    _write_pixel16((uint16_t)pix);
                }
                if (w == (i += len)) break;
            }
            param->src_x = src_x;
            param->src_y++;
        } while (++y != hend);
    }
}

void Panel_PCBA5981::writePixels(pixelcopy_t* param, uint32_t len, bool use_dma)
{
    _start_memorywrite();

    uint32_t xpos = _win_xs;
    uint32_t ypos = _win_ys;

    do
    {
        uint32_t w = std::min<uint32_t>(len, _win_xe + 1 - xpos);
        xpos += w;
        if (xpos > _win_xe) { xpos = _win_xs; ++ypos; }

        for (uint32_t i = 0; i < w; i++)
        {
            uint32_t pix = 0;
            param->fp_copy(&pix, 0, 1, param);
            _write_pixel16((uint16_t)pix);
        }
        len -= w;
    } while (len);
}

void Panel_PCBA5981::writeRawPixels(uint16_t x, uint16_t y,
                                     uint16_t w, const uint16_t* data)
{
    if (w == 0) return;
    bool tr = _in_transaction;
    if (!tr) begin_transaction();
    setWindow(x, y, x + w - 1, y);
    _start_memorywrite();
    for (uint16_t i = 0; i < w; i++) _write_pixel16(data[i]);
    if (!tr) end_transaction();
}

void Panel_PCBA5981::writeRawFrame(const uint16_t* data, uint16_t w, uint16_t h)
{
    bool tr = _in_transaction;
    if (!tr) begin_transaction();
    setWindow(0, 0, w - 1, h - 1);
    _start_memorywrite();
    uint32_t count = (uint32_t)w * h;
    for (uint32_t i = 0; i < count; i++) _write_pixel16(data[i]);
    if (!tr) end_transaction();
}

// DIAGNOSTIC: row-at-a-time write. Each row is a fresh 480x1 active window
// + a small per-row [0x80]+480-byte burst, instead of one 230,400-byte burst.
// Slower than the bulk path but uses the same code shape as writeRawPixels.
void Panel_PCBA5981::writeRawFrame8bppRowByRow(const uint8_t* clut8)
{
    bool tr = _in_transaction;
    if (!tr) begin_transaction();

    _write_reg(0x02, 0x40);   // MSD natural

    static const uint8_t kPrefix = 0x80;
    const uint16_t W = timing.h_display;
    const uint16_t H = timing.v_display;
    for (uint16_t y = 0; y < H; y++) {
        setWindow(0, y, W - 1, y);
        _start_memorywrite();   // sets active window to (0,y)-(W-1,y), selects MRWDP

        cs_control(false);
        _bus->writeBytes(&kPrefix, 1, true, false);
        _bus->wait();
        _bus->writeBytes(clut8 + (uint32_t)y * W, W, true, true);
        _bus->wait();
        cs_control(true);
    }

    _write_reg(0x02, _reg02);
    _flg_memorywrite = false;
    if (!tr) end_transaction();
}

void Panel_PCBA5981::writeRawFrame8bpp(const uint8_t* clut8, uint32_t count)
{
    if (count == 0) return;
    bool tr = _in_transaction;
    if (!tr) begin_transaction();

    // Animation frames are pixel-order natural (L→R, T→B). Temporarily override
    // REG[02h] MSD to natural order so the write cursor starts at top-left;
    // restore the rotation setting after the burst.
    _write_reg(0x02, 0x40);

    // ---- Datasheet §5 Figure 5-3 Part 1 compliance ----
    // Step 1 (CVSSA, REG[50h-53h]) is set by the caller via setCanvasAddress()
    // in displayAnimFrameBegin(); we don't repeat it here.
    // Step 2: Canvas Image Width. Reasserted defensively in case a prior
    //         DMA / BTE path changed it.
    _write_reg16(0x54, (uint16_t)timing.h_display);
    // Step 3b: Active Window color depth (XY mode, 8bpp). The AW XY/W/H part
    //          (step 3a) is written by _set_active_window inside
    //          _start_memorywrite() below — datasheet step order is preserved
    //          because REG[5Eh] doesn't overlap REG[56h-5Dh].
    _write_reg(0x5E, 0x00);
    // Step 4: Memory port destination = Image buffer (graphic mode, target SDRAM).
    _write_reg(0x03, 0x00);
    // Step 5: Wait for chip core idle (any prior BTE/GDE kick still draining)
    //         before we start streaming. Bus->wait() only drains the host SPI
    //         FIFO; it does not see the chip's internal state machine.
    {
        auto tb = millis();
        while (_read_status() & 0x08) {
            if (millis() - tb > 50) break;
        }
    }

    setWindow(0, 0, timing.h_display - 1, timing.v_display - 1);
    _start_memorywrite();  // Step 3a: AW XY/W/H; Step 6: select REG[04h]; CS ends HIGH

    // LT7680 burst write protocol:
    //   0x80 prefix (A0=1, RW#=0) enters streaming mode; all subsequent bytes
    //   with CS held go directly to the chip's Memory Write FIFO, which drains
    //   asynchronously into SDRAM with auto-incrementing address (REG[04h]).
    //   kPrefix is a 1-byte polling write; clut8 (ps_malloc 4-byte aligned) is DMA.
    // The whole frame is written in a single CS-held burst — re-asserting CS
    // between chunks (multiple [0x80] prefixes) was observed to gradually
    // corrupt SDRAM over many frames.
    static const uint8_t kPrefix = 0x80;
    cs_control(false);
    _bus->writeBytes(&kPrefix, 1, true, false);
    _bus->wait();
    _bus->writeBytes(clut8, count, true, true);
    _bus->wait();
    cs_control(true);

    // After CS deasserts, the chip still has up to a FIFO's worth of pending
    // bytes destined for the LAST SDRAM rows. STSR bit 6 = "Memory Write FIFO
    // Empty" — wait for it to go high before we let any subsequent op (e.g.
    // MISA flip) proceed.
    auto t0 = millis();
    while ((_read_status() & 0x40) == 0) {
        if (millis() - t0 > 50) break;   // safety: ~3 frame periods
    }

    // The historical "32-px right shift" glitch chased here was a symptom of
    // running the ESP32 host SPI bus at 80 MHz, which exceeds the LT7680's
    // safe receive window on this PCB and produces undefined SDRAM-write
    // behavior. The fix is to clock the bus at ≤40 MHz — the ceiling
    // documented in this driver. Recovery code (canvas read-back,
    // post-failure register snapshot, software reset) was removed; it was
    // papering over a clock-rate violation. See cfg.freq_write below /
    // [docs/HARDWARE.md](docs/HARDWARE.md) for the limit.

    _write_reg(0x02, _reg02);  // restore rotation
    _flg_memorywrite = false;
    if (!tr) end_transaction();
}

//============================================================================
// SPI Master helpers (§10.2, REG[B8h–BBh])
//============================================================================

// SFCLK = CPLL / (2 * (div + 1)).  div=1 → 25 MHz @ 100 MHz CPLL.
// W25Q128 supports up to 50 MHz (Normal Read); 25 MHz gives headroom on PCB traces.
static constexpr uint8_t SPI_MASTER_CLK_DIV   = 1;
// SPIMCR2 (B9h): {1'b0, mask, SS#_sel, ss_active, ovfirqen, emtirqen, cpol, cpha}
// Mode 3 (CPOL=1, CPHA=1), SFCS1# (bit5=1) — the onboard 128M flash is wired to
// SFCS1#, confirmed by reference example always using SCS=1 in DMA_24bit_Block and
// LCD_Select_Outside_Font_Init. Using SFCS0# (0x1F/0x0F) leaves the clock idle.
static constexpr uint8_t SPIMCR2_CS_ASSERT    = 0x3F;  // SS#_sel=1(SFCS1#), ss_active=1, Mode3
static constexpr uint8_t SPIMCR2_CS_DEASSERT  = 0x2F;  // SS#_sel=1(SFCS1#), ss_active=0, Mode3

void Panel_PCBA5981::_select_reg(uint8_t reg)
{
    _flg_memorywrite = false;
    _bus->wait();
    cs_control(true);
    cs_control(false);
    _bus->writeCommand((uint32_t)reg << 8, 16);  // [0x00][reg]
}

uint8_t Panel_PCBA5981::_read_reg_byte(uint8_t reg)
{
    _select_reg(reg);
    return _read_byte();
}

void Panel_PCBA5981::_spi_cs_assert(void)
{
    // Deassert first — resets both TX and RX FIFOs (§10.2 FIFO Overrun).
    _write_reg(0xB9, SPIMCR2_CS_DEASSERT);
    _write_reg(0xB9, SPIMCR2_CS_ASSERT);
}

void Panel_PCBA5981::_spi_cs_deassert(void)
{
    _write_reg(0xB9, SPIMCR2_CS_DEASSERT);
}

void Panel_PCBA5981::_spi_tx(uint8_t data)
{
    _write_reg(0xB8, data);  // SPIDR write → TX FIFO
}

// Wait for the SPI master TX FIFO to drain.
// Polling SPIMSR (BAh) via the host SPI bus while the SPI master is running blocks
// the LT7680's own SPI engine — bit5 never returns, TX never completes.
// Fix: use a fixed delay sized for the worst case (16 bytes at the current SFCLK).
// At div=1 (25 MHz): 16 * 8 / 25e6 = 51 µs. 150 µs gives 3× margin.
uint8_t Panel_PCBA5981::_spi_wait_done(void)
{
    delayMicroseconds(150);
    return _read_reg_byte(0xBA);  // SPIMSR — SPI master is idle by now; safe to read
}

// Send buf[0..len) in ≤16-byte chunks (FIFO depth = 16 bytes).
// RX bytes are ignored (not drained) — caller must deassert CS before
// any subsequent RX to reset the RX FIFO.
void Panel_PCBA5981::_spi_write_buf(const uint8_t* buf, uint16_t len)
{
    while (len > 0) {
        uint16_t chunk = (len > 16) ? 16 : len;
        for (uint16_t i = 0; i < chunk; i++) _spi_tx(*buf++);
        _spi_wait_done();
        len -= chunk;
    }
}

void Panel_PCBA5981::_flash_write_enable(void)
{
    _spi_cs_assert();
    _spi_tx(0x06);  // WREN
    _spi_wait_done();
    _spi_cs_deassert();
    Serial.println("[FLASH] WREN sent");
}

bool Panel_PCBA5981::_flash_wait_ready(uint32_t timeout_ms)
{
    auto t = millis();
    while (true) {
        // RDSR: send cmd + 1 dummy to clock in status byte.
        _spi_cs_assert();
        _spi_tx(0x05);   // RDSR1
        _spi_tx(0x00);   // dummy — clocks in status byte
        _spi_wait_done();
        // Read 2 bytes from RX FIFO: discard byte 0 (received during 0x05 cmd),
        // keep byte 1 (status received during dummy).
        _select_reg(0xB8);
        (void)_read_byte();
        uint8_t sr = _read_byte();
        _spi_cs_deassert();

        Serial.printf("[FLASH] SR1=0x%02X WIP=%d\n", sr, sr & 1);
        if (!(sr & 0x01)) return true;

        if (millis() - t > timeout_ms) {
            Serial.printf("[FLASH] _flash_wait_ready TIMEOUT SR1=0x%02X\n", sr);
            return false;
        }
        delay(1);
    }
}

//============================================================================
// Public Serial Flash API
//============================================================================

uint32_t Panel_PCBA5981::flashReadJEDECID(void)
{
    bool tr = _in_transaction;
    if (!tr) begin_transaction();

    Serial.println("[FLASH] --- flashReadJEDECID ---");
    _write_reg(0xBB, SPI_MASTER_CLK_DIV);
    Serial.printf("[FLASH] CLK div=%u  SPIMSR before=%02X\n",
                  SPI_MASTER_CLK_DIV, _read_reg_byte(0xBA));

    // 0x9F + 3 dummy bytes → 4 RX bytes (1 garbage + 3 ID bytes).
    _spi_cs_assert();
    Serial.printf("[FLASH] CS asserted  SPIMSR=%02X\n", _read_reg_byte(0xBA));

    _spi_tx(0x9F);
    _spi_tx(0x00); _spi_tx(0x00); _spi_tx(0x00);
    Serial.printf("[FLASH] TX queued    SPIMSR=%02X\n", _read_reg_byte(0xBA));

    uint8_t spimsr = _spi_wait_done();
    Serial.printf("[FLASH] TX done      SPIMSR=%02X\n", spimsr);

    _select_reg(0xB8);
    uint8_t r0 = _read_byte();   // garbage (received while sending 0x9F cmd)
    uint8_t mfr  = _read_byte();
    uint8_t type = _read_byte();
    uint8_t cap  = _read_byte();
    _spi_cs_deassert();

    Serial.printf("[FLASH] RX raw: %02X %02X %02X %02X\n", r0, mfr, type, cap);

    uint32_t id = ((uint32_t)mfr << 16) | ((uint32_t)type << 8) | cap;
    Serial.printf("[FLASH] JEDEC ID: 0x%06X  mfr=0x%02X type=0x%02X cap=0x%02X\n",
                  id, mfr, type, cap);

    if (!tr) end_transaction();
    return id;
}

void Panel_PCBA5981::flashEraseSector(uint32_t addr)
{
    bool tr = _in_transaction;
    if (!tr) begin_transaction();

    _write_reg(0xBB, SPI_MASTER_CLK_DIV);
    Serial.printf("[FLASH] Sector erase 0x%06X ...\n", addr);

    _flash_write_enable();

    _spi_cs_assert();
    _spi_tx(0x20);                        // Sector Erase (4 KB)
    _spi_tx((addr >> 16) & 0xFF);
    _spi_tx((addr >>  8) & 0xFF);
    _spi_tx( addr        & 0xFF);
    _spi_wait_done();
    _spi_cs_deassert();

    bool ok = _flash_wait_ready(3000);
    Serial.printf("[FLASH] Sector erase %s\n", ok ? "OK" : "FAILED");

    if (!tr) end_transaction();
}

void Panel_PCBA5981::flashPageProgram(uint32_t addr, const uint8_t* data, uint16_t len)
{
    if (len == 0 || len > 256) {
        Serial.printf("[FLASH] flashPageProgram bad len=%u\n", len);
        return;
    }
    bool tr = _in_transaction;
    if (!tr) begin_transaction();

    _write_reg(0xBB, SPI_MASTER_CLK_DIV);
    Serial.printf("[FLASH] Page program 0x%06X len=%u\n", addr, len);

    _flash_write_enable();

    _spi_cs_assert();
    _spi_tx(0x02);                        // Page Program
    _spi_tx((addr >> 16) & 0xFF);
    _spi_tx((addr >>  8) & 0xFF);
    _spi_tx( addr        & 0xFF);
    _spi_wait_done();
    _spi_write_buf(data, len);
    _spi_cs_deassert();

    bool ok = _flash_wait_ready(10);      // typ 0.4 ms, max 3 ms
    Serial.printf("[FLASH] Page program %s\n", ok ? "OK" : "FAILED");

    if (!tr) end_transaction();
}

void Panel_PCBA5981::flashReadBytes(uint32_t addr, uint8_t* buf, uint16_t len)
{
    if (len == 0) return;
    bool tr = _in_transaction;
    if (!tr) begin_transaction();

    _write_reg(0xBB, SPI_MASTER_CLK_DIV);
    Serial.printf("[FLASH] Read 0x%06X len=%u\n", addr, len);

    // Send Read Data cmd (03h) + 3-byte address (4 bytes total TX, 4 bytes RX garbage).
    _spi_cs_assert();
    _spi_tx(0x03);
    _spi_tx((addr >> 16) & 0xFF);
    _spi_tx((addr >>  8) & 0xFF);
    _spi_tx( addr        & 0xFF);
    _spi_wait_done();
    // Drain 4 RX FIFO bytes received during the command phase.
    _select_reg(0xB8);
    _read_byte(); _read_byte(); _read_byte(); _read_byte();

    // Data phase: send dummy bytes to clock in data, read RX FIFO in 16-byte chunks.
    for (uint16_t i = 0; i < len; ) {
        uint16_t chunk = ((len - i) > 16) ? 16 : (len - i);
        for (uint16_t j = 0; j < chunk; j++) _spi_tx(0x00);
        _spi_wait_done();
        _select_reg(0xB8);
        for (uint16_t j = 0; j < chunk; j++) buf[i++] = _read_byte();
    }
    _spi_cs_deassert();

    if (!tr) end_transaction();
}

//============================================================================
// DMA: Serial Flash → SDRAM canvas (§10.3.3, Fig 10-12 polling mode)
//============================================================================

void Panel_PCBA5981::dmaFlashBlock(uint32_t flash_addr,
                                    uint16_t flash_src_width,
                                    uint32_t canvas_dst_addr,
                                    uint16_t dst_x, uint16_t dst_y,
                                    uint16_t block_w, uint16_t block_h)
{
    bool tr = _in_transaction;
    if (!tr) begin_transaction();

    Serial.printf("[DMA] Flash→SDRAM src=0x%06X srcW=%u dst=(%u,%u) wh=(%u,%u)\n",
                  flash_addr, flash_src_width, dst_x, dst_y, block_w, block_h);

    uint32_t saved_canvas = _canvas_addr;

    // Flash source start address (BCh–BFh = DMA_SSTR, LSB first).
    _write_reg32(0xBC, flash_addr);

    // Destination canvas in SDRAM.
    _write_reg32(0x50, canvas_dst_addr);           // CVSSA
    _write_reg16(0x54, (uint16_t)timing.h_display); // CVS_IMWTH

    // Destination position inside the canvas.
    _write_reg16(0xC0, dst_x);   // DMA_DX
    _write_reg16(0xC2, dst_y);   // DMA_DY

    // Block dimensions.
    _write_reg16(0xC6, block_w); // DMAW_WTH
    _write_reg16(0xC8, block_h); // DMAW_HIGH

    // Source image width in flash (stride, in pixels).
    _write_reg16(0xCA, flash_src_width); // DMA_SWTH

    // Canvas: 8bpp, XY/block addressing mode.
    _write_reg(0x5E, 0x00);

    // SFL_CTRL (B7h): bit7=24-bit addr, bit6=normal read, bit5=SS#_sel(1=SFCS1#).
    // 0xE0 = SFCS1# + 24-bit + normal read — matches reference DMA_24bit_Block(SCS=1,...).
    _write_reg(0xB7, 0xE0);

    auto t = millis();

    // DMA start (REG[B6h] bit0 = 1).
    _write_reg(0xB6, 0x01);

    // Poll STSR bit3: 1=busy, 0=done.
    while (_read_status() & 0x08) {
        if (millis() - t > 2000) {
            Serial.println("[DMA] TIMEOUT");
            break;
        }
    }

    uint32_t elapsed = millis() - t;
    Serial.printf("[DMA] Done in %lums\n", elapsed);

    // Restore drawing canvas so LGFX draw ops continue to target it.
    if (canvas_dst_addr != saved_canvas) {
        _write_reg32(0x50, saved_canvas);
        _write_reg16(0x54, (uint16_t)timing.h_display);
    }
    _canvas_addr = saved_canvas;
    _flg_memorywrite = false;

    if (!tr) end_transaction();
}

//============================================================================
// User-defined character (UCG) glyph engine (§8.2)
//============================================================================

void Panel_PCBA5981::cgramSetStart(uint32_t cgram_addr)
{
    bool tr = _in_transaction;
    if (!tr) begin_transaction();
    _write_reg32(0xDB, cgram_addr);   // REG[DBh-DEh] CGRAM_STR
    if (!tr) end_transaction();
}

// Write one ≤64-byte run into SDRAM as a single 64-wide × 1-tall block-mode
// memory write (the same proven path as writeRawFrame8bpp). Caller frames the
// transaction and restores the canvas. Multi-row block writes at a relocated
// canvas were observed to corrupt the data; single rows round-trip cleanly.
void Panel_PCBA5981::_cgram_write_row(uint32_t dst_addr, const uint8_t* data, uint16_t n)
{
    _flg_memorywrite = false;
    _write_reg(0x03, 0x00);                         // graphic mode, image buffer
    _write_reg(0x5E, 0x00);                         // block, 8bpp
    _write_reg32(0x50, dst_addr);                   // CVSSA = row base
    _write_reg16(0x54, 64);                         // CVS_IMWTH = 64 (matches window)

    _win_xs = 0; _win_ys = 0; _win_xe = 63; _win_ye = 0;
    _start_memorywrite();                           // active window (0,0,64,1) + select REG[04h]

    static const uint8_t kPrefix = 0x80;
    cs_control(false);
    _bus->writeBytes(&kPrefix, 1, true, false);
    _bus->wait();
    _bus->writeBytes(data, n, true, true);
    _bus->wait();
    cs_control(true);

    auto t0 = millis();
    while ((_read_status() & 0x40) == 0) {          // wait Memory Write FIFO empty
        if (millis() - t0 > 50) break;
    }
}

// CGRAM glyph bytes live in SDRAM. Write them 64 bytes (one 16x32 glyph) at a
// time — each a proven single-row block write — so the engine reads contiguous
// stride-64 glyph data at cgram_addr + code*64.
void Panel_PCBA5981::cgramWrite(uint32_t cgram_addr, const uint8_t* data, uint32_t len)
{
    if (!data || len == 0) return;
    bool tr = _in_transaction;
    if (!tr) begin_transaction();

    uint32_t saved_canvas = _canvas_addr;

    for (uint32_t off = 0; off < len; off += 64) {
        uint16_t n = (uint16_t)((len - off) > 64 ? 64 : (len - off));
        _cgram_write_row(cgram_addr + off, data + off, n);
    }

    // Restore the previous drawing canvas + full-width stride.
    _write_reg32(0x50, saved_canvas);
    _write_reg16(0x54, (uint16_t)timing.h_display);
    _canvas_addr = saved_canvas;
    _flg_memorywrite = false;

    if (!tr) end_transaction();
}

// Read CGRAM bytes back (diagnostic) using the proven block-mode read path.
void Panel_PCBA5981::cgramRead(uint32_t cgram_addr, uint8_t* buf, uint32_t len)
{
    if (!buf || len == 0) return;
    bool tr = _in_transaction;
    if (!tr) begin_transaction();

    uint32_t saved_canvas = _canvas_addr;
    const uint16_t W = 64;
    uint16_t H = (uint16_t)((len + W - 1) / W);

    _write_reg32(0x50, cgram_addr);
    _write_reg16(0x54, W);
    _write_reg(0x5E, 0x00);
    _start_memoryread(0, 0, W, H);                  // selects REG[04h] read, discards dummy
    for (uint32_t i = 0; i < len; i++) buf[i] = _read_byte();

    _write_reg32(0x50, saved_canvas);
    _write_reg16(0x54, (uint16_t)timing.h_display);
    _canvas_addr = saved_canvas;
    _flg_memorywrite = false;

    if (!tr) end_transaction();
}

void Panel_PCBA5981::drawChar(uint16_t code, uint16_t x, uint16_t y,
                              uint16_t fg565, uint16_t bg565,
                              uint8_t heightCode, uint8_t enlarge, bool transparentBg,
                              uint8_t charSource)
{
    bool tr = _in_transaction;
    if (!tr) begin_transaction();

    // Foreground (REG[D2h-D4h]) / Background (REG[D5h-D7h]) colors, RGB888.
    auto wr888 = [&](uint8_t base, uint16_t c) {
        _write_reg(base + 0, (uint8_t)((c >> 8) & 0xF8));  // R5 → R8
        _write_reg(base + 1, (uint8_t)((c >> 3) & 0xFC));  // G6 → G8
        _write_reg(base + 2, (uint8_t)((c << 3) & 0xF8));  // B5 → B8
    };
    wr888(0xD2, fg565);
    wr888(0xD5, bg565);

    // CCR1 (REG[CDh]): enlarge ×1..×4 on width+height, optional BG transparency.
    uint8_t en = (uint8_t)((enlarge > 0 ? enlarge - 1 : 0) & 0x03);
    uint8_t ccr1 = (uint8_t)((en << 2) | en);
    if (transparentBg) ccr1 |= (1 << 6);
    _write_reg(0xCD, ccr1);

    // CCR0 (REG[CCh]): char source bits[7:6] (0=internal CGROM, 1=external
    // CGROM, 2=user-defined CGRAM), height bits[5:4].
    _write_reg(0xCC, (uint8_t)(((charSource & 0x03) << 6) | ((heightCode & 0x03) << 4)));

    // Canvas in block (XY) 8bpp mode and a full-screen active window — the
    // character engine renders into the active window and clips outside it.
    _write_reg(0x5E, 0x00);
    _set_active_window(0, 0, timing.h_display, timing.v_display);

    // Text write position.
    _write_reg16(0x63, x);   // F_CURX
    _write_reg16(0x65, y);   // F_CURY

    // Enter text mode (REG[03h] bit2=1), dest = image buffer.
    _flg_memorywrite = false;
    _write_reg(0x03, 0x04);

    // Character code is written to MRWDP (REG[04h]) as two ordinary data writes,
    // high byte first — NOT a [0x80] auto-increment stream (that path is for
    // bulk image data and would scatter the two bytes as pixels).
    _write_reg(0x04, (uint8_t)(code >> 8));
    _write_reg(0x04, (uint8_t)(code & 0xFF));

    auto t0 = millis();
    while (_read_status() & 0x08) {       // wait core idle
        if (millis() - t0 > 50) break;
    }

    _write_reg(0x03, 0x00);               // back to graphic mode
    _flg_memorywrite = false;

    if (!tr) end_transaction();
}

//============================================================================
// BTE block copy
//============================================================================

void Panel_PCBA5981::copyRect(uint_fast16_t dst_x, uint_fast16_t dst_y,
                               uint_fast16_t w, uint_fast16_t h,
                               uint_fast16_t src_x, uint_fast16_t src_y)
{
    if (w == 0 || h == 0) return;

    bool positive = (dst_y < src_y) || (dst_y == src_y && dst_x <= src_x);

    _write_reg(0x91, (uint8_t)(0xC0 | (positive ? 0x02 : 0x03)));
    _write_reg(0x92, 0x00);   // S0 = 8bpp, dest = 8bpp

    // Both source and destination resolve to the active canvas; copyRect()
    // is an in-canvas blit. Cross-slot copies go through blitFrames().
    _write_reg32(0x93, _canvas_addr);
    _write_reg16(0x97, (uint16_t)timing.h_display);
    _write_reg16(0x99, (uint16_t)src_x);
    _write_reg16(0x9B, (uint16_t)src_y);

    _write_reg32(0xA7, _canvas_addr);
    _write_reg16(0xAB, (uint16_t)timing.h_display);
    _write_reg16(0xAD, (uint16_t)dst_x);
    _write_reg16(0xAF, (uint16_t)dst_y);

    _write_reg16(0xB1, (uint16_t)w);
    _write_reg16(0xB3, (uint16_t)h);

    _write_reg(0x90, 0x10);
    _wait_busy();

    _flg_memorywrite = false;
}

//============================================================================
// Read back from SDRAM via [0xC0]+read
//============================================================================
// Bytes are streamed back in the same order they were written:
// for 16bpp, low byte then high byte per pixel. Total bytes per pixel = 2.
//
// Speed note: each _read_byte() is a 16-bit CS-toggled SPI transaction with
// per-byte overhead. At 20 MHz freq_read on 480x480 RGB565 (460,800 bytes)
// expect roughly half a second. Acceptable for one-shot save/load; not for
// per-frame readback.
void Panel_PCBA5981::readRect(uint_fast16_t x, uint_fast16_t y,
                               uint_fast16_t w, uint_fast16_t h,
                               void* dst, pixelcopy_t* param)
{
    if (w == 0 || h == 0) return;

    bool tr = _in_transaction;
    if (!tr) begin_transaction();

    _start_memoryread((uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h);

    if (param->dst_bits == 16)
    {
        // Fast path: dst is RGB565. Stream straight in, no conversion.
        uint16_t* d = (uint16_t*)dst;
        uint32_t total = (uint32_t)w * h;
        for (uint32_t i = 0; i < total; i++)
        {
            uint8_t lo = _read_byte();
            uint8_t hi = _read_byte();
            d[i] = (uint16_t)(((uint16_t)hi << 8) | lo);
        }
    }
    else
    {
        // Generic path: read into a 16bpp line buffer, route through pixelcopy
        // for whatever destination format the caller wanted.
        static uint16_t lineBuf[480];
        for (uint32_t row = 0; row < h; row++)
        {
            for (uint32_t col = 0; col < w; col++)
            {
                uint8_t lo = _read_byte();
                uint8_t hi = _read_byte();
                lineBuf[col] = (uint16_t)(((uint16_t)hi << 8) | lo);
            }
            param->src_data = lineBuf;
            param->fp_copy(dst, row * w, row * w + w, param);
        }
    }

    _flg_memorywrite = false;  // next write reconfigures the port

    if (!tr) end_transaction();
}

//----------------------------------------------------------------------------
 }
}
