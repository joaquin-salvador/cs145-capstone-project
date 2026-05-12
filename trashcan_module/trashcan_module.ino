// ReSiklo - Trash Can
//
// This module is triggered when orchestrator posts RECYCLE
//
// States:
//  IDLE
//  WAITING_FOR_BOTTLE
//  BOTTLE_DEPOSITED
//  TIMEOUT
//
//  Detection Signals:
//    label = BOTTLE | NO_OBJECT
//    led   = BOTTLE_DETECTED | NO_BOTTLE
//
// Endpoints:
//  /                                 - dashboard
//  /status                           - JSON status snapshot
//  /pir                              - JSON HC-SR04 reading
//  /trigger                          - orchestrator starts a redemption session
//  /update                           - inference_server pushes label/led
//  /flaps                            - manual flap toggle (test override)
//
// References:
//  - HC-SR04 ultrasonic distance sensor:
//    https://lastminuteengineers.com/arduino-sr04-ultrasonic-sensor-tutorial/
//  - TowerPro MG996R servo (4.8-7.2V, ~11 kg-cm):
//    https://servodatabase.com/servo/towerpro/mg996r
//  - ESP8266 Servo library:
//    https://github.com/esp8266/Arduino
//  - mDNS
//    https://randomnerdtutorials.com/esp32-mdns-arduino/
//  - ESP8226 mDNS must be pumped
//    https://forum.arduino.cc/t/mdns-not-working-esp8226/925304/5

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <Servo.h>

// WiFi credentials
#ifndef STASSID
#define STASSID "netnetnet"
#define STAPSK  "hRkh777r"
#endif
const char* ssid     = STASSID;
const char* password = STAPSK;

// Pins
const int LEFT_PIN  = 5;    // Left flap servo (PWM)  - GPIO5
const int RIGHT_PIN = 4;    // Right flap servo (PWM) - GPIO4
const int PIN_TRIG  = 14;   // HC-SR04 TRIG           - GPIO14
const int PIN_ECHO  = 12;   // HC-SR04 ECHO (divider) - GPIO12

// Servo angles
// CLOSED is the idle position
// OPEN is the deposit position
const int LEFT_CLOSED  = 90;
const int RIGHT_CLOSED = 90;
const int LEFT_OPEN    = 0;
const int RIGHT_OPEN   = 0;

// Tuning
const unsigned long BOTTLE_WAIT_MS    = 20000;  // Phase 1 timeout
const unsigned long DEPOSIT_WAIT_MS   = 6000;   // Phase 2 timeout
const unsigned long DEPOSIT_HOLD_MS   = 800;    // post-detect hold before closing
const unsigned long SETTLE_MS         = 400;    // post-open settle before reading
const float         DEPOSIT_CM        = 12.0;
const unsigned long PING_INTERVAL     = 60;

ESP8266WebServer server(1145);
Servo leftFlap;
Servo rightFlap;

// Signals
String  lastClassification = "NO_OBJECT";
String  ledStatus          = "NO_BOTTLE";
volatile bool bottleVisible = false;

// States
String  sessionState  = "IDLE";
int     depositsLocal = 0;

// Flap state
bool flapsOpen = false;

// Re-entry guard
volatile bool sessionInProgress = false;

// HC-SR04 cache (para di kailangan magping palagi)
bool          bottleInBin     = false;
float         lastDistanceCm  = -1.0;
unsigned long lastBinCheck    = 0;
const unsigned long BIN_CHECK_INTERVAL = 200;
const float   BOTTLE_IN_BIN_CM = 6.0;

// HC-SR04 Ultrasonic Sensor
float readDistanceCm() {
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);

  unsigned long us = pulseIn(PIN_ECHO, HIGH, 25000UL);
  if (us == 0) return -1.0;
  return (us * 0.0343f) / 2.0f;
}

void checkBinState() {
  float d = readDistanceCm();
  lastDistanceCm = d;
  bool wasInBin = bottleInBin;
  bottleInBin = (d > 0 && d < BOTTLE_IN_BIN_CM);
  if (bottleInBin != wasInBin) {
    Serial.printf("[SENSOR] State change: %s (%.1f cm)\n",
                  bottleInBin ? "BOTTLE_IN_BIN" : "EMPTY", d);
  }
}

// Dual Flap Servo Control
void openFlaps() {
  leftFlap.write(LEFT_OPEN);
  rightFlap.write(RIGHT_OPEN);
  flapsOpen = true;
  Serial.println("[SERVOS] OPEN");
}

void closeFlaps() {
  leftFlap.write(LEFT_CLOSED);
  rightFlap.write(RIGHT_CLOSED);
  flapsOpen = false;
  Serial.println("[SERVOS] CLOSED");
}

