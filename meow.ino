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

// ── WiFi ─────────────────────────────────────────
const char* ssid     = "Joyskie18 2-G";
const char* password = "CACHERO 1973";

// ── Supabase ──────────────────────────────────────
const char* supabaseUrl = "https://bjmokvjpitviifjbnlvs.supabase.co/rest/v1/status";
const char* supabaseKey = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImJqbW9rdmpwaXR2aWlmamJubHZzIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NzQxMzkzMTEsImV4cCI6MjA4OTcxNTMxMX0.yEp_qZzGTArPrc03Om7_LL0jUiTDckUiaWZyxj047Ng";
const char* ntpServer   = "pool.ntp.org";
const long  gmtOffset   = 8 * 3600; // GMT+8 Philippines

// ── TFT ──────────────────────────────────────────
#define TFT_CS  D8
#define TFT_DC  D2
#define TFT_RST D1
Adafruit_ILI9341 tft = Adafruit_ILI9341(TFT_CS, TFT_DC, TFT_RST);

// ── MAX30102 ──────────────────────────────────────
MAX30105 particleSensor;
const byte RATE_SIZE = 6;
byte  rates[RATE_SIZE];
byte  rateSpot       = 0;
long  lastBeat       = 0;
float beatsPerMinute = 0;
int   beatAvg        = 0;

#define FINGER_ON_THRESHOLD  50000
#define FINGER_OFF_THRESHOLD 40000
#define NO_BEAT_TIMEOUT      4000
#define BEAT_FILTER_ALPHA    0.95
#define BEAT_THRESHOLD       400
#define MIN_BEAT_MS          400
#define MAX_BEAT_MS          1500

bool  fingerPresent = false;
unsigned long lastBeatTime = 0;
float irDC = 0, irPrev = 0, peakVal = 0;
bool  risingEdge = false;

// ── DS18B20 ───────────────────────────────────────
#define ONE_WIRE_BUS D0
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature ds18b20(&oneWire);

// ── DHT11 ────────────────────────────────────────
#define DHTPIN  D9
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);

// ── Timing ───────────────────────────────────────
unsigned long lastSensorUpdate = 0;
unsigned long lastSupabasePost = 0;
#define SENSOR_INTERVAL   2000
#define SUPABASE_INTERVAL 5000

float lastBodyTemp = 0, lastAirTemp = 0, lastHumidity = 0;

// ─────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Wire.begin(D4, D3);

  // TFT
  tft.begin();
  tft.setRotation(1);
  tft.fillScreen(ILI9341_BLACK);

  // Boot screen
  tft.setTextColor(ILI9341_CYAN);
  tft.setTextSize(2);
  tft.setCursor(60, 80);
  tft.print("Health Monitor");
  tft.setTextSize(1);
  tft.setTextColor(ILI9341_DARKGREY);
  tft.setCursor(90, 110);
  tft.print("Connecting to WiFi...");

  // WiFi
  WiFi.begin(ssid, password);
  int dots = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    tft.print(".");
    Serial.print(".");
  }
  Serial.println("\nWiFi Connected!");

  // NTP
  configTime(gmtOffset, 0, ntpServer);
  while (time(nullptr) < 100000) delay(500);
  Serial.println("Time synced!");

  // MAX30102
  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println("MAX30102 not found!");
  } else {
    particleSensor.setup(33, 4, 2, 400, 411, 4096);
    particleSensor.enableDIETEMPRDY();
    Serial.println("MAX30102 ready.");
  }

  // DS18B20 + DHT11
  ds18b20.begin();
  dht.begin();

  // Draw main layout
  tft.fillScreen(ILI9341_BLACK);
  drawLayout();
}

