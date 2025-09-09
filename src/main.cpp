// ── FILE: src/main.cpp ───────────────────────────────────────────────────────
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <FS.h>
#include <SPIFFS.h>
#include <time.h>
#include "config.h"
#include <driver/ledc.h>
#include <AudioGeneratorMP3.h>
#include <AudioOutputI2S.h>
#include <AudioFileSourcePROGMEM.h>

// Struct lưu lịch sử chat
struct Turn {
  String role;
  String content;
};
#define MAX_TURNS 4

// LCD config
#include <Arduino_GFX_Library.h>
struct LcdCandidate { int sck, mosi, miso, dc, cs, rst, bl; const char* name; };
static LcdCandidate LCD_CANDIDATES[] = {
  {36, 35, -1,  7,  6,  5, 10, "S3-Round(1): SCK36 MOSI35 DC7 CS6 RST5 BL10"},
  {12, 11, -1,  9, 10,  8, 14, "XiaoZhi-like: SCK12 MOSI11 DC9 CS10 RST8 BL14"},
  {40, 41, -1, 38, 39, 42, 45, "S3-Dev-Alt: SCK40 MOSI41 DC38 CS39 RST42 BL45"},
  {48, 47, -1, 38, 39, 40, 45, "S3-HiPins:  SCK48 MOSI47 DC38 CS39 RST40 BL45"},
};
static const size_t LCD_CAND_COUNT = sizeof(LCD_CANDIDATES)/sizeof(LcdCandidate);
static LcdCandidate g_found = {-1,-1,-1,-1,-1,-1,-1, "(none)"};

static bool try_init_lcd(const LcdCandidate &c, Arduino_GFX* &out_gfx) {
  if (c.bl >= 0) { pinMode(c.bl, OUTPUT); digitalWrite(c.bl, LOW); }
  Arduino_DataBus *tbus = new Arduino_HWSPI(c.dc, c.cs, c.sck, c.mosi, c.miso);
  Arduino_GFX *tgfx = new Arduino_GC9A01(tbus, c.rst, 0, true, LCD_WIDTH, LCD_HEIGHT);
  if (!tgfx->begin()) {
    delete tgfx;
    return false;
  }
  out_gfx = tgfx;
  if (c.bl >= 0) {
    pinMode(c.bl, OUTPUT);
    digitalWrite(c.bl, HIGH);
  }
  return true;
}

// ── Global display objects ──
Arduino_GFX *gfx = nullptr;

// ── Time (NTP) ──
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 7 * 3600, 60 * 1000);

// ── Audio playback ──
AudioGeneratorWAV *wav = nullptr;
AudioOutputI2S *out = nullptr;
AudioFileSourceSPIFFS *file = nullptr;

// ── State ──
unsigned long lastIdleRedraw = 0;
String cachedWeather = "--°C";
String cachedCity = String(OWM_CITY);

// ── Lịch sử chat ──
static Turn history[MAX_TURNS * 2];
static int turns = 0;
static void clearHistory(){ turns = 0; }
static void pushTurn(const String& role, const String& content){
  if (turns >= MAX_TURNS*2){
    for (int i=2; i<MAX_TURNS*2; ++i) history[i-2] = history[i];
    turns -= 2;
  }
  history[turns++] = { role, content };
}

// ── Helpers ──
static void drawCenteredText(const String &s, int16_t y, uint8_t sz);
static void drawWrapped(const String &text, int16_t x, int16_t y, int16_t w, uint8_t sz, uint16_t color);
static void drawIdleScreen();
static void drawWeatherBadge(int16_t cx, int16_t cy);
static void drawChatScreen(const String &userText, const String &assistantText);

// ── Secure HTTP client ──
static WiFiClientSecure makeTLS()
{
  WiFiClientSecure client;
  client.setInsecure();
  return client;
}

// ── WiFi connect ──
static void connectWiFi()
{
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("\n[WiFi] Connecting to %s", WIFI_SSID);
  uint8_t tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 60)
  {
    delay(500);
    Serial.print(".");
    tries++;
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.printf("[WiFi] Connected, IP: %s\n", WiFi.localIP().toString().c_str());
  }
  else
  {
    Serial.println("[WiFi] Failed. Rebooting in 5s...");
    delay(5000);
    ESP.restart();
  }
}

