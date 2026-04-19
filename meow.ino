// ═══════════════════════════════════════════════════════════════
//  Health Monitor — ESP8266
//  Sensors : ILI9341 TFT, MAX30102, DS18B20, DHT11
//  Network : WiFi (non-blocking) + Supabase UPSERT
// ═══════════════════════════════════════════════════════════════

#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <Wire.h>
#include <MAX30105.h>
#include <heartRate.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <DHT.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>

// ── WiFi credentials ──────────────────────────────
const char* ssid     = "Joyskie18 5-G";
const char* password = "CACHERO 1973";

// ── Supabase ──────────────────────────────────────
const char* supabaseUrl = "https://bjmokvjpitviifjbnlvs.supabase.co/rest/v1/status";
const char* supabaseKey = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImJqbW9rdmpwaXR2aWlmamJubHZzIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NzQxMzkzMTEsImV4cCI6MjA4OTcxNTMxMX0.yEp_qZzGTArPrc03Om7_LL0jUiTDckUiaWZyxj047Ng";
const char* ntpServer   = "pool.ntp.org";
const long  gmtOffset   = 8 * 3600; // GMT+8 Philippines

// ── TFT pins ──────────────────────────────────────
#define TFT_CS  D8
#define TFT_DC  D2
#define TFT_RST D1
// SCK  → D5 | MOSI → D7 | MISO → D6 (hardware SPI)
Adafruit_ILI9341 tft = Adafruit_ILI9341(TFT_CS, TFT_DC, TFT_RST);

// ── MAX30102 ──────────────────────────────────────
MAX30105 particleSensor;

const byte    RATE_SIZE       = 6;
byte          rates[RATE_SIZE];
byte          rateSpot        = 0;
long          lastBeat        = 0;
float         beatsPerMinute  = 0;
int           beatAvg         = 0;

#define FINGER_ON_THRESHOLD   50000
#define FINGER_OFF_THRESHOLD  40000
#define NO_BEAT_TIMEOUT       4000
#define BEAT_FILTER_ALPHA     0.95
#define BEAT_THRESHOLD        400
#define MIN_BEAT_MS           400
#define MAX_BEAT_MS           1500

bool          fingerPresent   = false;
unsigned long lastBeatTime    = 0;
float         irDC            = 0;
float         irPrev          = 0;
float         peakVal         = 0;
bool          risingEdge      = false;

// ── DS18B20 ───────────────────────────────────────
#define ONE_WIRE_BUS D0
OneWire          oneWire(ONE_WIRE_BUS);
DallasTemperature ds18b20(&oneWire);

// ── DHT11 ────────────────────────────────────────
#define DHTPIN  D9
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);

// ── WiFi state machine ────────────────────────────
enum WiFiConnState { WIFI_IDLE, WIFI_CONNECTING, WIFI_CONNECTED };
WiFiConnState WiFiConnState = WIFI_IDLE;
unsigned long wifiTimer   = 0;
unsigned long retryTimer  = 0;
#define WIFI_TIMEOUT_MS   15000   // give up after 15s
#define WIFI_RETRY_MS     30000   // retry after 30s

// ── Timing ───────────────────────────────────────
unsigned long lastSensorUpdate = 0;
unsigned long lastSupabasePost = 0;
#define SENSOR_INTERVAL    2000
#define SUPABASE_INTERVAL  5000

// ── Sensor values (shared across functions) ───────
float lastBodyTemp = 0;
float lastAirTemp  = 0;
float lastHumidity = 0;


