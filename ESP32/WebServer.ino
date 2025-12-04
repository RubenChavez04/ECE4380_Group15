#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <FS.h>
#include <time.h>

// ===================== UART defs =====================
#define STM_UART   Serial2
#define STM_BAUD   115200
#define STM_RX_PIN 16   // ESP32 RX2
#define STM_TX_PIN 17   // ESP32 TX2

// ===================== WiFi / AP =====================
const char* WIFI_STA_SSID = "TTUguest";
const char* WIFI_STA_PASS = "maskedraiders";

const char* WIFI_AP_SSID  = "ESP32_Group15";
const char* WIFI_AP_PASS  = "12345678";

WebServer server(80);

// ===================== Time config (CST-ish) =====================
const char* ntpServer          = "pool.ntp.org";
long        gmtOffset_sec      = -6 * 3600;  // CST
int         daylightOffset_sec = 3600;       // if you want DST, else 0

// ===================== Sample storage (2 channels) =====================
// We keep a FIFO of samples covering the last 24 hours.
// Each sample: timestamp + S1 power (mW) + S2 power (mW).

struct SampleRecord {
  uint32_t t;   // epoch seconds (ESP time)
  float    s1;  // INA219_1 power (mW)
  float    s2;  // INA219_2 power (mW)
};

// 24h window seconds
const uint32_t HISTORY_WINDOW_SEC = 24UL * 3600UL;

// Number of samples to keep in RAM/file.
// At LOG_INTERVAL_SEC=10, 24h => ~8640 samples; this buffer size gives headroom.
// If you want finer time resolution, lower LOG_INTERVAL_SEC and/or increase MAX_SAMPLES.
const uint32_t LOG_INTERVAL_SEC = 10;    // one stored point every 10 seconds
const size_t   MAX_SAMPLES      = 9000;  // ring buffer capacity

SampleRecord historyBuf[MAX_SAMPLES];
size_t       histStart = 0;   // index of oldest sample
size_t       histCount = 0;   // number of valid samples in buffer

volatile float g_latestS1_mW = 0.0f;
volatile float g_latestS2_mW = 0.0f;

const char* HISTORY_FILE = "/history.bin";

// For decimating to LOG_INTERVAL_SEC
uint32_t lastLogSec = 0;

// UART line buffer
char stmRxLine[128];
size_t stmRxLen = 0;

// ===================== History helpers =====================

void clearHistory() {
  histStart = 0;
  histCount = 0;
  for (size_t i = 0; i < MAX_SAMPLES; i++) {
    historyBuf[i].t  = 0;
    historyBuf[i].s1 = 0.0f;
    historyBuf[i].s2 = 0.0f;
  }
}

// Add a sample, trimming anything older than 24h and limited by MAX_SAMPLES.
void addSample(uint32_t ts, float s1_mW, float s2_mW) {
  uint32_t cutoff = (ts > HISTORY_WINDOW_SEC) ? (ts - HISTORY_WINDOW_SEC) : 0;

  // Drop samples older than the 24h window
  while (histCount > 0) {
    size_t idx = histStart;
    if (historyBuf[idx].t >= cutoff) break;
    histStart = (histStart + 1) % MAX_SAMPLES;
    histCount--;
  }

  // If buffer full, drop oldest
  if (histCount == MAX_SAMPLES) {
    histStart = (histStart + 1) % MAX_SAMPLES;
    histCount--;
  }

  // Append at end (newest)
  size_t pos = (histStart + histCount) % MAX_SAMPLES;
  historyBuf[pos].t  = ts;
  historyBuf[pos].s1 = s1_mW;
  historyBuf[pos].s2 = s2_mW;
  histCount++;
}

// Save history as compact binary: [uint32_t count][t,s1,s2]*count
void saveHistory() {
  File f = LittleFS.open(HISTORY_FILE, FILE_WRITE);
  if (!f) {
    Serial.println("[ESP] Failed to open history file for write");
    return;
  }
  uint32_t count = histCount;
  f.write((uint8_t*)&count, sizeof(count));
  for (size_t i = 0; i < histCount; i++) {
    size_t idx = (histStart + i) % MAX_SAMPLES;
    f.write((uint8_t*)&historyBuf[idx].t,  sizeof(historyBuf[idx].t));
    f.write((uint8_t*)&historyBuf[idx].s1, sizeof(historyBuf[idx].s1));
    f.write((uint8_t*)&historyBuf[idx].s2, sizeof(historyBuf[idx].s2));
  }
  f.close();
  Serial.printf("[ESP] Saved history (%lu samples)\n", (unsigned long)count);
}

