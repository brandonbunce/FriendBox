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
  void drawFilledRectGeo(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t rgb565)
  {
    static_cast<lgfx::Panel_PCBA5981 *>(panel())->drawFilledRectGeo(x1, y1, x2, y2, rgb565);
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
};
