#include <WiFi.h>
#include <WebServer.h>

#define STM_UART Serial12
#define STM_BAUD 115200
#define STM_RX_PIN 16 //TX pin on esp
#define STM_TX_PIN 17 //RX pin on esp


//set the constants for the wifi ap and station
const char* WIFI_STA_SSID = "Chavez";
const char* WIFI_STA_PASS = "8067865671";

const char* WIFI_AP_SSID = "ESP32_Group15";
const char* WIFI_AP_PASS = "12345678";

WebServer server(80);

//HTML code sent to users that connect
const char index_html[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Power Monitor Test</title>

<style>
  body { font-family: system-ui, sans-serif; background:#111; color:#eee; padding:16px; }
  .row { display:flex; gap:12px; flex-wrap:wrap; align-items:center; margin-bottom:12px; }
  button, input[type=number]{
    padding:10px 14px; border-radius:10px; border:1px solid #333; background:#222; color:#eee;
  }
  button:hover { background:#2a2a2a; cursor:pointer; }
  .card{ background:#181818; border-radius:14px; padding:16px; box-shadow: 0 0 24px rgba(0,0,0,0.2); }
  .kpi { font-size: 14px; opacity:0.8; }
  .kpi strong{ font-size: 18px; }
</style>
</head>

<body>
<h2>ESP32/STM32 Monitor</h2>

<div class="row card">
  <div class="kpi">Power: <strong id="powr">--</strong> mW</div>
</div>

<div class="row card">
  <button onclick="relayOneOn()">Relay 1 ON</button>
  <button onclick="relayOneOff()">Relay 1 OFF</button>
  <span class="kpi">Threshold (mA):</span>
  <input id="threshOne" type="number" placeholder="2500" min="1" step="1"/>
  <button onclick="setOneThresh()">Set Relay One Threshold</button>
</div>

<div class="row card">
  <button onclick="relayTwoOn()">Relay 2 ON</button>
  <button onclick="relayTwoOff()">Relay 2 OFF</button>
  <span class="kpi">Threshold (mA):</span>
  <input id="threshTwo" type="number" placeholder="2500" min="1" step="1"/>
  <button onclick="setTwoThresh()">Set Relay Two Threshold</button>
</div>

<script>
// ----- Button Functions -----
function relayOneOn() {
  fetch('/relayone/on', { method:'POST' });
}
function relayOneOff() {
  fetch('/relayone/off', { method:'POST' });
}
function relayTwoOn() {
  fetch('/relaytwo/on', { method:'POST' });
}
function relayTwoOff() {
  fetch('/relaytwo/off', { method:'POST' });
}
function setOneThresh() {
  let v = document.getElementById('threshOne').value || '2500';
  fetch('/threshone?mA=' + encodeURIComponent(v), { method:'POST' });
}
function setTwoThresh() {
  let v = document.getElementById('threshTwo').value || '2500';
  fetch('/threshtwo?mA=' + encodeURIComponent(v), { method:'POST' });
}
</script>

</body>
</html>
)HTML";

void sendHTML(){
  server.send_P(200, "text/html", index_html);
}

void handleRelayOneOn(){
  Serial.println("Relay One On");
  server.send(200, "text/plain", "OK");
}

void handleRelayTwoOn(){
  Serial.println("Relay Two On");
  server.send(200, "text/plain", "OK");
}

void handleRelayOneOff(){
  Serial.println("Relay One Off");
  server.send(200, "text/plain", "OK");
}

void handleRelayTwoOff(){
  Serial.println("Relay Two Off");
  server.send(200, "text/plain", "OK");
}

void handleThreshOne() {
  if (server.hasArg("mA")) {
    String value = server.arg("mA");
    Serial.print("[ESP32] Threshold set to: ");
    Serial.println(value);
  } else {
    Serial.println("[ESP32] Threshold missing parameter!");
  }
  server.send(200, "text/plain", "OK");
}

void handleThreshTwo() {
  if (server.hasArg("mA")) {
    String value = server.arg("mA");
    Serial.print("[ESP32] Threshold set to: ");
    Serial.println(value);
  } else {
    Serial.println("[ESP32] Threshold missing parameter!");
  }
  server.send(200, "text/plain", "OK");
}

void setup() {
  Serial.begin(115200);
  STM_UART.begin(STM_BAUD, SERIAL_8N1, STM_RX_PIN, STM_TX_PIN);
  delay(200);
  
  //connect to the internet, needed so the html can get chart.js
  WiFi.mode(WIFI_AP_STA); //allows the esp to connect to an AP and act as an AP
  WiFi.begin(WIFI_STA_SSID, WIFI_STA_PASS);
  Serial.print("Connecting to STA");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 8000){
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Connected to STA");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("STA conn failed");
  }

  //start esp access point
  WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASS);
  Serial.println("AP started");
  Serial.println("AP IP:");
  Serial.print(WiFi.softAPIP());

  //getter and post methods for webpage, each function will send specific data via UART
  server.on("/", HTTP_GET, sendHTML);
  server.on("/relayone/on", HTTP_POST, handleRelayOneOn);
  server.on("/relaytwo/on", HTTP_POST, handleRelayTwoOn);
  server.on("/relayone/off", HTTP_POST, handleRelayOneOff);
  server.on("/relaytwo/off", HTTP_POST, handleRelayTwoOff);
  server.on("/threshone", HTTP_POST, handleThreshOne);
  server.on("/threshtwo", HTTP_POST, handleThreshTwo);

  server.begin();
  Serial.println("server started");
}

void loop() {
  server.handleClient();
}