// Recycling Session (Core Logic)
// Note po na two-phase recycling tayo:
//  Phase 1 (WAITING_FOR_BOTTLE):
//    User pressed RECYCLE. Hintay until sure na may bote nga sha
//  Phase 2 (BOTTLE_SHOWN -> WAITING_FOR_DEPOSIT):
//    Ayun na nga may bote na sha. Bubukas yung flaps tas antabay yung
//    sensor para macheck kung may tinapon nga --> minimum functionalities list
void runSession() {
  if (sessionInProgress) {
    Serial.println("[SESSION] Re-entry blocked - already running");
    return;
  }
  sessionInProgress = true;

  Serial.println("[SESSION] START - awaiting bottle visibility");
  sessionState = "WAITING_FOR_BOTTLE";

  // Phase 1: pakita mo yung bote
  unsigned long phase1_t0 = millis();
  bool bottleShown = false;
  while (millis() - phase1_t0 < BOTTLE_WAIT_MS) {
    server.handleClient();
    if (bottleVisible) {
      Serial.println("[SESSION] Bottle visible -> opening flaps");
      bottleShown = true;
      break;
    }
    delay(20);
  }

  if (!bottleShown) {
    Serial.println("[SESSION] TIMEOUT - no bottle shown");
    sessionState = "TIMEOUT";
    delay(1500);
    sessionState = "IDLE";
    sessionInProgress = false;
    return;
  }

  // Phase 2: patapon po nung bote
  sessionState = "BOTTLE_SHOWN";
  openFlaps();
  delay(SETTLE_MS);  // let the ultrasonic readings stabilize

  sessionState = "WAITING_FOR_DEPOSIT";
  unsigned long phase2_t0 = millis();
  unsigned long lastPing  = 0;
  bool deposited = false;

  while (millis() - phase2_t0 < DEPOSIT_WAIT_MS) {
    server.handleClient();

    if (millis() - lastPing >= PING_INTERVAL) {
      lastPing = millis();
      float d = readDistanceCm();
      lastDistanceCm = d;
      bottleInBin    = (d > 0 && d < BOTTLE_IN_BIN_CM);

      // HC-SR04 sees something
      if (d > 0 && d < DEPOSIT_CM) {
        Serial.printf("[SESSION] Deposit detected at %.1f cm\n", d);
        deposited = true;
        break;
      }
    }
    delay(5);
  }

  // Hold briefly so the bottle clears the opening before the lid shuts
  delay(DEPOSIT_HOLD_MS);
  closeFlaps();

  if (deposited) {
    depositsLocal += 1;
    sessionState = "BOTTLE_DEPOSITED";
    Serial.printf("[SESSION] BOTTLE_DEPOSITED (total: %d)\n", depositsLocal);
  } else {
    sessionState = "TIMEOUT";
    Serial.println("[SESSION] TIMEOUT - no deposit");
  }

  delay(1000);
  sessionState = "IDLE";
  sessionInProgress = false;
}

// HTTP Handlers
void handleRoot() {
  String page = "<!doctype html><html><head><title>ReSiklo - Trash Can</title>"
    "<script>"
    "setInterval(function(){"
    "fetch('/status').then(function(r){return r.json();}).then(function(d){"
    "document.getElementById('det').innerText=d.label;"
    "document.getElementById('led').innerText=d.led;"
    "document.getElementById('state').innerText=d.session;"
    "document.getElementById('deps').innerText=d.deposits;"
    "document.getElementById('binStatus').innerText=d.bottleInBin?'BOTTLE_IN_BIN':'EMPTY';"
    "document.getElementById('flapState').innerText=d.flapsOpen?'OPEN':'CLOSED';"
    "document.getElementById('flapBtn').innerText=d.flapsOpen?'Close Flaps':'Open Flaps';"
    "});"
    "fetch('/sensor').then(function(r){return r.json();}).then(function(d){"
    "var t='CLEAR';"
    "if(d.bottleInBin) t='BOTTLE_IN_BIN';"
    "else if(d.objectNear) t='OBJECT_NEAR';"
    "document.getElementById('distReading').innerText=t;"
    "document.getElementById('distRaw').innerText='Distance: '+(d.distance>0?d.distance.toFixed(1)+' cm':'out of range');"
    "});"
    "},200);"
    "function trigger(){fetch('/trigger',{method:'POST'});}"
    "function toggleFlaps(){fetch('/flaps',{method:'POST'});}"
    "</script></head><body>"
    "<h1>ReSiklo Trash Can</h1>"

    "<h2>Live Distance Sensor</h2>"
    "<p id='distReading'>CLEAR</p>"
    "<p id='distRaw'>Distance: -- cm</p>"

    "<h2>Bin Status</h2>"
    "<p>Bin: <span id='binStatus'>EMPTY</span></p>"

    "<h2>Detection Status</h2>"
    "<p>YOLO Detection: <span id='det'>" + lastClassification + "</span></p>"
    "<p>Inference Signal: <span id='led'>" + ledStatus + "</span></p>"

    "<h2>Session Control</h2>"
    "<p>Session State: <span id='state'>" + sessionState + "</span></p>"
    "<p>Flap Position: <span id='flapState'>" + String(flapsOpen ? "OPEN" : "CLOSED") + "</span></p>"
    "<p>Deposits: <span id='deps'>" + String(depositsLocal) + "</span></p>"
    "<button onclick='trigger()'>Start Recycling Session</button> "
    "<button id='flapBtn' onclick='toggleFlaps()'>" + String(flapsOpen ? "Close Flaps" : "Open Flaps") + "</button>"

    "</body></html>";
  server.send(200, "text/html", page);
}

