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

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

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
// XPT2046 Touch Driver — software bit-bang SPI
// 完全唔靠 LovyanGFX touch API，避開 SPI bus 衝突 + 版本兼容問題
// CYD pins: T_CLK=26, T_MOSI=32, T_MISO=39, T_CS=33
// =====================================================
#define T_CLK 26
#define T_MOSI 32
#define T_MISO 39
#define T_CS 33

// XPT2046 command bytes
#define XPT2046_CMD_X  0x90  // 12-bit differential X position
#define XPT2046_CMD_Y  0xD0  // 12-bit differential Y position
#define XPT2046_CMD_Z1 0xB0  // 12-bit differential Z1 (pressure for detect)

void touchInit() {
  pinMode(T_CLK, OUTPUT);
  pinMode(T_MOSI, OUTPUT);
  pinMode(T_MISO, INPUT);
  pinMode(T_CS, OUTPUT);
  digitalWrite(T_CS, HIGH);
  digitalWrite(T_CLK, LOW);
  digitalWrite(T_MOSI, LOW);
  Serial.println("【Touch】XPT2046 init done (software bit-bang SPI)");
}

uint16_t touchReadRaw(uint8_t cmd) {
  digitalWrite(T_CS, LOW);

  // Send 8-bit command (MSB first)
  for (int i = 7; i >= 0; i--) {
    digitalWrite(T_MOSI, (cmd >> i) & 1);
    digitalWrite(T_CLK, HIGH);
    delayMicroseconds(2);
    digitalWrite(T_CLK, LOW);
    delayMicroseconds(2);
  }

  // XPT2046 喺第 8 個 clock 之後開始 drive MISO (bus turnaround)
  // 讀 12-bit result
  uint16_t result = 0;
  for (int i = 11; i >= 0; i--) {
    digitalWrite(T_CLK, HIGH);
    delayMicroseconds(2);
    if (digitalRead(T_MISO)) {
      result |= (1 << i);
    }
    digitalWrite(T_CLK, LOW);
    delayMicroseconds(2);
  }

  // 收尾 — send 2 個 clock cycle 釋放 bus
  digitalWrite(T_CLK, HIGH);
  delayMicroseconds(2);
  digitalWrite(T_CLK, LOW);
  delayMicroseconds(2);

  digitalWrite(T_CS, HIGH);
  return result;
}

bool touchIsPressed() {
  // 讀 Z1 (pressure) 兩次取平均 — 更穩定，避免 noise spike
  uint16_t z1a = touchReadRaw(XPT2046_CMD_Z1);
  uint16_t z1b = touchReadRaw(XPT2046_CMD_Z1);
  uint16_t z1 = (z1a + z1b) / 2;

  // ✅ Throttled debug log — 每 500ms 先 log 一次 (避免 spam)
  static uint16_t lastLoggedZ1 = 0;
  static unsigned long lastLogTime = 0;
  unsigned long now = millis();
  if (z1 > 30 && (now - lastLogTime > 500 || (z1 > 30 && lastLoggedZ1 <= 30))) {
    Serial.printf("【Touch】Z1=%d (撳到！)\n", z1);
    lastLoggedZ1 = z1;
    lastLogTime = now;
  }

  return z1 > 30;  // 降低 threshold (50 → 30，更敏感)
}

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
#define FIRMWARE_VERSION   "1.0.0"                 // 每次 release 之前人手改呢度 (對齊 git tag)
#define GITHUB_USER        "Anthony114hk"          // GitHub username
#define GITHUB_REPO        "mini-eta-helper"       // GitHub repo 名
#define OTA_ASSET_NAME     "kmb-eta-display.bin"   // GitHub Release 上 .bin 檔名
#define LONG_PRESS_MS      10000                   // 長按 10 秒觸發 OTA page
#define OTA_UPDATE_MAGIC   0x45555354              // "EUST" magic — 升級後寫住防止 boot loop

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
unsigned long otaTouchDown = 0;  // long-press timer

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

