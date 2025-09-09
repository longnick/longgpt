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

extern const uint8_t mp3data[] PROGMEM;  // your MP3 bytes
AudioGeneratorMP3 mp3;
AudioOutputI2S out;
AudioFileSourcePROGMEM src(mp3data, sizeof(mp3data));

void setup() {
  out.SetPinout(…, …, …);  // your BCK/LRCK/DOUT pins
  mp3.begin(&src, &out);
}
void loop() {
  if (mp3.isRunning()) mp3.loop();
}

// Display
#include <Arduino_GFX_Library.h>

// ==== AUTO-DETECT LCD PINS (avoids manual trial) ====
struct LcdCandidate { int sck, mosi, miso, dc, cs, rst, bl; const char* name; };
// Shortlist based on common ESP32-S3 round GC9A01 boards (kept small to avoid side effects)
static LcdCandidate LCD_CANDIDATES[] = {
  {36, 35, -1,  7,  6,  5, 10, "S3-Round(1): SCK36 MOSI35 DC7 CS6 RST5 BL10"},
  {12, 11, -1,  9, 10,  8, 14, "XiaoZhi-like: SCK12 MOSI11 DC9 CS10 RST8 BL14"},
  {40, 41, -1, 38, 39, 42, 45, "S3-Dev-Alt: SCK40 MOSI41 DC38 CS39 RST42 BL45"},
  {48, 47, -1, 38, 39, 40, 45, "S3-HiPins:  SCK48 MOSI47 DC38 CS39 RST40 BL45"},
};
static const size_t LCD_CAND_COUNT = sizeof(LCD_CANDIDATES)/sizeof(LcdCandidate);

static LcdCandidate g_found = {-1,-1,-1,-1,-1,-1,-1, "(none)"};

static bool try_init_lcd(const LcdCandidate &c, Arduino_GFX* &out_gfx) {
  // Backlight pin prepared but duty set after success
  if (c.bl >= 0) { pinMode(c.bl, OUTPUT); digitalWrite(c.bl, LOW); }
  Arduino_DataBus *tbus = new Arduino_HWSPI(c.dc, c.cs, c.sck, c.mosi, c.miso);
  Arduino_GFX *tgfx = new Arduino_GC9A01(tbus, c.rst, 0, true, LCD_WIDTH, LCD_HEIGHT);
  if (!tgfx->begin()) {
    delete tgfx; // bus deleted by gfx destructor normally, but on fail ensure cleanup
    return false;
  }
  out_gfx = tgfx;
  if (c.bl >= 0) {
    // light up after successful init
    pinMode(c.bl, OUTPUT);
    digitalWrite(c.bl, HIGH);
  }
  return true;
}


// Audio (playback)
#include <AudioFileSourcePROGMEM.h>
#include <AudioFileSourceBuffer.h>
#include <AudioFileSourceSPIFFS.h>
#include <AudioGeneratorWAV.h>
#include <AudioOutputI2S.h>

// ── Global display objects ──
// Will be created after auto-detect
Arduino_GFX *gfx = nullptr;
#if LCD_DRIVER_GC9A01
// runtime created in setup()
#else
#error "Please select a supported round LCD driver."
#endif

// ── Time (NTP) ──
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 7 * 3600 /*GMT+7*/, 60 * 1000 /*update every 60s*/);

// ── Audio playback ──
AudioGeneratorWAV *wav = nullptr;
AudioOutputI2S *out = nullptr;
AudioFileSourceSPIFFS *file = nullptr;

// ── State ──
unsigned long lastIdleRedraw = 0;
String cachedWeather = "--°C";
String cachedCity = String(OWM_CITY);
String cachedIcon = "";

// ── Helpers ──
static void drawIdleScreen()
{
  if (!gfx) return;
  // Circle background
  gfx->fillCircle(LCD_WIDTH/2, LCD_HEIGHT/2, LCD_WIDTH/2, BLACK);

  // Time
  String hh = String(timeClient.getHours()); if (hh.length()==1) hh = "0"+hh;
  String mm = String(timeClient.getMinutes()); if (mm.length()==1) mm = "0"+mm;
  String clock = hh + ":" + mm;

  gfx->setTextColor(WHITE);
  drawCenteredText(clock, 80, 4);

  // Date line
  time_t raw = timeClient.getEpochTime();
  struct tm *ti = localtime(&raw);
  char datebuf[32]; strftime(datebuf, sizeof(datebuf), "%a %d-%m-%Y", ti);
  drawCenteredText(String(datebuf), 110, 2);

  // Weather badge
  drawWeatherBadge(LCD_WIDTH/2, 160);

  // Hint
  gfx->setTextSize(1);
  drawCenteredText(String("Nói: Hey ") + DEVICE_NAME, 210, 1);
}
;
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

