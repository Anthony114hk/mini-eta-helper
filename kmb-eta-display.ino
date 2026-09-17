#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <time.h>
#include <WebServer.h>
#include <Update.h>
#include <Wire.h>
#include <SPI.h>

// ✅ Touch SPI — Hardware SPI (HSPI) 取代 bit-bang，更穩定
SPIClass touchSPI(HSPI);

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

// ✅ 沒有額外 font file — 用 efontTW_16 (u8g2 點陣字, 108 ASCII + 371 CJK)
//    加自製 bitmap overlay 補字 (見下方 glyph_overlays.h)
// 因為 gb2312a font 太大 (163KB) 會爆 firmware (1.875MB limit)
//
// ⚠️ efontTW_16 覆蓋率實測 (對過全量 KMB stop + route 資料):
//    站名 1118 個字 → 缺 17 個, 目的地 454 個字 → 缺 5 個
//    實際會出現嘅缺失字只有 埗 邨 鰂 脷 · → 已用 bitmap overlay 補齊
// ⚠️ 呢個字型冇 emoji / ✓✗⚙🔄 之類符號, LCD 上唔可以用 (只會顯示空白)

// ✅ Forward declarations — Arduino IDE 會喺 .ino file 頂部自動插入所有 function prototype
// 但 IDE 唔識 struct 嘅 forward dependency。如果 prototype (例如 drawBusLine(BusGroup&))
// 喺 struct BusGroup 定義之前出現就會 fail。所以要喺度先 declare 一次。
struct BusGroup;
struct StopData;
struct WeatherData;

// =====================================================
// LCD 設定 (CYD 240x320 ST7789) — Touch 用 software bit-bang SPI，唔靠 LovyanGFX
// 因為你嘅 LovyanGFX 太舊 (連 Touch_XPT2046::setSPI/setTouch 都冇)
// =====================================================
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
// 缺失字形 bitmap overlay (取代舊嘅手繪「埗」bitmap)
//
// efontTW_16 (LovyanGFX u8g2 字型) 覆蓋 KMB 站名/目的地 1118 個字裡面嘅 1101 個。
// 實際資料會出現但字型冇嘅字:
//   埗 U+57D7 (深水埗, 16 條路線)    邨 U+90A8 (322 條路線 ← 最多)
//   鰂 U+9C02 (鰂魚涌)              脷 U+8137 (鴨脷洲)
//   · U+00B7 (路線 B1 目的地)
// → 用 16x16 1-bit bitmap overlay 補上, 唔會改字
// 其餘 12 個 (峯/栢/滙/琼/窰/蔴/乪/䃟/担/叠/麖/鱲) 已對過全量 route + stop 資料, 唔會出現
// =====================================================
#include "glyph_overlays.h"

// =====================================================
// 除錯開關: 1 = 每次畫字都 log 出嚟 (查亂碼 / 查寬度用)
//   會 print「字串 + 闊度 + 邊幾個字冇字形」
//   平時留 0, 唔會拖慢 LCD
// =====================================================
#define DEBUG_DRAW_TEXT 0

// 回傳 encoding 對應嘅 bitmap, 冇就 nullptr
const uint8_t* findGlyphOverlay(const char* p) {
  for (int i = 0; i < GLYPH_OVERLAY_COUNT; i++) {
    if (strncmp(p, GLYPH_OVERLAYS[i].utf8, 3) == 0) return GLYPH_OVERLAYS[i].bitmap;
  }
  return nullptr;
}

// 該 encoding 用咗幾多 byte (只支援 UTF-8 BMP 範圍的字)
inline int utf8Len(unsigned char c) {
  if (c < 0x80) return 1;
  if ((c & 0xE0) == 0xC0) return 2;
  return 3;
}

#if DEBUG_DRAW_TEXT
// 印出字串, 同埋標示每個字係「字型有」/「bitmap 補」/「兩個都冇 (會空白)」
void debugDumpString(const char* tag, const char* s, int width) {
  Serial.printf("【Draw】%s w=%d: %s\n", tag, width, s);
  const char* p = s;
  int missing = 0;
  while (*p) {
    if (findGlyphOverlay(p)) { p += 3; continue; }
    int n = utf8Len((unsigned char)*p);
    char buf[8] = {0};
    memcpy(buf, p, (n < 7) ? n : 7);
    // 用 lcd.textWidth 側面探測: 冇字形嘅字唔會佔寬度
    if (lcd.textWidth(buf) == 0) {
      Serial.printf("   ⚠️ 冇字形 (會空白): %s  U+", buf);
      unsigned int cp = 0;
      if (n == 3) cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
      else if (n == 2) cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F);
      else cp = (unsigned char)p[0];
      Serial.printf("%04X\n", cp);
      missing++;
    }
    p += n;
  }
  if (missing) Serial.printf("   → 共 %d 個字冇字形\n", missing);
}
#endif

// =====================================================
// 畫字串 + bitmap overlay 補字
// 例: "深水埗 ETA" → drawString("深水") + bitmap(埗) + drawString(" ETA")
// ⚠️ 用 lcd.drawString() 而唔係 lcd.print() → 唔會 wrap 去下一行
// =====================================================
int drawStringWithBu(const char* s, int x, int y, uint16_t color) {
  lcd.setFont(&fonts::efontTW_16);
  lcd.setTextSize(1);
  lcd.setTextColor(color, TFT_BLACK);

  int curX = x;
  const char* p = s;

  while (*p) {
    const uint8_t* bmp = findGlyphOverlay(p);
    if (bmp) {
      // lcd.drawString() 會將字身抬高 2px (u8g2 y_offset = -2),
      // bitmap 已經預留同樣嘅 2px 上邊距, 所以直接畫就同旁邊嘅字對齊
      lcd.drawBitmap(curX, y, bmp, 16, 16, color);
      curX += 16;
      p += 3;
      continue;
    }

    // 搵下一個 overlay 字 (或者字串尾)
    const char* next = p + utf8Len((unsigned char)*p);
    while (*next && !findGlyphOverlay(next)) next += utf8Len((unsigned char)*next);

    size_t chunkLen = (size_t)(next - p);
    if (chunkLen > 0) {
      char buf[256];
      if (chunkLen >= sizeof(buf)) chunkLen = sizeof(buf) - 1;
      memcpy(buf, p, chunkLen);
      buf[chunkLen] = '\0';

      lcd.drawString(buf, curX, y);
      curX += lcd.textWidth(buf);
    }
    p += chunkLen;
  }

#if DEBUG_DRAW_TEXT
  debugDumpString("lcd", s, curX - x);
#endif
  return curX - x;
}

int drawStringWithBu(const String& s, int x, int y, uint16_t color) {
  return drawStringWithBu(s.c_str(), x, y, color);
}

// 量度含 overlay 字嘅字串實際闊度 (跑馬燈同截字要用)
int textWidthWithBu(const char* s) {
  lcd.setFont(&fonts::efontTW_16);
  lcd.setTextSize(1);
  int w = 0;
  const char* p = s;
  while (*p) {
    if (findGlyphOverlay(p)) { w += 16; p += 3; continue; }
    const char* next = p + utf8Len((unsigned char)*p);
    while (*next && !findGlyphOverlay(next)) next += utf8Len((unsigned char)*next);
    char buf[256];
    size_t n = (size_t)(next - p);
    if (n >= sizeof(buf)) n = sizeof(buf) - 1;
    memcpy(buf, p, n);
    buf[n] = '\0';
    w += lcd.textWidth(buf);
    p += n;
  }
  return w;
}

int textWidthWithBu(const String& s) { return textWidthWithBu(s.c_str()); }

// =====================================================
// XPT2046 Touch Driver — software bit-bang SPI
// 完全唔靠 LovyanGFX touch API，避開 SPI bus 衝突 + 版本兼容問題
// CYD pins: T_CLK=26, T_MOSI=32, T_MISO=39, T_CS=33
// =====================================================
#define T_CLK 25   // ✅ Corrected per board silkscreen: TP CLK = IO25 (was IO26, caused no-response)
#define T_MOSI 32
#define T_MISO 39
#define T_CS 33

// =====================================================
// ⚠️ 以下係一整套 touch pin 診斷工具 (runTouchPinScan),
//    只係換 CYD 板子 / touch 完全冇反應時才需要手動呼叫。
//    平時唔用 → 用 ENABLE_TOUCH_PIN_SCAN 包住, 免得佔 flash
//    同埋撳 GPIO 0 時撞到呢啲 pin。要用就把下面 0 改成 1,
//    再喺 setup() 加 runTouchPinScan();
// =====================================================
#define ENABLE_TOUCH_PIN_SCAN 0

#if ENABLE_TOUCH_PIN_SCAN
// Pin-scan diagnostic — 用 GPIO 0 撳落去觸發
struct PinVariant {
  const char* name;
  int clk, mosi, miso, cs;
};

const PinVariant PIN_VARIANTS[] = {
  {"A: CLK=26 MOSI=32 MISO=39 CS=33", 26, 32, 39, 33},
  {"B: CLK=25 MOSI=33 MISO=39 CS=26", 25, 33, 39, 26},
  {"C: CLK=25 MOSI=33 MISO=36 CS=26", 25, 33, 36, 26},  // GPIO 36 似 PENIRQ (stable HIGH)
  {"D: CLK=14 MOSI=13 MISO=12 CS=15", 14, 13, 12, 15},
  {"E: CLK=26 MOSI=32 MISO=34 CS=33", 26, 32, 34, 33},  // 試 MISO=34
  {"F: CLK=26 MOSI=32 MISO=35 CS=33", 26, 32, 35, 33},  // 試 MISO=35
  {"G: CLK=25 MOSI=32 MISO=39 CS=33", 25, 32, 39, 33},  // MOSI=32 + CLK=25 mix
  {"H: CLK=27 MOSI=32 MISO=39 CS=33", 27, 32, 39, 33},  // CLK=27 (CYDv2 嘅 CLK)
};

uint16_t scanReadZ1(int clk, int mosi, int miso, int cs, bool mode3) {
  pinMode(clk, OUTPUT);
  pinMode(mosi, OUTPUT);
  pinMode(miso, INPUT);
  pinMode(cs, OUTPUT);
  digitalWrite(cs, HIGH);
  digitalWrite(clk, mode3 ? HIGH : LOW);  // Mode 3 = idle HIGH
  digitalWrite(mosi, LOW);

  digitalWrite(cs, LOW);
  uint8_t cmd = 0xB0;
  for (int i = 7; i >= 0; i--) {
    digitalWrite(mosi, (cmd >> i) & 1);
    if (mode3) {
      // Mode 3: idle HIGH, data sampled on rising edge
      digitalWrite(clk, LOW);
      delayMicroseconds(2);
      digitalWrite(clk, HIGH);
      delayMicroseconds(2);
    } else {
      // Mode 0: idle LOW, data sampled on rising edge
      digitalWrite(clk, HIGH);
      delayMicroseconds(2);
      digitalWrite(clk, LOW);
      delayMicroseconds(2);
    }
  }
  uint16_t result = 0;
  for (int i = 11; i >= 0; i--) {
    if (mode3) {
      digitalWrite(clk, LOW);
      delayMicroseconds(2);
      if (digitalRead(miso)) result |= (1 << i);
      digitalWrite(clk, HIGH);
      delayMicroseconds(2);
    } else {
      digitalWrite(clk, HIGH);
      delayMicroseconds(2);
      if (digitalRead(miso)) result |= (1 << i);
      digitalWrite(clk, LOW);
      delayMicroseconds(2);
    }
  }
  if (!mode3) {
    digitalWrite(clk, HIGH);
    delayMicroseconds(2);
    digitalWrite(clk, LOW);
  }
  digitalWrite(cs, HIGH);
  return result;
}

void runTouchPinScan() {
  Serial.println("\n========================================");
  Serial.println("【Scan】Touch pin scan 開始 — 撳住畫面任何位置");
  Serial.println("========================================");

  // ==========================================
  // 第一階段: 試 FT6336 capacitive touch (I2C interface)
  // 好多新版 CYD 用 FT6336 而唔係 XPT2046
  // ==========================================
  Serial.println("\n========== FT6336 I2C scan (capacitive touch) ==========");
  Serial.println("  撳住畫面期間，下面每個 (SDA, SCL) pair 會試");

  struct I2CPair { int sda; int scl; };
  const I2CPair I2C_PAIRS[] = {
    {21, 22},  // 最常見 FT6336
    {27, 22},  // 另一個 variant
    {32, 33},  // 同 touch SPI 重疊
    {21, 27},
  };
  const int NUM_I2C = 4;

  for (int i = 0; i < NUM_I2C; i++) {
    auto& p = I2C_PAIRS[i];
    Serial.printf("\n[I2C] 試 SDA=%d SCL=%d\n", p.sda, p.scl);
    Wire.begin(p.sda, p.scl);
    Wire.beginTransmission(0x38);  // FT6336 default I2C address
    uint8_t err = Wire.endTransmission();
    if (err == 0) {
      Serial.printf("  ✓ I2C device found at 0x38! 撳住畫面試讀 touch...\n");
      delay(100);
      // 讀 touch point count
      Wire.beginTransmission(0x38);
      Wire.write(0x02);  // TD_STATUS register
      Wire.endTransmission();
      Wire.requestFrom((int)0x38, 1);
      uint8_t touchCount = Wire.read();
      Serial.printf("  TD_STATUS = %d (touch points)\n", touchCount);

      // 讀 X1, Y1 coordinates
      if (touchCount > 0) {
        Wire.beginTransmission(0x38);
        Wire.write(0x03);
        Wire.endTransmission();
        Wire.requestFrom((int)0x38, 4);
        uint16_t x = (Wire.read() & 0x0F) << 8 | Wire.read();
        uint16_t y = (Wire.read() & 0x0F) << 8 | Wire.read();
        Serial.printf("  Touch @ (X=%d, Y=%d) — 撳住唔放!\n", x, y);
      }
    } else {
      Serial.printf("  ✗ 無 device at 0x38 (err=%d)\n", err);
    }
    delay(200);
  }

  // ==========================================
  // 第二階段: XPT2046 SPI scan (舊版 touch)
  // ==========================================

  // 先試下 PENIRQ (GPIO 36) 嘅反應
  Serial.println("\n【Scan】GPIO 36 (PENIRQ?) 靜態讀數:");
  pinMode(36, INPUT);
  for (int i = 0; i < 3; i++) {
    int v = digitalRead(36);
    Serial.printf("  GPIO36=%d (HIGH=%s, LOW=%s)\n", v, v ? "1" : "0", v ? "0" : "1");
    delay(200);
  }
  Serial.println("  ⚠️ 撳住畫面期間再 log 一次，如果數值變咗 = 呢個 pin 接到 PENIRQ");

  delay(500);

  for (int mode = 0; mode < 2; mode++) {
    const char* modeName = (mode == 0) ? "SPI MODE 0 (CLK idle LOW)" : "SPI MODE 3 (CLK idle HIGH)";
    Serial.printf("\n========== %s ==========\n", modeName);

    for (int v = 0; v < 8; v++) {
      auto& p = PIN_VARIANTS[v];
      Serial.printf("\n【Scan】Variant %s\n", p.name);

      uint16_t base = scanReadZ1(p.clk, p.mosi, p.miso, p.cs, mode == 1);
      delay(50);
      uint16_t touch = scanReadZ1(p.clk, p.mosi, p.miso, p.cs, mode == 1);
      delay(50);
      uint16_t release = scanReadZ1(p.clk, p.mosi, p.miso, p.cs, mode == 1);

      // Read GPIO 36 during scan
      int irq = digitalRead(36);

      Serial.printf("  base=%d  touch=%d  release=%d  delta=%d  IRQ(36)=%d\n",
                    base, touch, release, abs((int)touch - (int)release), irq);
      delay(100);
    }
  }

  Serial.println("\n========================================");
  Serial.println("【Scan】完成！");
  Serial.println("  ✓ Work = 任何 mode 嘅 touch ≠ release (即係 SPI 讀到變化)");
  Serial.println("  ✓ IRQ 變 = GPIO 36 真係 PENIRQ");
  Serial.println("========================================\n");
}

