#pragma once
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

// Waveshare ESP32-S3-Touch-LCD-1.69 pin map
//   LCD:   SCLK 6, MOSI 7, DC 4, CS 5, RST 8, BL 15
//   Touch: SDA 11, SCL 10, INT 14, RST 13 (CST816, addr 0x15)
class LGFX : public lgfx::LGFX_Device
{
  lgfx::Panel_ST7789 _panel;
  lgfx::Bus_SPI _bus;
  lgfx::Light_PWM _light;
  lgfx::Touch_CST816S _touch;

public:
  LGFX()
  {
    {
      auto cfg = _bus.config();
      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 40000000;
      cfg.freq_read = 16000000;
      cfg.spi_3wire = false;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = 6;
      cfg.pin_mosi = 7;
      cfg.pin_miso = -1;
      cfg.pin_dc = 4;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs = 5;
      cfg.pin_rst = 8;
      cfg.pin_busy = -1;
      cfg.panel_width = 240;
      cfg.panel_height = 280;
      cfg.offset_x = 0;
      cfg.offset_y = 20; // 240x280 glass sits in a 240x320 controller
      cfg.readable = false;
      cfg.invert = true;
      cfg.rgb_order = false;
      cfg.dlen_16bit = false;
      cfg.bus_shared = false;
      _panel.config(cfg);
    }
    {
      auto cfg = _light.config();
      cfg.pin_bl = 15;
      cfg.invert = false;
      cfg.freq = 44100;
      cfg.pwm_channel = 7;
      _light.config(cfg);
      _panel.setLight(&_light);
    }
    {
      auto cfg = _touch.config();
      cfg.x_min = 0;
      cfg.x_max = 239;
      cfg.y_min = 0;
      cfg.y_max = 279;
      cfg.pin_int = 14;
      cfg.pin_rst = 13;
      cfg.bus_shared = false;
      cfg.offset_rotation = 0;
      cfg.i2c_port = 0;
      cfg.i2c_addr = 0x15;
      cfg.pin_sda = 11;
      cfg.pin_scl = 10;
      cfg.freq = 400000;
      _touch.config(cfg);
      _panel.setTouch(&_touch);
    }
    setPanel(&_panel);
  }
};