// ── Weather fetch ──
static bool fetchWeather()
{
  if (WiFi.status() != WL_CONNECTED) return false;
  String url;
#if defined(OWM_LAT)
  url = String("https://api.openweathermap.org/data/2.5/weather?lat=") + OWM_LAT + "&lon=" + OWM_LON + "&units=metric&lang=vi&appid=" + OWM_API_KEY;
#else
  url = String("https://api.openweathermap.org/data/2.5/weather?q=") + OWM_CITY + "," + OWM_COUNTRY + "&units=metric&lang=vi&appid=" + OWM_API_KEY;
#endif
  WiFiClientSecure client = makeTLS();
  HTTPClient https;
  if (!https.begin(client, url)) return false;
  int code = https.GET();
  if (code == 200)
  {
    DynamicJsonDocument doc(2048);
    DeserializationError err = deserializeJson(doc, https.getStream());
    if (!err)
    {
      float t = doc["main"]["temp"].as<float>();
      const char* name = doc["name"] | OWM_CITY;
      cachedCity = String(name);
      cachedWeather = String((int)round(t)) + "°C";
      https.end();
      return true;
    }
  }
  https.end();
  return false;
}

// ── OpenAI Chat ──
static String normalizeLower(const String &s){ String t=s; t.toLowerCase(); t.trim(); return t; }
static bool startsWithCI(const String &s, const String &prefix){ String a=s; a.toLowerCase(); String b=prefix; b.toLowerCase(); a.trim(); b.trim(); return a.startsWith(b); }
static String callChatGPT(const String &user_text)
{
  if (WiFi.status() != WL_CONNECTED) return "(Không có WiFi)";
  const char *endpoint = "https://api.openai.com/v1/chat/completions";
  DynamicJsonDocument payload(12288);
  payload["model"] = OPENAI_CHAT_MODEL;
  JsonArray messages = payload.createNestedArray("messages");
  JsonObject sys = messages.createNestedObject();
  sys["role"] = "system";
  sys["content"] = String("Bạn là trợ lý tiếng Việt, tên là \"") + DEVICE_NAME + "\". Trả lời ngắn gọn, tự nhiên, rõ ràng.";
  for (int i = max(0, turns - MAX_TURNS*2); i < turns; ++i) {
    JsonObject m = messages.createNestedObject();
    m["role"] = history[i].role;
    m["content"] = history[i].content;
  }
  JsonObject usr = messages.createNestedObject();
  usr["role"] = "user";
  usr["content"] = user_text;
  payload["temperature"] = 0.7;
  String body; serializeJson(payload, body);
  WiFiClientSecure client = makeTLS();
  HTTPClient https;
  https.begin(client, endpoint);
  https.addHeader("Content-Type", "application/json");
  https.addHeader("Authorization", String("Bearer ") + OPENAI_API_KEY);
  int code = https.POST(body);
  if (code != 200)
  {
    String err = String("HTTP ") + code + ": " + https.getString();
    https.end();
    return err;
  }
  DynamicJsonDocument doc(12288);
  deserializeJson(doc, https.getStream());
  https.end();
  const char *reply = doc["choices"][0]["message"]["content"] | "(không có phản hồi)";
  pushTurn("user", user_text);
  pushTurn("assistant", String(reply));
  return String(reply);
}

// ── OpenAI TTS → WAV ──
static bool ttsToWav(const String &text, const char *savePath)
{
  if (WiFi.status() != WL_CONNECTED) return false;
  const char *endpoint = "https://api.openai.com/v1/audio/speech";
  DynamicJsonDocument payload(4096);
  payload["model"] = "gpt-4o-mini-tts";
  payload["voice"] = OPENAI_TTS_VOICE;
  payload["input"] = text;
  payload["format"] = "wav";
  String body; serializeJson(payload, body);
  WiFiClientSecure client = makeTLS();
  HTTPClient https;
  https.begin(client, endpoint);
  https.addHeader("Content-Type", "application/json");
  https.addHeader("Authorization", String("Bearer ") + OPENAI_API_KEY);
  int code = https.POST(body);
  if (code != 200)
  {
    Serial.printf("[TTS] HTTP %d\n", code);
    return false;
  }
  File f = SPIFFS.open(savePath, FILE_WRITE);
  if (!f)
  {
    Serial.println("[TTS] open file failed");
    https.end();
    return false;
  }
  WiFiClient *stream = https.getStreamPtr();
  uint8_t buff[1024];
  int len;
  while ((len = stream->readBytes((char*)buff, sizeof(buff))) > 0)
  {
    f.write(buff, len);
  }
  f.close();
  https.end();
  return true;
}

