// ReSiklo - Rewards Dispenser
//
// This module is triggered when orchestrator posts REDEEM
// 
// States:
//  IDLE
//  DISPENSING
//  WAITING_FOR_CLAIM
//  REWARD_CLAIMED
//  TIMEOUT
//
// Endpoints:
//  /                                 - dashboard
//  /status                           - JSON status snapshot
//  /pir                              - JSON PIR reading
//  /trigger                          - orchestrator starts a redemption session
//
// References:
//  - HC-SR501 PIR sensor wiring & warm-up:
//    https://lastminuteengineers.com/pir-sensor-arduino-tutorial/
//  - TowerPro MG90S micro servo (4.8-6V, ~1.8 kg-cm):
//    https://servodatabase.com/servo/towerpro/mg90s
//  - ESP32Servo library:
//    https://github.com/jkb-git/ESP32Servo
//  - mDNS
//    https://randomnerdtutorials.com/esp32-mdns-arduino/

#include <WiFi.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ESP32Servo.h>

// WiFi credentials
#ifndef STASSID
#define STASSID "netnetnet"
#define STAPSK  "hRkh777r"
#endif
const char* ssid     = STASSID;
const char* password = STAPSK;

// Pins
const int PIN_SERVO = 12;   // MG90S signal (PWM) - GPIO 12
const int PIN_PIR   = 14;   // HC-SR501 OUT       - GPIO 14

// Servo Angles
const int GATE_REST = 180;
const int GATE_DROP = 90;
const unsigned long DROP_HOLD_MS = 700;

// Tuning
const unsigned long CLAIM_WINDOW_MS  = 15000;
const unsigned long PIR_WARMUP_MS    = 30000;
const unsigned long PIR_POLL_MS      = 50;

WebServer server(1145);
Servo gate;

// Initialize session state
String  sessionState     = "IDLE";
int     dispensedLocal   = 0;
int     claimedLocal     = 0;
bool    lastClaim        = false;
unsigned long bootMillis = 0;

// HC-SR501 PIR
bool readMotion() { 
  return digitalRead(PIN_PIR) == HIGH; 
}

bool pirReady() { 
  return millis() - bootMillis >= PIR_WARMUP_MS; 
}

// MG90S Servo
void dropOneReward() {
  Serial.println("[SERVO] DISPENSING");
  gate.write(GATE_DROP);
  delay(DROP_HOLD_MS);
  gate.write(GATE_REST);
  Serial.println("[SERVO] GATE_CLOSED");
}

// Redemption Session
void runSession() {
  Serial.println("[SESSION] START");

  if (!pirReady()) {
    unsigned long remaining = PIR_WARMUP_MS - (millis() - bootMillis);
    Serial.printf("[SESSION] PIR warming up (%lu ms left)\n", remaining);
  }

  sessionState = "DISPENSING";
  dropOneReward();
  dispensedLocal += 1;

  sessionState = "WAITING_FOR_CLAIM";
  unsigned long t0 = millis();
  unsigned long lastPoll = 0;
  bool claimed = false;

  while (millis() - t0 < CLAIM_WINDOW_MS) {
    server.handleClient();
    if (millis() - lastPoll >= PIR_POLL_MS) {
      lastPoll = millis();
      if (readMotion()) {
        Serial.println("[SESSION] REWARD_CLAIMED");
        claimed = true;
        break;
      }
    }
    delay(5);
  }

  if (claimed) {
    claimedLocal += 1;
    lastClaim     = true;
    sessionState  = "REWARD_CLAIMED";
    Serial.printf("[SESSION] Claims: %d / Dispensed: %d\n",
                  claimedLocal, dispensedLocal);
  } else {
    lastClaim    = false;
    sessionState = "TIMEOUT";
    Serial.println("[SESSION] TIMEOUT");
  }

  delay(1500);
  sessionState = "IDLE";
}

