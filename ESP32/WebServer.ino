#include <WiFi.h>
#include <WebServer.h>
#include <time.h>
#include <LittleFS.h>

// ------------------------- WiFi + NTP -------------------------

const char *WIFI_SSID     = "TTUguest";
const char *WIFI_PASSWORD = "maskedraiders";

const char *AP_SSID       = "ESP32_Group15";
const char *AP_PASSWORD   = "12345678";

// NTP config
const char *NTP_SERVER = "pool.ntp.org";
const long  GMT_OFFSET_SEC = 0;       // adjust for your timezone if needed
const int   DAYLIGHT_OFFSET_SEC = 0;  // adjust if using DST

// ------------------------- STM UART -------------------------

// Adjust RX/TX pins to your wiring
HardwareSerial STM_UART(1);
const int STM_RX_PIN = 16;  // ESP32 RX from STM TX
const int STM_TX_PIN = 17;  // ESP32 TX to STM RX

// ------------------------- Web server -------------------------

WebServer server(80);

// ------------------------- Sample buffer (15 minutes) -------------------------

// We store up to this many samples; if sampling ~1 Hz, 900 = 15 minutes
const int MAX_SAMPLES = 900;

struct Sample {
  long epoch;   // epoch seconds from NTP (time_t is long on ESP32)
  float s1W;    // S1 power in Watts
  float s2W;    // S2 power in Watts
};

Sample samples[MAX_SAMPLES];
int sampleCount = 0;   // number of valid samples in buffer
int writeIndex  = 0;   // circular index where next sample will be written

float latestS1W = 0.0f;
float latestS2W = 0.0f;

// How long to keep in window (seconds)
const long WINDOW_SECONDS = 15L * 60L; // 15 minutes

// ------------------------- UART parsing -------------------------

char lineBuf[128];
int  lineLen = 0;

// Parse "T=38560,S1_P=0.000,S2_P=0.179"
//  -> S1_P and S2_P are already in W from the STM
void handleStmLine(const char *line) {
  const char *s1Ptr = strstr(line, "S1_P=");
  const char *s2Ptr = strstr(line, "S2_P=");
  if (!s1Ptr || !s2Ptr) {
    return;
  }

  float s1W = 0.0f;
  float s2W = 0.0f;

  s1Ptr += 5;  // skip "S1_P="
  s2Ptr += 5;  // skip "S2_P="

  s1W = (float)atof(s1Ptr);  // already Watts
  s2W = (float)atof(s2Ptr);  // already Watts

  // Get epoch time from NTP
  time_t nowEpoch = time(NULL);
  if (nowEpoch < 946684800L) { // 2000-01-01 sanity check
    return;
  }

  latestS1W = s1W;
  latestS2W = s2W;

  // Store into circular buffer
  samples[writeIndex].epoch = (long)nowEpoch;
  samples[writeIndex].s1W   = s1W;
  samples[writeIndex].s2W   = s2W;

  writeIndex++;
  if (writeIndex >= MAX_SAMPLES) {
    writeIndex = 0;
  }
  if (sampleCount < MAX_SAMPLES) {
    sampleCount++;
  }
}

// Read STM UART, build lines, and feed to parser
void pollStmUart() {
  while (STM_UART.available() > 0) {
    char c = (char)STM_UART.read();

    // Build a line until newline or carriage return
    if (c == '\n' || c == '\r') {
      if (lineLen > 0) {
        if (lineLen >= (int)sizeof(lineBuf)) {
          lineLen = (int)sizeof(lineBuf) - 1;
        }
        lineBuf[lineLen] = '\0';

        // -------------------------------
        // DEBUG PRINT:
        Serial.println(lineBuf);
        // -------------------------------

        // Feed the parser
        handleStmLine(lineBuf);

        lineLen = 0;
      }
    } else {
      // Store characters for the line buffer
      if (lineLen < (int)sizeof(lineBuf) - 1) {
        lineBuf[lineLen++] = c;
      }
    }
  }
}


// ------------------------- HTTP Helpers -------------------------

// Simple helper to send 200 text/plain
void sendOk() {
  server.send(200, "text/plain", "OK");
}

// Serve a file from LittleFS with a given content type
void serveFile(const char *path, const char *contentType) {
  if (!LittleFS.exists(path)) {
    server.send(404, "text/plain", "File not found");
    return;
  }
  File f = LittleFS.open(path, "r");
  if (!f) {
    server.send(500, "text/plain", "Failed to open file");
    return;
  }
  server.streamFile(f, contentType);
  f.close();
}

// ------------------------- HTTP Handlers -------------------------

// Relay / threshold endpoints ------------------------------------

void handleRelayOneOn() {
  STM_UART.println("RELAY1=ON");
  sendOk();
}

void handleRelayOneOff() {
  STM_UART.println("RELAY1=OFF");
  sendOk();
}

void handleRelayTwoOn() {
  STM_UART.println("RELAY2=ON");
  sendOk();
}

void handleRelayTwoOff() {
  STM_UART.println("RELAY2=OFF");
  sendOk();
}

