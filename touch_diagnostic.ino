// =====================================================
// ESP32 CYD Touch Diagnostic v3 — 超全面 pin scan
// =====================================================
// 測試 bit-bang + HSPI，多 pin 組合，多 command，多 I2C address

#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>

struct Result {
  bool xpt2046Found;
  int xpt2046PinIdx;
  int xpt2046Mode;
  int xpt2046Method;  // 0=bitbang, 1=HSPI
  uint8_t xpt2046Cmd;
  uint16_t xpt2046Value;

  bool ft6336Found;
  int ft6336PinIdx;
  uint8_t ft6336Addr;
  uint8_t ft6336Status;

  bool gt911Found;
  int gt911PinIdx;
};
Result res;
void initResult() {
  res.xpt2046Found = false; res.xpt2046PinIdx = -1; res.xpt2046Mode = -1;
  res.xpt2046Method = -1; res.xpt2046Cmd = 0; res.xpt2046Value = 0;
  res.ft6336Found = false; res.ft6336PinIdx = -1;
  res.ft6336Addr = 0; res.ft6336Status = 0;
  res.gt911Found = false; res.gt911PinIdx = -1;
}

// =====================================================
// 大量 pin 組合 — 涵蓋 Sunton / Elecrow / 淘寶 clone
// =====================================================
struct PinCombo {
  const char* name;
  int clk, mosi, miso, cs;
};
const PinCombo SPI_COMBOS[] = {
  // Sunton v1
  {"S1: CLK=26 MOSI=32 MISO=39 CS=33",   26, 32, 39, 33},
  {"S2: CLK=25 MOSI=33 MISO=39 CS=26",   25, 33, 39, 26},
  // CYD v2 / v3
  {"S3: CLK=27 MOSI=32 MISO=39 CS=33",   27, 32, 39, 33},
  {"S4: CLK=22 MOSI=21 MISO=35 CS=33",   22, 21, 35, 33},
  // 共享 SPI bus
  {"S5: CLK=14 MOSI=13 MISO=12 CS=33",   14, 13, 12, 33},
  {"S6: CLK=14 MOSI=13 MISO=12 CS=2",    14, 13, 12,  2},
  {"S7: CLK=14 MOSI=13 MISO=12 CS=15",   14, 13, 12, 15},
  // Misc variants
  {"S8: CLK=18 MOSI=23 MISO=39 CS=5",    18, 23, 39,  5},
  {"S9: CLK=2  MOSI=15 MISO=13 CS=33",    2, 15, 13, 33},
  {"S10: CLK=0 MOSI=2 MISO=15 CS=33",     0,  2, 15, 33},
  {"S11: CLK=15 MOSI=13 MISO=12 CS=33",  15, 13, 12, 33},
  {"S12: CLK=14 MOSI=2 MISO=12 CS=33",   14,  2, 12, 33},
  // S13: 確診 working pinout — 底版 silkscreen 寫 TP CLK=IO25, CS=IO33, DIN=IO32, OUT=IO39
  // 呢個 combo 喺 touch_test.ino 上 confirm work (2026-09-13)
  {"S13: CLK=25 MOSI=32 MISO=39 CS=33 [CONFIRMED]",   25, 32, 39, 33},
};
const int NUM_SPI = 13;

struct I2CCombo {
  const char* name;
  int sda, scl;
  uint8_t addr;
};
const I2CCombo I2C_COMBOS[] = {
  {"FT SDA=21 SCL=22 @0x38",  21, 22, 0x38},
  {"FT SDA=27 SCL=22 @0x38",  27, 22, 0x38},
  {"FT SDA=33 SCL=32 @0x38",  33, 32, 0x38},
  {"FT SDA=21 SCL=27 @0x38",  21, 27, 0x38},
  {"FT SDA=22 SCL=21 @0x38",  22, 21, 0x38},
  {"GT SDA=21 SCL=22 @0x5D",  21, 22, 0x5D},
  {"GT SDA=21 SCL=22 @0x14",  21, 22, 0x14},
  {"FT SDA=27 SCL=14 @0x38",  27, 14, 0x38},
  {"FT SDA=33 SCL=27 @0x38",  33, 27, 0x38},
};
const int NUM_I2C = 9;

