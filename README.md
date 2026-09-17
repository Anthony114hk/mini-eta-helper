# 🚌 ESP32 KMB Bus ETA Display

![Hardware: ESP32 CYD (240×320 ST7789 + XPT2046 touch)](https://img.shields.io/badge/hardware-ESP32_CYD-blue)
![Framework: Arduino + LovyanGFX](https://img.shields.io/badge/framework-Arduino%20%2B%20LovyanGFX-green)
![License: MIT](https://img.shields.io/badge/license-MIT-yellow)

Hong Kong KMB bus real-time arrival display for the **ESP32 CYD** (Cheap Yellow Display) — 240×320 ST7789 LCD with resistive touch. Also shows weather (HKO Open Data), warnings, and forecast.

## Features

- 🚌 **Real-time KMB ETA** for up to 5 bus stops (Hong Kong)
- 🌦️ **HKO weather** — temperature, humidity, rainfall, warnings, forecast
- 📺 **Scrolling marquee** for long weather strings
- ⏰ **Flip clock mode** — tap screen (or GPIO 0 short-press) to toggle between bus data and 7-segment LED clock
- ⚙️ **WiFi + stop config portal** (WiFiManager + WebServer) — GPIO 0 double-tap
- 🔄 **OTA updates from GitHub Releases** — GPIO 0 button long-press 3s

## Hardware

- ESP32-2432S028R (CYD v1/v2) — 240×320 ST7789 IPS, XPT2046 touch
- USB-C for power + flashing

## Pin Map

| Function | GPIO |
|---|---|
| LCD BL | 27 |
| LCD CS | 5 |
| LCD DC | 2 |
| LCD RST | 4 |
| Touch CS | 33 |
| Touch CLK | 25 |
| Touch DIN | 32 |
| Touch DO | 39 |
| Touch IRQ | 36 |

## Build & Flash

1. Install Arduino IDE + ESP32 board support
2. Install libraries:
   - `LovyanGFX`
   - `ArduinoJson` (v7)
   - `WiFiManager`
   - `Preferences` (built-in)
3. Open `kmb-eta-display.ino`, select board "ESP32 Dev Module" (or CYD variant)
4. Upload

## First Boot

1. On first boot (no WiFi config), device opens `ESP32_Smart_Clock` AP
2. Connect phone → 192.168.4.1 → enter WiFi + up to 5 KMB stop IDs
3. Save → device reboots and shows bus ETAs

Stop IDs are KMB format, e.g. `20080C0DBE40B5D2` (route+bound+stop+seq hash).

## OTA Updates

1. Bump `FIRMWARE_VERSION` in `kmb-eta-display.ino` (e.g. `1.0.6` → `1.0.7`) — the OTA check compares this string against the GitHub release `tag_name`
2. Build firmware in Arduino IDE
3. Export compiled binary: **Sketch → Export Compiled Binary** → produces `.ino.bin`
4. Rename to `kmb-eta-display.bin`
5. Create a [GitHub Release](https://github.com/Anthony114hk/mini-eta-helper/releases/new):
   - Tag: `v1.0.0`, `v1.0.1`, ... (一定要同 `FIRMWARE_VERSION` 完全一樣)
   - Attach `kmb-eta-display.bin`
   - Publish
6. On ESP32: **hold GPIO 0 button for 3+ seconds** → OTA page appears
7. Page shows current vs latest version + an "立即升級" button
8. Short-press GPIO 0 button (or tap the screen) to upgrade → device downloads, flashes, and reboots

> ⚠️ **`glyph_overlays.h` 一定要同 `kmb-eta-display.ino` 放埋一齊** (Arduino IDE 靠相對路徑 `#include`),否則編譯會 fail。
>
> ⚠️ **Tag 同 `FIRMWARE_VERSION` 必須一致** — `checkLatestRelease()` 係直接比較 `tag_name` 同 `FIRMWARE_VERSION` 兩個字串,唔會做 version parsing。

### Publishing checklist

```bash
# 1) 確認版本號兩個地方一致
grep FIRMWARE_VERSION kmb-eta-display.ino      # 例如 "1.0.7"
# 2) commit + push 源碼
git add kmb-eta-display.ino glyph_overlays.h README.md
git commit -m "v1.0.7: <改咗咩>"
git push origin main
# 3) 用 Arduino IDE Export Compiled Binary → 改名 kmb-eta-display.bin
# 4) 去 GitHub Releases 開 tag v1.0.7 + attach 個 .bin
```

### Changelog

**v1.0.15** — Fix weather marquee (threshold + bitmap width) + red bus icon
- **Bug 1 (天氣唔 scroll)**: `weather.needMarquee = (textWidth > 320)` 係舊 top-of-screen 嘅 threshold (320px)。新 footer 只有 210px 闊,大部分天氣字串 textWidth 200-300 → needMarquee = false → 永遠唔 scroll
- Fix: 改 threshold 為 210 (footer 寬度);同時改用 `textWidthWithBu()` 計 bitmap overlay 寬度 (之前用 `lcd.textWidth()` 會少算 16px/每個 bitmap char)
- **Bug 2 (巴士 icon 改紅色)**: 主頁 + expanded view 嘅 bus icon 改用 TFT_RED

**v1.0.14** — Footer redesigned: only "更新於 HH:MM" + 天氣預報跑馬燈
- 移除 footer 右邊 "撳路線睇更多" 提示 (唔再需要,row tap 行為已知)
- 加返天氣預報跑馬燈喺 footer 右邊 (x=110-318, 208px 闊,cyan)
- Footer layout: 左 6-110「更新於 23:17」+ 右 110-318「weather.summary」跑馬燈
- 加 `drawFooter()` + `drawWeatherFooter()` helpers,提取共用 footer 邏輯
- Marquee 只喺 main page 跑 (expanded view 唔郁)
- displayCurrentPage() 開始時 reset weather.scrollX = 0

**v1.0.13** — Fix calibration range (raw ADC) — Y was not inverted, range was too wide
- 用戶實測 4 角 raw mapping 數據:
  - 左上 → (-8, 242)       右上 → (147, 229)
  - 左下 → (-4, 130)       右下 → (147, 139)
- 修正: `TOUCH_Y_INVERT=1 → 0` (rawY LOW = 物理頂部,唔需要 invert)
- 修正: 加 `X_RAW_MIN=250 / X_RAW_MAX=1900` + `Y_RAW_MIN/MAX` 常數 — 之前用 [200, 3900] 範圍太闊,呢塊板 rawY 只覆蓋 [~170, ~1900],所以 over-map 令到 bottom row 撳到只去到 ty=130 唔係 240
- 改 calibration 後: top → ty≈0, bottom → ty≈240,row tap 終於對返 ✓

**v1.0.12** — Fix Y-axis inversion + remove redundant sub-header
- **Issue 1**: 撳 row N 展開咗另一條 row → CYD 板子 Y 軸方向反轉,加 `TOUCH_Y_INVERT=1` flag (預設開)。如果 X 軸都反,改 `TOUCH_X_INVERT=1`
- **Issue 2**: 刪除 expanded view 嘅 sub-header stop name (destination = stop name → 重複顯示)。新 design 只剩 route + destination 單行 header
- Header 太長自動截短 (用 `textWidthWithBu()` + UTF-8 safe 切字)
- touchGetPoint() 加 `【Touch】raw(...) → screen(...)` log 方便 debug

**v1.0.11** — Fix row tap (calibration mapping + sample on press-down)
- **Bug 修咗**: `checkTouchTap()` 之前用 RAW XPT2046 值 (200-3900) 同 screen pixel (30-210) 比較,冇 calibration mapping → row tap 永遠唔 trigger
- `touchGetPoint()` 改為回傳 **screen pixel 座標** (rotation=1 landscape 320x240),內部用 `map(rawX, 200, 3900, 0, 320)` + `map(rawY, 200, 3900, 0, 240)` 做 calibration
- **Sample 時機修咗**: 之前 release 時 call `touchGetPoint()` → 手指已離開,讀到 invalid → tap ignored。改為 press-down edge 即時 sample 座標,release 時用 cached 值
- 加 static `pressX` / `pressY` 喺 `checkTouchTap()` cache press-down 位置

**v1.0.10** — Long-press fires WHILE pressing (not on release)
- v1.0.9 嘅長撳 3 秒邏輯要 release 先觸發 → 用戶體驗差 (按住等幾秒乜都唔見到,要放開先跳)
- v1.0.10 改為按住到 3 秒 mark 即時 toggle flip clock (唔需要放開手指)
- 加 `longPressFired` static flag 防止 hold > 3 秒重覆觸發
- Release handler 會跳過已經 fire 過嘅 press (避免 double-trigger)

**v1.0.9** — Long-press 3s toggle flip clock (instead of short tap)
- `checkTouchTap()` 行為改動:
  - **短撳 (< 3 秒)**: 展開 / 退出 row (維持 v1.0.8 行為)
  - **長撳 (≥ 3 秒)**: toggle flip clock (入 / 出) — 取代舊版「撳任何位置 toggle flip clock」
- Flip clock mode 內短撳會 ignored (避免誤撳展開 row)
- 加 `LONG_PRESS_MS = 3000` constant + `pressStartMs` static tracker

**v1.0.8** — UI redesign (single ETA + tap-to-expand) on top of v1.0.7 GPIO rewrite
- **每行只顯示一個時間 (倒數分鐘)** — 之前每行擺全部班次「21:50 22:15 22:40」跑馬燈 scroll,睇唔到下一班幾時到;新 design 只顯示下一班 ETA + 顏色編碼 (RED ≤ 0 分 / ORANGE < 3 分 / WHITE < 10 分 / LIGHTGREY > 10 分)
- **Tap row 展開 full-screen route detail** — 撳任何一個 bus row (y=30-210, 36px 高) → 顯示該路線嘅全部班次 list (絕對時間 + 倒數分鐘),撳左上「← 返回」退出
- **長撳 3 秒 toggle flip clock** — 短撳 (tap) 只係展開 row / 退出 expanded;按住畫面 3 秒先跳去時鐘,再按住 3 秒返回巴士頁
- **新 layout**: 深藍 header bar (TFT_NAVY, y=0-28) 顯示 stop name / 路線總數 / 當前時間 / 頁數,5 行 bus rows × 36px (y=30-210),footer 顯示更新時間 + 提示
- 移除舊版 weather 跑馬燈 + bus line 跑馬燈 (新 design 唔需要 scroll,慳 CPU)
- 加 4 個 helper functions: `etaMinutesFromNow()` / `etaToRemainingText()` / `etaToColor()` / `drawBusIcon()` (16×12 巴士 icon)
- 啟用 `touchGetPoint()` (原本 #if 0 包住,v1.0.8 需要 row tap 座標)
- 加 `expandedRouteIdx` / `expandedStopIdx` state variables
- Firmware size: 約 1,865 KB (94%)

**v1.0.6** — Fix OTA early-EOF + Chinese glyph coverage (邨/鰂/脷 were missing)
- **OTA `read 提前 EOF` 真正原因**: `Stream::readBytes()` 會逐個 byte 呼叫 `read()`,而 `NetworkClientSecure::read()` 係「buffer 冇 data 就即刻 return -1」嘅語意。舊 code 只要 `read <= 0` 就 `Update.abort()`,一次都唔重試 → 表面睇落好似「下載斷咗」,其實係自己 abort 咗
- Fix: 對齊官方 `Update.writeStream()` 策略 — `read <= 0` 唔再即死,100ms 後重試,最多 30 秒冇新 data 才放棄;改用 raw `stream->read()` + 2KB heap buffer;完工核對 `written == Content-Length`;每次進度報告加 `heap=`,失敗 dump `connected/available/heap/shortReads`
- 加 app partition size 前置檢查 (1,961,984 bytes) — image 過大時即刻講清楚,唔會等到 `Update.begin()` 神秘 fail
- **中文字亂碼根因**: 用程式解析 `efontTW_16` 字型表,對比全量 KMB 資料 (6,741 站名 + 1,600 路線紀錄) → 站名字集 1,118 個字缺 17 個,目的地字集 454 個字缺 5 個。**實際會出現嘅只有 5 個:邨(322 條路線!)、埗、·、鰂、脷**
- 新增 `glyph_overlays.h`: 由 MingLiU_HKSCS 渲染嘅 16×16 bitmap overlay,一次補齊 5 個字 (取代舊版只補「埗」一個),並修正 bitmap 垂直對齊 (舊版冇計 u8g2 `y_offset = -2`,「深水埗」個埗會高 2px)
- `drawStringWithBu()` 改為通用字型查表;新增 `textWidthWithBu()` — 舊版跑馬燈用 `lcd.textWidth()` 量度,唔知 bitmap 存在會少算 16px → 兩份 copy 疊埋
- **排版重疊修正**: 時間每 10 秒重繪時會抹走頁數指示器 `[1/3]`;長站名會壓住時鐘 (加 185px 截字);OTA「最新版本」一行約 336px 爆出 320px 畫面 (拆兩行)
- **LCD 上移除 emoji / ✓✗⚙🔄** — `efontTW_16` 冇呢啲 glyph,只會顯示空白
- **`setTextSize(2)` 全部移除** — efontTW_16 本身係 16px 點陣字,開 2× 變 32×32,`ESP32_Smart_Clock` 會變 512px 爆畫面;`setFont()` 係唔會重置 text size 嘅
- 同路線重複開行修正 (API `dest_tc` 一變就開新行,會撐爆 40 行上限)
- 死碼清理: 移除 `fixHongKongWords()` (no-op)、`mapTouchToLCD()`、`LONG_PRESS_MS`、`OTA_UPDATE_MAGIC`、`otaTouchDown`;`runTouchPinScan()` / `touchGetPoint()` 改用 `#if` 開關包住
- 新增 `DEBUG_DRAW_TEXT` 開關: 開 1 就會 log 每次畫嘅字串 + 闊度,並自動標出邊個字冇字形
- Firmware size: 1,860,643 bytes (94%)

**v1.0.5** — Remove touch debug log
- `touchIsPressed()` 每秒 print `【Touch】Z1=0 (a=0 b=0 c=0)` 嘅 debug log 已移除 (之前診斷 touch chip 時加)

**v1.0.4** — Fix read-after-redirect EOF (v1.0.3 redirect worked but stream immediately EOF)
> ⚠️ 呢個版本嘅診斷 (「SSL state 污染」) 後來證明係**錯嘅方向** — v1.0.6 先搵到真正原因 (readBytes 短讀 + 唔重試)。redirect 處理本身冇問題,保留。
- v1.0.3 manually followed GitHub 302 → 200 successfully, but `stream->readBytes()` returned 0 immediately (read 0/1860112 bytes)
- Root cause: reusing WiFiClientSecure across redirects polluted SSL state — server may have closed the connection after sending headers
- Fix: each redirect iteration uses a **brand new** `WiFiClientSecure` + `HTTPClient` (heap-allocated, deleted on next iteration)
- Added `stream->available()` check with 200ms wait before first read, to detect dead sockets early
- Added `stream->connected()` in EOF error message for debugging

**v1.0.3** — Manual redirect follow for GitHub release (failed: stream EOF after redirect)
- ESP32 HTTPClient auto-redirect failed for GitHub release 302 (Location header lost in SSL buffer)
- `performOTA()` now manually follows up to 3 redirects: reads `Location` header on 302, closes socket, re-`begin()` + `GET()` on new URL
- Successfully followed `github.com → release-assets.githubusercontent.com` redirect chain (confirmed in Serial Monitor)
- However: stream immediately EOF'd at 0 bytes read — see v1.0.4 fix

**v1.0.2** — OTA download logging + redirect fix (failed: ESP32 HTTPClient didn't follow redirect)
- `performOTA()`: detailed Serial logging (URL / HTTP code / size / progress + timing)
- Attempted `http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS)` — but ESP32 HTTPClient couldn't extract `Location` header from GitHub release 302 response over SSL

**v1.0.1** — `埗` bitmap fix + 13M marquee overlap fix
- Custom 16×16 `埗` (U+57D7) bitmap rendered via `lcd.drawBitmap` overlay for stop names containing 深水埗 (since `efontTW_16` lacks this character)
- Switch `lcd.print` → `lcd.drawString` in `drawStringWithBu()` — eliminates text-wrap that caused 13M marquee to visually overlap two text copies
- `u8g2_gb2312a_font.h` (Simplified Chinese font) removed to fit firmware in 1.875 MB partition
- Touch chip now working (CLK=25 fix); flip-clock mode + GPIO 0 button navigation enabled

**v1.0.0** — initial release

## Touch Diagnostic

If the touch chip is suspected faulty (no response to finger taps), flash `touch_test.ino` instead of the main firmware:

1. Open `touch_test.ino` in Arduino IDE
2. Sketch → Upload
3. Open Serial Monitor @ 115200 baud
4. Touch any of the 16 zones (0–F) on the 4×4 grid
5. Tap the center zone ("5" with red "R" badge) to reset all counters

The test renders a 4×4 grid of zones. Each tap:
- Flips the zone from black to green
- Increments a per-zone counter (bottom-right corner)
- Logs the raw X/Y, Z1 pressure, and zone ID to Serial

If no touch registers within 5 seconds, a red "TOUCH NOT RESPONDING" banner appears and Serial logs the warning every 5 s. This confirms the chip is dead or the pinout is wrong.

For a deeper sweep across known chip types and pin combinations, flash `touch_diagnostic.ino` instead — it scans 13 XPT2046 pin sets × bit-bang + HSPI × 6 commands plus 9 FT6336/FT6206 + 2 GT911 I²C combos (≈177 combinations total). Hold a finger on the screen for the full ~60 s scan.

> **This CYD's touch chip is alive** — the earlier "scan-confirmed dead" diagnosis (2026-09-12) was a false negative caused by a pin mismatch: the board silkscreen shows TP CLK wired to GPIO 25, but `T_CLK` was defined as 26. None of the original S1–S12 pin combos in the diagnostic tested CLK=25 + MOSI=32 + CS=33 (the correct combo). Fixed 2026-09-13: `T_CLK=25` is now the working pinout. Diagnostic now includes S13 covering this combo for future reference.

## License

MIT