#endif  // ENABLE_TOUCH_PIN_SCAN

// XPT2046 command bytes
#define XPT2046_CMD_X  0x90  // 12-bit differential X position
#define XPT2046_CMD_Y  0xD0  // 12-bit differential Y position
#define XPT2046_CMD_Z1 0xB0  // 12-bit differential Z1 (pressure for detect)

void touchInit() {
  // ✅ Hardware SPI (HSPI) — 比 bit-bang 穩定好多
  touchSPI.begin(T_CLK, T_MISO, T_MOSI, T_CS);
  pinMode(T_CS, OUTPUT);
  digitalWrite(T_CS, HIGH);
  Serial.println("【Touch】XPT2046 init done (HSPI hardware SPI)");
}

// ✅ Wake-up routine — 用 hardware SPI dummy reads 喚醒 XPT2046
void touchWakeUp() {
  Serial.println("【Touch】Waking up XPT2046 (20 dummy HSPI reads)...");
  for (int i = 0; i < 20; i++) {
    touchReadRaw(XPT2046_CMD_Z1);
    delay(2);
  }
  Serial.println("【Touch】Wake-up done");
}

uint16_t touchReadRaw(uint8_t cmd) {
  // ✅ Hardware SPI — ESP32 HSPI driver
  // SPI mode 0 (CPOL=0, CPHA=0), 1MHz clock, MSB first
  digitalWrite(T_CS, LOW);
  touchSPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));

  // Send 8-bit command
  touchSPI.transfer(cmd);
  // XPT2046 needs ~2us to start driving MISO after 8th clock
  // We use 16-bit transfer to read 12-bit data in one go (clock for conversion + data)
  uint16_t result = touchSPI.transfer16(0x0000);

  touchSPI.endTransaction();
  digitalWrite(T_CS, HIGH);

  // XPT2046 returns 12-bit value in bits 11-0 of the 16-bit result
  return result >> 4;  // Shift to get 12-bit value (top 12 bits)
}

bool touchIsPressed() {
  // ✅ Hardware SPI 自動管理 pin modes
  // 讀 3 次取 max — 防止某次 read 撞 noise
  uint16_t z1a = touchReadRaw(XPT2046_CMD_Z1);
  delay(1);
  uint16_t z1b = touchReadRaw(XPT2046_CMD_Z1);
  delay(1);
  uint16_t z1c = touchReadRaw(XPT2046_CMD_Z1);
  uint16_t z1 = max(max(z1a, z1b), z1c);
  return z1 > 30;
}

// =====================================================
// ⚠️ 未使用: 讀 XPT2046 原始座標 (校準用)
//    而家 tap 偵測只靠 touchIsPressed(), 唔需要座標 (避免 calibration 偏差)
//    要校準嘅話把下面 0 改成 1, 再自己喺 loop 呼叫 touchGetPoint()
// =====================================================
#define ENABLE_TOUCH_RAW_POINT 1   // ✅ v1.0.8: UI redesign 需要 row tap 座標

#if ENABLE_TOUCH_RAW_POINT
bool touchGetPoint(int* x, int* y) {
  // Warm-up reads (丟第一次嘅轉換結果)
  touchReadRaw(XPT2046_CMD_X);
  touchReadRaw(XPT2046_CMD_Y);

  // 取 5 個 sample 平均
  uint32_t sumX = 0, sumY = 0;
  for (int i = 0; i < 5; i++) {
    sumX += touchReadRaw(XPT2046_CMD_X);
    sumY += touchReadRaw(XPT2046_CMD_Y);
  }
  *x = sumX / 5;
  *y = sumY / 5;

  // ✅ Valid range check — XPT2046 未撳時 X/Y 通常接近 0 或 4095 (floating)
  //    真實 touch 通常喺 200-3900 之間
  bool valid = (*x > 200 && *x < 3900 && *y > 200 && *y < 3900);
  Serial.printf("【Touch】X=%d Y=%d valid=%s\n", *x, *y, valid ? "YES" : "NO");
  return valid;
}
#endif  // ENABLE_TOUCH_RAW_POINT

Preferences preferences;
bool shouldSaveConfig = false;

// =====================================================
// 設定
// =====================================================
const int MAX_STOPS = 5;               // 最多 5 個巴士站
const int MAX_GROUPS_PER_STOP = 40;    // 每個站最多 40 條路線
const int itemsPerPage = 5;            // 一頁顯示幾多條
const int maxDisplayWidth = 300;       // 顯示寬度 (留 20px 邊距)
const long networkInterval = 30000;    // 30 秒更新一次 API
const long pageInterval = 15000;       // 15 秒自動翻頁
const long marqueeInterval = 50;       // 跑馬燈刷新率
const float scrollSpeed = 1.5;         // 跑馬燈速度 (像素/幀)
const long weatherInterval = 900000;   // 15 分鐘更新天氣

// =====================================================
// OTA — GitHub Releases
// =====================================================
#define FIRMWARE_VERSION   "1.0.9"                 // 每次 release 之前人手改呢度 (對齊 git tag)
#define GITHUB_USER        "Anthony114hk"          // GitHub username
#define GITHUB_REPO        "mini-eta-helper"       // GitHub repo 名
#define OTA_ASSET_NAME     "kmb-eta-display.bin"   // GitHub Release 上 .bin 檔名
// (已移除未使用嘅 LONG_PRESS_MS / OTA_UPDATE_MAGIC — 兩者都冇任何地方引用)

// =====================================================
// Flip Clock Display Mode (GPIO 0 button idle mode)
// =====================================================
// 24h HH:MM 7-segment LED-style jump-clock on the CYD 320x240 screen.

// Colors (RGB565)
#define FC_BG_COLOR        0x0000   // pure black
#define FC_DIGIT_COLOR     0xFFFF   // white segments
#define FC_HINT_COLOR      0x5070A0 // dim blue hint at bottom

// v1.0.8: UI redesign header bar 顏色 (深藍底)
#define TFT_NAVY           0x000F   // RGB565: 深藍 (#000050 系)

// 7-segment digit cell layout (70x200 each, full-screen fill)
// Total inner width: 4*70 + 2*4 (digit gaps) + 2*6 (colon gaps) = 300
// Side margin: (320-300)/2 = 10
#define FC_CARD_W          70
#define FC_CARD_H          200
#define FC_CARD_Y          20   // top y of digit cells (leaves 20px top + 20px bottom)
#define FC_LEFT_X          10   // hour tens
#define FC_H1_X            84   // hour ones
#define FC_COLON_X         160  // center x of colon
#define FC_M0_X            166  // minute tens
#define FC_M1_X            240  // minute ones (ends at 310, right margin 10)

// Button state machine
//   ⚠️ v1.0.6 重設計: 移除「短撳 toggle flip clock」 (冇人用)
#define BTN_DEBOUNCE_MS    30
#define DOUBLE_TAP_MS      3000
// 長撳 GPIO 0 OTA page 嘅時間 (撳住 20 秒 → 跳 OTA check)
#define LONG_PRESS_OTA     20000
// safety: 撳住 > 20 秒都即時觸發, 唔等放開
#define BTN_HOLD_TIMEOUT   20000

// OTA state
enum OtaState {
  OTA_IDLE = 0,
  OTA_CHECKING,    // check GitHub API 中
  OTA_AVAILABLE,   // 搵到新版本
  OTA_UPTODATE,    // 已係最新
  OTA_FAILED,      // check 失敗 (network / JSON)
  OTA_DOWNLOADING, // 下載 + flash 中
  OTA_SUCCESS,     // 成功，1 秒後 reboot
  OTA_ERROR        // flash 失敗
};

OtaState otaState = OTA_IDLE;
String otaLatestVersion = "";
String otaBinUrl = "";
int otaProgress = 0;            // 0-100

// Flip clock mode
bool flipClockMode = false;
unsigned long lastFlipClockUpdate = 0;
char fcPrevH0 = ' ', fcPrevH1 = ' ', fcPrevM0 = ' ', fcPrevM1 = ' ';  // for diff-redraw

// v1.0.8 UI redesign: tap-to-expand state
//    expandedRouteIdx >= 0 → 顯示 expanded route detail page
//    tap row N → set expandedRouteIdx = N
//    tap 「← 返回」button → set expandedRouteIdx = -1
int expandedRouteIdx = -1;
int expandedStopIdx = -1;

// GPIO 0 button state machine
enum BtnState { BTN_IDLE, BTN_WAIT_RELEASE, BTN_WAIT_DOUBLE_TAP };
BtnState btnState = BTN_IDLE;
unsigned long btnPressStart = 0;
unsigned long btnReleaseTime = 0;

// =====================================================
// 資料結構
// =====================================================
struct BusGroup {
  String route;
  String dest;
  String timeParts[3];
  int etaCount = 0;
  String fullText;
  int textWidth = 0;
  float scrollX = 0;
  bool needMarquee = false;
};

struct StopData {
  String stopId;
  String stopName;
  BusGroup groups[MAX_GROUPS_PER_STOP];
  int totalGroups = 0;
};

StopData stops[MAX_STOPS];
int stopCount = 0;
int currentPage = 0;

// =====================================================
// 天氣資料
// =====================================================
struct WeatherData {
  String summary;
  int textWidth = 0;
  float scrollX = 0;
  bool needMarquee = false;
  bool loaded = false;
};

WeatherData weather;

// WiFiManager string buffers (要係持久記憶體)
char stop1_buf[33] = "";
char stop2_buf[33] = "";
char stop3_buf[33] = "";
char stop4_buf[33] = "";
char stop5_buf[33] = "";

// =====================================================
// 自訂 Web UI 設定頁 (取代 WiFiManager，簡單 HTML 表單)
// ⚠️ 一定要放喺所有 global variable 之後，
// 否則 handleConfigRoot/handleConfigSave 用到嘅 stopN_buf 未 declare。
// =====================================================
WebServer configServer(80);