String fixHongKongWords(String input) {
  input.replace("邨", "村");
  return input;
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
          stops[stopIdx].stopName = fixHongKongWords(String(name));
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
// 渲染單行 (支持跑馬燈)
// =====================================================
void drawBusLine(BusGroup& group, int yPos, bool clearFirst) {
  lcd.setFont(&fonts::efontTW_16);
  lcd.setTextSize(1);
  lcd.setClipRect(10, yPos, maxDisplayWidth, 25);

  lcd.setTextColor(TFT_WHITE, TFT_BLACK);

  if (group.needMarquee) {
    int x1 = 10 - (int)group.scrollX;
    int x2 = x1 + group.textWidth + 40;

    // ✅ 只清右邊 scrollSpeed+1 像素嘅殘留 (唔再 fillRect 成個 strip → 唔再閃)
    if (clearFirst) {
      int trailW = (int)ceil(scrollSpeed) + 1;
      int clearX1 = x1 + group.textWidth;
      if (clearX1 >= 10 && clearX1 < 10 + maxDisplayWidth) {
        lcd.fillRect(clearX1, yPos, min(trailW, 10 + maxDisplayWidth - clearX1), 25, TFT_BLACK);
      }
      int clearX2 = x2 + group.textWidth;
      if (clearX2 >= 10 && clearX2 < 10 + maxDisplayWidth) {
        lcd.fillRect(clearX2, yPos, min(trailW, 10 + maxDisplayWidth - clearX2), 25, TFT_BLACK);
      }
    }

    lcd.drawString(group.fullText, x1, yPos);
    if (x2 < 10 + maxDisplayWidth) {
      lcd.drawString(group.fullText, x2, yPos);
    }
  } else {
    if (clearFirst) {
      lcd.fillRect(10, yPos, maxDisplayWidth, 25, TFT_BLACK);
    }
    lcd.drawString(group.fullText, 10, yPos);
  }

  lcd.clearClipRect();
}

// =====================================================
// 渲染當前頁
// =====================================================
void displayCurrentPage() {
  lcd.fillScreen(TFT_BLACK);

  int stopIdx, groupStart, groupCount;
  getPageInfo(currentPage, stopIdx, groupStart, groupCount);

  // ✅ 天氣跑馬燈 (y=0 至 y=18)
  if (weather.loaded) {
    lcd.setFont(&fonts::efontTW_16);
    lcd.setClipRect(0, 0, 320, 18);
    lcd.setTextColor(TFT_CYAN, TFT_BLACK);

    if (weather.needMarquee) {
      int x1 = -(int)weather.scrollX;
      lcd.drawString(weather.summary, x1, 0);
      int x2 = x1 + weather.textWidth + 40;
      lcd.drawString(weather.summary, x2, 0);
    } else {
      lcd.drawString(weather.summary, 0, 0);
    }
    lcd.clearClipRect();
  }
  lcd.drawFastHLine(0, 19, 320, TFT_DARKGREY);

  // Header (y=22 至 y=40)
  lcd.setFont(&fonts::efontTW_16);

  lcd.setTextColor(TFT_YELLOW, TFT_BLACK);
  lcd.setCursor(10, 24);
  if (stops[stopIdx].stopName != "") {
    lcd.print(stops[stopIdx].stopName + " ETA");
  } else {
    lcd.print("Stop " + String(stopIdx + 1) + " ETA");
  }

  lcd.setTextColor(TFT_GREEN, TFT_BLACK);
  lcd.setCursor(195, 24);
  lcd.print(getSystemTime());

  int totalPages = calculateTotalPages();
  if (totalPages == 0) totalPages = 1;

  if (totalPages > 1) {
    lcd.setCursor(265, 24);
    lcd.setTextColor(TFT_CYAN, TFT_BLACK);
    lcd.printf("[%d/%d]", currentPage + 1, totalPages);
  }
  lcd.drawFastHLine(0, 44, 320, TFT_BLUE);

  if (groupCount == 0) {
    lcd.setCursor(10, 60);
    lcd.setTextColor(TFT_ORANGE, TFT_BLACK);
    lcd.println("沒有實時巴士班次");
    return;
  }

  // 巴士行由 y=48 開始
  int yPosition = 48;
  for (int i = groupStart; i < groupStart + groupCount; i++) {
    stops[stopIdx].groups[i].scrollX = 0;
    drawBusLine(stops[stopIdx].groups[i], yPosition, false);
    yPosition += 35;
  }
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
              String destStr = (dest_tc != nullptr) ? fixHongKongWords(String(dest_tc)) : "";
              String timePart = String(eta).substring(11, 16);

              int matchIdx = -1;
              for (int g = 0; g < stops[s].totalGroups; g++) {
                if (stops[s].groups[g].route == routeStr && stops[s].groups[g].dest == destStr) {
                  matchIdx = g;
                  break;
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
          lcd.setFont(&fonts::efontTW_16);
          for (int i = 0; i < stops[s].totalGroups; i++) {
            String timeString = "";
            for (int t = 0; t < stops[s].groups[i].etaCount; t++) {
              timeString += stops[s].groups[i].timeParts[t] + " ";
            }
            stops[s].groups[i].fullText = stops[s].groups[i].route + " 往 " +
                                          stops[s].groups[i].dest + " " + timeString;
            stops[s].groups[i].textWidth = lcd.textWidth(stops[s].groups[i].fullText);
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

    lcd.setTextColor(TFT_YELLOW, TFT_BLACK);
    lcd.setCursor(10, 50);
    lcd.setTextSize(2);
    lcd.println("進入設定模式");

    lcd.setTextSize(1);
    lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    lcd.setCursor(10, 100);
    lcd.println("用手機/電腦開瀏覽器:");

    lcd.setTextColor(TFT_CYAN, TFT_BLACK);
    lcd.setTextSize(2);
    lcd.setCursor(10, 130);
    lcd.printf("http://%s/", ip.toString().c_str());

    lcd.setTextSize(1);
    lcd.setTextColor(TFT_GREEN, TFT_BLACK);
    lcd.setCursor(10, 180);
    lcd.println("改完按 Save → 自動重啟");

    lcd.setTextColor(TFT_DARKGREY, TFT_BLACK);
    lcd.setCursor(10, 210);
    lcd.printf("SSID:%s  RSSI:%ddBm", WiFi.SSID().c_str(), WiFi.RSSI());
    lcd.setCursor(10, 230);
    lcd.printf("Subnet: %s", WiFi.subnetMask().toString().c_str());

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
        lcd.print("⚙ CONFIG");
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
    lcd.setTextColor(TFT_YELLOW, TFT_BLACK);
    lcd.setCursor(10, 80);
    lcd.setTextSize(2);
    lcd.println("WiFi 未連線");

    lcd.setTextSize(1);
    lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    lcd.setCursor(10, 130);
    lcd.println("用手機連 WiFi:");
    lcd.setCursor(50, 155);
    lcd.setTextColor(TFT_CYAN, TFT_BLACK);
    lcd.setTextSize(2);
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

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  String url = "https://api.github.com/repos/" GITHUB_USER "/" GITHUB_REPO "/releases/latest";
  http.setTimeout(10000);
  http.addHeader("User-Agent", "ESP32-OTA-Checker");
  http.addHeader("Accept", "application/vnd.github+json");

  Serial.printf("【OTA】GET %s\n", url.c_str());
  if (!http.begin(client, url)) {
    otaState = OTA_FAILED;
    Serial.println("【OTA】http.begin() 失敗");
    return;
  }

  int code = http.GET();
  Serial.printf("【OTA】HTTP code: %d\n", code);
  if (code != HTTP_CODE_OK) {
    otaState = OTA_FAILED;
    http.end();
    return;
  }

  String body = http.getString();
  http.end();
  Serial.printf("【OTA】Got %d bytes\n", body.length());

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
      otaBinUrl = a["browser_download_url"].as<String>();
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
// =====================================================
void performOTA(String binUrl) {
  otaState = OTA_DOWNLOADING;
  otaProgress = 0;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(30000);
  Serial.printf("【OTA】下載中: %s\n", binUrl.c_str());

  if (!http.begin(client, binUrl)) {
    otaState = OTA_ERROR;
    Serial.println("【OTA】http.begin() 失敗");
    return;
  }

  int code = http.GET();
  Serial.printf("【OTA】HTTP code: %d\n", code);
  if (code != HTTP_CODE_OK) {
    otaState = OTA_ERROR;
    http.end();
    return;
  }

  int total = http.getSize();
  Serial.printf("【OTA】size=%d bytes\n", total);
  if (total <= 0 || total > 2 * 1024 * 1024) {
    Serial.println("【OTA】size 異常 (要 <= 2MB)");
    otaState = OTA_ERROR;
    http.end();
    return;
  }

  if (!Update.begin(total)) {
    Serial.printf("【OTA】Update.begin() 失敗: %s\n", Update.errorString());
    otaState = OTA_ERROR;
    http.end();
    return;
  }

  WiFiClient *stream = http.getStreamPtr();
  uint8_t buf[1024];
  int written = 0;
  int lastReportedPct = -1;

  while (written < total) {
    int toRead = min((int)sizeof(buf), total - written);
    int read = stream->readBytes(buf, toRead);
    if (read <= 0) {
      Serial.println("【OTA】read 提前 EOF");
      otaState = OTA_ERROR;
      Update.abort();
      http.end();
      return;
    }
    if (Update.write(buf, read) != read) {
      Serial.printf("【OTA】Update.write() 失敗: %s\n", Update.errorString());
      otaState = OTA_ERROR;
      Update.abort();
      http.end();
      return;
    }
    written += read;

    int pct = (written * 100) / total;
    if (pct != lastReportedPct && pct % 10 == 0) {
      lastReportedPct = pct;
      Serial.printf("【OTA】%d%% (%d/%d bytes)\n", pct, written, total);
      otaProgress = pct;
    }
  }

  if (!Update.end()) {
    Serial.printf("【OTA】Update.end() 失敗: %s\n", Update.errorString());
    otaState = OTA_ERROR;
    http.end();
    return;
  }

  http.end();
  otaProgress = 100;
  otaState = OTA_SUCCESS;
  Serial.println("【OTA】✓ flash 成功，1 秒後 reboot");
  delay(1000);
  ESP.restart();
}

// =====================================================
// Touch 座標映射: raw XPT2046 (12-bit) → LCD pixels (rotation 1 = 320x240 landscape)
// =====================================================
bool mapTouchToLCD(int rawX, int rawY, int* lcdX, int* lcdY) {
  // CYD raw range typically: X 200-3900, Y 200-3900
  // Rotation 1: width=320, height=240
  // X 軸可能要 swap (視乎 CYD 版本)
  int x = map(rawX, 200, 3900, 0, 320);
  int y = map(rawY, 200, 3900, 0, 240);
  // Clamp
  if (x < 0) x = 0; if (x > 319) x = 319;
  if (y < 0) y = 0; if (y > 239) y = 239;
  *lcdX = x;
  *lcdY = y;
  return true;
}

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
  lcd.println("🔄 OTA 線上更新");

  lcd.drawFastHLine(0, 25, 320, TFT_DARKGREY);

  // 版本資訊
  int y = 40;
  lcd.setTextColor(TFT_WHITE, TFT_BLACK);
  lcd.setCursor(10, y);
  lcd.printf("目前版本: v%s", FIRMWARE_VERSION);
  y += 22;

  lcd.setCursor(10, y);
  lcd.print("最新版本: ");
  switch (otaState) {
    case OTA_CHECKING:
      lcd.setTextColor(TFT_YELLOW, TFT_BLACK);
      lcd.println("檢查中...");
      break;
    case OTA_AVAILABLE:
      lcd.setTextColor(TFT_GREEN, TFT_BLACK);
      lcd.printf("v%s ✓ 有更新\n", otaLatestVersion.c_str());
      break;
    case OTA_UPTODATE:
      lcd.setTextColor(TFT_GREEN, TFT_BLACK);
      lcd.printf("v%s ✓ 已是最新\n", otaLatestVersion.c_str());
      break;
    case OTA_FAILED:
      lcd.setTextColor(TFT_RED, TFT_BLACK);
      lcd.println("✗ 檢查失敗");
      break;
    default:
      lcd.setTextColor(TFT_DARKGREY, TFT_BLACK);
      lcd.println("--");
  }
  y += 22;

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
      lcd.println("✓ 升級成功！重新啟動中...");
      y += 22;
      break;
    case OTA_ERROR:
      lcd.setTextColor(TFT_RED, TFT_BLACK);
      lcd.println("✗ 升級失敗，請重試");
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
// OTA page 嘅 touch handler — 撳「立即升級」trigger performOTA()
// =====================================================
void handleOTAPageTouch(int tx, int ty) {
  // 撳「立即升級」按鈕範圍
  if (otaState == OTA_AVAILABLE && tx >= 105 && tx <= 215 && ty >= 165 && ty <= 200) {
    Serial.println("【OTA】撳立即升級");
    performOTA(otaBinUrl);
    drawOTAPage();  // 重畫顯示新狀態
    return;
  }
  // 撳其他地方退出
  Serial.println("【OTA】退出 OTA page");
  exitOTAPage();
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
// Main loop
// =====================================================
void loop() {
  // ==========================================
  // OTA page 長按偵測 + touch handler (優先於其他)
  // ==========================================
  if (touchIsPressed()) {
    if (otaTouchDown == 0) {
      otaTouchDown = millis();
      Serial.println("【OTA】撳到喇，繼續長按 10 秒觸發 OTA");
    }

    // ✅ Long-press 進度 log (3s / 6s / 9s) — 等 user 知道有反應
    unsigned long held = millis() - otaTouchDown;
    static unsigned long lastProgressLog = 0;
    if (otaState == OTA_IDLE && held > 0 && held < LONG_PRESS_MS &&
        millis() - lastProgressLog > 1500) {
      int secLeft = (LONG_PRESS_MS - held) / 1000 + 1;
      Serial.printf("【OTA】長按中... 仲要多 %d 秒觸發 OTA\n", secLeft);
      lastProgressLog = millis();
    }

    // OTA page 顯示中：唔處理長按 timeout (因為下面 handleOTAPageTouch 已經 return)
    if (otaState != OTA_IDLE) {
      int rawX, rawY;
      if (touchGetPoint(&rawX, &rawY)) {
        int tx, ty;
        mapTouchToLCD(rawX, rawY, &tx, &ty);
        handleOTAPageTouch(tx, ty);
      }
      otaTouchDown = 0;  // reset
    }
    // 長按 10 秒 → 觸發 OTA page
    else if (held >= LONG_PRESS_MS) {
      Serial.printf("【OTA】✓ 長按 10 秒偵測到，入 OTA page\n");
      otaTouchDown = 0;
      otaState = OTA_CHECKING;
      drawOTAPage();              // 先畫「檢查中...」避免空屏 10s
      checkLatestRelease();        // 同步等 check 完，再畫結果
      drawOTAPage();               // 重畫最新狀態
      // 唔 return — 繼續 loop 等 user 撳掣或退出
    }
  } else {
    otaTouchDown = 0;
  }

  // OTA page 進行中 → 唔做其他嘢 (例如唔好干擾下載)
  if (otaState == OTA_DOWNLOADING || otaState == OTA_SUCCESS) {
    delay(100);
    return;
  }

  // GPIO 0 按鈕處理:
  //   短撳 (< 1 秒): 入設定模式 (WiFiManager)
  //   長撳 (>= 2 秒): reset WiFi 設定 + reboot
  if (digitalRead(0) == LOW) {
    int count = 0;
    while (digitalRead(0) == LOW && count < 20) { delay(100); count++; }
    if (count >= 20) {
      Serial.println("【Reset】GPIO 0 長撳 2 秒，reset WiFi + reboot");
      WiFiManager wm;
      wm.resetSettings();
      ESP.restart();
    } else {
      Serial.println("【設定】GPIO 0 短撳，入設定模式 (backup for touch fail)");
      enterConfigMode(true);
    }
  }

  if (stopCount == 0) {
    return;
  }

  // ✅ OTA page 顯示中 (但非 download 中) → 唔好 overwrite OTA page
  if (otaState != OTA_IDLE && otaState != OTA_DOWNLOADING && otaState != OTA_SUCCESS) {
    delay(50);
    return;
  }

  unsigned long currentMillis = millis();

  int totalPages = calculateTotalPages();
  if (totalPages == 0) totalPages = 1;

  // 跑馬燈更新
  if (currentMillis - lastMarqueeUpdate >= marqueeInterval) {
    // ✅ 天氣跑馬燈
    if (weather.loaded && weather.needMarquee) {
      weather.scrollX += scrollSpeed;
      if (weather.scrollX >= (weather.textWidth + 40)) {
        weather.scrollX = 0;
      }
      lcd.setFont(&fonts::efontTW_16);
      lcd.setClipRect(0, 0, 320, 18);
      // ✅ 唔再 fillRect 成條 strip (setTextColor 嘅 bg 已經會覆蓋 glyph 背景)
      //    只清右邊 scrollSpeed 像素闊嘅殘留尾 (glyph 向左移之後嘅尾巴)
      lcd.setTextColor(TFT_CYAN, TFT_BLACK);
      int x1 = -(int)weather.scrollX;
      int x2 = x1 + weather.textWidth + 40;
      int trailW = (int)ceil(scrollSpeed) + 1;

      // 清 copy 1 右邊嘅尾 (只清可視範圍)
      int clearX1 = x1 + weather.textWidth;
      if (clearX1 >= 0 && clearX1 < 320) {
        lcd.fillRect(clearX1, 0, min(trailW, 320 - clearX1), 18, TFT_BLACK);
      }
      // 清 copy 2 右邊嘅尾 (如果 copy 2 有進入畫面)
      int clearX2 = x2 + weather.textWidth;
      if (clearX2 >= 0 && clearX2 < 320) {
        lcd.fillRect(clearX2, 0, min(trailW, 320 - clearX2), 18, TFT_BLACK);
      }

      lcd.drawString(weather.summary, x1, 0);
      // copy 2 喺可視範圍內先畫
      if (x2 < 320) {
        lcd.drawString(weather.summary, x2, 0);
      }
      lcd.clearClipRect();
    }

    int stopIdx, groupStart, groupCount;
    getPageInfo(currentPage, stopIdx, groupStart, groupCount);

    int yPosition = 48;
    for (int i = groupStart; i < groupStart + groupCount; i++) {
      BusGroup& group = stops[stopIdx].groups[i];
      if (group.needMarquee) {
        group.scrollX += scrollSpeed;
        if (group.scrollX >= (group.textWidth + 40)) {
          group.scrollX = 0;
        }
        drawBusLine(group, yPosition, true);
      }
      yPosition += 35;
    }
    lastMarqueeUpdate = currentMillis;
  }

  // ✅ 定期更新天氣 (15 分鐘)
  if (currentMillis - lastWeatherUpdate >= weatherInterval) {
    Serial.println("【天氣】定期更新中...");
    fetchWeather();
    lastWeatherUpdate = currentMillis;
  }

  // 時間更新 (每 10 秒)
  if (currentMillis - lastClockUpdate >= 10000) {
    lcd.setFont(&fonts::efontTW_16);
    lcd.setTextColor(TFT_GREEN, TFT_BLACK);
    lcd.setCursor(195, 24);
    lcd.print(getSystemTime());
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