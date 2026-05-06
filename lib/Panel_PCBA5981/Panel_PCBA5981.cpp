/*----------------------------------------------------------------------------/
  Lovyan GFX panel driver for the FriendBox PCBA5981 hardware revision.
  See Panel_PCBA5981.hpp for protocol notes.
/----------------------------------------------------------------------------*/

#include "Panel_PCBA5981.hpp"

#include <Arduino.h>

#include <lgfx/v1/Bus.hpp>
#include <lgfx/v1/platforms/common.hpp>
#include <lgfx/v1/misc/pixelcopy.hpp>

namespace lgfx
{
 inline namespace v1
 {
//----------------------------------------------------------------------------

// SDRAM refresh interval for 60 MHz MCLK, 8192 rows, 64 ms period.
//   sdram_itv = (64000000 / 8192) / (1000 / 60) - 2  = 486
static constexpr uint16_t SDRAM_REFRESH_INTERVAL = 486;

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
    while ((_read_status() & 0x04) == 0x00) {}
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

void Panel_PCBA5981::_write_pixel16(uint16_t color)
{
    _cmd16((uint16_t)(((color & 0xFF) << 8) | 0x80));   // low byte
    _cmd16((uint16_t)(((color >>   8) << 8) | 0x80));   // high byte
}

void Panel_PCBA5981::_set_forecolor(uint32_t rawcolor)
{
    uint16_t c = (uint16_t)rawcolor;
    _write_reg(0xD2, (uint8_t)(c >> 8));
    _write_reg(0xD3, (uint8_t)(c >> 3));
    _write_reg(0xD4, (uint8_t)(c << 3));
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

void Panel_PCBA5981::setMainImageAddress(uint32_t addr)
{
    bool tr = _in_transaction;
    if (!tr) begin_transaction();
    _write_reg32(0x20, addr);   // MISA: Main Image Start Address
    if (!tr) end_transaction();
}

void Panel_PCBA5981::setCanvasAddress(uint32_t addr)
{
    bool tr = _in_transaction;
    if (!tr) begin_transaction();
    _write_reg32(0x50, addr);   // CVSSA: Canvas Start Address
    _flg_memorywrite = false;   // force active-window reload on next access
    if (!tr) end_transaction();
}

// Filled rectangle via the Geometric Drawing Engine (datasheet section 6.3).
// Programs the rectangle endpoints into REG[68h-6Fh], the foreground colour
// into REG[D2h-D4h], then sets REG[76h] = bit7|bit6|bits[5:4]=10b which is
// "start | fill | rectangle". Polls STSR bit3 for completion.
void Panel_PCBA5981::drawFilledRectGeo(uint16_t x1, uint16_t y1,
                                        uint16_t x2, uint16_t y2,
                                        uint16_t rgb565)
{
    bool tr = _in_transaction;
    if (!tr) begin_transaction();

    // Foreground colour - REG[D2h] R[7:3] in bits[7:3], REG[D3h] G[7:2] in
    // bits[7:2], REG[D4h] B[7:3] in bits[7:3]. _set_forecolor handles the
    // bit-packing for an RGB565 value.
    _set_forecolor(rgb565);

    // Start (top-left) and end (bottom-right) points.
    _write_reg16(0x68, x1);   // DLHSR : start X
    _write_reg16(0x6A, y1);   // DLVSR : start Y
    _write_reg16(0x6C, x2);   // DLHER : end   X
    _write_reg16(0x6E, y2);   // DLVER : end   Y

    // DCR1 = 0xD0:
    //   bit7=1   start
    //   bit6=1   fill
    //   bits[5:4]=10b   draw rectangle
    _write_reg(0x76, 0xD0);

    _wait_busy();
    _flg_memorywrite = false;

    if (!tr) end_transaction();
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
    // S0 = 16bpp, dest = 16bpp.
    _write_reg(0x92, 0x21);

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
    if (!Panel_Device::init(use_reset)) return false;

    startWrite(true);

    // ---- Wait for normal-operation mode (STSR bit1 == 0) ----
    {
        auto t = millis();
        while (_read_status() & 0x02)
        {
            if (millis() - t > 500) { endWrite(); return false; }
        }
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
    _write_reg(0x02, 0x40);   // MACR: 16bpp RGB565, L->R, T->B
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

    _write_reg(0x10, 0x04);   // MPWCTR: main window 16bpp (bits[3:2]=01b)

    // ---- Main image / canvas ----
    _write_reg32(0x20, CANVAS_BASE_ADDR);
    _write_reg16(0x24, (uint16_t)timing.h_display);
    _write_reg16(0x26, 0);
    _write_reg16(0x28, 0);

    _write_reg32(0x50, CANVAS_BASE_ADDR);
    _write_reg16(0x54, (uint16_t)timing.h_display);

    _write_reg(0x5E, 0x01);   // AW_COLOR: XY mode, 16bpp

    _win_xs = 0;  _win_ys = 0;
    _win_xe = (uint16_t)(timing.h_display - 1);
    _win_ye = (uint16_t)(timing.v_display - 1);
    _set_active_window(0, 0, timing.h_display, timing.v_display);

    endWrite();

    // ---- ST7701S panel init via shared SPI pins ----
    if (st7701s_pins.pin_cs >= 0 && st7701s_pins.pin_clk >= 0 && st7701s_pins.pin_din >= 0)
    {
        _bus->release();
        _st7701s_init_sequence();
        _bus->init();
    }

    // ---- LT7680 Display ON ----
    startWrite(true);
    {
        uint8_t dpcr = 0x40;                  // bit6 = display ON
        if (timing.pclk_rising) dpcr |= 0x80;
        _write_reg(0x12, dpcr);
    }
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

    _write_reg(0x02, (uint8_t)(0x40 | wdir));

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
    _set_forecolor(rawcolor);
    _write_reg(0x91, 0xCC);
    _write_reg(0x92, 0x01);

    _write_reg32(0xA7, CANVAS_BASE_ADDR);
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
    _write_reg(0x92, 0x21);

    _write_reg32(0x93, CANVAS_BASE_ADDR);
    _write_reg16(0x97, (uint16_t)timing.h_display);
    _write_reg16(0x99, (uint16_t)src_x);
    _write_reg16(0x9B, (uint16_t)src_y);

    _write_reg32(0xA7, CANVAS_BASE_ADDR);
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