const char CONFIG_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1, user-scalable=no">
  <title>KMB Bus ETA - 巴士站設定</title>
  <style>
    /* ✅ 超壓縮 — 5 field + Save 掣可以 fit 喺 400px viewport
       用 maximum-scale=1 + user-scalable=no 防止 iOS Safari zoom input 時放大成個 page */
    * { box-sizing: border-box; margin: 0; padding: 0; }
    html, body { min-height: 100%; }
    body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
           max-width: 420px; margin: 0 auto; padding: 6px 10px;
           background: #1a1a1a; color: #fff;
           overflow-y: auto; font-size: 13px; }
    h1 { color: #ffeb3b; text-align: center; font-size: 15px; margin: 0 0 2px; }
    .info { color: #aaa; text-align: center; font-size: 11px; margin-bottom: 4px; }
    form { display: flex; flex-direction: column; }
    .field { display: flex; align-items: center; margin: 1px 0; gap: 6px; }
    .field label { color: #bbb; font-size: 11px; min-width: 60px; flex-shrink: 0; }
    .field input { flex: 1; padding: 2px 6px; font-size: 13px; line-height: 1.2;
                   font-family: inherit;
                   border: 1px solid #555; background: #2a2a2a; color: #fff;
                   border-radius: 3px; height: 22px;
                   -webkit-appearance: none; appearance: none; }
    .field input:focus { outline: none; border-color: #ffeb3b; }
    button { width: 100%; padding: 4px; background: #ffeb3b; color: #000;
             border: none; font-size: 13px; font-weight: bold;
             border-radius: 3px; cursor: pointer; margin-top: 6px; height: 26px;
             -webkit-appearance: none; appearance: none; }
    button:hover { background: #fff176; }
    .help { color: #666; font-size: 10px; text-align: center; margin-top: 4px; }
  </style>
</head>
<body>
  <h1>🚌 巴士站 ID 設定</h1>
  <div class="info">輸入 KMB 巴士站編號 (例: 012345)，留空 = 略過該站</div>
  <form action="/save" method="POST" id="configForm">
    <input type="hidden" name="confirmed" value="0" id="confirmed">
    <div class="field"><label>Stop 1:</label>
      <input name="stop1" value="%s" placeholder="012345" maxlength="32"></div>
    <div class="field"><label>Stop 2:</label>
      <input name="stop2" value="%s" placeholder="067890" maxlength="32"></div>
    <div class="field"><label>Stop 3:</label>
      <input name="stop3" value="%s" placeholder="0B1234" maxlength="32"></div>
    <div class="field"><label>Stop 4:</label>
      <input name="stop4" value="%s" placeholder="0C5678" maxlength="32"></div>
    <div class="field"><label>Stop 5:</label>
      <input name="stop5" value="%s" placeholder="0D9012" maxlength="32"></div>
    <button type="submit" onclick="document.getElementById('confirmed').value='1';">💾 儲存並重新啟動</button>
  </form>
  <div class="help">儲存後 ESP32 將自動重啟</div>
</body>
</html>
)rawliteral";

const char SAVED_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html><head><meta charset="UTF-8"><title>已儲存</title>
<style>body{font-family:sans-serif;text-align:center;padding:60px;background:#1a1a1a;color:#fff;}
h1{color:#4caf50;}p{color:#aaa;}</style></head>
<body><h1>✅ 已儲存！</h1><p>ESP32 將於 3 秒後重新啟動...</p>
<p>請返回總頁面查看新嘅巴士班次</p></body></html>
)rawliteral";

void handleConfigRoot() {
  Serial.printf("【Web】收到 GET / from %s\n", configServer.client().remoteIP().toString().c_str());
  Serial.printf("【Web】Heap before malloc: %u bytes\n", ESP.getFreeHeap());

  // ✅ 用 heap 取代 stack buffer — 4096 bytes 喺 ESP32 main task 8KB stack 太大
  //    之前用 stack 4096 → stack overflow → "Double exception" panic → reboot
  const size_t HTML_BUF_SIZE = 4096;
  char* html = (char*)malloc(HTML_BUF_SIZE);
  if (html == nullptr) {
    Serial.println("【Web】⚠ malloc HTML buffer 失敗！");
    configServer.send(500, "text/plain", "Out of memory");
    return;
  }

  // ⚠ Buffer 一定要夠大：CONFIG_HTML ~2913 bytes + 5 stop 值 × 33 bytes = ~3078 bytes max
  int written = snprintf(html, HTML_BUF_SIZE, CONFIG_HTML,
                         stop1_buf, stop2_buf, stop3_buf, stop4_buf, stop5_buf);
  if (written >= (int)HTML_BUF_SIZE) {
    Serial.printf("【Web】⚠ HTML buffer overflow! written=%d >= %d\n", written, HTML_BUF_SIZE);
  }

  // ✅ 防止 browser cache 舊 HTML — 強制每次重新 load
  configServer.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  configServer.sendHeader("Pragma", "no-cache");
  configServer.sendHeader("Expires", "0");
  configServer.send(200, "text/html; charset=utf-8", html);

  free(html);
  Serial.printf("【Web】Heap after free: %u bytes\n", ESP.getFreeHeap());
}

void handleConfigSave() {
  Serial.println("========================================");
  Serial.printf("【Web】收到 POST /save from %s\n", configServer.client().remoteIP().toString().c_str());
  Serial.printf("【Web】Method: %d (1=GET, 2=POST)\n", (int)configServer.method());
  Serial.printf("【Web】Args count: %d\n", configServer.args());
  Serial.printf("【Web】confirmed: '%s'\n", configServer.arg("confirmed").c_str());
  Serial.printf("【Web】Origin: '%s'\n", configServer.header("Origin").c_str());
  Serial.printf("【Web】Referer: '%s'\n", configServer.header("Referer").c_str());
  Serial.printf("【Web】User-Agent: '%s'\n", configServer.header("User-Agent").c_str());
  Serial.println("========================================");

  // ✅ 防 browser autofill / password manager auto-submit — 必須用戶明確撳 Save 掣先接受
  if (configServer.arg("confirmed") != "1") {
    Serial.println("【Web】⚠️ REJECTED: confirmed flag missing (拒絕 auto-submit / CSRF)");
    configServer.send(403, "text/plain",
                      "Auto-submit rejected. Please click the Save button explicitly.");
    return;
  }

  Serial.println("【Web】=== handleConfigSave 被觸發 — 直接儲存到 Preferences ===");
  if (configServer.hasArg("stop1")) strncpy(stop1_buf, configServer.arg("stop1").c_str(), 32);
  if (configServer.hasArg("stop2")) strncpy(stop2_buf, configServer.arg("stop2").c_str(), 32);
  if (configServer.hasArg("stop3")) strncpy(stop3_buf, configServer.arg("stop3").c_str(), 32);
  if (configServer.hasArg("stop4")) strncpy(stop4_buf, configServer.arg("stop4").c_str(), 32);
  if (configServer.hasArg("stop5")) strncpy(stop5_buf, configServer.arg("stop5").c_str(), 32);

  // ✅ 修正: 之前只清 stop1_buf，stop2-5 都可能殘留舊資料
  //    統一處理所有 5 個 buffer (strncpy 唔保證 null-terminate 如果 arg == 32 chars)
  char* allBufs[] = { stop1_buf, stop2_buf, stop3_buf, stop4_buf, stop5_buf };
  for (int b = 0; b < 5; b++) {
    for (int i = 0; i < 33; i++) {
      if (allBufs[b][i] == '\0') {
        for (int j = i + 1; j < 33; j++) allBufs[b][j] = '\0';
        break;
      }
    }
  }

  // ✅ 直接讀 buffer 入 stops[] + 寫入 Preferences (唔靠 restart read-back)
  //    之前 bug: handleConfigSave 只寫 buffer → ESP.restart() → enterConfigMode(false)
  //    用舊 stops[] 覆蓋 buffer (stop1_buf=A→A, stop3_buf=C→保留但之後冇讀返)
  //    結果新增嘅 stop3/stop4/stop5 永遠消失 → 得返原本嘅 2 個 stop
  stopCount = 0;
  for (int i = 0; i < MAX_STOPS; i++) {
    String id = String(allBufs[i]);
    id.trim();
    if (id.length() > 0) {
      stops[stopCount].stopId = id;
      stops[stopCount].stopName = "";  // 清空舊站名等 updateBusData 重新攞
      stopCount++;
    }
  }
  saveStopIds();  // ✅ 直接寫入 Preferences → restart 後 loadStopIds() 會載返

  shouldSaveConfig = true;
  configServer.send(200, "text/html; charset=utf-8", SAVED_HTML);
  Serial.printf("【Web】已儲存 %d 個 stop: stop1='%s' stop2='%s' stop3='%s' stop4='%s' stop5='%s'\n",
                stopCount, stop1_buf, stop2_buf, stop3_buf, stop4_buf, stop5_buf);
  delay(3000);
  ESP.restart();
}

// 計時器
unsigned long lastNetworkUpdate = 0;
unsigned long lastPageSwitch = 0;
unsigned long lastClockUpdate = 0;
unsigned long lastMarqueeUpdate = 0;
unsigned long lastWeatherUpdate = 0;

// =====================================================
// 工具函式
// =====================================================
void saveConfigCallback() {
  shouldSaveConfig = true;
}

String getSystemTime() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return "--:--";
  char timeStr[6];
  sprintf(timeStr, "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
  return String(timeStr);
}

// =====================================================
// 計算總頁數 (所有 stop 加埋)
// =====================================================
int calculateTotalPages() {
  int pages = 0;
  for (int s = 0; s < stopCount; s++) {
    if (stops[s].totalGroups > 0) {
      pages += (stops[s].totalGroups + itemsPerPage - 1) / itemsPerPage;
    }
  }
  return pages;
}

// =====================================================
// 攞當前 page 對應邊個 stop 同邊段 groups
// 例如: stop1 有 7 條 → page 0,1 屬 stop1
//       stop2 有 3 條 → page 2 屬 stop2
// =====================================================
void getPageInfo(int pageIdx, int& stopIdx, int& groupStart, int& groupCount) {
  int accumulated = 0;
  for (int s = 0; s < stopCount; s++) {
    int n = stops[s].totalGroups;
    if (n == 0) continue;
    int numPages = (n + itemsPerPage - 1) / itemsPerPage;
    if (pageIdx >= accumulated && pageIdx < accumulated + numPages) {
      stopIdx = s;
      int offsetInStop = pageIdx - accumulated;
      groupStart = offsetInStop * itemsPerPage;
      groupCount = min(itemsPerPage, n - groupStart);
      return;
    }
    accumulated += numPages;
  }
  // fallback
  stopIdx = 0;
  groupStart = 0;
  groupCount = 0;
}

// =====================================================
// API: 攞站名
// =====================================================
void fetchStopName(int stopIdx) {
  if (WiFi.status() != WL_CONNECTED) return;
  if (stops[stopIdx].stopId == "") return;

  WiFiClient client;
  HTTPClient http;
  String url = "http://data.etabus.gov.hk/v1/transport/kmb/stop/" + stops[stopIdx].stopId;

  http.setTimeout(10000);
  if (http.begin(client, url)) {
    int httpResponseCode = http.GET();
    if (httpResponseCode == HTTP_CODE_OK) {
      DynamicJsonDocument doc(4096);
      DeserializationError error = deserializeJson(doc, http.getStream());

      if (!error) {
        const char* name = doc["data"]["name_tc"];
        if (name != nullptr && strlen(name) > 0) {
          stops[stopIdx].stopName = String(name);
          Serial.printf("【API】Stop %d (%s) 站名: %s\n",
                        stopIdx + 1,
                        stops[stopIdx].stopId.c_str(),
                        stops[stopIdx].stopName.c_str());
          // ✅ 持久化到 Preferences (重啟都記得)
          preferences.begin("bus_config", false);
          preferences.putString(("name_" + String(stopIdx)).c_str(), stops[stopIdx].stopName);
          preferences.end();
        }
      } else {
        Serial.printf("【API】Stop %d JSON 解析失敗: %s\n", stopIdx + 1, error.c_str());
      }
    }
    http.end();
  }
}

// =====================================================
// API: 攞齊所有天氣警告 (warnsum: 雷暴/暴雨/颱風/寒冷/酷熱...)
// =====================================================
void fetchWeatherWarnings(String& summary) {
  // ✅ 等前一個 HTTPS (rhrread) 嘅 SSL/TCP connection 完全 cleanup
  //    連續 HTTPS request 容易 hang 喺 TLS handshake (ESP32 Arduino core 已知 issue)
  delay(150);
  yield();
  Serial.printf("【警告】Heap: %u bytes\n", ESP.getFreeHeap());

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  String url = "https://data.weather.gov.hk/weatherAPI/opendata/weather.php?dataType=warnsum&lang=tc";

  http.setTimeout(10000);
  http.addHeader("User-Agent", "Mozilla/5.0 ESP32 WeatherDisplay/1.0");
  http.addHeader("Accept-Encoding", "identity");

  Serial.println("【警告】請求中...");
  Serial.println("【警告】http.begin() 開始...");
  if (http.begin(client, url)) {
    Serial.println("【警告】http.begin() OK, http.GET() 開始...");
    int code = http.GET();
    Serial.printf("【警告】http.GET() returned: %d\n", code);
    if (code == HTTP_CODE_OK) {
      String response = http.getString();
      DynamicJsonDocument doc(8192);
      DeserializationError error = deserializeJson(doc, response);

      if (!error) {
        // ✅ 列出所有可能嘅警告類別
        const char* warningFields[] = {
          "tropicalCycloneWarning",   // 颱風 (T1/T3/T8/T9/T10)
          "rainstormWarning",          // 暴雨 (黃/紅/黑)
          "thunderstormWarning",       // 雷暴
          "strongMonsoonWarning",      // 強烈季候風
          "coldWeatherWarning",        // 寒冷
          "hotWeatherWarning",         // 酷熱
          "fireDangerWarning",         // 火災危險
          "otherWarnings"              // 其他
        };

        int warningCount = 0;
        for (const char* field : warningFields) {
          if (doc[field].is<JsonArray>()) {
            for (JsonObject w : doc[field].as<JsonArray>()) {
              const char* name = w["name"].as<const char*>();
              if (name != nullptr && strlen(name) > 0) {
                summary += " | " + String(name);
                warningCount++;
              }
            }
          }
        }
        Serial.printf("【警告】共 %d 個生效中\n", warningCount);
      } else {
        Serial.printf("【警告】JSON 解析失敗: %s\n", error.c_str());
      }
    } else {
      Serial.printf("【警告】HTTP 失敗 code=%d\n", code);
    }
    http.end();
  } else {
    Serial.println("【警告】http.begin() 失敗");
  }
}

// =====================================================
// API: 攞 9 日預報 (fnd: 降雨概率 PSR / 最高最低溫)
// =====================================================
void fetchWeatherForecast(String& summary) {
  // ✅ 等前一個 HTTPS (warnsum) 嘅 SSL/TCP connection 完全 cleanup
  delay(150);
  yield();
  Serial.printf("【預報】Heap: %u bytes\n", ESP.getFreeHeap());

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  String url = "https://data.weather.gov.hk/weatherAPI/opendata/weather.php?dataType=fnd&lang=tc";

  http.setTimeout(10000);
  http.addHeader("User-Agent", "Mozilla/5.0 ESP32 WeatherDisplay/1.0");
  http.addHeader("Accept-Encoding", "identity");

  Serial.println("【預報】請求中...");
  Serial.println("【預報】http.begin() 開始...");
  if (http.begin(client, url)) {
    Serial.println("【預報】http.begin() OK, http.GET() 開始...");
    int code = http.GET();
    Serial.printf("【預報】http.GET() returned: %d\n", code);
    if (code == HTTP_CODE_OK) {
      String response = http.getString();
      DynamicJsonDocument doc(16384);
      DeserializationError error = deserializeJson(doc, response);

      if (!error) {
        // ✅ 取今日 (index 0) 嘅 PSR (Probability of Significant Rain)
        if (doc["weatherForecast"].is<JsonArray>() &&
            doc["weatherForecast"].as<JsonArray>().size() > 0) {
          const char* psr = doc["weatherForecast"][0]["PSR"].as<const char*>();
          if (psr != nullptr && strlen(psr) > 0) {
            summary += " 降雨概率 " + String(psr);
            Serial.printf("【預報】PSR: %s\n", psr);
          } else {
            Serial.println("【預報】PSR 欄位空");
          }

          // 順便 log 今日預報畀 debug
          const char* fw = doc["weatherForecast"][0]["forecastWeather"].as<const char*>();
          if (fw != nullptr) {
            Serial.printf("【預報】今日: %s\n", fw);
          }
        } else {
          Serial.println("【預報】weatherForecast 陣列空");
        }
      } else {
        Serial.printf("【預報】JSON 解析失敗: %s\n", error.c_str());
      }
    } else {
      Serial.printf("【預報】HTTP 失敗 code=%d\n", code);
    }
    http.end();
  } else {
    Serial.println("【預報】http.begin() 失敗");
  }
}

// =====================================================
// API: 攞香港天文台天氣 (rhrread: 即時溫度+濕度+雨量 + warnsum 警告)
// =====================================================
void fetchWeather() {
  if (WiFi.status() != WL_CONNECTED) return;

  String url = "https://data.weather.gov.hk/weatherAPI/opendata/weather.php?dataType=rhrread&lang=tc";

  // ✅ Retry 3 次 — TLS handshake / 網絡短暫失敗常見 (HTTP code -1)
  String response = "";
  const int MAX_ATTEMPTS = 3;

  for (int attempt = 1; attempt <= MAX_ATTEMPTS && response == ""; attempt++) {
    if (attempt > 1) {
      Serial.printf("【天氣】⚠ retry %d/%d (delay 1.5s)...\n", attempt, MAX_ATTEMPTS);
      delay(1500);
      yield();
    }

    // ✅ HTTPS 要用 WiFiClientSecure，setInsecure() 跳過 cert 驗證
    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    http.setTimeout(10000);
    http.addHeader("User-Agent", "Mozilla/5.0 ESP32 WeatherDisplay/1.0");
    http.addHeader("Accept", "application/json");
    http.addHeader("Accept-Encoding", "identity");

    Serial.printf("【天氣】請求中 (attempt %d/%d): %s\n", attempt, MAX_ATTEMPTS, url.c_str());

    if (http.begin(client, url)) {
      int code = http.GET();
      Serial.printf("【天氣】HTTP code: %d\n", code);
      if (code == HTTP_CODE_OK) {
        response = http.getString();
        Serial.printf("【天氣】Got %d bytes\n", response.length());
      } else {
        Serial.printf("【天氣】HTTP 失敗 code=%d\n", code);
      }
      http.end();
    } else {
      Serial.println("【天氣】http.begin() 失敗");
    }
  }

  if (response == "") {
    Serial.printf("【天氣】⚠ all %d attempts failed, 保留舊天氣資料 (weather.loaded=%d)\n",
                  MAX_ATTEMPTS, weather.loaded ? 1 : 0);
    return;
  }

  Serial.println("【天氣】✓ rhrread success, 開始解析...");

  DynamicJsonDocument doc(32768);
  DeserializationError error = deserializeJson(doc, response);

  if (!error) {
    String summary = "";

    // 氣溫 (取首個站 = 香港天文台)
    if (doc["temperature"]["data"].is<JsonArray>() &&
        doc["temperature"]["data"].as<JsonArray>().size() > 0) {
      float temp = doc["temperature"]["data"][0]["value"].as<float>();
      summary += "氣溫 " + String((int)temp) + "°C";
    }

    // 濕度
    if (doc["humidity"]["data"].is<JsonArray>() &&
        doc["humidity"]["data"].as<JsonArray>().size() > 0) {
      int humidity = doc["humidity"]["data"][0]["value"].as<int>();
      summary += " 濕度 " + String(humidity) + "%";
    }

    // 雨量 (過去 1 小時最大值)
    if (doc["rainfall"]["data"].is<JsonArray>()) {
      float maxRain = 0;
      for (JsonObject r : doc["rainfall"]["data"].as<JsonArray>()) {
        float v = r["max"].as<float>();
        if (v > maxRain) maxRain = v;
      }
      if (maxRain > 0) {
        summary += " 雨量 " + String(maxRain, 1) + "mm";
      }
    }

    // ✅ warningMessage (rhrread 頂層 field, 陣列 of strings, 例: ["一號熱帶氣旋警告信號現正生效。"])
    if (doc["warningMessage"].is<JsonArray>()) {
      for (JsonVariant msg : doc["warningMessage"].as<JsonArray>()) {
        const char* m = msg.as<const char*>();
        if (m != nullptr && strlen(m) > 0) {
          summary += " | " + String(m);
          Serial.printf("【天氣】warningMessage: %s\n", m);
        }
      }
    }

    // ✅ 用 warnsum endpoint 攞齊所有警告 (包括雷暴)
    fetchWeatherWarnings(summary);

    // ✅ 用 fnd endpoint 攞降雨概率 (PSR)
    fetchWeatherForecast(summary);

    if (summary != "") {
      lcd.setFont(&fonts::efontTW_16);
      weather.summary = summary;
      weather.textWidth = lcd.textWidth(summary);
      weather.scrollX = 0;
      weather.needMarquee = (weather.textWidth > 320);
      weather.loaded = true;

      Serial.printf("【天氣】%s (闊度 %d, 跑馬燈 %s)\n",
                    summary.c_str(), weather.textWidth,
                    weather.needMarquee ? "ON" : "OFF");
    } else {
      Serial.println("【天氣】所有欄位都空 (API 回應可能異常)");
    }
  } else {
    Serial.printf("【天氣】JSON 解析失敗: %s\n", error.c_str());
  }
}

// =====================================================
// v1.0.8 UI redesign: ETA 倒數 helper functions
//   輸入 ETA "21:50" + 當前時間 → "5 分" / "即將" / "1 時 30 分"
//   處理跨午夜情況 (例如 23:50 → 00:30)
// =====================================================
int etaMinutesFromNow(String etaTime) {
  if (etaTime.length() < 5) return 999;  // 冇時間數據
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return 999;  // NTP 未同步
  int nowMin = timeinfo.tm_hour * 60 + timeinfo.tm_min;

  int etaH = etaTime.substring(0, 2).toInt();
  int etaM = etaTime.substring(3, 5).toInt();
  int etaMin = etaH * 60 + etaM;

  // 跨午夜: 如果 ETA 比 now 細超過 12 小時, 當下日計 (加 24h)
  if (etaMin < nowMin - 720) etaMin += 1440;

  return etaMin - nowMin;
}

// 將 minutes 差轉成中文顯示字串
String etaToRemainingText(int minutesDiff) {
  if (minutesDiff <= 0) return "即將";
  if (minutesDiff < 60) return String(minutesDiff) + " 分";
  int hr = minutesDiff / 60;
  int mn = minutesDiff % 60;
  if (mn == 0) return String(hr) + " 時";
  return String(hr) + " 時" + String(mn) + " 分";
}

// 根據 minutes 揀顏色
uint16_t etaToColor(int minutesDiff) {
  if (minutesDiff <= 0) return TFT_RED;        // 即將到站 (0 或過咗)
  if (minutesDiff < 3) return TFT_ORANGE;      // < 3 分鐘 (好快到)
  if (minutesDiff < 10) return TFT_WHITE;      // 正常 (< 10 分)
  return TFT_LIGHTGREY;                         // 仲有時間 (> 10 分)
}

// ✅ 簡單巴士 icon (16x12 rounded rect + 三個車窗 + 兩個車輪)
void drawBusIcon(int x, int y, uint16_t color) {
  lcd.fillRoundRect(x, y, 16, 12, 2, color);
  lcd.fillRect(x + 2, y + 2, 3, 3, TFT_BLACK);
  lcd.fillRect(x + 6, y + 2, 3, 3, TFT_BLACK);
  lcd.fillRect(x + 10, y + 2, 3, 3, TFT_BLACK);
  lcd.fillRect(x + 2, y + 8, 1, 2, TFT_BLACK);
  lcd.fillRect(x + 13, y + 8, 1, 2, TFT_BLACK);
}

// =====================================================
// 渲染單行 bus route (v1.0.8 新 design: 1 個 ETA countdown + 撳展開)
//   Layout (320x36): [icon] [route] [destination] [time countdown]
//   冇跑馬燈 — 每行只顯示下一班到站時間 (倒數分鐘)
// =====================================================
void drawBusLine(BusGroup& group, int yPos, bool clearFirst) {
  lcd.setFont(&fonts::efontTW_16);
  lcd.setTextSize(1);

  if (clearFirst) {
    lcd.fillRect(0, yPos, 320, 36, TFT_BLACK);
  }

  // Bus icon (16x12) @ x=6, y=yPos+10
  drawBusIcon(6, yPos + 10, TFT_WHITE);

  // Route number (cyan, efontTW_16)
  lcd.setTextColor(TFT_CYAN, TFT_BLACK);
  drawStringWithBu(group.route, 28, yPos + 10, TFT_CYAN);

  // Destination (white, truncate if too long)
  String dest = group.dest;
  int maxDestW = 130;
  if (textWidthWithBu(dest) > maxDestW) {
    while (dest.length() > 0 && textWidthWithBu(dest + "...") > maxDestW) {
      dest.remove(dest.length() - 1);
    }
    dest += "...";
  }
  lcd.setTextColor(TFT_WHITE, TFT_BLACK);
  drawStringWithBu(dest, 90, yPos + 10, TFT_WHITE);

  // ETA countdown (colored by minutes) — 右邊對齊
  if (group.etaCount > 0 && group.timeParts[0].length() >= 5) {
    int minutesDiff = etaMinutesFromNow(group.timeParts[0]);
    String timeText = etaToRemainingText(minutesDiff);
    uint16_t timeColor = etaToColor(minutesDiff);
    lcd.setTextColor(timeColor, TFT_BLACK);
    // 右對齊: 由 x=316 往左 draw
    int tw = lcd.textWidth(timeText);
    lcd.setCursor(316 - tw, yPos + 10);
    lcd.print(timeText);
  } else {
    lcd.setTextColor(TFT_DARKGREY, TFT_BLACK);
    lcd.setCursor(260, yPos + 10);
    lcd.print("尾班");
  }

  // Row separator line (底部分隔)
  lcd.drawFastHLine(0, yPos + 35, 320, TFT_DARKGREY);
}

// =====================================================
// 渲染當前頁 (v1.0.8 新 design: dark blue header bar + 5 條 bus rows)
//   landscape 320x240 layout:
//     y=0-28:   Header bar (dark blue TFT_NAVY, stop name + time + page)
//     y=28-30:  Separator
//     y=30-210: 5 條 bus rows × 36px
//     y=210-240: Footer (更新於 HH:MM + 撳 row 提示)
// =====================================================
void displayCurrentPage() {
  lcd.fillScreen(TFT_BLACK);

  // ✅ 如果有 expanded route → 顯示 full-screen route detail
  if (expandedRouteIdx >= 0 && expandedStopIdx >= 0
      && expandedRouteIdx < stops[expandedStopIdx].totalGroups) {
    drawExpandedRoute(stops[expandedStopIdx].groups[expandedRouteIdx], expandedStopIdx);
    return;
  }

  int stopIdx, groupStart, groupCount;
  getPageInfo(currentPage, stopIdx, groupStart, groupCount);

  // === Header bar (dark blue) y=0-28 ===
  lcd.fillRect(0, 0, 320, 28, TFT_NAVY);

  // Stop name (yellow, left)
  lcd.setFont(&fonts::efontTW_16);
  lcd.setTextSize(1);
  lcd.setTextColor(TFT_YELLOW, TFT_NAVY);
  lcd.setCursor(6, 7);
  if (stops[stopIdx].stopName != "") {
    drawStringWithBu(stops[stopIdx].stopName, 6, 7, TFT_YELLOW);
  } else {
    lcd.print("Stop " + String(stopIdx + 1));
  }

  // Route count badge (cyan) — middle-right
  lcd.setTextColor(TFT_CYAN, TFT_NAVY);
  lcd.setCursor(170, 7);
  lcd.printf("%d 路線", stops[stopIdx].totalGroups);

  // Current time (white) — right
  lcd.setTextColor(TFT_WHITE, TFT_NAVY);
  lcd.setCursor(245, 7);
  lcd.print(getSystemTime());

  // Page indicator (cyan) — far right
  int totalPages = calculateTotalPages();
  if (totalPages == 0) totalPages = 1;
  if (totalPages > 1) {
    lcd.setTextColor(TFT_CYAN, TFT_NAVY);
    lcd.setCursor(290, 7);
    lcd.printf("%d/%d", currentPage + 1, totalPages);
  }

  // === Separator ===
  lcd.drawFastHLine(0, 28, 320, TFT_DARKGREY);

  // === Bus rows (y=30 至 y=210, 5 rows × 36px) ===
  if (groupCount == 0) {
    lcd.setCursor(60, 110);
    lcd.setTextColor(TFT_ORANGE, TFT_BLACK);
    lcd.print("沒有實時巴士班次");
    // Footer
    lcd.drawFastHLine(0, 216, 320, TFT_DARKGREY);
    lcd.setCursor(6, 220);
    lcd.setTextColor(TFT_DARKGREY, TFT_BLACK);
    lcd.printf("更新於 %s", getSystemTime().c_str());
    return;
  }

  int yPosition = 30;
  for (int i = groupStart; i < groupStart + groupCount; i++) {
    drawBusLine(stops[stopIdx].groups[i], yPosition, false);
    yPosition += 36;
  }

  // === Footer (y=216-240) ===
  lcd.drawFastHLine(0, 216, 320, TFT_DARKGREY);
  lcd.setCursor(6, 220);
  lcd.setTextColor(TFT_DARKGREY, TFT_BLACK);
  lcd.printf("更新於 %s", getSystemTime().c_str());

  lcd.setTextColor(TFT_DARKGREY, TFT_BLACK);
  lcd.setCursor(180, 220);
  lcd.print("撳路線睇更多");
}

// =====================================================
// Full-screen route detail page (撳 row 後展開) — v1.0.8
//   顯示該 route 嘅全部 ETA (倒數分鐘 list)
//   撳「← 返回」(x=4-50, y=0-36) → 關閉 expanded view
// =====================================================
void drawExpandedRoute(BusGroup& group, int stopIdx) {
  lcd.fillScreen(TFT_BLACK);

  // === Header bar ===
  lcd.fillRect(0, 0, 320, 36, TFT_NAVY);

  // 「← 返回」button (x=4-50, y=0-36)
  lcd.setFont(&fonts::efontTW_16);
  lcd.setTextColor(TFT_WHITE, TFT_NAVY);
  lcd.setCursor(6, 10);
  lcd.print("← 返回");

  // Route + destination (yellow)
  lcd.setCursor(58, 10);
  lcd.setTextColor(TFT_YELLOW, TFT_NAVY);
  lcd.printf("%s 往 %s", group.route.c_str(), group.dest.c_str());

  // Sub-header (stop name)
  lcd.setTextColor(TFT_CYAN, TFT_NAVY);
  lcd.setCursor(58, 26);
  if (stops[stopIdx].stopName != "") {
    drawStringWithBu(stops[stopIdx].stopName, 58, 26, TFT_CYAN);
  } else {
    lcd.print("Stop " + String(stopIdx + 1));
  }

  lcd.drawFastHLine(0, 36, 320, TFT_DARKGREY);

  // === ETA list ===
  lcd.setFont(&fonts::efontTW_16);
  lcd.setTextSize(1);

  if (group.etaCount == 0) {
    lcd.setCursor(60, 120);
    lcd.setTextColor(TFT_ORANGE, TFT_BLACK);
    lcd.print("沒有班次");
    return;
  }

  int yPos = 80;
  const int rowH = 40;
  const int maxRows = 4;  // (240-80-header) / 40 = ~4 rows
  for (int i = 0; i < group.etaCount && i < maxRows; i++) {
    String etaTime = group.timeParts[i];
    int minutesDiff = etaMinutesFromNow(etaTime);
    String timeText = etaToRemainingText(minutesDiff);
    uint16_t timeColor = etaToColor(minutesDiff);

    // Row background: alternating subtle
    uint16_t rowBg = (i % 2 == 0) ? TFT_BLACK : 0x0841;  // very dark grey
    lcd.fillRect(0, yPos, 320, rowH, rowBg);

    // Bus icon
    drawBusIcon(10, yPos + 14, TFT_WHITE);

    // ETA index (e.g. "1.", "2.")
    lcd.setTextColor(TFT_DARKGREY, rowBg);
    lcd.setCursor(34, yPos + 14);
    lcd.printf("%d.", i + 1);

    // Absolute time (white)
    lcd.setTextColor(TFT_WHITE, rowBg);
    lcd.setCursor(64, yPos + 14);
    lcd.print(etaTime);

    // Arrow → countdown
    lcd.setTextColor(TFT_DARKGREY, rowBg);
    lcd.setCursor(118, yPos + 14);
    lcd.print("→");

    // Countdown (colored by minutes)
    lcd.setTextColor(timeColor, rowBg);
    int tw = lcd.textWidth(timeText);
    lcd.setCursor(316 - tw, yPos + 14);
    lcd.print(timeText);

    yPos += rowH;
  }

  // === Footer ===
  lcd.drawFastHLine(0, 222, 320, TFT_DARKGREY);
  lcd.setCursor(6, 226);
  lcd.setTextColor(TFT_DARKGREY, TFT_BLACK);
  lcd.printf("共 %d 班 · 更新於 %s", group.etaCount, getSystemTime().c_str());
}

// =====================================================
// API: 更新所有 stop 嘅班次
// =====================================================
void updateBusData() {
  if (WiFi.status() != WL_CONNECTED) return;

  // ✅ Retry 攞返遺漏嘅站名 (例如之前 API 失敗)
  for (int s = 0; s < stopCount; s++) {
    if (stops[s].stopName == "" && stops[s].stopId != "") {
      Serial.printf("【重試】Stop %d 站名未取得，重新攞...\n", s + 1);
      fetchStopName(s);
    }
  }

  for (int s = 0; s < stopCount; s++) {
    if (stops[s].stopId == "") continue;

    WiFiClient client;
    HTTPClient http;
    String url = "http://data.etabus.gov.hk/v1/transport/kmb/stop-eta/" + stops[s].stopId;

    http.setTimeout(10000);
    if (http.begin(client, url)) {
      int httpResponseCode = http.GET();
      if (httpResponseCode == HTTP_CODE_OK) {
        DynamicJsonDocument doc(24576);
        DeserializationError error = deserializeJson(doc, http.getStream());

        if (!error) {
          JsonArray dataArray = doc["data"].as<JsonArray>();
          stops[s].totalGroups = 0;

          for (JsonObject bus : dataArray) {
            const char* route = bus["route"];
            const char* eta = bus["eta"];
            const char* dest_tc = bus["dest_tc"];

            if (eta != nullptr && strlen(eta) > 0 && String(eta) != "") {
              String routeStr = (route != nullptr) ? String(route) : "";
              String destStr = (dest_tc != nullptr) ? String(dest_tc) : "";
              String timePart = String(eta).substring(11, 16);

              // 1) 完全一樣 (路線 + 目的地) → 併入
              int matchIdx = -1;
              for (int g = 0; g < stops[s].totalGroups; g++) {
                if (stops[s].groups[g].route == routeStr && stops[s].groups[g].dest == destStr) {
                  matchIdx = g;
                  break;
                }
              }

              // 2) 同路線, 目的地字串少少唔同 (API 有時會加/減「(經…)」之類)
              //    → 併入現有嗰行, 唔好開新行 (唔係嘅話同一路線會撐爆 40 行上限)
              if (matchIdx == -1) {
                for (int g = 0; g < stops[s].totalGroups; g++) {
                  if (stops[s].groups[g].route == routeStr) { matchIdx = g; break; }
                }
              }

              if (matchIdx != -1) {
                if (stops[s].groups[matchIdx].etaCount < 3) {
                  stops[s].groups[matchIdx].timeParts[stops[s].groups[matchIdx].etaCount] = timePart;
                  stops[s].groups[matchIdx].etaCount++;
                }
              } else {
                if (stops[s].totalGroups >= MAX_GROUPS_PER_STOP) break;

                stops[s].groups[stops[s].totalGroups].route = routeStr;
                stops[s].groups[stops[s].totalGroups].dest = destStr;
                stops[s].groups[stops[s].totalGroups].timeParts[0] = timePart;
                stops[s].groups[stops[s].totalGroups].etaCount = 1;
                stops[s].totalGroups++;
              }
            }
          }

          // 計算字串寬度，判斷邊行要跑馬燈
          // ⚠️ 一定要用 textWidthWithBu(): lcd.textWidth() 唔知 bitmap overlay,
          //    會少算 16px, 令跑馬燈寬度同實際差一個字 → 兩份 copy 疊埋 (重疊 bug)
          lcd.setFont(&fonts::efontTW_16);
          for (int i = 0; i < stops[s].totalGroups; i++) {
            String timeString = "";
            for (int t = 0; t < stops[s].groups[i].etaCount; t++) {
              timeString += stops[s].groups[i].timeParts[t] + " ";
            }
            stops[s].groups[i].fullText = stops[s].groups[i].route + " 往 " +
                                          stops[s].groups[i].dest + " " + timeString;
            stops[s].groups[i].textWidth = textWidthWithBu(stops[s].groups[i].fullText);
            stops[s].groups[i].scrollX = 0;
            stops[s].groups[i].needMarquee = (stops[s].groups[i].textWidth > maxDisplayWidth);
          }

          Serial.printf("【網路】Stop %d 更新：%d 條路線。\n", s + 1, stops[s].totalGroups);
        }
      }
      http.end();
    }
  }

  // 防止 currentPage 越界
  int tp = calculateTotalPages();
  if (tp > 0 && currentPage >= tp) currentPage = 0;

  displayCurrentPage();
}

// =====================================================
// 儲存 / 載入 stop IDs (逗號分隔)
// =====================================================
void loadStopIds() {
  preferences.begin("bus_config", true);  // ✅ read-only mode
  String all = preferences.getString("stop_ids", "");

  // 兼容舊版單 stop 格式
  if (all == "") {
    String legacy = preferences.getString("stop_id", "");
    if (legacy.length() > 0) all = legacy;
  }

  stopCount = 0;
  if (all != "") {
    int start = 0;
    for (int i = 0; i <= all.length(); i++) {
      if (i == all.length() || all[i] == ',') {
        String id = all.substring(start, i);
        id.trim();
        if (id.length() > 0 && stopCount < MAX_STOPS) {
          stops[stopCount].stopId = id;
          // ✅ 順便載入已持久化嘅站名
          stops[stopCount].stopName = preferences.getString(("name_" + String(stopCount)).c_str(), "");
          stopCount++;
        }
        start = i + 1;
      }
    }
  }
  preferences.end();
  Serial.printf("【Config】載入 %d 個 stop。\n", stopCount);
}

void saveStopIds() {
  String all = "";
  for (int i = 0; i < stopCount; i++) {
    if (i > 0) all += ",";
    all += stops[i].stopId;
  }
  preferences.begin("bus_config", false);
  preferences.putString("stop_ids", all);

  // ✅ 先清除所有舊 name keys (避免殘留)
  for (int i = 0; i < MAX_STOPS; i++) {
    preferences.remove(("name_" + String(i)).c_str());
  }

  // ✅ 寫入新站名
  for (int i = 0; i < stopCount; i++) {
    if (stops[i].stopName != "") {
      preferences.putString(("name_" + String(i)).c_str(), stops[i].stopName);
    }
  }

  preferences.end();
  Serial.printf("【Config】儲存 stop IDs: %s\n", all.c_str());
}

// =====================================================
// 進入設定模式 (按鈕 / Setup 共用)
//   blocking = false (Setup 用): WiFiManager.autoConnect 處理首次 WiFi
//   blocking = true (按鈕用):    自訂 Web UI 開 5 個 stop ID field
// =====================================================
void enterConfigMode(bool blocking) {
  // 顯示 "進入設定" 過場畫面
  lcd.fillScreen(TFT_BLACK);
  lcd.setFont(&fonts::efontTW_16);

  // 將已存嘅 stop ID 填入 buffer (Web form / WiFiManager 都會讀呢啲)
  if (stopCount > 0) strncpy(stop1_buf, stops[0].stopId.c_str(), 32);
  if (stopCount > 1) strncpy(stop2_buf, stops[1].stopId.c_str(), 32);
  if (stopCount > 2) strncpy(stop3_buf, stops[2].stopId.c_str(), 32);
  if (stopCount > 3) strncpy(stop4_buf, stops[3].stopId.c_str(), 32);
  if (stopCount > 4) strncpy(stop5_buf, stops[4].stopId.c_str(), 32);

  // === 第一階段: WiFi 連線 (blocking = false 嘅 setup 流程) ===
  if (!blocking) {
    // ✅ AP mode 畫面 — WiFiManager 啟動 AP 後會阻塞等 user 設 WiFi，期間要顯示指示
    //    全部 size 1 確保 fit 320px 闊 (esp. "ESP32_Smart_Clock" 16 chars × size 2 會 overflow)
    lcd.fillScreen(TFT_BLACK);
    lcd.setFont(&fonts::efontTW_16);
    lcd.setTextSize(1);

    lcd.setTextColor(TFT_YELLOW, TFT_BLACK);
    lcd.setCursor(10, 30);
    lcd.println("WiFi 未連線");

    lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    lcd.setCursor(10, 70);
    lcd.println("用手機連 WiFi:");
    lcd.setCursor(10, 100);
    lcd.println("SSID:");

    lcd.setTextColor(TFT_CYAN, TFT_BLACK);
    lcd.setCursor(10, 130);
    lcd.println("ESP32_Smart_Clock");

    lcd.setTextColor(TFT_GREEN, TFT_BLACK);
    lcd.setCursor(10, 170);
    lcd.println("→ 連上後開瀏覽器");
    lcd.setCursor(10, 195);
    lcd.println("  http://192.168.4.1");
    lcd.setCursor(10, 220);
    lcd.println("  輸入 WiFi 密碼 → Save");

    WiFiManager wm;
    wm.setSaveConfigCallback(saveConfigCallback);
    wm.setConnectTimeout(15);
    if (!wm.autoConnect("ESP32_Smart_Clock")) {
      ESP.restart();
    }
    return;
  }

  // === 第二階段: 用戶按鈕觸發 → 開自訂 Web UI ===
  if (WiFi.status() == WL_CONNECTED) {
    // ✅ WiFi 已連線 → 喺現有 IP 開 Web server
    IPAddress ip = WiFi.localIP();

    // ✅ 加強診斷 log — 顯示 SSID / RSSI / Gateway / DNS
    Serial.println("========================================");
    Serial.printf("【Web】WiFi SSID: %s\n", WiFi.SSID().c_str());
    Serial.printf("【Web】WiFi RSSI: %d dBm\n", WiFi.RSSI());
    Serial.printf("【Web】STA IP: %s\n", ip.toString().c_str());
    Serial.printf("【Web】Gateway: %s\n", WiFi.gatewayIP().toString().c_str());
    Serial.printf("【Web】DNS:     %s\n", WiFi.dnsIP().toString().c_str());
    Serial.println("========================================");

    // ⚠️ 一定要 setTextSize(1)
    //    efontTW_16 本身已經係 16px 點陣字, 開 size 2 會變 32x32:
    //    「進入設定模式」= 6 字 × 32 = 192px (連埋高度 32+ 都會壓到下面嘅字),
    //    「用手機連 WiFi」+ size 2 更加會爆出 320px 畫面 → 睇落就係亂碼/疊字
    lcd.setTextSize(1);
    lcd.setTextColor(TFT_YELLOW, TFT_BLACK);
    lcd.setCursor(10, 50);
    lcd.println("進入設定模式");

    lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    lcd.setCursor(10, 90);
    lcd.println("用手機/電腦開瀏覽器:");

    lcd.setTextColor(TFT_CYAN, TFT_BLACK);
    lcd.setCursor(10, 120);
    lcd.printf("http://%s/", ip.toString().c_str());

    lcd.setTextColor(TFT_GREEN, TFT_BLACK);
    lcd.setCursor(10, 160);
    lcd.println("改完按 Save → 自動重啟");

    lcd.setTextColor(TFT_DARKGREY, TFT_BLACK);
    lcd.setCursor(10, 200);
    lcd.printf("SSID:%s", WiFi.SSID().c_str());
    lcd.setCursor(10, 225);
    lcd.printf("RSSI:%ddBm", WiFi.RSSI());

    // Register routes + start server
    configServer.on("/", HTTP_GET, handleConfigRoot);
    configServer.on("/save", HTTP_POST, handleConfigSave);
    configServer.begin();
    Serial.printf("【Web】Config server started: http://%s/ (port 80)\n", ip.toString().c_str());
    Serial.println("【Web】>>> 進入 config mode while loop (等 Save / 30 min timeout / handleConfigSave 觸發)");

    // Block 直到 user Save (shouldSaveConfig=true)
    shouldSaveConfig = false;
    unsigned long startMs = millis();
    unsigned long lastLcdRefresh = millis();
    // ✅ Timeout 延長到 30 分鐘，畀多啲時間 debug network 問題
    while (!shouldSaveConfig) {
      configServer.handleClient();
      delay(10);

      // ✅ 每 2 秒 redraw 「⚙ CONFIG」badge (右上角) — 確保 LCD stay 喺 config mode visible state
      if (millis() - lastLcdRefresh > 2000) {
        lastLcdRefresh = millis();
        lcd.setFont(&fonts::efontTW_16);
        lcd.setTextSize(1);
        lcd.fillRect(230, 24, 90, 18, TFT_BLACK);  // 清舊 badge
        lcd.setTextColor(TFT_RED, TFT_BLACK);
        lcd.setCursor(230, 24);
        lcd.print("[CONFIG]");
        Serial.printf("【Web】config mode alive (%lu sec elapsed)\n", (millis() - startMs) / 1000);
      }

      if (millis() - startMs > 1800000UL) {  // 30 min
        Serial.println("【Web】30 分鐘 timeout，自動離開");
        break;
      }
    }
    Serial.printf("【Web】<<< 離開 config mode (用了 %lu sec, shouldSaveConfig = %s)\n",
                  (millis() - startMs) / 1000, shouldSaveConfig ? "TRUE (Save 觸發)" : "FALSE (timeout)");
    configServer.stop();

  } else {
    // ⚠️ WiFi 未連線 → Fallback 用 WiFiManager (緊急 reconnect WiFi)
    //    同上一段一樣: 全部 size 1 (size 2 嘅 "ESP32_Smart_Clock" 會係 512px 闊, 爆畫面)
    lcd.setTextSize(1);
    lcd.setTextColor(TFT_YELLOW, TFT_BLACK);
    lcd.setCursor(10, 70);
    lcd.println("WiFi 未連線");

    lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    lcd.setCursor(10, 120);
    lcd.println("用手機連 WiFi:");
    lcd.setTextColor(TFT_CYAN, TFT_BLACK);
    lcd.setCursor(10, 150);
    lcd.println("ESP32_Smart_Clock");
    lcd.setCursor(10, 200);
    lcd.setTextColor(TFT_GREEN, TFT_BLACK);
    lcd.println("正在啟動 WiFiManager...");
    delay(2500);

    WiFiManager wm;
    wm.setSaveConfigCallback(saveConfigCallback);

    WiFiManagerParameter p_stop1("stop1", "巴士站 1 ID (留空=略過)", stop1_buf, 32);
    WiFiManagerParameter p_stop2("stop2", "巴士站 2 ID (留空=略過)", stop2_buf, 32);
    WiFiManagerParameter p_stop3("stop3", "巴士站 3 ID (留空=略過)", stop3_buf, 32);
    WiFiManagerParameter p_stop4("stop4", "巴士站 4 ID (留空=略過)", stop4_buf, 32);
    WiFiManagerParameter p_stop5("stop5", "巴士站 5 ID (留空=略過)", stop5_buf, 32);
    wm.addParameter(&p_stop1);
    wm.addParameter(&p_stop2);
    wm.addParameter(&p_stop3);
    wm.addParameter(&p_stop4);
    wm.addParameter(&p_stop5);
    wm.setConnectTimeout(15);
    shouldSaveConfig = false;
    wm.startConfigPortal("ESP32_Smart_Clock");

    if (shouldSaveConfig) {
      stopCount = 0;
      String inputs[] = { p_stop1.getValue(), p_stop2.getValue(), p_stop3.getValue(), p_stop4.getValue(), p_stop5.getValue() };
      for (int i = 0; i < MAX_STOPS; i++) {
        String id = inputs[i];
        id.trim();
        if (id.length() > 0) {
          stops[stopCount].stopId = id;
          stops[stopCount].stopName = "";
          stopCount++;
        }
      }
    }
  }

  // 用戶改完之後: 讀取 buffer 入 stops[]
  stopCount = 0;
  char* bufs[] = { stop1_buf, stop2_buf, stop3_buf, stop4_buf, stop5_buf };
  for (int i = 0; i < MAX_STOPS; i++) {
    String id = String(bufs[i]);
    id.trim();
    if (id.length() > 0) {
      stops[stopCount].stopId = id;
      stops[stopCount].stopName = "";  // 清空舊站名等重新攞
      stopCount++;
    }
  }
  if (stopCount > 0) {
    saveStopIds();
    // 重新攞每個 stop 嘅站名
    for (int i = 0; i < stopCount; i++) {
      fetchStopName(i);
    }
  }

  // 重設 timer 避免一輪瘋狂更新
  lastNetworkUpdate = millis();
  lastPageSwitch = millis();
  lastClockUpdate = millis();
  lastMarqueeUpdate = millis();
}

// =====================================================
// OTA: 檢查 GitHub 最新 release
// =====================================================
void checkLatestRelease() {
  if (WiFi.status() != WL_CONNECTED) {
    otaState = OTA_FAILED;
    Serial.println("【OTA】WiFi 斷線，無法 check");
    return;
  }

  otaState = OTA_CHECKING;
  otaLatestVersion = "";
  otaBinUrl = "";

  String url = "https://api.github.com/repos/" GITHUB_USER "/" GITHUB_REPO "/releases/latest";
  Serial.printf("【OTA】GET %s\n", url.c_str());

  String body = "";
  int code = -1;

  // ✅ Retry 3 次 — TLS handshake / DNS 偶爾失敗常見
  for (int attempt = 1; attempt <= 3; attempt++) {
    if (attempt > 1) {
      Serial.printf("【OTA】retry #%d ...\n", attempt);
      delay(1500);
    }

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(15000);

    HTTPClient http;
    http.setTimeout(15000);  // 15s — TLS handshake 第一次可能慢
    http.addHeader("User-Agent", "ESP32-OTA-Checker");
    http.addHeader("Accept", "application/vnd.github+json");

    if (!http.begin(client, url)) {
      Serial.println("【OTA】http.begin() 失敗");
      continue;
    }
    code = http.GET();
    Serial.printf("【OTA】HTTP code: %d\n", code);

    if (code != HTTP_CODE_OK) {
      http.end();
      continue;
    }

    body = http.getString();
    http.end();
    Serial.printf("【OTA】Got %d bytes\n", body.length());

    if (body.length() > 0) break;  // 成功拿到 body，唔再 retry
  }

  if (code != HTTP_CODE_OK || body.length() == 0) {
    otaState = OTA_FAILED;
    return;
  }

  DynamicJsonDocument doc(8192);
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    Serial.printf("【OTA】JSON 解析失敗: %s\n", err.c_str());
    otaState = OTA_FAILED;
    return;
  }

  otaLatestVersion = doc["tag_name"].as<String>();

  // 搵 asset .bin URL
  JsonArray assets = doc["assets"].as<JsonArray>();
  for (JsonObject a : assets) {
    if (String(a["name"].as<const char*>()) == OTA_ASSET_NAME) {
      // ✅ 直接用 GitHub API 嘅 direct CDN URL，避免 ESP32 跟 github.com -> release-assets redirect
      //    (URL pattern: https://github.com/{user}/{repo}/releases/download/{tag}/{file}
      //                 -> https://release-assets.githubusercontent.com/...)
      String direct = a["url"].as<String>();
      // GitHub API asset object 嘅 "url" 已經係 CDN URL with auth header needed (較複雜)
      // 用 browser_download_url 但 ESP32 redirect 會自動 follow (line 622-660 HTTPClient.cpp)
      otaBinUrl = a["browser_download_url"].as<String>();
      Serial.printf("【OTA】asset URL (browser_download_url): %s\n", otaBinUrl.c_str());
      Serial.printf("【OTA】asset API URL (with auth): %s\n", direct.c_str());
      break;
    }
  }

  if (otaBinUrl == "") {
    Serial.printf("【OTA】⚠ 找不到 asset '%s'\n", OTA_ASSET_NAME);
    otaState = OTA_FAILED;
    return;
  }

  Serial.printf("【OTA】latest=%s, current=%s, url=%s\n",
                otaLatestVersion.c_str(), FIRMWARE_VERSION, otaBinUrl.c_str());

  if (otaLatestVersion == FIRMWARE_VERSION) {
    otaState = OTA_UPTODATE;
  } else {
    otaState = OTA_AVAILABLE;
  }
}

// =====================================================
// OTA: 下載 .bin + flash (blocking，畫面會 freeze 直至完成)
//
// ✅ v1.0.6 fix — 「read 提前 EOF」真正原因:
//   ESP32 Arduino core 3.x 嘅 Stream::readBytes() 係逐個 byte 呼叫 read()。
//   而 NetworkClientSecure::read() 係「唔夠 data 就即刻 return -1」嘅語意
//   (internal ssl buffer 空 → WANT_READ → -1)。所以只要 TLS record 之間有
//   少少抖動，readBytes() 就會讀少幾個 byte 或者直接 return 0。
//   舊 code 將 read <= 0 當成「致命 EOF」→ 即刻 Update.abort()，
//   之後 loop() 因為 otaState 唔再係 OTA_IDLE 就淨係 delay() 空轉，
//   表面睇落就好似下載「停咗 / 斷咗線」。
//
//   Fix (對齊官方 Update.writeStream() 嘅 retry 策略):
//     1. read <= 0 唔再即刻當 fatal — 100ms 後重試，最多 30 秒冇新 data 才放棄
//     2. 每次讀 2KB (唔再 1KB 逐個 byte 拗)，減少 per-byte read() 開銷
//     3. 完成後核對 written == Content-Length
//     4. 失敗前 dump 完整診斷 (connected / available / heap) 方便再查
// =====================================================
#define OTA_READ_BUF        2048                   // 每次讀取 byte 數
#define OTA_STALL_LIMIT     300                    // 連續讀唔到 × 100ms = 最多等 30 秒

void performOTA(String binUrl) {
  otaState = OTA_DOWNLOADING;
  otaProgress = 0;

  Serial.printf("\n========== OTA START ==========\n");
  Serial.printf("【OTA】binUrl: %s\n", binUrl.c_str());

  // === 第一步：手動 follow redirect，每次用全新 socket ===
  String currentUrl = binUrl;
  int code = -1;
  String newLoc;
  int redirectCount = 0;
  WiFiClientSecure *client = nullptr;
  HTTPClient *http = nullptr;

  while (redirectCount < 4) {
    // ✅ 每個 redirect loop iteration 用全新 WiFiClientSecure + HTTPClient
    //    (唔 reuse，因為 SSL state + socket buffer 喺 redirect 過程中污染)
    if (client) { delete client; client = nullptr; }
    if (http) { delete http; http = nullptr; }

    client = new WiFiClientSecure();
    client->setInsecure();
    client->setTimeout(60000);

    http = new HTTPClient();
    http->setTimeout(60000);

    if (!http->begin(*client, currentUrl)) {
      Serial.printf("【OTA】❌ http.begin() 失敗 (URL: %s)\n", currentUrl.c_str());
      otaState = OTA_ERROR;
      delete client; delete http;
      return;
    }

    Serial.printf("【OTA】GET #%d: %s\n", redirectCount, currentUrl.c_str());
    code = http->GET();
    Serial.printf("【OTA】HTTP code: %d\n", code);

    if (code == HTTP_CODE_MOVED_PERMANENTLY || code == HTTP_CODE_FOUND || code == HTTP_CODE_TEMPORARY_REDIRECT) {
      newLoc = http->getLocation();
      Serial.printf("【OTA】↪ redirect → %s\n", newLoc.c_str());
      if (newLoc.length() == 0) {
        Serial.println("【OTA】❌ Location header 空白");
        otaState = OTA_ERROR;
        delete client; delete http;
        return;
      }
      http->end();
      currentUrl = newLoc;
      redirectCount++;
      continue;
    }
    break;  // 非 redirect response
  }

  if (code != HTTP_CODE_OK) {
    Serial.printf("【OTA】❌ HTTP code 非 200, 係 %d\n", code);
    otaState = OTA_ERROR;
    if (http) { http->end(); delete http; }
    if (client) delete client;
    return;
  }

  int total = http->getSize();
  Serial.printf("【OTA】Content-Length: %d bytes (%.2f MB)\n", total, total / 1048576.0);
  Serial.printf("【OTA】free heap = %u bytes\n", ESP.getFreeHeap());
  if (total <= 0 || total > 2 * 1024 * 1024) {
    Serial.printf("【OTA】❌ size 異常 %d\n", total);
    otaState = OTA_ERROR;
    http->end(); delete http; delete client;
    return;
  }
  // ✅ app partition (PartitionScheme=min_spiffs) = 0x1E0000 = 1966080 bytes
  //    image 大過 partition 嘅話 Update.begin() 會即 fail，所以早啲講清楚
  if (total > 1961984) {
    Serial.printf("【OTA】❌ bin (%d) 大過 app partition (%d) — 縮細 firmware 或改 PartitionScheme\n", total, 1961984);
    otaState = OTA_ERROR;
    http->end(); delete http; delete client;
    return;
  }

  Serial.println("【OTA】Update.begin()...");
  if (!Update.begin(total)) {
    Serial.printf("【OTA】❌ Update.begin() 失敗: %s\n", Update.errorString());
    otaState = OTA_ERROR;
    http->end(); delete http; delete client;
    return;
  }

  WiFiClient *stream = http->getStreamPtr();
  if (!stream) {
    Serial.println("【OTA】❌ http.getStreamPtr() 返 nullptr");
    otaState = OTA_ERROR;
    Update.abort();
    http->end(); delete http; delete client;
    return;
  }

  Serial.printf("【OTA】stream connected=%d, available=%d\n", stream->connected(), stream->available());

  // ✅ 用 heap 唔用 stack — loop task 得 8KB stack，2KB buffer 落 stack 太搏
  uint8_t *buf = (uint8_t *)malloc(OTA_READ_BUF);
  if (!buf) {
    Serial.println("【OTA】❌ malloc read buffer 失敗");
    otaState = OTA_ERROR;
    Update.abort();
    http->end(); delete http; delete client;
    return;
  }

  int written      = 0;
  int stallRetries = 0;        // 連續讀唔到嘅次數
  int shortReads   = 0;        // 讀得少過要求嘅次數 (唔致命，只係統計)
  int lastPct      = -1;
  unsigned long t0 = millis();

  while (written < total) {
    size_t want = (size_t)(total - written);
    if (want > OTA_READ_BUF) want = OTA_READ_BUF;

    int r = stream->read(buf, want);   // ✅ 直接用 raw read，唔用 readBytes 逐個 byte 拗

    if (r > 0) {
      stallRetries = 0;
      if ((size_t)r < want) shortReads++;

      if (Update.write(buf, r) != (size_t)r) {
        Serial.printf("【OTA】❌ Update.write() 失敗: %s\n", Update.errorString());
        otaState = OTA_ERROR;
        Update.abort();
        free(buf); http->end(); delete http; delete client;
        return;
      }
      written += r;

      int pct = (written * 100) / total;
      if (pct != lastPct && pct % 10 == 0) {
        lastPct = pct;
        otaProgress = pct;
        Serial.printf("【OTA】%d%% (%d/%d bytes, %lu ms, heap=%u)\n",
                      pct, written, total, millis() - t0, ESP.getFreeHeap());
      }
      delay(1);   // feed watchdog + 讓 WiFi stack 有時間收包
      continue;
    }

    // r == 0：暫時冇 data — 唔係 EOF，等一等再試
    stallRetries++;
    if (stallRetries % 50 == 0) {   // 每 5 秒報一次，睇到係「慢」定係「死」
      Serial.printf("【OTA】…等待 data (%d/%d, connected=%d, available=%d, 已等 %d 秒)\n",
                    written, total, stream->connected(), stream->available(), stallRetries / 10);
    }
    if (stallRetries >= OTA_STALL_LIMIT) {
      Serial.printf("【OTA】❌ 30 秒冇新 data 先當失敗 (written=%d/%d)\n", written, total);
      Serial.printf("【OTA】   connected=%d available=%d heap=%u shortReads=%d\n",
                    stream->connected(), stream->available(), ESP.getFreeHeap(), shortReads);
      otaState = OTA_ERROR;
      Update.abort();
      free(buf); http->end(); delete http; delete client;
      return;
    }
    delay(100);
  }

  free(buf);

  if (written != total) {
    Serial.printf("【OTA】❌ 下載唔完整: %d/%d\n", written, total);
    otaState = OTA_ERROR;
    Update.abort();
    http->end(); delete http; delete client;
    return;
  }

  Serial.println("【OTA】Update.end()...");
  if (!Update.end()) {
    Serial.printf("【OTA】❌ Update.end() 失敗: %s\n", Update.errorString());
    otaState = OTA_ERROR;
    http->end(); delete http; delete client;
    return;
  }

  http->end();
  delete http;
  delete client;
  otaProgress = 100;
  otaState = OTA_SUCCESS;
  Serial.printf("【OTA】✓ flash 成功 (%lu ms, shortReads=%d), 1 秒後 reboot\n",
                millis() - t0, shortReads);
  Serial.println("========== OTA END ==========\n");
  delay(1000);
  ESP.restart();
}

// =====================================================
// ⚠️ 已移除未使用嘅 mapTouchToLCD()
//    原因: OTA page / 主畫面嘅 tap 偵測只用 touchIsPressed() 判斷「有冇撳」,
//    完全唔用座標 (避免 CYD raw range 校準偏差)。如果日後要做「撳某個掣」,
//    再按呢個 mapping 加返:
//      x = map(rawX, 200, 3900, 0, 320);  y = map(rawY, 200, 3900, 0, 240);
// =====================================================

// =====================================================
// OTA UI Page — 顯示版本資訊 + 升級按鈕
// =====================================================
void drawOTAPage() {
  lcd.fillScreen(TFT_BLACK);
  lcd.setFont(&fonts::efontTW_16);
  lcd.setTextSize(1);

  // Title
  lcd.setTextColor(TFT_CYAN, TFT_BLACK);
  lcd.setCursor(10, 5);
  // ⚠️ 唔好用 emoji / ✓✗⚙🔄 — efontTW_16 冇呢啲 glyph (只會顯示空白)
  lcd.println("OTA 線上更新");

  lcd.drawFastHLine(0, 25, 320, TFT_DARKGREY);

  // 版本資訊
  // ⚠️ 「最新版本」同版本號一定要分兩行:
  //    「最新版本: v1.0.5 ✓ 已是最新」實測約 21 個全形字 = 336px > 320px,
  //    舊 code 一齊 print → 尾幾個字被推出畫面
  int y = 36;
  lcd.setTextColor(TFT_WHITE, TFT_BLACK);
  lcd.setCursor(10, y);
  lcd.printf("目前版本: v%s", FIRMWARE_VERSION);
  y += 20;

  lcd.setCursor(10, y);
  lcd.setTextColor(TFT_WHITE, TFT_BLACK);
  lcd.print("最新版本:");
  y += 20;

  lcd.setCursor(10, y);
  switch (otaState) {
    case OTA_CHECKING:
      lcd.setTextColor(TFT_YELLOW, TFT_BLACK);
      lcd.println("檢查中...");
      break;
    case OTA_AVAILABLE:
      lcd.setTextColor(TFT_GREEN, TFT_BLACK);
      lcd.printf("%s  有更新", otaLatestVersion.c_str());
      break;
    case OTA_UPTODATE:
      lcd.setTextColor(TFT_GREEN, TFT_BLACK);
      lcd.printf("%s  已是最新", otaLatestVersion.c_str());
      break;
    case OTA_FAILED:
      lcd.setTextColor(TFT_RED, TFT_BLACK);
      lcd.println("X 檢查失敗");
      break;
    default:
      lcd.setTextColor(TFT_DARKGREY, TFT_BLACK);
      lcd.println("--");
  }
  y += 20;

  // 狀態訊息
  lcd.setCursor(10, y);
  switch (otaState) {
    case OTA_DOWNLOADING:
      lcd.setTextColor(TFT_YELLOW, TFT_BLACK);
      lcd.printf("下載中... %d%%", otaProgress);
      // 進度條
      lcd.drawRect(10, y + 22, 300, 12, TFT_WHITE);
      lcd.fillRect(12, y + 24, (otaProgress * 296) / 100, 8, TFT_GREEN);
      y += 40;
      break;
    case OTA_SUCCESS:
      lcd.setTextColor(TFT_GREEN, TFT_BLACK);
      lcd.println("升級成功！重新啟動中...");
      y += 22;
      break;
    case OTA_ERROR:
      lcd.setTextColor(TFT_RED, TFT_BLACK);
      lcd.println("X 升級失敗，請重試");
      y += 22;
      break;
    default:
      y += 22;
      break;
  }

  // 升級按鈕 — 110x35, (105, 165)
  lcd.drawRect(105, 165, 110, 35, TFT_WHITE);
  lcd.setTextColor(TFT_WHITE, TFT_BLACK);
  lcd.setCursor(125, 174);
  if (otaState == OTA_AVAILABLE) {
    lcd.setTextColor(TFT_GREEN, TFT_BLACK);
    lcd.println("立即升級");
  } else if (otaState == OTA_DOWNLOADING) {
    lcd.setTextColor(TFT_YELLOW, TFT_BLACK);
    lcd.println("下載中...");
  } else if (otaState == OTA_UPTODATE) {
    lcd.setTextColor(TFT_DARKGREY, TFT_BLACK);
    lcd.println("已是最新");
  } else {
    lcd.setTextColor(TFT_DARKGREY, TFT_BLACK);
    lcd.println("暫不可用");
  }

  // 返回提示
  lcd.setTextColor(TFT_DARKGREY, TFT_BLACK);
  lcd.setCursor(60, 215);
  lcd.println("撳其他地方退出");
}

// =====================================================
// 退出 OTA page，回到主畫面
// =====================================================
void exitOTAPage() {
  otaState = OTA_IDLE;
  otaLatestVersion = "";
  otaBinUrl = "";
  otaProgress = 0;
  // 重畫主畫面 (OTA page 之前 fillScreen(TFT_BLACK) 過，唔重畫會空白)
  currentPage = 0;
  lastPageSwitch = millis();
  lastClockUpdate = 0;  // 強制時間立即更新
  lastMarqueeUpdate = 0;  // 強制 marquee 立即更新
  displayCurrentPage();
}

// =====================================================
// OTA page 嘅 GPIO 0 button handler
//   短撳 (< 1.5秒): 升級 (如有更新) / 退出 (否則)
//   長撳 (>= 1.5秒): 強制退出
// =====================================================
void handleOTAPageButton() {
  if (digitalRead(0) == LOW) {
    unsigned long start = millis();
    int count = 0;
    while (digitalRead(0) == LOW && count < 50) { delay(100); count++; }
    unsigned long held = millis() - start;

    if (held < 1500) {
      // 短撳
      if (otaState == OTA_AVAILABLE) {
        Serial.println("【OTA】GPIO 0 短撳觸發升級");
        performOTA(otaBinUrl);
        drawOTAPage();
      } else {
        Serial.println("【OTA】GPIO 0 短撳退出 OTA page");
        exitOTAPage();
      }
    } else {
      // 長撳
      Serial.println("【OTA】GPIO 0 長撳強制退出 OTA page");
      exitOTAPage();
    }
  }
}

// =====================================================
// OTA page 嘅 touch handler
//   撳任何位置 → 升級 (如有更新) / 退出 (否則)
//   (因為 OTA page 已經係 modal 狀態，single tap 處理最簡單直接，
//    避免 touch 座標 calibration 偏差撳唔到掣嘅問題)
// =====================================================
void handleOTAPageTouch() {
  static bool wasPressed = false;
  static unsigned long lastTapMs = 0;
  const unsigned long TAP_DEBOUNCE = 300;

  bool pressed = touchIsPressed();
  if (pressed && !wasPressed) {
    wasPressed = true;
  } else if (!pressed && wasPressed) {
    wasPressed = false;
    if (millis() - lastTapMs > TAP_DEBOUNCE) {
      lastTapMs = millis();
      // 只要確認有 touch event 就 trigger (唔理位置，避免 calibration 問題)
      if (otaState == OTA_AVAILABLE) {
        Serial.println("【OTA】Touch 觸發升級");
        performOTA(otaBinUrl);
        drawOTAPage();
      } else {
        Serial.println("【OTA】Touch 退出 OTA page");
        exitOTAPage();
      }
    }
  }
}

// =====================================================
// GPIO 0 button state machine (主畫面時用, v1.0.7)
//
//   撳一下 (< 3s, 放開)              → 無視
//   撳兩下 (3s 內撳 2 下)             → enterConfigMode(true)
//   撳住 3-20s (放開)                → 觸發 OTA check
//   撳住 > 20s (唔放, 死撳)          → reset WiFi + reboot
//
// ⚠️ 短撳 toggle flip clock 已移除
// ⚠️ 中間撳 3-20s 唔會清 WiFi — 真係要清要死撳 20 秒以上
//    (防止唔小心撳耐咗, 唔見晒 WiFi 設定要重新配)
// =====================================================
void pollGPIO0Button() {
  bool pressed = (digitalRead(0) == LOW);

  switch (btnState) {
    case BTN_IDLE: {
      if (pressed) {
        delay(BTN_DEBOUNCE_MS);
        if (digitalRead(0) == LOW) {
          btnPressStart = millis();
          btnState = BTN_WAIT_RELEASE;
        }
      }
      break;
    }

    case BTN_WAIT_RELEASE: {
      if (!pressed) {
        unsigned long held = millis() - btnPressStart;
        btnReleaseTime = millis();

        if (held < DOUBLE_TAP_MS) {
          // 短撳 → 等 double-tap window
          btnState = BTN_WAIT_DOUBLE_TAP;
        } else {
          // 撳住 3-20s (放開) → OTA check
          Serial.println("【OTA】GPIO 0 長撳 3-20 秒 (放開), 觸發 OTA check");
          triggerOTACheck();
          btnState = BTN_IDLE;
        }
      } else if (millis() - btnPressStart > BTN_HOLD_TIMEOUT) {
        // safety: 撳住 > 20s 仲未放 → reset WiFi + reboot
        Serial.println("【Reset】GPIO 0 撳住 > 20 秒 (唔放), reset WiFi + reboot");
        WiFiManager wm;
        wm.resetSettings();
        ESP.restart();
      }
      break;
    }

    case BTN_WAIT_DOUBLE_TAP: {
      if (pressed) {
        delay(BTN_DEBOUNCE_MS);
        if (digitalRead(0) == LOW) {
          // confirmed double-tap!
          Serial.println("【設定】GPIO 0 雙撳, 入設定模式");
          enterConfigMode(true);
          while (digitalRead(0) == LOW) delay(10);  // wait for release
          btnState = BTN_IDLE;
        }
      } else if (millis() - btnReleaseTime > DOUBLE_TAP_MS) {
        // double-tap window expired (只撳咗一下) → 原本係短撳, 而家當 double-tap 失敗
        // v1.0.6 移除 flip clock, 呢個 case 變成 no-op
        Serial.println("【Info】GPIO 0 單撳, 已 ignore (v1.0.6 移除 flip clock 切換)");
        btnState = BTN_IDLE;
      }
      break;
    }
  }
}

// =====================================================
// Touch tap detection — v1.0.6 已停用 (flip clock 切換搬走)
//
// ⚠️ 保留呢個 function 但內部變 no-op, 避免 loop() 改動
//    如果日後想加 tap 功能 (例如撳兩下 LCD 入設定), 改呢度就得
// =====================================================
void checkTouchTap() {
  static bool wasPressed = false;
  static unsigned long pressStartMs = 0;
  static unsigned long lastTapMs = 0;
  const unsigned long TAP_DEBOUNCE = 300;
  const unsigned long LONG_PRESS_MS = 3000;  // ✅ v1.0.8: hold 3 秒 toggle flip clock

  bool pressed = touchIsPressed();

  if (pressed && !wasPressed) {
    // Press down edge — 記住開始時間
    wasPressed = true;
    pressStartMs = millis();
  } else if (!pressed && wasPressed) {
    // Release edge
    wasPressed = false;
    unsigned long held = millis() - pressStartMs;

    // ✅ 長撳 3 秒先 → toggle flip clock (入 / 出)
    if (held >= LONG_PRESS_MS) {
      Serial.printf("【Touch】長撳 %lu ms → toggle flip clock\n", held);
      toggleFlipClock();
      return;
    }

    // 短撳 (< 3 秒) — 先過 debounce
    if (millis() - lastTapMs <= TAP_DEBOUNCE) return;
    lastTapMs = millis();

    // ✅ Flip clock mode → 短撳唔做嘢 (避免誤撳展開 row)
    if (flipClockMode) {
      Serial.println("【Touch】flip clock mode, short tap ignored");
      return;
    }

    // ✅ Expanded route → 撳「← 返回」button (x=4-60, y=0-36) 退出
    if (expandedRouteIdx >= 0) {
      int tx, ty;
      if (touchGetPoint(&tx, &ty)) {
        if (tx < 60 && ty < 36) {
          Serial.println("【Touch】退出 expanded route");
          expandedRouteIdx = -1;
          expandedStopIdx = -1;
          displayCurrentPage();
        }
        // 其他位置: 唔做嘢 (避免誤撳退出)
      }
      return;
    }

    // ✅ Main page: 撳 row → expanded that route
    int tx, ty;
    if (touchGetPoint(&tx, &ty)) {
      // Row y range: 30-210 (5 rows × 36px)
      if (ty >= 30 && ty < 210) {
        int rowIdx = (ty - 30) / 36;  // 0-4
        int stopIdx, groupStart, groupCount;
        getPageInfo(currentPage, stopIdx, groupStart, groupCount);
        int targetGroup = groupStart + rowIdx;
        if (targetGroup < groupStart + groupCount && targetGroup < stops[stopIdx].totalGroups) {
          Serial.printf("【Touch】expanded route #%d (%s 往 %s)\n",
                        targetGroup, stops[stopIdx].groups[targetGroup].route.c_str(),
                        stops[stopIdx].groups[targetGroup].dest.c_str());
          expandedRouteIdx = targetGroup;
          expandedStopIdx = stopIdx;
          displayCurrentPage();
        }
      }
    }
  }
}

// =====================================================
// 觸發 OTA check + 顯示 OTA page (由 GPIO 0 長撳 20秒 觸發)
// =====================================================
void triggerOTACheck() {
  Serial.println("【OTA】GPIO 0 長撳 20 秒, 開始 check GitHub release");
  otaState = OTA_CHECKING;
  drawOTAPage();
  checkLatestRelease();
  drawOTAPage();
}

// =====================================================
// Setup
// =====================================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  pinMode(0, INPUT_PULLUP);

  pinMode(27, OUTPUT); digitalWrite(27, HIGH);
  pinMode(22, OUTPUT); digitalWrite(22, HIGH);
  pinMode(21, OUTPUT); digitalWrite(21, HIGH);

  lcd.init();
  lcd.setRotation(1);
  lcd.fillScreen(TFT_BLACK);

  // ✅ Touch init (software bit-bang SPI，唔靠 LovyanGFX touch API)
  touchInit();
  touchWakeUp();  // 多 read 喚醒 XPT2046

  lcd.setFont(&fonts::efontTW_16);
  lcd.setTextSize(1);

  lcd.setTextColor(TFT_YELLOW, TFT_BLACK);
  lcd.setCursor(10, 10);
  lcd.println("KMB System Init...");

  loadStopIds();

  lcd.fillScreen(TFT_BLACK);
  lcd.setTextColor(TFT_WHITE, TFT_BLACK);
  lcd.setCursor(10, 10);
  lcd.println("Connecting Wi-Fi...");

  // ✅ 用共用嘅 enterConfigMode() 處理首次 WiFi 連線 (非阻塞 = autoConnect)
  enterConfigMode(false);

  // ✅ 攞天氣
  fetchWeather();
  lastWeatherUpdate = millis();

  lcd.fillScreen(TFT_BLACK);
  lcd.setTextColor(TFT_GREEN, TFT_BLACK);
  lcd.setCursor(10, 10);
  lcd.println("WiFi Connected!");

  configTime(8 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  delay(1000);

  if (stopCount > 0) {
    updateBusData();
  } else {
    lcd.fillScreen(TFT_BLACK);
    lcd.setCursor(10, 60);
    lcd.setTextColor(TFT_ORANGE, TFT_BLACK);
    lcd.println("請到 WiFi Manager 設定巴士站 ID");
    lcd.setCursor(10, 90);
    lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    lcd.println("短撳 GPIO 0 入設定模式");
  }

  lastNetworkUpdate = millis();
  lastPageSwitch = millis();
  lastClockUpdate = millis();
  lastMarqueeUpdate = millis();
}

// =====================================================
// 7-Segment LED-style jump clock (24h HH:MM, black bg, white segments)
// =====================================================

// 7-segment bitmask for digits 0-9 (bit 0=a, 1=b, 2=c, 3=d, 4=e, 5=f, 6=g)
const uint8_t SEG7_MAP[10] = {
  0x3F,  // 0
  0x06,  // 1
  0x5B,  // 2
  0x4F,  // 3
  0x66,  // 4
  0x6D,  // 5
  0x7D,  // 6
  0x07,  // 7
  0x7F,  // 8
  0x6F   // 9
};

// Segment dimensions (within a 70x200 cell)
const int SEG_H_LEN = 50;   // horizontal segment length
const int SEG_V_LEN = 82;   // vertical segment length (h/2 - t - gap = 100-12-3 = 85 max)
const int SEG_T      = 12;   // segment thickness
const int SEG_R      = 2;    // segment end corner radius
const int SEG_GAP    = 3;    // gap between segments

// Draw a single 7-segment digit (0-9) at (cellX, cellY)
void drawFlipDigit(int cellX, int cellY, char digit) {
  // Clear cell first
  lcd.fillRect(cellX, cellY, FC_CARD_W, FC_CARD_H, FC_BG_COLOR);

  if (digit < '0' || digit > '9') return;
  uint8_t s = SEG7_MAP[digit - '0'];
  uint16_t c = FC_DIGIT_COLOR;

  // Compute key positions
  int cx    = cellX + FC_CARD_W / 2;
  int yTop  = cellY + SEG_GAP;
  int yMid  = cellY + FC_CARD_H / 2;
  int yBot  = cellY + FC_CARD_H - SEG_T - SEG_GAP;
  int xL    = cellX + SEG_GAP;
  int xR    = cellX + FC_CARD_W - SEG_T - SEG_GAP;

  // a: top horizontal
  if (s & 0x01) lcd.fillRoundRect(cx - SEG_H_LEN/2, yTop, SEG_H_LEN, SEG_T, SEG_R, c);
  // f: top-left vertical
  if (s & 0x20) lcd.fillRoundRect(xL, yTop + SEG_T/2, SEG_T, SEG_V_LEN, SEG_R, c);
  // b: top-right vertical
  if (s & 0x02) lcd.fillRoundRect(xR, yTop + SEG_T/2, SEG_T, SEG_V_LEN, SEG_R, c);
  // g: middle horizontal
  if (s & 0x40) lcd.fillRoundRect(cx - SEG_H_LEN/2, yMid - SEG_T/2, SEG_H_LEN, SEG_T, SEG_R, c);
  // e: bottom-left vertical
  if (s & 0x10) lcd.fillRoundRect(xL, yMid, SEG_T, SEG_V_LEN, SEG_R, c);
  // c: bottom-right vertical
  if (s & 0x04) lcd.fillRoundRect(xR, yMid, SEG_T, SEG_V_LEN, SEG_R, c);
  // d: bottom horizontal
  if (s & 0x08) lcd.fillRoundRect(cx - SEG_H_LEN/2, yBot, SEG_H_LEN, SEG_T, SEG_R, c);
}

// Draw the colon (two filled circles)
void drawFlipColon() {
  lcd.fillCircle(FC_COLON_X, FC_CARD_Y + 90, 8, FC_DIGIT_COLOR);
  lcd.fillCircle(FC_COLON_X, FC_CARD_Y + 150, 8, FC_DIGIT_COLOR);
}

// Full repaint (called when entering flip clock mode)
void drawFlipClock(bool fullRedraw) {
  if (fullRedraw) {
    lcd.fillScreen(FC_BG_COLOR);

    // 4 digits + colon
    drawFlipDigit(FC_LEFT_X, FC_CARD_Y, '0');
    drawFlipDigit(FC_H1_X,   FC_CARD_Y, '0');
    drawFlipColon();
    drawFlipDigit(FC_M0_X,   FC_CARD_Y, '0');
    drawFlipDigit(FC_M1_X,   FC_CARD_Y, '0');
  }
}

// 1Hz update — only redraws digits that changed
void updateFlipClock() {
  String t = getSystemTime();  // "HH:MM" 24h
  if (t == "--:--") return;    // NTP not yet synced
  if (t.length() < 5) return;

  char h0 = t[0];
  char h1 = t[1];
  char m0 = t[3];
  char m1 = t[4];

  if (h0 != fcPrevH0) { drawFlipDigit(FC_LEFT_X, FC_CARD_Y, h0); fcPrevH0 = h0; }
  if (h1 != fcPrevH1) { drawFlipDigit(FC_H1_X,   FC_CARD_Y, h1); fcPrevH1 = h1; }
  if (m0 != fcPrevM0) { drawFlipDigit(FC_M0_X,   FC_CARD_Y, m0); fcPrevM0 = m0; }
  if (m1 != fcPrevM1) { drawFlipDigit(FC_M1_X,   FC_CARD_Y, m1); fcPrevM1 = m1; }
}

// Toggle between bus data display and flip clock display.
void toggleFlipClock() {
  flipClockMode = !flipClockMode;
  if (flipClockMode) {
    Serial.println("【Clock】進入 jump clock mode");
    fcPrevH0 = fcPrevH1 = fcPrevM0 = fcPrevM1 = ' ';  // force re-paint
    drawFlipClock(true);
    lastFlipClockUpdate = millis();
  } else {
    Serial.println("【Clock】返回 bus data");
    displayCurrentPage();
  }
}

// =====================================================
// Main loop
// =====================================================
void loop() {
  // ==========================================
  // OTA page 進行中 → 用 GPIO 0 button 控制，唔做其他嘢
  // ==========================================
  if (otaState != OTA_IDLE) {
    if (otaState == OTA_DOWNLOADING || otaState == OTA_SUCCESS) {
      delay(100);
      return;
    }
    handleOTAPageButton();  // 等 GPIO 0 button 短/長撳
    handleOTAPageTouch();   // 等 touch tap (撳升級掣 / 退出)
    delay(50);
    return;
  }

  // ==========================================
  // GPIO 0 button state machine (主畫面時):
  //   單撳 → toggle flip clock
  //   雙撳 (3s 內撳 2 下) → 入設定模式
  //   長撳 2-3s → reset WiFi
  //   長撳 3+秒 → OTA check
  // ==========================================
  pollGPIO0Button();

  // ==========================================
  // Touch tap (v1.0.8):
  //   短撳 (< 3 秒):
  //     - expanded route → tap「← 返回」button 退出
  //     - main page row tap → expanded that route
  //   長撳 (≥ 3 秒): toggle flip clock (入 / 出)
  // ==========================================
  checkTouchTap();

  // ==========================================
  // Flip clock mode → 1Hz 更新時間，跳過 bus/weather loop
  // ==========================================
  if (flipClockMode) {
    if (millis() - lastFlipClockUpdate >= 1000) {
      updateFlipClock();
      lastFlipClockUpdate = millis();
    }
    delay(50);
    return;
  }

  if (stopCount == 0) {
    return;
  }

  unsigned long currentMillis = millis();

  int totalPages = calculateTotalPages();
  if (totalPages == 0) totalPages = 1;

  // ✅ v1.0.8: 新 design 冇跑馬燈, 唔需要 marquee update loop
  //           (留低 lastMarqueeUpdate 純粹為咗向後兼容其他可能引用嘅 code)

  // ✅ 定期更新天氣 (15 分鐘)
  if (currentMillis - lastWeatherUpdate >= weatherInterval) {
    Serial.println("【天氣】定期更新中...");
    fetchWeather();
    lastWeatherUpdate = currentMillis;
  }

  // ✅ v1.0.8: 時間更新 (每 10 秒) - 新 layout header bar (y=0-28) 入面
  //   Header bar 已經 fillRect 過, 直接 redraw header 入面嘅時間部份就得
  if (currentMillis - lastClockUpdate >= 10000) {
    if (expandedRouteIdx < 0) {
      // 只喺 main page 重畫 header 入面嘅時間
      lcd.fillRect(240, 0, 80, 28, TFT_NAVY);  // 清舊時間殘影 (dark blue 底色)
      lcd.setFont(&fonts::efontTW_16);
      lcd.setTextColor(TFT_WHITE, TFT_NAVY);
      lcd.setCursor(245, 7);
      lcd.print(getSystemTime());
    }
    lastClockUpdate = currentMillis;
  }

  // 自動翻頁 (多過一頁先翻)
  if (totalPages > 1 && currentMillis - lastPageSwitch >= pageInterval) {
    currentPage = (currentPage + 1) % totalPages;
    displayCurrentPage();
    lastPageSwitch = currentMillis;
  }

  // 網路更新
  if (currentMillis - lastNetworkUpdate >= networkInterval) {
    updateBusData();
    lastNetworkUpdate = currentMillis;
  }
}