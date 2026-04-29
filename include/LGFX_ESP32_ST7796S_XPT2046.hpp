#pragma once
 
#define LGFX_USE_V1
 
#include <LovyanGFX.hpp>
 
// Custom configuration example for using LovyanGFX on ESP32
 
/*
Duplicate this file, give it a new name, and modify the settings to match your environment.
Include the created file from your user program to use it.
 
You may place the duplicated file in the library's lgfx_user folder,
but be aware that it may be deleted when the library is updated.
 
For safe operation, create a backup or place it in your user project folder.
//*/
 
 
/// Create a class with custom settings, derived from LGFX_Device.
class LGFX : public lgfx::LGFX_Device
{
/*
 The class name can be changed from "LGFX" to another name.
 If using alongside AUTODETECT, "LGFX" is already in use, so change it to something else.
 Also, when using multiple panels simultaneously, give each a different name.
 * If you change the class name, you must also change the constructor name to match.
 
 You are free to choose any name, but to prepare for growing configurations,
 for example, if configuring an ILI9341 via SPI on an ESP32 DevKit-C:
  LGFX_DevKitC_SPI_ILI9341
 Using a name like this and matching the file name to the class name makes it easier to manage.
//*/
 
 
// Prepare an instance matching the type of panel you are connecting.
//lgfx::Panel_GC9A01      _panel_instance;
//lgfx::Panel_GDEW0154M09 _panel_instance;
//lgfx::Panel_HX8357B     _panel_instance;
//lgfx::Panel_HX8357D     _panel_instance;
//lgfx::Panel_ILI9163     _panel_instance;
//lgfx::Panel_ILI9341     _panel_instance;
//lgfx::Panel_ILI9342     _panel_instance;
//lgfx::Panel_ILI9481     _panel_instance;
//lgfx::Panel_ILI9486     _panel_instance;
//lgfx::Panel_ILI9488     _panel_instance;
//lgfx::Panel_IT8951      _panel_instance;
//lgfx::Panel_RA8875      _panel_instance;
//lgfx::Panel_SH110x      _panel_instance; // SH1106, SH1107
//lgfx::Panel_SSD1306     _panel_instance;
//lgfx::Panel_SSD1327     _panel_instance;
//lgfx::Panel_SSD1331     _panel_instance;
//lgfx::Panel_SSD1351     _panel_instance; // SSD1351, SSD1357
//lgfx::Panel_SSD1963     _panel_instance;
//lgfx::Panel_ST7735      _panel_instance;
//lgfx::Panel_ST7735S     _panel_instance;
//lgfx::Panel_ST7789      _panel_instance;
lgfx::Panel_ST7796      _panel_instance;
 
 
// Prepare an instance matching the type of bus connecting to the panel.
  lgfx::Bus_SPI       _bus_instance;   // SPI bus instance
//lgfx::Bus_I2C       _bus_instance;   // I2C bus instance (ESP32 only)
//lgfx::Bus_Parallel8 _bus_instance;   // 8-bit parallel bus instance (ESP32 only)
 
// Prepare an instance if backlight control is available. (Delete if not needed)
  lgfx::Light_PWM     _light_instance;
 
// Prepare an instance matching the type of touchscreen. (Delete if not needed)
//lgfx::Touch_CST816S          _touch_instance;
//lgfx::Touch_FT5x06           _touch_instance; // FT5206, FT5306, FT5406, FT6206, FT6236, FT6336, FT6436
//lgfx::Touch_GSL1680E_800x480 _touch_instance; // GSL_1680E, 1688E, 2681B, 2682B
//lgfx::Touch_GSL1680F_800x480 _touch_instance;
//lgfx::Touch_GSL1680F_480x272 _touch_instance;
//lgfx::Touch_GSLx680_320x320  _touch_instance;
//lgfx::Touch_GT911            _touch_instance;
//lgfx::Touch_STMPE610         _touch_instance;
//lgfx::Touch_TT21xxx          _touch_instance; // TT21100
lgfx::Touch_XPT2046          _touch_instance;
 
public:
 