// Load from LittleFS, then drop anything older than 24h relative to *current* time.
void loadHistory() {
  if (!LittleFS.exists(HISTORY_FILE)) {
    Serial.println("[ESP] No history file; starting empty");
    clearHistory();
    return;
  }

  File f = LittleFS.open(HISTORY_FILE, FILE_READ);
  if (!f) {
    Serial.println("[ESP] Failed to open history file for read");
    clearHistory();
    return;
  }

  uint32_t count = 0;
  if (f.read((uint8_t*)&count, sizeof(count)) != sizeof(count)) {
    Serial.println("[ESP] History file header error");
    f.close();
    clearHistory();
    return;
  }

  clearHistory();
  uint32_t nowSec = (uint32_t)time(nullptr);
  uint32_t cutoff = (nowSec > HISTORY_WINDOW_SEC) ? (nowSec - HISTORY_WINDOW_SEC) : 0;

  for (uint32_t i = 0; i < count; i++) {
    SampleRecord s;
    if (f.read((uint8_t*)&s.t, sizeof(s.t))   != sizeof(s.t) ||
        f.read((uint8_t*)&s.s1, sizeof(s.s1)) != sizeof(s.s1) ||
        f.read((uint8_t*)&s.s2, sizeof(s.s2)) != sizeof(s.s2)) {
      Serial.println("[ESP] History file truncated");
      break;
    }
    if (s.t >= cutoff) {
      addSample(s.t, s.s1, s.s2);
    }
  }

  f.close();
  Serial.printf("[ESP] Loaded history (%lu samples after cutoff)\n",
                (unsigned long)histCount);
}

// ===================== STM UART RX =====================

// Parse a line like: T=12345,S1_P=1250.8,S2_P=321.5
void processStmLine(const char* line) {
  float s1_p = 0.0f;
  float s2_p = 0.0f;
  // We ignore T from STM; we'll use NTP epoch time
  unsigned long dummyT = 0;

  // Simple manual parsing; you can also use strtok/strchr.
  // Example with sscanf if you want stricter format:
  // sscanf(line, "T=%lu,S1_P=%f,S2_P=%f", &dummyT, &s1_p, &s2_p);

  // Here we’ll do a robust parse:
  char buf[128];
  strncpy(buf, line, sizeof(buf)-1);
  buf[sizeof(buf)-1] = '\0';

  char* token = strtok(buf, ",");
  while (token != nullptr) {
    char* eq = strchr(token, '=');
    if (eq) {
      *eq = '\0';
      const char* key = token;
      const char* val = eq + 1;
      if (strcmp(key, "T") == 0) {
        dummyT = strtoul(val, nullptr, 10);
      } else if (strcmp(key, "S1_P") == 0) {
        s1_p = atof(val);
      } else if (strcmp(key, "S2_P") == 0) {
        s2_p = atof(val);
      }
    }
    token = strtok(nullptr, ",");
  }

  // Update latest values
  g_latestS1_mW = s1_p;
  g_latestS2_mW = s2_p;

  // Get ESP epoch seconds (from NTP)
  uint32_t nowSec = (uint32_t)time(nullptr);
  if (nowSec == 0) {
    // NTP not ready yet; you could fallback to millis()/1000 here if you want.
    return;
  }

  // Decimate to one stored sample every LOG_INTERVAL_SEC
  if (lastLogSec == 0 || (nowSec - lastLogSec) >= LOG_INTERVAL_SEC) {
    addSample(nowSec, s1_p, s2_p);
    lastLogSec = nowSec;

    static int saveCounter = 0;
    if (++saveCounter >= 10) {  // save every 10 stored samples
      saveCounter = 0;
      saveHistory();
    }

    Serial.printf("[ESP] Logged: t=%lu, S1=%.2f mW, S2=%.2f mW\n",
                  (unsigned long)nowSec, s1_p, s2_p);
  }
}

void handleStmUart() {
  while (STM_UART.available()) {
    char c = STM_UART.read();
    Serial.write(c);  // forward STM debug to USB Serial

    if (c == '\n' || c == '\r') {
      if (stmRxLen > 0) {
        stmRxLine[stmRxLen] = '\0';
        processStmLine(stmRxLine);
        stmRxLen = 0;
      }
    } else {
      if (stmRxLen < sizeof(stmRxLine) - 1) {
        stmRxLine[stmRxLen++] = c;
      } else {
        // overflow, discard
        stmRxLen = 0;
      }
    }
  }
}

