#pragma once

#define LGFX_USE_V1

#include <LovyanGFX.hpp>
#include <driver/i2c.h>

#include <Panel_PCBA5981.hpp>

// LovyanGFX configuration for the FriendBox PCBA5981 hardware revision.
//   Display    : LT7680A controller + ST7701S 480x480 RGB panel (BuyDisplay ER-TFT040-3)
//   Touch      : GT911 capacitive
// The ST7701S panel init is bit-banged over the same SPI pins as the LT7680
// (pin_cs / pin_sclk / pin_mosi) during Panel_PCBA5981::init(). The LovyanGFX
// SPI bus is released for that phase and re-claimed afterwards.

class LGFX : public lgfx::LGFX_Device
{
  lgfx::Panel_PCBA5981 _panel_instance;
  lgfx::Bus_SPI        _bus_instance;
  lgfx::Light_PWM      _light_instance;
  lgfx::Touch_GT911    _touch_instance;

public:
  LGFX(void)
  {
    {
      auto cfg = _bus_instance.config();
      cfg.spi_host    = SPI3_HOST;
      cfg.spi_mode    = 0;
      cfg.freq_write  = 80000000;
      cfg.freq_read   = 20000000;
      cfg.spi_3wire   = false;
      cfg.use_lock    = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk    = 18;
      cfg.pin_mosi    = 23;
      cfg.pin_miso    = 19;
      cfg.pin_dc      = -1;   // Unused on LT7680 (D/C is encoded in the SPI prefix byte).
      _bus_instance.config(cfg);
      _panel_instance.setBus(&_bus_instance);
    }

    {
      auto cfg = _panel_instance.config();
      cfg.pin_cs           =     5;
      cfg.pin_rst          =    17;
      cfg.pin_busy         =    -1;
      cfg.panel_width      =   480;
      cfg.panel_height     =   480;
      cfg.offset_x         =     0;
      cfg.offset_y         =     0;
      cfg.offset_rotation  =     0;
      cfg.dummy_read_pixel =     0;
      cfg.dummy_read_bits  =     0;
      cfg.readable         = false;
      cfg.invert           = false;
      cfg.rgb_order        = false;
      cfg.dlen_16bit       = false;
      cfg.bus_shared       = false;
      _panel_instance.config(cfg);

      // ST7701S requires separate initialization on the PCBA5981. Set the pins here corresponding to
      // LCD_CS, LCD_CLK, and LCD_DIN on the board so they can be used during that phase.
      _panel_instance.st7701s_pins.pin_cs  =  25;
      _panel_instance.st7701s_pins.pin_clk = 26;
      _panel_instance.st7701s_pins.pin_din = 33;
    }

    {
      auto cfg = _light_instance.config();
      cfg.pin_bl      = 33;
      cfg.invert      = false;
      cfg.freq        = 44100;
      cfg.pwm_channel = 7;
      _light_instance.config(cfg);
      _panel_instance.setLight(&_light_instance);
    }

    {
      auto cfg = _touch_instance.config();
      cfg.x_min           = 0;
      cfg.x_max           = 480;
      cfg.y_min           = 0;
      cfg.y_max           = 480;
      cfg.pin_int         = GPIO_NUM_27;
      cfg.pin_rst         = GPIO_NUM_14;
      cfg.bus_shared      = false;
      cfg.offset_rotation = 1;
      cfg.i2c_port        = I2C_NUM_0;
      cfg.pin_sda         = GPIO_NUM_13;
      cfg.pin_scl         = GPIO_NUM_32;
      cfg.freq            = 400000;
      cfg.i2c_addr        = 0x14;
      _touch_instance.config(cfg);
      _panel_instance.setTouch(&_touch_instance);
    }

    setPanel(&_panel_instance);
  }

  // Expose Panel_PCBA5981-specific hardware drawing so callers can use tft.xxx()
  // without needing to cast tft.panel() themselves.
  //
  // These wrappers accept signed coordinates and clip / skip as needed before
  // handing off to the panel methods (which require valid 13-bit unsigned
  // values - the LT7680's coordinate registers are 13 bits wide and a negative
  // input would wrap into the addressable space). Direct callers of
  // panel()->fillRectGPU / fillCircleGPU still have to obey that contract.
  void fillRectGPU(int32_t x1, int32_t y1, int32_t x2, int32_t y2, uint16_t rgb565)
  {
    if (x1 > x2) std::swap(x1, x2);
    if (y1 > y2) std::swap(y1, y2);
    int32_t w = width();
    int32_t h = height();
    if (x2 < 0 || y2 < 0 || x1 >= w || y1 >= h) return;
    if (x1 < 0)     x1 = 0;
    if (y1 < 0)     y1 = 0;
    if (x2 >= w)    x2 = w - 1;
    if (y2 >= h)    y2 = h - 1;
    static_cast<lgfx::Panel_PCBA5981 *>(panel())->fillRectGPU(
        (uint16_t)x1, (uint16_t)y1, (uint16_t)x2, (uint16_t)y2, rgb565);
  }
  void fillCircleGPU(int32_t cx, int32_t cy, int32_t r, uint16_t rgb565)
  {
    if (r < 0) return;
    int32_t w = width();
    int32_t h = height();
    // Bounding box doesn't intersect the canvas - skip the kick entirely.
    if (cx + r < 0 || cy + r < 0 || cx - r >= w || cy - r >= h) return;
    // Top/left overhang would cause 13-bit underflow in the rasteriser's
    // pixel-address math; the chip's Active Window clipper might still drop
    // the wrapped pixels but it's unverified, so use the software path.
    // Right/bottom overhang is fine - the AW clip handles it cleanly.
    if (cx - r < 0 || cy - r < 0)
    {
      fillCircle(cx, cy, r, rgb565);
      return;
    }
    static_cast<lgfx::Panel_PCBA5981 *>(panel())->fillCircleGPU(
        (uint16_t)cx, (uint16_t)cy, (uint16_t)r, rgb565);
  }
  void writeRawPixels(uint16_t x, uint16_t y, uint16_t w, const uint16_t* data)
  {
    static_cast<lgfx::Panel_PCBA5981 *>(panel())->writeRawPixels(x, y, w, data);
  }
  void setCanvasAddress(uint32_t addr)
  {
    static_cast<lgfx::Panel_PCBA5981 *>(panel())->setCanvasAddress(addr);
  }
  void setMainImageAddress(uint32_t addr)
  {
    static_cast<lgfx::Panel_PCBA5981 *>(panel())->setMainImageAddress(addr);
  }
  void blitFrames(uint32_t src_addr, uint16_t src_x, uint16_t src_y,
                  uint32_t dst_addr, uint16_t dst_x, uint16_t dst_y,
                  uint16_t w, uint16_t h)
  {
    static_cast<lgfx::Panel_PCBA5981 *>(panel())->blitFrames(
        src_addr, src_x, src_y, dst_addr, dst_x, dst_y, w, h);
  }
};