// =====================================================
// Bit-bang SPI (slow but reliable timing)
// =====================================================
uint16_t bbRead(int clk, int mosi, int miso, int cs, uint8_t cmd) {
  pinMode(clk, OUTPUT); pinMode(mosi, OUTPUT);
  pinMode(miso, INPUT); pinMode(cs, OUTPUT);
  digitalWrite(cs, HIGH); digitalWrite(clk, LOW);

  digitalWrite(cs, LOW);
  for (int i = 7; i >= 0; i--) {
    digitalWrite(mosi, (cmd >> i) & 1);
    digitalWrite(clk, HIGH); delayMicroseconds(10);
    digitalWrite(clk, LOW); delayMicroseconds(10);
  }
  uint16_t result = 0;
  for (int i = 11; i >= 0; i--) {
    digitalWrite(clk, HIGH); delayMicroseconds(10);
    if (digitalRead(miso)) result |= (1 << i);
    digitalWrite(clk, LOW); delayMicroseconds(10);
  }
  digitalWrite(clk, HIGH); delayMicroseconds(10);
  digitalWrite(clk, LOW);
  digitalWrite(cs, HIGH);
  return result;
}

bool testBitBang(int clk, int mosi, int miso, int cs) {
  const uint8_t commands[] = {0x90, 0xD0, 0xB0, 0xC0, 0xF0, 0xE0, 0xA0};
  const char* cmdNames[] = {"X", "Y", "Z1", "Z2", "Y8", "X8", "Xs"};

  for (int c = 0; c < 7; c++) {
    uint16_t maxVal = 0;
    for (int i = 0; i < 5; i++) {
      uint16_t v = bbRead(clk, mosi, miso, cs, commands[c]);
      if (v > maxVal) maxVal = v;
      delay(2);
    }
    if (maxVal > 50 && maxVal != 4095) {  // 排除 false positive 4095
      Serial.printf("  ✓ BB cmd %s (0x%02X) = %d\n", cmdNames[c], commands[c], maxVal);
      return true;
    }
  }
  return false;
}

// =====================================================
// Hardware SPI
// =====================================================
uint16_t hspiRead(SPIClass& spi, int cs, uint8_t cmd, uint8_t mode) {
  digitalWrite(cs, LOW);
  spi.beginTransaction(SPISettings(500000, MSBFIRST, mode == 0 ? SPI_MODE0 : SPI_MODE3));
  spi.transfer(cmd);
  uint16_t result = spi.transfer16(0x0000);
  spi.endTransaction();
  digitalWrite(cs, HIGH);
  return result >> 4;
}

bool testHSPI(int clk, int mosi, int miso, int cs) {
  SPIClass spi(HSPI);
  spi.begin(clk, miso, mosi, cs);

  const uint8_t commands[] = {0x90, 0xD0, 0xB0, 0xC0, 0xF0, 0xE0};
  const char* cmdNames[] = {"X", "Y", "Z1", "Z2", "Y8", "X8"};

  for (int mode = 0; mode < 2; mode++) {
    for (int c = 0; c < 6; c++) {
      uint16_t maxVal = 0;
      for (int i = 0; i < 5; i++) {
        uint16_t v = hspiRead(spi, cs, commands[c], mode);
        if (v > maxVal) maxVal = v;
        delay(2);
      }
      if (maxVal > 50 && maxVal != 4095) {
        Serial.printf("  ✓ HSPI mode %d cmd %s (0x%02X) = %d\n", mode, cmdNames[c], commands[c], maxVal);
        spi.end();
        return true;
      }
    }
  }
  spi.end();
  return false;
}

// =====================================================
// I2C scan
// =====================================================
bool testI2C(int sda, int scl, uint8_t addr) {
  Wire.begin(sda, scl);
  Wire.beginTransmission(addr);
  uint8_t err = Wire.endTransmission();
  if (err != 0) { Wire.end(); return false; }

  Wire.beginTransmission(addr);
  Wire.write(0x02);
  Wire.endTransmission();
  Wire.requestFrom((int)addr, 1);
  if (Wire.available()) {
    uint8_t td = Wire.read();
    if (td <= 5) {
      Serial.printf("  ✓ FT6x36 @ 0x%02X SDA=%d SCL=%d, TD_STATUS=%d\n", addr, sda, scl, td);
      Wire.end();
      return true;
    }
  }
  Wire.end();
  return false;
}

