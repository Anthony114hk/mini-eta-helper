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

// =====================================================
// Grid geometry
// =====================================================
const int TITLE_H = 20;
const int FOOTER_H = 60;
const int GRID_TOP = TITLE_H;
const int GRID_BOTTOM = 320 - FOOTER_H;  // 260
const int CELL_W = 80;
const int CELL_H = (GRID_BOTTOM - GRID_TOP) / 4;  // 60
const int GRID_COLS = 4;
const int GRID_ROWS = 4;
const int NUM_ZONES = GRID_COLS * GRID_ROWS;  // 16

// Hex digit label for each zone index (0..15). Zone 5 displays "5" with a red "R" badge.
const char ZONE_LABELS[NUM_ZONES] = {
  '0','1','2','3',
  '4','5','6','7',
  '8','9','A','B',
  'C','D','E','F'
};
const int RESET_ZONE = 5;

// =====================================================
// Per-zone state
// =====================================================
struct ZoneState {
  uint16_t taps = 0;
};
ZoneState zones[NUM_ZONES];

// =====================================================
// Draw title bar
// =====================================================
void drawTitleBar() {
  lcd.fillRect(0, 0, 320, TITLE_H, TFT_YELLOW);
  lcd.setFont(&fonts::efontTW_16);
  lcd.setTextSize(1);
  lcd.setTextColor(TFT_BLACK, TFT_YELLOW);
  lcd.setCursor(4, 2);
  lcd.print("ESP32 CYD Touch Test v1");
}

// =====================================================
// Draw a single zone (default or pressed)
// =====================================================
void drawZone(int idx, bool pressed) {
  int row = idx / GRID_COLS;
  int col = idx % GRID_COLS;
  int x = col * CELL_W;
  int y = GRID_TOP + row * CELL_H;

  uint16_t fill = pressed ? TFT_GREEN : TFT_BLACK;
  uint16_t border = (idx == RESET_ZONE) ? TFT_RED : TFT_DARKGREY;
  uint16_t textColor = pressed ? TFT_BLACK : TFT_WHITE;

  lcd.fillRect(x, y, CELL_W, CELL_H, fill);
  lcd.drawRect(x, y, CELL_W, CELL_H, border);

  // Hex digit (large, centered)
  lcd.setFont(&fonts::efontTW_16);
  lcd.setTextSize(2);
  lcd.setTextColor(textColor, fill);
  lcd.setCursor(x + 28, y + 12);
  lcd.print(ZONE_LABELS[idx]);

  // Reset badge "R" in top-left corner
  if (idx == RESET_ZONE) {
    lcd.setTextSize(1);
    lcd.setTextColor(TFT_RED, fill);
    lcd.setCursor(x + 4, y + 4);
    lcd.print("R");
  }

  // Tap count in bottom-right corner
  lcd.setTextSize(1);
  lcd.setTextColor(textColor, fill);
  lcd.setCursor(x + CELL_W - 14, y + CELL_H - 12);
  lcd.print(zones[idx].taps);
}

// =====================================================
// Draw entire 4x4 grid (all zones in current state)
// =====================================================
void drawGrid() {
  for (int i = 0; i < NUM_ZONES; i++) {
    drawZone(i, false);
  }
}

// =====================================================
// Draw empty footer (placeholder, expanded in Task 7)
// =====================================================
void drawFooter() {
  lcd.fillRect(0, GRID_BOTTOM, 320, FOOTER_H, TFT_BLACK);
  lcd.setFont(&fonts::efontTW_16);
  lcd.setTextSize(1);
  lcd.setTextColor(TFT_CYAN, TFT_BLACK);
  lcd.setCursor(4, GRID_BOTTOM + 4);
  lcd.print("Total taps: 0");
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n========================================");
  Serial.println("【Touch Test】Boot OK — LCD initialized");
  Serial.println("========================================");

  lcd.init();
  lcd.setRotation(1);
  lcd.fillScreen(TFT_BLACK);

  drawTitleBar();
  drawGrid();
  drawFooter();
}

void loop() {
  delay(1000);
}