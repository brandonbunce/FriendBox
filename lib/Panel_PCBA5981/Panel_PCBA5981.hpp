/*----------------------------------------------------------------------------/
  Lovyan GFX panel driver for the FriendBox PCBA5981 hardware revision.

Hardware:
  - Levetop LT7680A graphics controller (register-compatible with ER5517)
  - Sitronix ST7701S 480x480 RGB panel
  - GT911 capacitive touch (configured separately at the LGFX level)

LT7680 SPI protocol (4-wire, CPOL=0, CPHA=0, MSBFIRST):
  Each frame is exactly 16 bits with its own CS pulse.
    Select register : [0x00][reg]   (CS-toggled 16 bits)
    Write data      : [0x80][data]  (CS-toggled 16 bits)
    Read status     : [0x40] + read byte
    Read data       : [0xC0] + read byte
  Pixel data is 16bpp little-endian: low byte first ([0x80][lo]) then high
  byte ([0x80][hi]); each byte is its own 16-bit transaction.

ST7701S panel init:
  On the PCBA5981 the ST7701S shares the LT7680 SPI pins (CS/SCLK/MOSI).
  init() bit-bangs the ST7701S 9-bit serial init sequence after the LT7680
  controller is configured but before the display is turned on, then hands
  the SPI bus back to LovyanGFX.
/----------------------------------------------------------------------------*/
#pragma once

#include <lgfx/v1/panel/Panel_Device.hpp>