// ─────────────────────────────────────────────────
void loop() {
  // ── MAX30102 beat detection (runs every loop) ───
  long irValue = particleSensor.getIR();

  if (irValue >= FINGER_ON_THRESHOLD) {
    if (!fingerPresent) {
      fingerPresent = true;
      beatsPerMinute = 0; beatAvg = 0; rateSpot = 0;
      irDC = irValue; irPrev = 0; peakVal = 0;
      lastBeat = millis(); lastBeatTime = millis();
      memset(rates, 0, sizeof(rates));
      Serial.println("Finger detected");
    }

    irDC = irDC * BEAT_FILTER_ALPHA + irValue * (1.0 - BEAT_FILTER_ALPHA);
    float irAC = irValue - irDC;
    long now = millis();
    long timeSinceLast = now - lastBeat;

    if (irAC > irPrev) {
      risingEdge = true;
      if (irAC > peakVal) peakVal = irAC;
    } else if (risingEdge && irAC < irPrev && timeSinceLast > MIN_BEAT_MS) {
      if (peakVal > BEAT_THRESHOLD && timeSinceLast < MAX_BEAT_MS) {
        beatsPerMinute = 60000.0 / timeSinceLast;
        lastBeat = now;
        lastBeatTime = now;
        peakVal = 0;

        rates[rateSpot++] = (byte)beatsPerMinute;
        rateSpot %= RATE_SIZE;

        int sum = 0, count = 0;
        for (byte i = 0; i < RATE_SIZE; i++) {
          if (rates[i] > 0) { sum += rates[i]; count++; }
        }
        if (count > 0) beatAvg = sum / count;
        Serial.printf("BEAT! BPM=%.1f Avg=%d\n", beatsPerMinute, beatAvg);
      }
      risingEdge = false;
      peakVal = 0;
    }
    irPrev = irAC;

    if (millis() - lastBeatTime > NO_BEAT_TIMEOUT && lastBeatTime > 0) {
      beatsPerMinute = 0; beatAvg = 0;
      memset(rates, 0, sizeof(rates));
      lastBeatTime = 0;
      Serial.println("Beat timeout");
    }

  } else if (irValue < FINGER_OFF_THRESHOLD) {
    if (fingerPresent) {
      fingerPresent = false;
      beatsPerMinute = 0; beatAvg = 0; rateSpot = 0;
      lastBeatTime = 0; irDC = 0; irPrev = 0; peakVal = 0;
      memset(rates, 0, sizeof(rates));
      Serial.println("Finger removed");
      clearBPMDisplay();
    }
  }

  // ── Read other sensors every 2s ─────────────────
  if (millis() - lastSensorUpdate >= SENSOR_INTERVAL) {
    lastSensorUpdate = millis();

    ds18b20.requestTemperatures();
    lastBodyTemp = ds18b20.getTempCByIndex(0);
    lastHumidity = dht.readHumidity();
    lastAirTemp  = dht.readTemperature();

    updateDisplay(beatAvg, lastBodyTemp, lastAirTemp, lastHumidity);

    Serial.printf("BPM=%d BodyTemp=%.1fC Air=%.1fC Hum=%.1f%% IR=%ld Finger=%s\n",
                  beatAvg, lastBodyTemp, lastAirTemp, lastHumidity,
                  irValue, fingerPresent ? "YES" : "NO");
  }

  // ── Post to Supabase every 5s ────────────────────
  if (millis() - lastSupabasePost >= SUPABASE_INTERVAL) {
    lastSupabasePost = millis();
    postToSupabase();
  }
}

// ─────────────────────────────────────────────────
void postToSupabase() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi lost, skipping post.");
    return;
  }

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  // UPSERT so we always update the same row (device = esp8266)
  String url = String(supabaseUrl) + "?on_conflict=device";
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", supabaseKey);
  http.addHeader("Authorization", String("Bearer ") + supabaseKey);
  http.addHeader("Prefer", "resolution=merge-duplicates,return=minimal");

  long now = time(nullptr);

  String json = "{";
  json += "\"device\":\"esp8266\",";
  json += "\"last_seen\":" + String(now) + ",";
  json += "\"bpm\":"       + String(beatAvg) + ",";
  json += "\"body_temp\":" + String(lastBodyTemp, 1) + ",";
  json += "\"air_temp\":"  + String(lastAirTemp,  1) + ",";
  json += "\"humidity\":"  + String(lastHumidity, 1);
  json += "}";

  int code = http.POST(json);
  Serial.printf("Supabase POST → %d\n", code);
  if (code != 200 && code != 201) {
    Serial.println(http.getString());
  }
  http.end();
}