// ═══════════════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== Health Monitor Boot ===");

  Wire.begin(D4, D3); // SDA=D4, SCL=D3

  // ── TFT ────────────────────────────────────────
  tft.begin();
  tft.setRotation(1); // landscape
  tft.fillScreen(ILI9341_BLACK);

  // Boot message
  tft.setTextColor(ILI9341_CYAN);
  tft.setTextSize(2);
  tft.setCursor(50, 90);
  tft.print("Health Monitor");
  tft.setTextSize(1);
  tft.setTextColor(ILI9341_DARKGREY);
  tft.setCursor(90, 118);
  tft.print("Initializing...");
  delay(800);

  // ── MAX30102 ───────────────────────────────────
  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println("MAX30102 not found! Check wiring.");
    tft.setTextColor(ILI9341_RED);
    tft.setCursor(10, 140);
    tft.print("MAX30102 ERROR");
  } else {
    // brightness=33, avgSamples=4, ledMode=2(Red+IR),
    // sampleRate=400, pulseWidth=411, adcRange=4096
    particleSensor.setup(33, 4, 2, 400, 411, 4096);
    particleSensor.enableDIETEMPRDY();
    Serial.println("MAX30102 ready.");
  }

  // ── DS18B20 ────────────────────────────────────
  ds18b20.begin();
  Serial.println("DS18B20 ready.");

  // ── DHT11 ──────────────────────────────────────
  dht.begin();
  Serial.println("DHT11 ready.");

  // ── Draw main layout ───────────────────────────
  tft.fillScreen(ILI9341_BLACK);
  drawLayout();

  // ── Start WiFi (non-blocking) ──────────────────
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false); // we manage reconnect ourselves
  wifiTick(); // kicks off first attempt
}


// ═══════════════════════════════════════════════════
//  LOOP
// ═══════════════════════════════════════════════════
void loop() {

  // ── 1. WiFi state machine (non-blocking) ────────
  switch (WiFiConnState) {
    case WIFI_CONNECTING:
      wifiTick();
      break;
    case WIFI_CONNECTED:
      wifiTick(); // watches for dropped connection
      break;
    case WIFI_IDLE:
      if (millis() - retryTimer >= WIFI_RETRY_MS) {
        wifiTick(); // retry after cooldown
      }
      break;
  }

  // ── 2. MAX30102 beat detection (every loop) ─────
  long irValue = particleSensor.getIR();

  if (irValue >= FINGER_ON_THRESHOLD) {
    // --- Finger just placed ---
    if (!fingerPresent) {
      fingerPresent = true;
      beatsPerMinute = 0;
      beatAvg   = 0;
      rateSpot  = 0;
      irDC      = irValue;
      irPrev    = 0;
      peakVal   = 0;
      risingEdge = false;
      lastBeat      = millis();
      lastBeatTime  = millis();
      memset(rates, 0, sizeof(rates));
      Serial.println("Finger detected.");
    }

    // DC removal — isolates the AC pulse wave
    irDC = irDC * BEAT_FILTER_ALPHA + irValue * (1.0 - BEAT_FILTER_ALPHA);
    float irAC = irValue - irDC;

    long now          = millis();
    long timeSinceLast = now - lastBeat;

    if (irAC > irPrev) {
      risingEdge = true;
      if (irAC > peakVal) peakVal = irAC;

    } else if (risingEdge && irAC < irPrev && timeSinceLast > MIN_BEAT_MS) {
      // Passed a peak — check if it's a valid beat
      if (peakVal > BEAT_THRESHOLD && timeSinceLast < MAX_BEAT_MS) {
        beatsPerMinute = 60000.0 / timeSinceLast;
        lastBeat      = now;
        lastBeatTime  = now;

        rates[rateSpot++] = (byte)beatsPerMinute;
        rateSpot %= RATE_SIZE;

        // Rolling average (skip empty slots)
        int sum = 0, count = 0;
        for (byte i = 0; i < RATE_SIZE; i++) {
          if (rates[i] > 0) { sum += rates[i]; count++; }
        }
        if (count > 0) beatAvg = sum / count;

        Serial.printf("BEAT! BPM=%.1f  Avg=%d  AC=%.1f  IR=%ld\n",
                      beatsPerMinute, beatAvg, irAC, irValue);
      }
      risingEdge = false;
      peakVal    = 0;
    }
    irPrev = irAC;

    // No beat for too long → reset
    if (millis() - lastBeatTime > NO_BEAT_TIMEOUT && lastBeatTime > 0) {
      Serial.println("Beat timeout — reposition finger.");
      beatsPerMinute = 0;
      beatAvg = 0;
      memset(rates, 0, sizeof(rates));
      lastBeatTime = 0;
    }

  } else if (irValue < FINGER_OFF_THRESHOLD) {
    // --- Finger removed ---
    if (fingerPresent) {
      fingerPresent  = false;
      beatsPerMinute = 0;
      beatAvg        = 0;
      rateSpot       = 0;
      lastBeatTime   = 0;
      irDC           = 0;
      irPrev         = 0;
      peakVal        = 0;
      risingEdge     = false;
      memset(rates, 0, sizeof(rates));
      Serial.println("Finger removed.");
      clearBPMDisplay();
    }
  }

  // ── 3. Read other sensors every 2s ──────────────
  if (millis() - lastSensorUpdate >= SENSOR_INTERVAL) {
    lastSensorUpdate = millis();

    ds18b20.requestTemperatures();
    lastBodyTemp = ds18b20.getTempCByIndex(0);
    lastHumidity = dht.readHumidity();
    lastAirTemp  = dht.readTemperature();

    updateDisplay(beatAvg, lastBodyTemp, lastAirTemp, lastHumidity);

    Serial.printf(
      "BPM=%d | Body=%.1fC | Air=%.1fC | Hum=%.1f%% | IR=%ld | Finger=%s | WiFi=%s\n",
      beatAvg, lastBodyTemp, lastAirTemp, lastHumidity, irValue,
      fingerPresent ? "YES" : "NO",
      WiFiConnState == WIFI_CONNECTED ? "OK" : "NO"
    );
  }

  // ── 4. Post to Supabase every 5s (only if online) ─
  if (WiFiConnState == WIFI_CONNECTED &&
      millis() - lastSupabasePost >= SUPABASE_INTERVAL) {
    lastSupabasePost = millis();
    postToSupabase();
  }
}