namespace lgfx
{
 inline namespace v1
 {
//----------------------------------------------------------------------------

  struct Panel_PCBA5981 : public Panel_Device
  {
    // PLL configuration for one oscillator (PPLL, MPLL, or CPLL).
    // F_out = XI * N / R / OD   where OD = 2^od_bits
    struct pll_t {
      uint16_t N  = 0;  // multiplier (9-bit: REG1 bit0 = N[8], REG2 = N[7:0])
      uint8_t  R  = 1;  // pre-divider [4:0] (REG1 bits[5:1])
      uint8_t  OD = 2;  // post-divider: 0=1x, 1=2x, 2=4x, 3=8x (REG1 bits[7:6])
    };

    // Display timing (all values in pixels).
    struct timing_t {
      uint16_t h_display       = 480;
      uint16_t h_back_porch    =  20;
      uint16_t h_front_porch   =  20;
      uint16_t h_sync_width    =  20;
      uint16_t v_display       = 480;
      uint16_t v_back_porch    =  20;
      uint16_t v_front_porch   =  12;
      uint16_t v_sync_width    =   3;
      uint8_t  pclk_rising          = 0;
      uint8_t  hsync_active_high    = 0;
      uint8_t  vsync_active_high    = 0;
      uint8_t  de_active_high       = 1;
    };

    // ST7701S pin assignments. On the PCBA5981 these alias the LT7680 SPI
    // pins; the bus is released for bit-bang init then re-claimed.
    struct st7701s_pins_t {
      int8_t pin_cs   = -1;
      int8_t pin_clk  = -1;
      int8_t pin_din  = -1;
    };

    Panel_PCBA5981(void)
    {
      _cfg.memory_width  = _cfg.panel_width  = 480;
      _cfg.memory_height = _cfg.panel_height = 480;
      _cfg.dummy_read_pixel = 0;
      _cfg.dummy_read_bits  = 0;

      // Defaults from the BuyDisplay ER-TFT040-3 reference (XI = 10 MHz):
      //   PPLL ~10 MHz pixel clock, MPLL/CPLL ~100 MHz.
      pll_pixel = { .N = 20,  .R = 5, .OD = 2 };
      pll_mem   = { .N = 100, .R = 5, .OD = 1 };
      pll_core  = { .N = 100, .R = 5, .OD = 1 };
    }

    pll_t          pll_pixel;
    pll_t          pll_mem;
    pll_t          pll_core;
    timing_t       timing;
    st7701s_pins_t st7701s_pins;

    bool init(bool use_reset) override;
    void beginTransaction(void) override;
    void endTransaction(void) override;

    // -- Multi-frame SDRAM addressing -------------------------------------
    // Bytes per frame at the chip's native 16bpp RGB565 depth.
    static constexpr uint32_t framebufferBytes16bpp(uint16_t w, uint16_t h)
    { return (uint32_t)w * h * 2; }

    // Switch the SDRAM address that the panel scans out for display.
    // Address must be 4-byte aligned.
    void setMainImageAddress(uint32_t addr);

    // Switch the SDRAM address that subsequent draws (and readRect) target.
    // Address must be 4-byte aligned. Use to direct ops into a frame slot
    // other than the displayed one (undo buffer, animation slot, etc.).
    void setCanvasAddress(uint32_t addr);

    // Hardware-accelerated rect copy between two SDRAM frame slots via the
    // BTE. Both slots are assumed to share the same image_width (panel width).
    // Use for backing-store snapshot/restore, undo capture, animation prep.
    void blitFrames(uint32_t src_addr, uint16_t src_x, uint16_t src_y,
                    uint32_t dst_addr, uint16_t dst_x, uint16_t dst_y,
                    uint16_t w, uint16_t h);

    // Filled-rectangle draw via the Geometric Drawing Engine (datasheet
    // section 6.3). Single REG[76h] kick - the chip rasterises in hardware,
    // no per-pixel SPI traffic. Coordinates are in canvas pixel space.
    void drawFilledRectGeo(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2,
                           uint16_t rgb565);

    // Write a row of native RGB565 pixels directly to the canvas, bypassing
    // the LovyanGFX pixelcopy machinery. Use this for bulk transfers where
    // the source is already in the panel's wire format (e.g. SD load path).
    void writeRawPixels(uint16_t x, uint16_t y, uint16_t w, const uint16_t* data);
    // ---------------------------------------------------------------------

    color_depth_t setColorDepth(color_depth_t depth) override;
    void setRotation(uint_fast8_t r) override;

    void writeCommand(uint32_t data, uint_fast8_t bit_length) override;
    void writeData(uint32_t data, uint_fast8_t bit_length) override;

    void waitDisplay(void) override;
    bool displayBusy(void) override;

    void writePixels(pixelcopy_t* param, uint32_t len, bool use_dma) override;
    void writeBlock(uint32_t rawcolor, uint32_t len) override;

    void setWindow(uint_fast16_t xs, uint_fast16_t ys, uint_fast16_t xe, uint_fast16_t ye) override;
    void drawPixelPreclipped(uint_fast16_t x, uint_fast16_t y, uint32_t rawcolor) override;
    void writeFillRectPreclipped(uint_fast16_t x, uint_fast16_t y, uint_fast16_t w, uint_fast16_t h, uint32_t rawcolor) override;
    void writeImage(uint_fast16_t x, uint_fast16_t y, uint_fast16_t w, uint_fast16_t h, pixelcopy_t* param, bool use_dma) override;

    void readRect(uint_fast16_t x, uint_fast16_t y, uint_fast16_t w, uint_fast16_t h, void* dst, pixelcopy_t* param) override;
    void copyRect(uint_fast16_t dst_x, uint_fast16_t dst_y, uint_fast16_t w, uint_fast16_t h, uint_fast16_t src_x, uint_fast16_t src_y) override;

    void setInvert(bool invert) override {}
    void setSleep(bool flg) override {}
    void setPowerSave(bool flg) override {}
    uint32_t readCommand(uint_fast16_t cmd, uint_fast8_t index, uint_fast8_t len) override { return 0; }
    uint32_t readData(uint_fast8_t index, uint_fast8_t len) override { return 0; }

  protected:
    uint32_t _latestcolor     = 0;
    uint16_t _win_xs          = 0;
    uint16_t _win_ys          = 0;
    uint16_t _win_xe          = 479;
    uint16_t _win_ye          = 479;
    bool     _in_transaction  = false;
    bool     _flg_memorywrite = false;

    const uint8_t* getInitCommands(uint8_t listno) const override { return nullptr; }

    void begin_transaction(void);
    void end_transaction(void);

    void _cmd16(uint16_t word);
    void _write_reg(uint8_t reg, uint8_t data);
    void _write_reg16(uint8_t reg_lo, uint16_t value);
    void _write_reg32(uint8_t reg_lsb, uint32_t value);

    uint8_t _read_status(void);
    bool    _wait_busy(uint32_t timeout_ms = 1000);
    void    _wait_sdram_ready(void);

    void _set_active_window(uint16_t x, uint16_t y, uint16_t w, uint16_t h);
    void _start_memorywrite(void);
    void _set_forecolor(uint32_t rawcolor);
    void _write_pixel16(uint16_t color);

    // Read one byte from the LT7680 memory port via the [0xC0] prefix
    // CS-toggled 16-bit transaction.
    uint8_t _read_byte(void);

    // Configure the read window and prime REG[04h] for sequential
    // [0xC0]-prefixed memory reads. Discards the dummy first byte that
    // the chip emits after a port-mode switch (datasheet section 13.4).
    void _start_memoryread(uint16_t x, uint16_t y, uint16_t w, uint16_t h);

    // Bit-bang the ST7701S 9-bit serial init sequence on the shared SPI pins.
    // The LovyanGFX bus must be released before calling and re-init'd after.
    void _st7701s_init_sequence(void);
  };

//----------------------------------------------------------------------------
 }
}