// HTTP Handlers
void handleRoot() {
  String page = "<!doctype html><html><head><title>ReSiklo - Rewards Dispenser</title>"
    "<script>"
    "setInterval(function(){"
    "fetch('/status').then(function(r){return r.json();}).then(function(d){"
    "document.getElementById('state').innerText=d.session;"
    "document.getElementById('disp').innerText=d.dispensed;"
    "document.getElementById('clm').innerText=d.claimed;"
    "document.getElementById('pirTxt').innerText=d.pirReady?'READY':'WARMING_UP';"
    "});"
    "fetch('/pir').then(function(r){return r.json();}).then(function(d){"
    "document.getElementById('pirReading').innerText=d.motion?'MOTION_DETECTED':'NO_MOTION';"
    "document.getElementById('pirRaw').innerText='PIN: '+(d.motion?'HIGH (1)':'LOW (0)');"
    "});"
    "},200);"
    "function trigger(){fetch('/trigger',{method:'POST'});}"
    "</script></head><body>"
    "<h1>ReSiklo Rewards Dispenser</h1>"

    "<h2>Live PIR Sensor</h2>"
    "<p id='pirReading'>NO_MOTION</p>"
    "<p id='pirRaw'>PIN: LOW (0)</p>"

    "<h2>Sensor Status</h2>"
    "<p>PIR: <span id='pirTxt'>WARMING_UP</span></p>"

    "<h2>Session Control</h2>"
    "<p>Session State: <span id='state'>" + sessionState + "</span></p>"
    "<p>Dispensed: <span id='disp'>" + String(dispensedLocal) + "</span></p>"
    "<p>Claimed: <span id='clm'>" + String(claimedLocal) + "</span></p>"
    "<button onclick='trigger()'>Dispense Reward</button>"

    "</body></html>";
  server.send(200, "text/html", page);
}

void handleStatus() {
  String json = "{";
  json += "\"session\":\""   + sessionState + "\",";
  json += "\"dispensed\":"   + String(dispensedLocal) + ",";
  json += "\"claimed\":"     + String(claimedLocal) + ",";
  json += "\"lastClaim\":"   + String(lastClaim ? "true" : "false") + ",";
  json += "\"pirReady\":"    + String(pirReady() ? "true" : "false") + ",";
  json += "\"pirMotion\":"   + String(readMotion() ? "true" : "false");
  json += "}";
  server.send(200, "application/json", json);
}

void handlePirStatus() {
  String json = "{";
  json += "\"motion\":" + String(readMotion() ? "true" : "false") + ",";
  json += "\"ready\":"  + String(pirReady() ? "true" : "false") + ",";
  json += "\"raw\":"    + String(digitalRead(PIN_PIR));
  json += "}";
  server.send(200, "application/json", json);
}

void handleTrigger() {
  server.send(200, "text/plain", "SESSION_STARTING");
  runSession();
}

void handleNotFound() { 
  server.send(404, "text/plain", "NOT_FOUND"); 
}

// Setup
void setup() {
  Serial.begin(115200);
  delay(1000);
  bootMillis = millis();
  Serial.println();
  Serial.println("ReSiklo - Rewards Dispenser");

  pinMode(PIN_PIR, INPUT);
  Serial.println("[INIT] PIR pin configured");

  ESP32PWM::allocateTimer(0);
  gate.setPeriodHertz(50);
  gate.attach(PIN_SERVO, 500, 2400);
  gate.write(GATE_REST);
  Serial.println("[INIT] Servo ready (REST)");
  Serial.printf("[INIT] PIR warm-up: %lu ms\n", PIR_WARMUP_MS);

  Serial.printf("[WIFI] Connecting to '%s'...\n", ssid);
  WiFi.begin(ssid, password);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500); Serial.print("."); attempts++;
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("[WIFI] Connected, IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("[WIFI] Connection FAILED");
  }

  if (MDNS.begin("resiklo-dispenser")) {
    MDNS.addService("http", "tcp", 1145);
    Serial.println("[MDNS] resiklo-dispenser.local advertised on port 1145");
  } else {
    Serial.println("[MDNS] FAILED to start");
  }

  server.on("/",        HTTP_GET,  handleRoot);
  server.on("/status",  HTTP_GET,  handleStatus);
  server.on("/pir",     HTTP_GET,  handlePirStatus);
  server.on("/trigger", HTTP_POST, handleTrigger);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("[HTTP] Server started on port 1145");
  Serial.println("[READY] System ready");
}

void loop() {
  server.handleClient();
}
