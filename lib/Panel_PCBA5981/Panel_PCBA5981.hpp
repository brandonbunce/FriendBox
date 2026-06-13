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

    // Backlight brightness via the LT7680's internal PWM (datasheet §9,
    // registers §13.7). Requires the board's backlight jump points set to
    // "internal PWM" (ER-PCBA5981: J1 open, J2 short — external is the
    // factory default). No ESP32 GPIO involved; tft.setBrightness() lands
    // here through the normal LovyanGFX path. 255 = full on; 0 = off
    // (residual 1/1024 duty — see _init_backlight_pwm).
    void setBrightness(uint8_t brightness) override;

    // -- Multi-frame SDRAM addressing -------------------------------------
    // Bytes per frame at the chip's native 16bpp RGB565 depth.
    static constexpr uint32_t framebufferBytes16bpp(uint16_t w, uint16_t h)
    { return (uint32_t)w * h * 2; }

    // Block until the next vertical blanking interval begins, then return.
    // Gates the MISA register write so the flip takes effect during blanking
    // rather than mid-scan, eliminating the torn-line artifact.
    // Uses REG[F0h]/[F1h] vsync interrupt flag (INTC1/INTC2 per RA8876/LT7680
    // register map). timeout_ms caps the wait in case vsync never fires.
    void waitVSync(uint32_t timeout_ms = 50);

    // Switch the SDRAM address that the panel scans out for display.
    // Address must be 4-byte aligned.
    void setMainImageAddress(uint32_t addr);

    // Read back the current MISA value from the chip.
    uint32_t readMainImageAddress(void);

    // Read back MIW (Main Image Width, REG[24h-25h]) and CIW (Canvas Image
    // Width, REG[54h-55h]). Both are set once in init() to the panel width
    // and never touched again — drift implies a register-clobber bug.
    uint16_t readMainImageWidth(void);
    uint16_t readCanvasImageWidth(void);

    // Read back MWULX (Main Window Upper-Left X, REG[26h-27h]) and MWULY
    // (REG[28h-29h]). These define where the scanout window starts WITHIN
    // the source buffer; set once at init to (0, 0). Drift to MWULX≠0 looks
    // like a horizontal scroll (image shifts left, right edge wraps onto
    // left) which matches the observed bug shape.
    uint16_t readMainWindowUpperLeftX(void);
    uint16_t readMainWindowUpperLeftY(void);

    // Read a single chip register byte. Exposed so the diagnostic in
    // display.cpp can dump a wider register set without growing this API
    // for every individual register.
    uint8_t  readRegByte(uint8_t reg) { return _read_reg_byte(reg); }

    // Reassert MIW + CIW + MWULX/Y + the full-screen active window.
    // Defensive: long playback runs were observed accumulating a stable
    // ~48px horizontal shift that persists across files. MWULX/Y are never
    // rewritten anywhere except init, so a stray write to those registers
    // would produce exactly the observed symptom.
    void reassertScanoutConfig(uint16_t width, uint16_t height);

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

    // BTE Memory Copy with Opacity (Picture Mode). Whole-bitmap alpha blend:
    //   DT = (S0 * alpha32/32) + (S1 * (1 - alpha32/32)),  alpha32 in 0..31.
    // For a UI fade-in pass S0 = overlay slot, S1 = dst = background canvas.
    void blitFramesAlpha(uint32_t s0_addr, uint16_t s0_x, uint16_t s0_y,
                         uint32_t s1_addr, uint16_t s1_x, uint16_t s1_y,
                         uint32_t dst_addr, uint16_t dst_x, uint16_t dst_y,
                         uint16_t w, uint16_t h, uint8_t alpha32);

    // Filled-rectangle draw via the Geometric Drawing Engine (datasheet
    // section 6.3). Single REG[76h] kick - the chip rasterises in hardware,
    // no per-pixel SPI traffic. Coordinates are in canvas pixel space.
    void fillRectGPU(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2,
                           uint16_t rgb565);

    // Filled-circle draw via the Geometric Drawing Engine (datasheet pg. 145,
    // REG[76h] DCR1). Single register kick - the chip's circle rasteriser
    // runs in hardware, no per-row SPI traffic. The caller must ensure the
    // bounding box fits inside the active window; off-canvas centres are not
    // handled here.
    void fillCircleGPU(uint16_t cx, uint16_t cy, uint16_t r,
                             uint16_t rgb565);

    // Filled / outline rounded-rectangle via the Geometric Drawing Engine
    // (datasheet §6.6). Endpoints inclusive; rx/ry are the corner X/Y radii.
    // Single REG[76h] kick - the chip rasterises in hardware.
    void fillRoundRectGPU(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2,
                          uint16_t rx, uint16_t ry, uint16_t rgb565);
    void drawRoundRectGPU(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2,
                          uint16_t rx, uint16_t ry, uint16_t rgb565);

    // Write a row of native RGB565 pixels directly to the canvas, bypassing
    // the LovyanGFX pixelcopy machinery. Use this for bulk transfers where
    // the source is already in the panel's wire format (e.g. SD load path).
    void writeRawPixels(uint16_t x, uint16_t y, uint16_t w, const uint16_t* data);

    // Stream a full w×h RGB565 frame to the canvas in one SPI burst.
    // Sets the window to (0,0,w-1,h-1), kicks _start_memorywrite once, then
    // streams all w*h pixels without any per-row transaction overhead.
    // The canvas address must already be set to the target slot by the caller.
    void writeRawFrame(const uint16_t* data, uint16_t w, uint16_t h);

    // Stream a full 8bpp frame (RGB332 CLUT indices, one byte per pixel) to
    // the canvas using a single CS-held DMA burst.
    //   Protocol: after CS asserts the LT7680 parses ONLY the first byte as a
    //   command byte; 0x80 (A0=1, RW#=0) enters "write to REG[04h]" mode and
    //   every subsequent byte with CS held goes directly to SDRAM (auto-inc).
    //   Packs [0x80][clut8[0]] as the first 16-bit write then DMA-streams the
    //   remaining count-1 bytes — no per-byte CS toggling.
    // clut8  – count bytes of RGB332 palette indices (matches _init_clut_rgb332)
    // count  – must equal panel_width * panel_height
    // The canvas address must already be set to the target slot by the caller.
    void writeRawFrame8bpp(const uint8_t* clut8, uint32_t count);

    // Diagnostic: same effect as writeRawFrame8bpp, but writes one row at a
    // time inside a 480x1 active window, instead of one 230,400-byte burst.
    void writeRawFrame8bppRowByRow(const uint8_t* clut8);

    // ---- Serial Flash public API (§10.2 SPI Master + §10.3 DMA) --------

    // Read 3-byte JEDEC ID (manufacturer, memory type, capacity).
    // Logs result to Serial. Returns 0xFFFFFF if no response.
    uint32_t flashReadJEDECID(void);

    // Erase one 4 KB sector. Blocks until erase completes or timeout.
    void flashEraseSector(uint32_t addr);

    // Program up to 256 bytes into one flash page. addr must be within a
    // single 256-byte page (caller is responsible for alignment).
    // Blocks until program completes or timeout.
    void flashPageProgram(uint32_t addr, const uint8_t* data, uint16_t len);

    // Read len bytes from flash via SPI Master (slow path — for verification).
    void flashReadBytes(uint32_t addr, uint8_t* buf, uint16_t len);

    // DMA a rectangular pixel block from Serial Flash to an SDRAM canvas
    // (§10.3.3, Fig 10-12 polling mode).
    //   flash_addr       – source byte address in flash
    //   flash_src_width  – pixel width of the image stored in flash
    //   canvas_dst_addr  – SDRAM start address of the destination canvas
    //   dst_x, dst_y     – top-left corner inside that canvas
    //   block_w, block_h – block dimensions to transfer
    // The drawing canvas (CVSSA) is restored to its previous value after DMA.
    void dmaFlashBlock(uint32_t flash_addr,
                       uint16_t flash_src_width,
                       uint32_t canvas_dst_addr,
                       uint16_t dst_x, uint16_t dst_y,
                       uint16_t block_w, uint16_t block_h);

    // ---- User-defined character (UCG) glyph engine (datasheet §8.2) ------
    // Point the character generator at the CGRAM base address in SDRAM
    // (REG[DBh-DEh] CGRAM_STR). Glyph N then lives at base + N*bytes_per_glyph.
    void cgramSetStart(uint32_t cgram_addr);

    // Upload raw UCG dot-matrix bytes into CGRAM (SDRAM) via the linear-mode
    // memory-write port — the "Initialize CGRAM from MCU" path. cgram_addr is
    // an absolute SDRAM byte address (CVSSA is forced to 0 for the write).
    void cgramWrite(uint32_t cgram_addr, const uint8_t* data, uint32_t len);

    // Read CGRAM bytes back (diagnostic).
    void cgramRead(uint32_t cgram_addr, uint8_t* buf, uint32_t len);

    // Render one user-defined character at (x,y) into the current canvas slot
    // using the hardware text engine. fg565/bg565 are the glyph + background
    // colors; heightCode 0/1/2 = 16/24/32-dot; enlarge 1..4; transparentBg
    // skips the background fill so the glyph composites over canvas content.
    // charSource: CCR0 bits[7:6] — 0=internal CGROM, 1=external CGROM,
    // 2=user-defined CGRAM (default for UCG glyphs).
    void drawChar(uint16_t code, uint16_t x, uint16_t y,
                  uint16_t fg565, uint16_t bg565,
                  uint8_t heightCode, uint8_t enlarge, bool transparentBg,
                  uint8_t charSource = 2);
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


    // Mirror of REG[50h] CVSSA (Canvas Start Address). All BTE source/dest
    // and GDE-clipped fills must read from here so that draws follow whatever
    // SDRAM slot setCanvasAddress() last selected. Hardcoding the base address
    // (slot 0) makes setCanvasAddress() a no-op for fill rects and copyRect.
    uint32_t _canvas_addr = 0;
    uint8_t  _reg02       = 0x40;  // current REG[02h] value (set by setRotation)

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

    void _round_rect_kick(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2,
                          uint16_t rx, uint16_t ry, uint16_t rgb565, bool fill);

    void _set_active_window(uint16_t x, uint16_t y, uint16_t w, uint16_t h);
    void _cgram_write_row(uint32_t dst_addr, const uint8_t* data, uint16_t n);
    void _start_memorywrite(void);
    void _set_forecolor(uint32_t rawcolor);
    void _write_pixel16(uint16_t color);  // converts RGB565→RGB332 index, writes 1 byte (8bpp)
    void _init_clut_rgb332(void);         // programs the 256-entry RGB332 CLUT at startup
    void _init_backlight_pwm(void);       // starts PWM timers 0+1 at full duty (backlight)

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

    // ---- SPI Master helpers (§10.2, REG[B8h–BBh]) -----------------------
    // Select a register without writing data (needed to prime [0xC0] reads).
    void _select_reg(uint8_t reg);
    // Select + read one byte from that register.
    uint8_t _read_reg_byte(uint8_t reg);

    // Assert / deassert SFCS1# via SPIMCR2 (REG[B9h]).
    // Assert also deasserts first to reset both TX and RX FIFOs (§10.2).
    void _spi_cs_assert(void);
    void _spi_cs_deassert(void);

    // Write one byte to the TX FIFO (REG[B8h] = SPIDR).
    void _spi_tx(uint8_t data);

    // Poll SPIMSR (BAh) until TX FIFO empty (bit7=1).
    // Returns the final SPIMSR byte; logs a warning on timeout.
    uint8_t _spi_wait_done(void);

    // Send len bytes in ≤16-byte chunks (FIFO depth); RX bytes discarded.
    void _spi_write_buf(const uint8_t* buf, uint16_t len);

    // Send WREN (06h) command.
    void _flash_write_enable(void);

    // Poll Status Register 1 (RDSR 05h) until WIP=0. Returns false on timeout.
    bool _flash_wait_ready(uint32_t timeout_ms = 5000);
  };

//----------------------------------------------------------------------------
 }
}