// ═══════════════════════════════════════════════════
//  WIFI STATE MACHINE
// ═══════════════════════════════════════════════════
void wifiTick() {
  switch (WiFiConnState) {

    case WIFI_IDLE:
      Serial.println("WiFi: connecting...");
      WiFi.disconnect(true);
      delay(100);
      WiFi.begin(ssid, password);
      WiFiConnState = WIFI_CONNECTING;
      wifiTimer = millis();
      updateWiFiIndicator();
      break;

    case WIFI_CONNECTING:
      if (WiFi.status() == WL_CONNECTED) {
        Serial.print("WiFi: connected! IP=");
        Serial.println(WiFi.localIP());
        configTime(gmtOffset, 0, ntpServer);
        WiFiConnState = WIFI_CONNECTED;
        updateWiFiIndicator();

      } else if (millis() - wifiTimer > WIFI_TIMEOUT_MS) {
        Serial.println("WiFi: timeout. Will retry in 30s.");
        WiFi.disconnect(true);
        WiFiConnState  = WIFI_IDLE;
        retryTimer = millis();
        // Set retryTimer far enough back so we don't instantly retry
        // but wait the full WIFI_RETRY_MS
        retryTimer = millis();
        updateWiFiIndicator();
      }
      break;

    case WIFI_CONNECTED:
      if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi: lost. Will retry in 30s.");
        WiFiConnState  = WIFI_IDLE;
        retryTimer = millis();
        updateWiFiIndicator();
      }
      break;
  }
}

void updateWiFiIndicator() {
  tft.fillRect(238, 0, 82, 12, ILI9341_BLACK);
  tft.setTextSize(1);
  switch (WiFiConnState) {
    case WIFI_CONNECTED:
      tft.setTextColor(ILI9341_GREEN);
      tft.setCursor(242, 2);
      tft.print("WiFi OK");
      break;
    case WIFI_CONNECTING:
      tft.setTextColor(ILI9341_YELLOW);
      tft.setCursor(242, 2);
      tft.print("WiFi...");
      break;
    case WIFI_IDLE:
      tft.setTextColor(ILI9341_DARKGREY);
      tft.setCursor(236, 2);
      tft.print("No WiFi");
      break;
  }
}


// ═══════════════════════════════════════════════════
//  SUPABASE POST
// ═══════════════════════════════════════════════════
void postToSupabase() {
  WiFiClientSecure client;
  client.setInsecure(); // skip certificate verification

  HTTPClient http;
  String url = String(supabaseUrl) + "?on_conflict=device";
  http.begin(client, url);
  http.setTimeout(5000); // 5s HTTP timeout — won't block long

  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey",        supabaseKey);
  http.addHeader("Authorization", String("Bearer ") + supabaseKey);
  http.addHeader("Prefer",        "resolution=merge-duplicates,return=minimal");

  long now = time(nullptr);

  String json = "{";
  json += "\"device\":\"esp8266\",";
  json += "\"last_seen\":"  + String(now)                   + ",";
  json += "\"bpm\":"        + String(beatAvg)               + ",";
  json += "\"body_temp\":"  + String(lastBodyTemp, 1)        + ",";
  json += "\"air_temp\":"   + String(lastAirTemp,  1)        + ",";
  json += "\"humidity\":"   + String(lastHumidity, 1);
  json += "}";

  int code = http.POST(json);
  Serial.printf("Supabase POST → %d\n", code);
  if (code != 200 && code != 201) {
    Serial.println(http.getString());
  }
  http.end();
}