static bool ttsToWav(const String &text, const char *savePath);
#if OPENAI_USE_WHISPER
static String sttFromMic(unsigned long msRecord = 5000);
#endif

// ── Secure HTTP client ──
static WiFiClientSecure makeTLS()
{
  WiFiClientSecure client;
  client.setInsecure(); // For simplicity. For production, install root CA.
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

// ── Setup ──
void setup() {
  Serial.begin(115200);
  WiFi.begin(ssid, pass);
  while (WiFi.status() != WL_CONNECTED) { delay(200); }
  timeClient.begin();
  timeClient.update();
  Serial.println(timeClient.getFormattedTime());
}

void loop() {
  timeClient.update();
  delay(1000);
}

  pinMode(BTN_TALK, INPUT_PULLUP);
  // Backlight via native LEDC (IDF) for maximum compatibility
  // Timer: 5 kHz, 8-bit; Channel: 1, LOW speed mode
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
    .duty       = 200,   // 0..255 brightness
    .hpoint     = 0
  };
  ledc_channel_config(&ledc_channel);

  // ==== LCD AUTO-DETECT ====
  Serial.println("[LCD] Auto-detecting pins...");
  for (size_t i=0; i<LCD_CAND_COUNT; ++i) {
    Arduino_GFX *tgfx = nullptr;
    if (try_init_lcd(LCD_CANDIDATES[i], tgfx)) {
      gfx = tgfx;
      g_found = LCD_CANDIDATES[i];
      Serial.printf("[LCD] OK: %s (SCK=%d MOSI=%d DC=%d CS=%d RST=%d BL=%d)
", g_found.name, g_found.sck, g_found.mosi, g_found.dc, g_found.cs, g_found.rst, g_found.bl);
      break;
    } else {
      Serial.printf("[LCD] Fail: %s
", LCD_CANDIDATES[i].name);
    }
  }
  if (!gfx) {
    Serial.println("[LCD] No candidate worked. Screen will stay black.
- Hãy gửi lại log Serial, mình sẽ thêm cấu hình mới.
");
  } else {
    // Backlight via LEDC (optional): only if BL is PWM-able. Keep simple HIGH above.
    gfx->fillScreen(BLACK);
    gfx->setTextColor(WHITE);
    gfx->setCursor(10, 120);
    gfx->setTextSize(1);
    gfx->println(F("LCD OK - init"));
  }
  gfx->setTextColor(WHITE);
  gfx->setCursor(20, 120);
  gfx->setTextSize(2);
  gfx->println(F("Booting XiaoZhi ChatGPT..."));

  // SPIFFS for storing WAV TTS
  if (!SPIFFS.begin(true))
  {
    Serial.println("[FS] SPIFFS mount failed");
  }

  connectWiFi();
  timeClient.begin();
  timeClient.update();

  // First weather fetch
  fetchWeather();
  drawIdleScreen();
}

// ── Loop ──
void loop()
{
  timeClient.update();

  // Redraw idle every 1s; refresh weather every 10 min
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

  // === Serial wake word & chat ===
  static String serialBuf;
  while (Serial.available()) {
    char ch = (char)Serial.read();
    if (ch == '
') continue;
    if (ch == '
') {
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
          Serial.printf("[LLM] %s
", reply.c_str());
        }
      }
    } else serialBuf += ch;
  }

  // === Push-to-talk ===
  if (digitalRead(BTN_TALK) == LOW)
  {
#if OPENAI_USE_WHISPER
    if (gfx){ gfx->fillCircle(LCD_WIDTH/2, LCD_HEIGHT/2, LCD_WIDTH/2, BLACK); }
    if (gfx){ gfx->setTextColor(WHITE); gfx->setTextSize(2); gfx->setCursor(30, 110); gfx->println(F("Đang ghi âm...")); }
    String text = sttFromMic(5000);
    Serial.printf("[STT] %s
", text.c_str());

    // Wake-word: "Hey LONG GPT" at start (case-insensitive)
    if (startsWithCI(text, String("hey ") + DEVICE_NAME)) {
      text.remove(0, (String("hey ") + DEVICE_NAME).length());
      text.trim();
    }

    if (gfx){ gfx->fillCircle(LCD_WIDTH/2, LCD_HEIGHT/2, LCD_WIDTH/2, BLACK); gfx->setCursor(10, 100); gfx->println(F("Đang hỏi ChatGPT...")); }

    String reply = callChatGPT(text);
    Serial.printf("[LLM] %s
", reply.c_str());
    drawChatScreen(text, reply);

    // TTS
    if (ttsToWav(reply, "/reply.wav"))
    {
      out = new AudioOutputI2S();
      out->SetPinout(I2S_SPK_SCK, I2S_SPK_WS, I2S_SPK_SD);
      file = new AudioFileSourceSPIFFS("/reply.wav");
      wav = new AudioGeneratorWAV();
      wav->begin(file, out);
      while (wav->isRunning()) { if (!wav->loop()) break; delay(1);} 
      wav->stop(); delete wav; wav=nullptr; delete file; file=nullptr; delete out; out=nullptr;
    }
#else
    Serial.println("[BTN] TALK pressed, but STT disabled.");
#endif
  }
}

  static unsigned long lastWx = 0;
  if (millis() - lastWx > 10UL * 60UL * 1000UL)
  {
    fetchWeather();
    lastWx = millis();
  }

  // PTT: hold BTN_TALK to speak
  if (digitalRead(BTN_TALK) == LOW)
  {
#if OPENAI_USE_WHISPER
    gfx->fillCircle(LCD_WIDTH/2, LCD_HEIGHT/2, LCD_WIDTH/2, BLACK);
    gfx->setTextColor(WHITE);
    gfx->setTextSize(2);
    gfx->setCursor(30, 110);
    gfx->println(F("Đang ghi âm..."));
    String userText = sttFromMic(5000); // 5s
    Serial.printf("[STT] %s\n", userText.c_str());

    gfx->fillCircle(LCD_WIDTH/2, LCD_HEIGHT/2, LCD_WIDTH/2, BLACK);
    gfx->setCursor(10, 100);
    gfx->println(F("Đang hỏi ChatGPT..."));

    String reply = callChatGPT(userText);
    Serial.printf("[LLM] %s\n", reply.c_str());

    // TTS to WAV and play
    if (ttsToWav(reply, "/reply.wav"))
    {
      out = new AudioOutputI2S();
      out->SetPinout(I2S_SPK_SCK, I2S_SPK_WS, I2S_SPK_SD);
      file = new AudioFileSourceSPIFFS("/reply.wav");
      wav = new AudioGeneratorWAV();
      wav->begin(file, out);
      while (wav->isRunning())
      {
        if (!wav->loop()) break;
        delay(1);
      }
      wav->stop();
      delete wav; wav = nullptr;
      delete file; file = nullptr;
      delete out; out = nullptr;
    }

    drawIdleScreen();
#else
    Serial.println("[BTN] TALK pressed, but STT disabled.");
#endif
  }
}

// ── UI ──
static void drawCenteredText(const String &s, int16_t y, uint8_t sz);
static void drawWrapped(const String &text, int16_t x, int16_t y, int16_t w, uint8_t sz, uint16_t color);

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
    if (c=='
' || c==' ') {
      String test=line + (line.length()?" ":"") + word;
      int16_t bx,by; uint16_t bw,bh; gfx->getTextBounds((char*)test.c_str(), x, y, &bx,&by,&bw,&bh);
      if (bw > w) { gfx->setCursor(x,y); gfx->print(line); y += (bh+2); line = word; }
      else { line = test; }
      word=""; if (c=='
'){ gfx->setCursor(x,y); gfx->print(line); y += (bh+2); line=""; }
    } else { word += c; }
  }
  if (word.length()){ String test=line + (line.length()?" ":"") + word; int16_t bx,by; uint16_t bw,bh; gfx->getTextBounds((char*)test.c_str(), x, y, &bx,&by,&bw,&bh); if (bw>w){ gfx->setCursor(x,y); gfx->print(line); y+=(bh+2); line=word; } else line=test; }
  if (line.length()){ gfx->setCursor(x,y); gfx->print(line);}  
}

static void drawChatScreen(const String &userText, const String &assistantText)
{
  if (!gfx) return;
  // background
  gfx->fillCircle(LCD_WIDTH/2, LCD_HEIGHT/2, LCD_WIDTH/2, BLACK);
  // Title
  gfx->setTextSize(2); gfx->setTextColor(WHITE); gfx->setCursor(14, 8); gfx->print(DEVICE_NAME);
  // User bubble
  gfx->fillRoundRect(10, 34, 220, 70, 10, 0x39E7 /*teal-ish*/);
  drawWrapped(userText, 16, 42, 208, 1, WHITE);
  // Assistant bubble
  gfx->fillRoundRect(10, 112, 220, 104, 10, 0x2104 /*dark gray*/);
  drawWrapped(assistantText, 16, 120, 208, 1, WHITE);
}
(const String &s, int16_t y, uint8_t sz)
{
  gfx->setTextSize(sz);
  int16_t x1, y1; uint16_t w, h;
  gfx->getTextBounds((char*)s.c_str(), 0, y, &x1, &y1, &w, &h);
  int16_t x = (LCD_WIDTH - (int)w)/2;
  gfx->setCursor(x, y);
  gfx->print(s);
}

static void drawIdleScreen()
{
  // Circle background
  gfx->fillCircle(LCD_WIDTH/2, LCD_HEIGHT/2, LCD_WIDTH/2, BLACK);

  // Time
  String hh = String(timeClient.getHours());
  if (hh.length() == 1) hh = "0" + hh;
  String mm = String(timeClient.getMinutes());
  if (mm.length() == 1) mm = "0" + mm;
  String clock = hh + ":" + mm;

  gfx->setTextColor(WHITE);
  drawCenteredText(clock, 80, 4);

  // Date line
  time_t raw = timeClient.getEpochTime();
  struct tm *ti = localtime(&raw);
  char datebuf[32];
  strftime(datebuf, sizeof(datebuf), "%a %d-%m-%Y", ti);
  drawCenteredText(String(datebuf), 110, 2);

  // Weather badge
  drawWeatherBadge(LCD_WIDTH/2, 160);

  // Hint
  gfx->setTextSize(1);
  drawCenteredText(String("Nhấn & giữ để nói"), 210, 1);
}

static void drawWeatherBadge(int16_t cx, int16_t cy)
{
  // simple rounded rect badge
  int16_t w = 160, h = 40;
  int16_t x = cx - w/2, y = cy - h/2;
  gfx->fillRoundRect(x, y, w, h, 10, 0x4210 /*dark gray*/);
  gfx->drawRoundRect(x, y, w, h, 10, WHITE);

  gfx->setTextColor(WHITE);
  gfx->setTextSize(2);
  gfx->setCursor(x+10, y+12);
  gfx->print(cachedCity);

  gfx->setCursor(x+w-70, y+12);
  gfx->print(cachedWeather);
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
// ── OpenAI Chat ──
static String callChatGPT(const String &user_text)
{
  if (WiFi.status() != WL_CONNECTED) return "(Không có WiFi)";

  const char *endpoint = "https://api.openai.com/v1/chat/completions";

  DynamicJsonDocument payload(12288);
  payload["model"] = OPENAI_CHAT_MODEL;
  JsonArray messages = payload.createNestedArray("messages");

  // System prompt with bot name
  JsonObject sys = messages.createNestedObject();
  sys["role"] = "system";
  sys["content"] = String("Bạn là trợ lý tiếng Việt, tên là \"") + DEVICE_NAME +
                   "\". Trả lời ngắn gọn, tự nhiên, rõ ràng.";

  // Recent history
  for (int i = max(0, turns - MAX_TURNS*2); i < turns; ++i) {
    JsonObject m = messages.createNestedObject();
    m["role"] = history[i].role;
    m["content"] = history[i].content;
  }

  // Current user message
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

  // update history
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
  payload["model"] = "gpt-4o-mini-tts";  // small fast TTS
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

  // Save WAV to SPIFFS
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
// ── Record PCM (I2S Mic) & Send to Whisper ──
#include <driver/i2s.h>

static void i2sMicBegin()
{
  i2s_config_t cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = 16000,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_I2S,
    .intr_alloc_flags = 0,
    .dma_buf_count = 8,
    .dma_buf_len = 256,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };
  i2s_pin_config_t pins = {
    .bck_io_num = I2S_MIC_SCK,
    .ws_io_num = I2S_MIC_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = I2S_MIC_SD
  };
  i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pins);
  i2s_zero_dma_buffer(I2S_NUM_0);
}

static void i2sMicEnd()
{
  i2s_driver_uninstall(I2S_NUM_0);
}

static String sttFromMic(unsigned long msRecord)
{
  i2sMicBegin();
  const uint32_t sampleRate = 16000;
  const uint32_t bytesPerSample = 2;
  uint32_t totalBytes = (sampleRate * bytesPerSample) * (msRecord / 1000);

  // Create a small WAV in RAM (PSRAM recommended) — or stream to SPIFFS first
  File f = SPIFFS.open("/record.wav", FILE_WRITE);
  if (!f) { i2sMicEnd(); return ""; }

  // Write WAV header placeholder
  uint8_t header[44] = {0};
  // RIFF header we'll patch sizes after recording
  memcpy(header, "RIFF\0\0\0\0WAVEfmt ", 16);
  uint32_t subchunk1Size = 16; memcpy(header+16, &subchunk1Size, 4);
  uint16_t audioFormat = 1; memcpy(header+20, &audioFormat, 2);
  uint16_t numChannels = 1; memcpy(header+22, &numChannels, 2);
  memcpy(header+24, &sampleRate, 4);
  uint32_t byteRate = sampleRate * bytesPerSample * numChannels; memcpy(header+28, &byteRate, 4);
  uint16_t blockAlign = bytesPerSample * numChannels; memcpy(header+32, &blockAlign, 2);
  uint16_t bitsPerSample = 16; memcpy(header+34, &bitsPerSample, 2);
  memcpy(header+36, "data\0\0\0\0", 8);
  f.write(header, 44);

  // Record loop
  uint8_t buf[512]; size_t br;
  uint32_t written = 0;
  unsigned long t0 = millis();
  while (millis() - t0 < msRecord)
  {
    i2s_read(I2S_NUM_0, buf, sizeof(buf), &br, portMAX_DELAY);
    f.write(buf, br);
    written += br;
  }
  // Patch sizes
  f.seek(4, SeekSet); uint32_t riffSize = 36 + written; f.write((uint8_t*)&riffSize, 4);
  f.seek(40, SeekSet); f.write((uint8_t*)&written, 4);
  f.close();
  i2sMicEnd();

  // Send to Whisper
  if (WiFi.status() != WL_CONNECTED) return "";
  WiFiClientSecure client = makeTLS();
  HTTPClient https;
  const char *endpoint = "https://api.openai.com/v1/audio/transcriptions";
  if (!https.begin(client, endpoint)) return "";

  String boundary = "----esp32formboundary" + String(millis());
  https.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
  https.addHeader("Authorization", String("Bearer ") + OPENAI_API_KEY);

  File rf = SPIFFS.open("/record.wav", FILE_READ);
  if (!rf) { https.end(); return ""; }

  // Build multipart manually
  String head = "--" + boundary + "\r\n";
  head += "Content-Disposition: form-data; name=\"model\"\r\n\r\nwhisper-1\r\n";
  head += "--" + boundary + "\r\n";
  head += "Content-Disposition: form-data; name=\"file\"; filename=\"record.wav\"\r\n";
  head += "Content-Type: audio/wav\r\n\r\n";

  String tail = "\r\n--" + boundary + "--\r\n";

  // POST length = head + file + tail (setSize works via sendRequest stream override)
  int contentLength = head.length() + rf.size() + tail.length();
  https.collectHeaders(NULL, 0);
  int code = https.sendRequest("POST", &rf, contentLength, head.c_str(), head.length(), tail.c_str(), tail.length());

  rf.close();
  if (code != 200)
  {
    String err = String("STT HTTP ") + code + ": " + https.getString();
    https.end();
    return err;
  }

  DynamicJsonDocument doc(4096);
  deserializeJson(doc, https.getStream());
  https.end();

  const char* text = doc["text"] | "";
  return String(text);
}
#endif

// ── END ──