void handleThreshOne() {
  if (server.hasArg("mW")) {
    String v = server.arg("mW"); // e.g. "2500"
    STM_UART.print("THRESH1=");
    STM_UART.println(v);
  }
  sendOk();
}

void handleThreshTwo() {
  if (server.hasArg("mW")) {
    String v = server.arg("mW");
    STM_UART.print("THRESH2=");
    STM_UART.println(v);
  }
  sendOk();
}

// /api/live -> JSON with last ~15 minutes of data in Watts
//   { "t":[epoch...], "s1":[...], "s2":[...] }
void handleApiLive() {
  time_t nowEpoch = time(NULL);
  long cutoff = (long)nowEpoch - WINDOW_SECONDS;

  String json;
  json.reserve(4096);  // rough guess

  json += "{\"t\":[";
  bool first = true;

  // Traverse samples from oldest to newest
  if (sampleCount > 0) {
    int oldestIndex = writeIndex - sampleCount;
    while (oldestIndex < 0) {
      oldestIndex += MAX_SAMPLES;
    }

    for (int i = 0; i < sampleCount; i++) {
      int idx = oldestIndex + i;
      if (idx >= MAX_SAMPLES) {
        idx -= MAX_SAMPLES;
      }

      Sample &s = samples[idx];

      if (s.epoch < cutoff) {
        continue;
      }

      if (!first) {
        json += ',';
      }
      json += (long)s.epoch;
      first = false;
    }
  }

  json += "],\"s1\":[";
  first = true;

  if (sampleCount > 0) {
    int oldestIndex = writeIndex - sampleCount;
    while (oldestIndex < 0) {
      oldestIndex += MAX_SAMPLES;
    }

    for (int i = 0; i < sampleCount; i++) {
      int idx = oldestIndex + i;
      if (idx >= MAX_SAMPLES) {
        idx -= MAX_SAMPLES;
      }

      Sample &s = samples[idx];

      if (s.epoch < cutoff) {
        continue;
      }

      if (!first) {
        json += ',';
      }
      json += String(s.s1W, 5);
      first = false;
    }
  }

  json += "],\"s2\":[";
  first = true;

  if (sampleCount > 0) {
    int oldestIndex = writeIndex - sampleCount;
    while (oldestIndex < 0) {
      oldestIndex += MAX_SAMPLES;
    }

    for (int i = 0; i < sampleCount; i++) {
      int idx = oldestIndex + i;
      if (idx >= MAX_SAMPLES) {
        idx -= MAX_SAMPLES;
      }

      Sample &s = samples[idx];

      if (s.epoch < cutoff) {
        continue;
      }

      if (!first) {
        json += ',';
      }
      json += String(s.s2W, 5);
      first = false;
    }
  }

  json += "]}";

  server.send(200, "application/json", json);
}

// Root and static file handlers ---------------------------------

void handleRoot() {
  serveFile("/index.html", "text/html");
}

void handleIndexHtml() {
  serveFile("/index.html", "text/html");
}

void handleChartJs() {
  serveFile("/chart.js", "application/javascript");
}

// ------------------------- Setup / Loop -------------------------

void setupWifi() {
  // AP + STA mode so ESP hosts its own network AND can connect to your router
  WiFi.mode(WIFI_AP_STA);

  // Start access point for local clients
  bool apOk = WiFi.softAP(AP_SSID, AP_PASSWORD);
  if (!apOk) {
    Serial.println("Failed to start AP");
  } else {
    Serial.print("AP IP: ");
    Serial.println(WiFi.softAPIP());
  }

  // Optionally connect to upstream WiFi for NTP/internet
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  // Don’t block forever if router isn’t available
  long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("STA IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("STA not connected (AP still running)");
  }
}


void setupNtp() {
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
  struct tm timeinfo;
  getLocalTime(&timeinfo);  // best-effort; can ignore result
}

void setup() {
  // USB debug
  Serial.begin(115200);

  // UART to STM
  STM_UART.begin(115200, SERIAL_8N1, STM_RX_PIN, STM_TX_PIN);

  // LittleFS
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed");
  }

  // WiFi + NTP
  setupWifi();
  setupNtp();

  // HTTP routes
  server.on("/",          HTTP_GET, handleRoot);
  server.on("/index.html",HTTP_GET, handleIndexHtml);
  server.on("/chart.js",  HTTP_GET, handleChartJs);

  server.on("/relayone/on",   HTTP_POST, handleRelayOneOn);
  server.on("/relayone/off",  HTTP_POST, handleRelayOneOff);
  server.on("/relaytwo/on",   HTTP_POST, handleRelayTwoOn);
  server.on("/relaytwo/off",  HTTP_POST, handleRelayTwoOff);
  server.on("/threshone",     HTTP_POST, handleThreshOne);
  server.on("/threshtwo",     HTTP_POST, handleThreshTwo);
  server.on("/api/live",      HTTP_GET,  handleApiLive);

  server.begin();
}

void loop() {
  // Handle HTTP requests
  server.handleClient();

  // Continuously read STM UART and update samples
  pollStmUart();
}