void handleStatus() {
  String json = "{";
  json += "\"label\":\""     + lastClassification + "\",";
  json += "\"led\":\""       + ledStatus          + "\",";
  json += "\"session\":\""   + sessionState       + "\",";
  json += "\"deposits\":"    + String(depositsLocal) + ",";
  json += "\"flapsOpen\":"   + String(flapsOpen ? "true" : "false") + ",";
  json += "\"bottleInBin\":" + String(bottleInBin ? "true" : "false");
  json += "}";
  server.send(200, "application/json", json);
}

void handleSensorStatus() {
  bool objectNear = (lastDistanceCm > 0 && lastDistanceCm < DEPOSIT_CM);
  String json = "{";
  json += "\"distance\":"    + String(lastDistanceCm, 1) + ",";
  json += "\"bottleInBin\":" + String(bottleInBin ? "true" : "false") + ",";
  json += "\"objectNear\":"  + String(objectNear ? "true" : "false");
  json += "}";
  server.send(200, "application/json", json);
}

// inference_server pushes here with some detection updates
void handleUpdate() {
  if (server.hasArg("label")) {
    lastClassification = server.arg("label");
    bottleVisible = (lastClassification == "BOTTLE");
    Serial.printf("[UPDATE] label=%s bottleVisible=%s\n",
                  lastClassification.c_str(), bottleVisible ? "YES" : "NO");
  }
  if (server.hasArg("led")) {
    ledStatus = server.arg("led");
  }
  server.send(200, "text/plain", "OK");
}

void handleTrigger() {
  server.send(200, "text/plain", "SESSION_STARTING");
  runSession();
}

void handleFlaps() {
  bool willOpen = !flapsOpen;
  server.send(200, "text/plain", willOpen ? "OPENING" : "CLOSING");
  Serial.printf("[FLAPS] Manual toggle -> %s\n", willOpen ? "OPEN" : "CLOSED");
  if (willOpen) openFlaps();
  else          closeFlaps();
}

void handleNotFound() { server.send(404, "text/plain", "NOT_FOUND"); }

// Setup
void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println();
  Serial.println("ReSiklo - Trash Can");

  pinMode(PIN_TRIG, OUTPUT); digitalWrite(PIN_TRIG, LOW);
  pinMode(PIN_ECHO, INPUT);
  Serial.println("[INIT] HC-SR04 pins configured");

  leftFlap.attach(LEFT_PIN);
  rightFlap.attach(RIGHT_PIN);
  closeFlaps();   // idle position is CLOSED; runSession() will open them
  Serial.println("[INIT] Servos attached, flaps CLOSED (idle)");

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

  if (MDNS.begin("resiklo-bin")) {
    MDNS.addService("http", "tcp", 1145);
    Serial.println("[MDNS] resiklo-bin.local advertised on port 1145");
  } else {
    Serial.println("[MDNS] FAILED to start");
  }

  server.on("/",        HTTP_GET,  handleRoot);
  server.on("/status",  HTTP_GET,  handleStatus);
  server.on("/sensor",  HTTP_GET,  handleSensorStatus);
  server.on("/update",  HTTP_POST, handleUpdate);
  server.on("/trigger", HTTP_POST, handleTrigger);
  server.on("/flaps",   HTTP_POST, handleFlaps);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("[HTTP] Server started on port 1145");
  Serial.println("[READY] System ready");
}

void loop() {
  server.handleClient();
  MDNS.update(); // ESP8266 mDNS is not interrupt-driven; must be pumped urkkk

  // Disposition check
  if (millis() - lastBinCheck >= BIN_CHECK_INTERVAL) {
    lastBinCheck = millis();
    checkBinState();
  }
}