// ===================== HTTP Handlers =====================

void handleIndexHTML() {
  File f = LittleFS.open("/index.html", "r");
  if (!f) {
    server.send(500, "text/plain", "Failed to open /index.html");
    return;
  }
  server.streamFile(f, "text/html");
  f.close();
}

void handleRelayOneOn() {
  STM_UART.println("RELAY1=ON");
  server.send(200, "text/plain", "OK");
}
void handleRelayOneOff() {
  STM_UART.println("RELAY1=OFF");
  server.send(200, "text/plain", "OK");
}
void handleRelayTwoOn() {
  STM_UART.println("RELAY2=ON");
  server.send(200, "text/plain", "OK");
}
void handleRelayTwoOff() {
  STM_UART.println("RELAY2=OFF");
  server.send(200, "text/plain", "OK");
}

// Thresholds now sent in **mW**
void handleThreshOne() {
  if (server.hasArg("mW")) {
    String v = server.arg("mW");
    STM_UART.print("THRESH1=");
    STM_UART.println(v);
  }
  server.send(200, "text/plain", "OK");
}

void handleThreshTwo() {
  if (server.hasArg("mW")) {
    String v = server.arg("mW");
    STM_UART.print("THRESH2=");
    STM_UART.println(v);
  }
  server.send(200, "text/plain", "OK");
}

// API: return history as JSON:
// { "t":[epoch...], "s1":[mW...], "s2":[mW...] }
void handleApiLive() {
  String out;
  out.reserve(8192);

  out += "{\"t\":[";
  for (size_t i = 0; i < histCount; i++) {
    size_t idx = (histStart + i) % MAX_SAMPLES;
    if (i > 0) out += ",";
    out += String((unsigned long)historyBuf[idx].t);
  }
  out += "],\"s1\":[";
  for (size_t i = 0; i < histCount; i++) {
    size_t idx = (histStart + i) % MAX_SAMPLES;
    if (i > 0) out += ",";
    out += String(historyBuf[idx].s1, 3);
  }
  out += "],\"s2\":[";
  for (size_t i = 0; i < histCount; i++) {
    size_t idx = (histStart + i) % MAX_SAMPLES;
    if (i > 0) out += ",";
    out += String(historyBuf[idx].s2, 3);
  }
  out += "]}";

  server.send(200, "application/json", out);
}

// ===================== SETUP =====================

void setup() {
  Serial.begin(115200);
  STM_UART.begin(STM_BAUD, SERIAL_8N1, STM_RX_PIN, STM_TX_PIN);

  // init LittleFS
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS error");
  } else {
    Serial.println("LittleFS mounted");
  }

  // set wifi mode for esp, act as AP and connect to STA
  WiFi.mode(WIFI_AP_STA);

  // connect STA to TTUguest
  WiFi.begin(WIFI_STA_SSID, WIFI_STA_PASS);
  Serial.print("Connecting to STA: ");
  Serial.println(WIFI_STA_SSID);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 8000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("STA connected.");
    Serial.print("STA IP: ");
    Serial.println(WiFi.localIP());

    // configure time via NTP
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    delay(2000);
    time_t now = time(nullptr);
    Serial.print("Current epoch: ");
    Serial.println((long)now);
  } else {
    Serial.println("STA not connected, NTP may fail");
  }

  // start AP
  WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASS);
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());

  // after time should be set (or at least attempted), load history
  loadHistory();

  // HTTP routes
  server.on("/", HTTP_GET, handleIndexHTML);

  server.on("/relayone/on",  HTTP_POST, handleRelayOneOn);
  server.on("/relayone/off", HTTP_POST, handleRelayOneOff);
  server.on("/relaytwo/on",  HTTP_POST, handleRelayTwoOn);
  server.on("/relaytwo/off", HTTP_POST, handleRelayTwoOff);

  server.on("/threshone", HTTP_POST, handleThreshOne);
  server.on("/threshtwo", HTTP_POST, handleThreshTwo);

  server.on("/api/live", HTTP_GET, handleApiLive);

  // serve local chart.js from LittleFS (since internet may be sketchy)
  server.serveStatic("/chart.js", LittleFS, "/chart.js");

  server.begin();
  Serial.println("HTTP server started");
}

void loop() {
  server.handleClient();
  handleStmUart();
}