#if OPENAI_USE_WHISPER
// ... giữ nguyên phần WHISPER STT như file gốc ...
#endif

// ── UI ──
static void drawCenteredText(const String &s, int16_t y, uint8_t sz)
{
  if (!gfx) return;
  gfx->setTextSize(sz);
  int16_t x1, y1; uint16_t w, h;
  gfx->getTextBounds((char*)s.c_str(), 0, y, &x1, &y1, &w, &h);
  int16_t x = (LCD_WIDTH - (int)w)/2;
  gfx->setCursor(x, y);
  gfx->setTextColor(WHITE);
  gfx->print(s);
}

static void drawWrapped(const String &text, int16_t x, int16_t y, int16_t w, uint8_t sz, uint16_t color)
{
  if (!gfx) return;
  gfx->setTextSize(sz);
  gfx->setTextColor(color);
  String line="", word="";
  for (size_t i=0;i<text.length();++i){
    char c=text[i];
    if (c=='\n' || c==' ') {
      String test=line + (line.length()?" ":"") + word;
      int16_t bx,by; uint16_t bw,bh; gfx->getTextBounds((char*)test.c_str(), x, y, &bx,&by,&bw,&bh);
      if (bw > w) { gfx->setCursor(x,y); gfx->print(line); y += (bh+2); line = word; }
      else { line = test; }
      word=""; if (c=='\n'){ gfx->setCursor(x,y); gfx->print(line); y += (bh+2); line=""; }
    } else { word += c; }
  }
  if (word.length()){ String test=line + (line.length()?" ":"") + word; int16_t bx,by; uint16_t bw,bh; gfx->getTextBounds((char*)test.c_str(), x, y, &bx,&by,&bw,&bh); if (bw>w){ gfx->setCursor(x,y); gfx->print(line); y+=(bh+2); line=word; } else line=test; }
  if (line.length()){ gfx->setCursor(x,y); gfx->print(line);}
}

static void drawChatScreen(const String &userText, const String &assistantText)
{
  if (!gfx) return;
  gfx->fillCircle(LCD_WIDTH/2, LCD_HEIGHT/2, LCD_WIDTH/2, BLACK);
  gfx->setTextSize(2); gfx->setTextColor(WHITE); gfx->setCursor(14, 8); gfx->print(DEVICE_NAME);
  gfx->fillRoundRect(10, 34, 220, 70, 10, 0x39E7);
  drawWrapped(userText, 16, 42, 208, 1, WHITE);
  gfx->fillRoundRect(10, 112, 220, 104, 10, 0x2104);
  drawWrapped(assistantText, 16, 120, 208, 1, WHITE);
}

static void drawIdleScreen()
{
  if (!gfx) return;
  gfx->fillCircle(LCD_WIDTH/2, LCD_HEIGHT/2, LCD_WIDTH/2, BLACK);
  String hh = String(timeClient.getHours());
  if (hh.length() == 1) hh = "0" + hh;
  String mm = String(timeClient.getMinutes());
  if (mm.length() == 1) mm = "0" + mm;
  String clock = hh + ":" + mm;
  gfx->setTextColor(WHITE);
  drawCenteredText(clock, 80, 4);
  time_t raw = timeClient.getEpochTime();
  struct tm *ti = localtime(&raw);
  char datebuf[32];
  strftime(datebuf, sizeof(datebuf), "%a %d-%m-%Y", ti);
  drawCenteredText(String(datebuf), 110, 2);
  drawWeatherBadge(LCD_WIDTH/2, 160);
  gfx->setTextSize(1);
  drawCenteredText(String("Nhấn & giữ để nói"), 210, 1);
}

static void drawWeatherBadge(int16_t cx, int16_t cy)
{
  int16_t w = 160, h = 40;
  int16_t x = cx - w/2, y = cy - h/2;
  gfx->fillRoundRect(x, y, w, h, 10, 0x4210);
  gfx->drawRoundRect(x, y, w, h, 10, WHITE);
  gfx->setTextColor(WHITE);
  gfx->setTextSize(2);
  gfx->setCursor(x+10, y+12);
  gfx->print(cachedCity);
  gfx->setCursor(x+w-70, y+12);
  gfx->print(cachedWeather);
}

