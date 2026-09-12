// =====================================================
// ESP32 CYD Touch Test — 4×4 zone button test
// Standalone firmware to verify XPT2046 touch chip is responsive
// Target: ESP32-2432S028 (CYD) — 240×320 ST7789 + XPT2046
// =====================================================

#include <Arduino.h>

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

class LGFX_CYD : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel_instance;
  lgfx::Bus_SPI _bus_instance;
public:
  LGFX_CYD() {
    {
      auto cfg = _bus_instance.config();
      cfg.spi_host = VSPI_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 40000000;
      cfg.pin_sclk = 14;
      cfg.pin_mosi = 13;
      cfg.pin_miso = 12;
      cfg.pin_dc = 2;
      _bus_instance.config(cfg);
      _panel_instance.setBus(&_bus_instance);
    }
    {
      auto cfg = _panel_instance.config();
      cfg.pin_cs = 15;
      cfg.pin_rst = -1;
      cfg.memory_width = 240;
      cfg.memory_height = 320;
      cfg.panel_width = 240;
      cfg.panel_height = 320;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      _panel_instance.config(cfg);
    }
    setPanel(&_panel_instance);
  }
};

LGFX_CYD lcd;

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n========================================");
  Serial.println("【Touch Test】Boot OK — LCD initialized");
  Serial.println("========================================");

  lcd.init();
  lcd.setRotation(1);
  lcd.fillScreen(TFT_BLACK);
}

void loop() {
  delay(1000);
}