// ═══════════════════════════════════════════════════
//  TFT DISPLAY
// ═══════════════════════════════════════════════════
void drawLayout() {
  // Section labels
  tft.setTextSize(1);

  tft.setTextColor(ILI9341_CYAN);
  tft.setCursor(4,   4); tft.print("Heart Rate");
  tft.setCursor(164, 4); tft.print("Body Temp");
  tft.setCursor(4, 124); tft.print("Air Temp");
  tft.setCursor(164,124); tft.print("Humidity");

  // Grid lines
  tft.drawFastVLine(158,  0, 240, ILI9341_DARKGREY);
  tft.drawFastHLine(0,  120, 320, ILI9341_DARKGREY);

  // WiFi indicator
  updateWiFiIndicator();
}

void clearBPMDisplay() {
  tft.fillRect(0, 14, 156, 104, ILI9341_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(ILI9341_DARKGREY);
  tft.setCursor(8, 58);
  tft.print("Place finger...");
}

void updateDisplay(int bpm, float bodyTemp, float airTemp, float humidity) {

  // ── BPM (top-left) ─────────────────────────────
  tft.fillRect(0, 14, 156, 104, ILI9341_BLACK);
  if (!fingerPresent) {
    tft.setTextSize(1);
    tft.setTextColor(ILI9341_DARKGREY);
    tft.setCursor(8, 58);
    tft.print("Place finger...");
  } else if (bpm == 0) {
    tft.setTextSize(1);
    tft.setTextColor(ILI9341_YELLOW);
    tft.setCursor(8, 54);
    tft.print("Measuring...");
    tft.setCursor(8, 68);
    tft.setTextColor(ILI9341_DARKGREY);
    tft.print("Hold still");
  } else {
    tft.setTextSize(3);
    tft.setTextColor(ILI9341_GREEN);
    tft.setCursor(8, 46);
    tft.print(bpm);
    tft.setTextSize(1);
    tft.setTextColor(ILI9341_DARKGREY);
    tft.setCursor(8, 92);
    tft.print("BPM");
  }

  // ── Body Temp (top-right) ──────────────────────
  tft.fillRect(160, 14, 158, 104, ILI9341_BLACK);
  tft.setTextSize(3);
  tft.setTextColor(ILI9341_ORANGE);
  tft.setCursor(164, 46);
  if (bodyTemp != DEVICE_DISCONNECTED_C && bodyTemp > -100) {
    tft.printf("%.1f", bodyTemp);
  } else {
    tft.print("ERR");
  }
  tft.setTextSize(1);
  tft.setTextColor(ILI9341_DARKGREY);
  tft.setCursor(164, 92);
  tft.print("deg C");

  // ── Air Temp (bottom-left) ─────────────────────
  tft.fillRect(0, 132, 156, 106, ILI9341_BLACK);
  tft.setTextSize(3);
  tft.setTextColor(ILI9341_YELLOW);
  tft.setCursor(8, 166);
  if (!isnan(airTemp)) {
    tft.printf("%.1f", airTemp);
  } else {
    tft.print("ERR");
  }
  tft.setTextSize(1);
  tft.setTextColor(ILI9341_DARKGREY);
  tft.setCursor(8, 212);
  tft.print("deg C");

  // ── Humidity (bottom-right) ────────────────────
  tft.fillRect(160, 132, 158, 106, ILI9341_BLACK);
  tft.setTextSize(3);
  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(164, 166);
  if (!isnan(humidity)) {
    tft.printf("%.1f", humidity);
  } else {
    tft.print("ERR");
  }
  tft.setTextSize(1);
  tft.setTextColor(ILI9341_DARKGREY);
  tft.setCursor(164, 212);
  tft.print("% RH");
}