// ── Setup ──
void setup() {
  Serial.begin(115200);

  // Backlight LEDC
  ledc_timer_config_t ledc_timer = {
    .speed_mode       = LEDC_LOW_SPEED_MODE,
    .duty_resolution  = LEDC_TIMER_8_BIT,
    .timer_num        = LEDC_TIMER_0,
    .freq_hz          = 5000,
    .clk_cfg          = LEDC_AUTO_CLK
  };
  ledc_timer_config(&ledc_timer);
  ledc_channel_config_t ledc_channel = {
    .gpio_num   = LCD_BL,
    .speed_mode = LEDC_LOW_SPEED_MODE,
    .channel    = LEDC_CHANNEL_1,
    .intr_type  = LEDC_INTR_DISABLE,
    .timer_sel  = LEDC_TIMER_0,
    .duty       = 200,
    .hpoint     = 0
  };
  ledc_channel_config(&ledc_channel);

  // LCD auto-detect
  Serial.println("[LCD] Auto-detecting pins...");
  for (size_t i=0; i<LCD_CAND_COUNT; ++i) {
    Arduino_GFX *tgfx = nullptr;
    if (try_init_lcd(LCD_CANDIDATES[i], tgfx)) {
      gfx = tgfx;
      g_found = LCD_CANDIDATES[i];
      Serial.printf("[LCD] OK: %s (SCK=%d MOSI=%d DC=%d CS=%d RST=%d BL=%d)\n", g_found.name, g_found.sck, g_found.mosi, g_found.dc, g_found.cs, g_found.rst, g_found.bl);
      break;
    } else {
      Serial.printf("[LCD] Fail: %s\n", LCD_CANDIDATES[i].name);
    }
  }
  if (!gfx) {
    Serial.println("[LCD] No candidate worked. Screen will stay black.\n- Hãy gửi lại log Serial, mình sẽ thêm cấu hình mới.\n");
  } else {
    gfx->fillScreen(BLACK);
    gfx->setTextColor(WHITE);
    gfx->setCursor(10, 120);
    gfx->setTextSize(1);
    gfx->println(F("LCD OK - init"));
    gfx->setTextColor(WHITE);
    gfx->setCursor(20, 120);
    gfx->setTextSize(2);
    gfx->println(F("Booting XiaoZhi ChatGPT..."));
  }

  // SPIFFS
  if (!SPIFFS.begin(true)) {
    Serial.println("[FS] SPIFFS mount failed");
  }

  connectWiFi();
  timeClient.begin();
  timeClient.update();

  pinMode(BTN_TALK, INPUT_PULLUP);

  // First weather fetch
  fetchWeather();
  drawIdleScreen();
}

// ── Loop ──
void loop()
{
  timeClient.update();
  // Redraw idle mỗi giây, cập nhật thời tiết mỗi 10 phút
  if (millis() - lastIdleRedraw > 1000)
  {
    if (gfx) drawIdleScreen();
    lastIdleRedraw = millis();
  }
  static unsigned long lastWx = 0;
  if (millis() - lastWx > 10UL * 60UL * 1000UL)
  {
    fetchWeather();
    lastWx = millis();
  }

  // Serial chat
  static String serialBuf;
  while (Serial.available()) {
    char ch = (char)Serial.read();
    if (ch == '\r') continue;
    if (ch == '\n') {
      String line = serialBuf; serialBuf = "";
      line.trim();
      if (line.length()) {
        const String WAKE = "hey " + String(DEVICE_NAME);
        if (startsWithCI(line, WAKE)) {
          line = line.substring(WAKE.length());
          line.trim();
        }
        if (line.length()==0) {
          drawChatScreen("(wake)", "Mình đang nghe đây!");
        } else {
          drawChatScreen(line, "Đang hỏi " + String(DEVICE_NAME) + "...");
          String reply = callChatGPT(line);
          drawChatScreen(line, reply);
          Serial.printf("[LLM] %s\n", reply.c_str());
        }
      }
    } else serialBuf += ch;
  }

  // Push-to-talk
  if (digitalRead(BTN_TALK) == LOW)
  {
#if OPENAI_USE_WHISPER
    // ... giữ nguyên phần ghi âm, gửi Whisper và phát TTS như file gốc ...
#else
    Serial.println("[BTN] TALK pressed, but STT disabled.");
#endif
  }
}