// ─────────────────────────────────────────────────
void drawLayout() {
  tft.setTextSize(1);
  tft.setTextColor(ILI9341_CYAN);
  tft.setCursor(4, 4);   tft.print("Heart Rate");
  tft.setCursor(164, 4); tft.print("Body Temp");
  tft.setCursor(4, 124); tft.print("Air Temp");
  tft.setCursor(164,124); tft.print("Humidity");

  // WiFi indicator
  tft.setTextColor(ILI9341_GREEN);
  tft.setCursor(270, 4);
  tft.print("WiFi OK");

  tft.drawFastVLine(158, 0,  240, ILI9341_DARKGREY);
  tft.drawFastHLine(0,  120, 320, ILI9341_DARKGREY);
}

void clearBPMDisplay() {
  tft.fillRect(0, 14, 156, 104, ILI9341_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(ILI9341_DARKGREY);
  tft.setCursor(8, 60);
  tft.print("Place finger...");
}

void updateDisplay(int bpm, float bodyTemp, float airTemp, float humidity) {
  // ── BPM (top left) ───────────────────────────────
  tft.fillRect(0, 14, 156, 104, ILI9341_BLACK);
  if (!fingerPresent) {
    tft.setTextSize(1);
    tft.setTextColor(ILI9341_DARKGREY);
    tft.setCursor(8, 60);
    tft.print("Place finger...");
  } else if (bpm == 0) {
    tft.setTextSize(1);
    tft.setTextColor(ILI9341_YELLOW);
    tft.setCursor(8, 56);
    tft.print("Measuring...");
  } else {
    tft.setTextSize(3);
    tft.setTextColor(ILI9341_GREEN);
    tft.setCursor(8, 50);
    tft.print(bpm);
    tft.setTextSize(1);
    tft.setTextColor(ILI9341_DARKGREY);
    tft.setCursor(8, 90);
    tft.print("BPM");
  }

  // ── Body Temp (top right) ─────────────────────────
  tft.fillRect(160, 14, 160, 104, ILI9341_BLACK);
  tft.setTextSize(3);
  tft.setTextColor(ILI9341_ORANGE);
  tft.setCursor(164, 50);
  if (bodyTemp != DEVICE_DISCONNECTED_C) tft.printf("%.1f", bodyTemp);
  else tft.print("ERR");
  tft.setTextSize(1);
  tft.setTextColor(ILI9341_DARKGREY);
  tft.setCursor(164, 90);
  tft.print("deg C");

  // ── Air Temp (bottom left) ────────────────────────
  tft.fillRect(0, 134, 156, 106, ILI9341_BLACK);
  tft.setTextSize(3);
  tft.setTextColor(ILI9341_YELLOW);
  tft.setCursor(8, 170);
  if (!isnan(airTemp)) tft.printf("%.1f", airTemp);
  else tft.print("ERR");
  tft.setTextSize(1);
  tft.setTextColor(ILI9341_DARKGREY);
  tft.setCursor(8, 210);
  tft.print("deg C");

  // ── Humidity (bottom right) ───────────────────────
  tft.fillRect(160, 134, 160, 106, ILI9341_BLACK);
  tft.setTextSize(3);
  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(164, 170);
  if (!isnan(humidity)) tft.printf("%.1f", humidity);
  else tft.print("ERR");
  tft.setTextSize(1);
  tft.setTextColor(ILI9341_DARKGREY);
  tft.setCursor(164, 210);
  tft.print("%");
}