  // Create a constructor and configure all settings here.
  // If you changed the class name, specify the same name for the constructor.
  LGFX(void)
  {
    { // Configure bus control settings.
      auto cfg = _bus_instance.config();    // Get the bus configuration structure.
 
// SPI bus settings
      cfg.spi_host = SPI3_HOST;     // Select which SPI to use  ESP32-S2,C3: SPI2_HOST or SPI3_HOST / ESP32: VSPI_HOST or HSPI_HOST
      // * Due to ESP-IDF version updates, VSPI_HOST and HSPI_HOST are deprecated. If you get errors, use SPI2_HOST or SPI3_HOST instead.
      cfg.spi_mode = 0  ;             // Set SPI communication mode (0 to 3)
      //cfg.freq_write = 27000000;    // SPI clock for transmission (max 80MHz, rounded to 80MHz divided by an integer)
      cfg.freq_write = 80000000;    // SPI clock for transmission (max 80MHz, rounded to 80MHz divided by an integer)
      //cfg.freq_read  = 20000000;    // SPI clock for receiving
      cfg.freq_read  = 20000000;    // SPI clock for receiving
      cfg.spi_3wire  = false;        // Set true if receiving is done on the MOSI pin
      cfg.use_lock   = true;        // Set true to use transaction locking
      cfg.dma_channel = SPI_DMA_CH_AUTO; // Set DMA channel to use (0=no DMA / 1=ch1 / 2=ch2 / SPI_DMA_CH_AUTO=auto)
      // * Due to ESP-IDF version updates, SPI_DMA_CH_AUTO (auto setting) is now recommended. Specifying ch1 or ch2 is deprecated.
      cfg.pin_sclk = 18;            // Set SPI SCLK pin number
      cfg.pin_mosi = 23;            // Set SPI MOSI pin number
      cfg.pin_miso = 19;            // Set SPI MISO pin number (-1 = disable)
      cfg.pin_dc   = 2;            // Set SPI D/C pin number  (-1 = disable)
     // If sharing an SPI bus with an SD card, MISO must be set and not omitted.
//*/
/*
// I2C bus settings
      cfg.i2c_port    = 0;          // Select which I2C port to use (0 or 1)
      cfg.freq_write  = 400000;     // Clock for transmission
      cfg.freq_read   = 400000;     // Clock for receiving
      cfg.pin_sda     = 21;         // Pin number where SDA is connected
      cfg.pin_scl     = 22;         // Pin number where SCL is connected
      cfg.i2c_addr    = 0x3C;       // I2C device address
//*/
/*
// 8-bit parallel bus settings
      cfg.i2s_port = I2S_NUM_0;     // Select which I2S port to use (I2S_NUM_0 or I2S_NUM_1) (Uses ESP32's I2S LCD mode)
      cfg.freq_write = 20000000;    // Transmission clock (max 20MHz, rounded to 80MHz divided by an integer)
      cfg.pin_wr =  4;              // Pin number where WR is connected
      cfg.pin_rd =  2;              // Pin number where RD is connected
      cfg.pin_rs = 15;              // Pin number where RS (D/C) is connected
      cfg.pin_d0 = 12;              // Pin number where D0 is connected
      cfg.pin_d1 = 13;              // Pin number where D1 is connected
      cfg.pin_d2 = 26;              // Pin number where D2 is connected
      cfg.pin_d3 = 25;              // Pin number where D3 is connected
      cfg.pin_d4 = 17;              // Pin number where D4 is connected
      cfg.pin_d5 = 16;              // Pin number where D5 is connected
      cfg.pin_d6 = 27;              // Pin number where D6 is connected
      cfg.pin_d7 = 14;              // Pin number where D7 is connected
//*/
 
      _bus_instance.config(cfg);    // Apply the settings to the bus.
      _panel_instance.setBus(&_bus_instance);      // Set the bus on the panel.
    }
 
    { // Configure display panel control settings.
      auto cfg = _panel_instance.config();    // Get the display panel configuration structure.
 
      cfg.pin_cs           =    15;  // Pin number where CS is connected   (-1 = disable)
      cfg.pin_rst          =    4;  // Pin number where RST is connected  (-1 = disable)
      cfg.pin_busy         =    -1;  // Pin number where BUSY is connected (-1 = disable)
 
      // * The following settings have typical default values for each panel, so if uncertain, try commenting them out.
 
      cfg.panel_width      =   320;  // Actual displayable width
      cfg.panel_height     =   480;  // Actual displayable height
      cfg.offset_x         =     0;  // Panel X-direction offset
      cfg.offset_y         =     0;  // Panel Y-direction offset
      cfg.offset_rotation  =     0;  // Rotation direction offset value 0~7 (4~7 are vertically flipped)
      cfg.dummy_read_pixel =     8;  // Number of dummy read bits before pixel readout
      cfg.dummy_read_bits  =     1;  // Number of dummy read bits before reading non-pixel data
      cfg.readable         =  true;  // Set true if data readback is possible
      cfg.invert           = false;  // Set true if panel brightness is inverted
      cfg.rgb_order        = false;  // Set true if red and blue are swapped on the panel
      cfg.dlen_16bit       = false;  // Set true for panels that send data in 16-bit units over 16-bit parallel or SPI
      cfg.bus_shared       =  false;  // Set true if sharing bus with an SD card (enables bus control in drawJpgFile, etc.)
 
// Only set the following if the display is misaligned on drivers with variable pixel counts, like ST7735 or ILI9163.
//    cfg.memory_width     =   240;  // Maximum width supported by the driver IC
//    cfg.memory_height    =   320;  // Maximum height supported by the driver IC
 
      _panel_instance.config(cfg);
    }
 
//*
    { // Configure backlight control settings. (Delete if not needed)
      auto cfg = _light_instance.config();    // Get the backlight configuration structure.
 
      cfg.pin_bl = 33;              // Pin number where the backlight is connected
      cfg.invert = false;           // Set true to invert backlight brightness
      cfg.freq   = 44100;           // Backlight PWM frequency
      cfg.pwm_channel = 7;          // PWM channel number to use
 
      _light_instance.config(cfg);
      _panel_instance.setLight(&_light_instance);  // Set the backlight on the panel.
    }
//*/
 
//*
    { // Configure touchscreen control settings. (Delete if not needed)
      auto cfg = _touch_instance.config();
 
      cfg.x_min      = 0;    // Minimum X value obtained from touchscreen (raw value)
      cfg.x_max      = 319;  // Maximum X value obtained from touchscreen (raw value)
      cfg.y_min      = 0;    // Minimum Y value obtained from touchscreen (raw value)
      cfg.y_max      = 479;  // Maximum Y value obtained from touchscreen (raw value)
      cfg.pin_int    = -1;   // Pin number where INT is connected
      cfg.bus_shared = false; // Set true if using a bus shared with the display
      cfg.offset_rotation = 3;// Adjustment value 0~7 if display and touch orientations don't match
 
// For SPI connection
      cfg.spi_host = SPI2_HOST ;// Select which SPI to use (HSPI_HOST or VSPI_HOST)
      cfg.freq = 2500000;     // Set SPI clock
      cfg.pin_sclk = 14;     // Pin number where SCLK is connected
      cfg.pin_mosi = 13;     // Pin number where MOSI is connected
      cfg.pin_miso = 32;     // Pin number where MISO is connected
      cfg.pin_cs   = 21;     // Pin number where CS is connected
 
// For I2C connection
      //cfg.i2c_port = 1;      // Select which I2C to use (0 or 1)
      //cfg.i2c_addr = 0x38;   // I2C device address number
      //cfg.pin_sda  = 23;     // Pin number where SDA is connected
      //cfg.pin_scl  = 32;     // Pin number where SCL is connected
      //cfg.freq = 400000;     // Set I2C clock
 
      _touch_instance.config(cfg);
      _panel_instance.setTouch(&_touch_instance);  // Set the touchscreen on the panel.
    }
//*/
 
    setPanel(&_panel_instance); // Set the panel to use.
  }
};