// =====================================================
// Main
// =====================================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n========================================");
  Serial.println("【TOUCH DIAG v3】超全面 scan");
  Serial.println("  ⚠️ 撳住畫面唔放 (~60 秒)");
  Serial.println("========================================");

  initResult();

  Serial.println("\n[XPT2046 - bit-bang SPI] 12 組 pin × 7 commands...");
  for (int i = 0; i < NUM_SPI; i++) {
    auto& p = SPI_COMBOS[i];
    if (testBitBang(p.clk, p.mosi, p.miso, p.cs)) {
      Serial.printf("  🎉 XPT2046 (bit-bang) 喺 %s work!\n", p.name);
      if (!res.xpt2046Found) {
        res.xpt2046Found = true; res.xpt2046PinIdx = i;
        res.xpt2046Method = 0; res.xpt2046Mode = 0;
      }
    }
  }

  Serial.println("\n[XPT2046 - hardware SPI (HSPI)] 12 組 pin × 2 mode × 6 commands...");
  for (int i = 0; i < NUM_SPI; i++) {
    auto& p = SPI_COMBOS[i];
    if (testHSPI(p.clk, p.mosi, p.miso, p.cs)) {
      Serial.printf("  🎉 XPT2046 (HSPI) 喺 %s work!\n", p.name);
      if (!res.xpt2046Found) {
        res.xpt2046Found = true; res.xpt2046PinIdx = i;
        res.xpt2046Method = 1; res.xpt2046Mode = 0;
      }
    }
  }

  Serial.println("\n[I2C] 9 組 pin + address 組合...");
  for (int i = 0; i < NUM_I2C; i++) {
    auto& c = I2C_COMBOS[i];
    if (testI2C(c.sda, c.scl, c.addr)) {
      if (!res.ft6336Found) {
        res.ft6336Found = true; res.ft6336PinIdx = i;
        res.ft6336Addr = c.addr;
      }
    }
  }

  Serial.println("\n========================================");
  Serial.println("【TOUCH DIAG v3】FINAL SUMMARY");
  Serial.println("========================================");
  Serial.flush();
  if (res.xpt2046Found) {
    auto& p = SPI_COMBOS[res.xpt2046PinIdx];
    const char* method = res.xpt2046Method == 0 ? "bit-bang" : "HSPI";
    Serial.printf("✅ XPT2046: %s, Pin組 %d %s, value=%d\n",
                  method, res.xpt2046PinIdx, p.name, res.xpt2046Value);
    Serial.flush();
  } else {
    Serial.println("❌ XPT2046: 12 pin × bitbang/HSPI × 多 commands 全部失敗");
    Serial.flush();
  }
  if (res.ft6336Found) {
    auto& c = I2C_COMBOS[res.ft6336PinIdx];
    Serial.printf("✅ FT6336: SDA=%d SCL=%d @0x%02X\n", c.sda, c.scl, res.ft6336Addr);
    Serial.flush();
  } else {
    Serial.println("❌ FT6336/FT6206: 9 組 I2C 全部失敗");
    Serial.flush();
  }
  if (res.gt911Found) {
    auto& c = I2C_COMBOS[res.gt911PinIdx];
    Serial.printf("✅ GT911: SDA=%d SCL=%d @0x%02X\n", c.sda, c.scl, c.addr);
    Serial.flush();
  } else {
    Serial.println("❌ GT911: 冇回應");
    Serial.flush();
  }
  Serial.println("========================================");
  Serial.flush();
  Serial.println("⚠️ 如果全部 ❌, 呢塊板 touch chip 應該真係壞咗 / 唔係標準 CYD");
  Serial.println("=== DIAG DONE ===");
  Serial.flush();
}

void loop() { delay(1000); }