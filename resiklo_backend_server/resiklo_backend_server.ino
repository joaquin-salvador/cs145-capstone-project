// ReSiklo - Intermediary Server
// 
// This is the intermediary server that acts
// as the state machine of the entire system.
// 
// Endpoints:
//  /                                 - dashboard
//  /state                            - sensor controls
//  /buttons                          - live button readings
//  /test/recycle                     - testing the recycle functionality                     
//  /test/redeem                      - testing the rewards functionality
//
// References
//  - mDNS
//    https://randomnerdtutorials.com/esp32-mdns-arduino/

#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// WiFi credentials
const char *ssid = "netnetnet";
const char *password = "hRkh777r";

// Local backend URLs
const char* BACKEND_MDNS_HOST = "resiklo-host";
const int   MOSIP_PORT        = 1714;
const int   REWARDS_PORT      = 1715;
String MOSIP_URL   = "";  // filled in setup() after MDNS resolution
String REWARDS_URL = "";  // filled in setup() after MDNS resolution

// Module URLs
const char* TRASHCAN_URL  = "http://resiklo-bin.local:1145";
const char* DISPENSER_URL = "http://resiklo-dispenser.local:1145";

// Pins
#define RXD2 4
#define TXD2 2
const int greenLed    = 18;
const int redLed      = 19;
const int BLUE_BUTTON = 23;   // RECYCLE
const int RED_BUTTON  = 5;    // REDEEM
const int BUZZER_PIN  = 15;   // active buzzer (2-pin: signal + GND)

// OLED Settings
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// States
String currentState = "IDLE";
String currentUin   = "";
String currentName  = "";

const char* WELCOME_MSG = "ReSiklo";

// Tuning
const unsigned long SELECTION_WINDOW_MS = 30000;  // Selection window
const unsigned long BEEP_MS = 150;                // Buzzer beep
const unsigned long DEBOUNCE_MS = 30;             // Debounce

WebServer server(80);

// Re-entry guard
volatile bool sessionInProgress = false;

bool setupComplete = false;

// HTTP Handlers
void handleRoot() {
  String page = "<!doctype html><html><head><title>ReSiklo Identity Verification (TEST)</title></head><body>";
  page += "<h1>ReSiklo Identity Verification</h1>";
  page += "<p>State: <b>" + currentState + "</b></p>";
  page += "<p>Last UIN: " + (currentUin.length() ? currentUin : String("(none)")) + "</p>";
  page += "<p>Backend: MOSIP @ " + (MOSIP_URL.length() ? MOSIP_URL : String("(unresolved)")) +
          " | REWARDS @ " + (REWARDS_URL.length() ? REWARDS_URL : String("(unresolved)")) + "</p>";

  page += "<h2>Push directly to the local rewards DB</h2>";
  page += "<p>The forms below POST to this ESP32, which forwards to the rewards backend.</p>";

  page += "<h3>Add eco-points (RECYCLE)</h3>";
  page += "<form method='POST' action='/test/recycle'>";
  page += "UIN: <input name='uin' value='5408602380'> ";
  page += "Points: <input name='points' value='10' size='4'> ";
  page += "<button type='submit'>POST /recycle</button>";
  page += "</form>";

  page += "<h3>Deduct eco-points (REDEEM)</h3>";
  page += "<form method='POST' action='/test/redeem'>";
  page += "UIN: <input name='uin' value='5408602380'> ";
  page += "Points: <input name='points' value='50' size='4'> ";
  page += "<button type='submit'>POST /redeem</button>";
  page += "</form>";

  page += "<h2>Endpoints</h2>";
  page += "<ul>";
  page += "<li>GET /state    - current state (plain text)</li>";
  page += "<li>GET /buttons  - live button pin diagnostic</li>";
  page += "</ul>";

  page += "<h2>Audit trail</h2>";
  page += "<p>RECYCLE, REDEEM, and REDEEM_DENIED events are logged in the rewards backend.</p>";
  page += "<p><a href='/audit'>View audit trail (HTML)</a></p>";

  page += "<p>QR input: GM861S only (Serial2). Webcam /qr path removed.</p>";

  page += "</body></html>";
  server.send(200, "text/html", page);
}

void handleState() { 
  server.send(200, "text/plain", currentState); 
}

void handleButtons() {
  int b = digitalRead(BLUE_BUTTON);
  int r = digitalRead(RED_BUTTON);
  String json = "{";
  json += "\"blue_pin\":"     + String(BLUE_BUTTON) + ",";
  json += "\"red_pin\":"      + String(RED_BUTTON)  + ",";
  json += "\"blue_raw\":"     + String(b)           + ",";
  json += "\"red_raw\":"      + String(r)           + ",";
  json += "\"blue_pressed\":" + String(b == LOW ? "true" : "false") + ",";
  json += "\"red_pressed\":"  + String(r == LOW ? "true" : "false");
  json += "}";
  server.send(200, "application/json", json);
}

// Proxy /test/recycle -> rewards backend /recycle
void handleTestRecycle() {
  String uin    = server.hasArg("uin")    ? server.arg("uin")    : "";
  String points = server.hasArg("points") ? server.arg("points") : "10";
  String body = "uin=" + uin + "&points=" + points;
  String url  = REWARDS_URL + "/recycle";
  String resp = postRequest(url, body);
  server.send(200, "text/plain",
              "POST " + url + "\nBody: " + body + "\n\n" + resp + "\n\nClick back to return.");
}

// Proxy /test/redeem -> rewards backend /redeem
void handleTestRedeem() {
  String uin    = server.hasArg("uin")    ? server.arg("uin")    : "";
  String points = server.hasArg("points") ? server.arg("points") : "50";
  String body = "uin=" + uin + "&points=" + points;
  String url  = REWARDS_URL + "/redeem";
  String resp = postRequest(url, body);
  server.send(200, "text/plain",
              "POST " + url + "\nBody: " + body + "\n\n" + resp + "\n\nClick back to return.");
}

// Proxy /audit -> rewards backend /audit
void handleAudit() {
  if (WiFi.status() != WL_CONNECTED) {
    server.send(503, "text/plain", "OFFLINE: WiFi not connected");
    return;
  }
  if (REWARDS_URL.length() == 0) {
    server.send(502, "text/plain",
                "BACKEND_UNRESOLVED: mDNS could not resolve " +
                String(BACKEND_MDNS_HOST) + ".local at boot.\n"
                "Start avahi-daemon on the backend host and reboot the ESP32.");
    return;
  }

  String url = REWARDS_URL + "/audit";
  if (server.hasArg("limit")) url += "?limit=" + server.arg("limit");

  HTTPClient http;
  http.begin(url);
  http.setTimeout(8000);
  int code = http.GET();
  if (code > 0) {
    server.send(code, "text/html", http.getString());
  } else {
    server.send(502, "text/plain",
                "BACKEND_UNREACHABLE: " + url + " (HTTPClient code " + String(code) + ")\n"
                "Is rewards_backend.py running on the backend host?");
  }
  http.end();
}

void handleNotFound() {
  server.send(404, "text/plain", "NOT_FOUND");
}

void setup() {
  // Start serial
  Serial.begin(115200);
  delay(1000); 
  Serial.println();
  Serial.println("ReSiklo - Intermediary Server / Orchestrator");

  // Initialize scanner
  Serial2.setRxBufferSize(2048);
  Serial2.begin(9600, SERIAL_8N1, RXD2, TXD2);
  Serial.println("[INIT] QR scanner UART2 ready (9600 baud, 2 KB RX buffer)");

  pinMode(greenLed, OUTPUT);
  pinMode(redLed, OUTPUT);
  pinMode(BUZZER_PIN,  OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  pinMode(BLUE_BUTTON, INPUT_PULLUP);
  pinMode(RED_BUTTON,  INPUT_PULLUP);
  Serial.println("[INIT] LEDs, buzzer, and buttons configured");

  // Test LED blink on startup
  digitalWrite(greenLed, HIGH);
  digitalWrite(redLed, HIGH);
  delay(500);
  digitalWrite(greenLed, LOW);
  digitalWrite(redLed, LOW);

  // Initialize OLED
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("[INIT] OLED FAILED");
    for(;;);
  }
  display.clearDisplay();
  display.setTextColor(WHITE);
  Serial.println("[INIT] OLED ready");

  // Connect Wi-Fi
  Serial.printf("[WIFI] Connecting to '%s'...\n", ssid);
  WiFi.begin(ssid, password);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500); Serial.print("."); attempts++;
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("[WIFI] Connected, IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("[WIFI] Connection FAILED (continuing for offline UI tests)");
  }

  server.on("/",             HTTP_GET,  handleRoot);
  server.on("/state",        HTTP_GET,  handleState);
  server.on("/buttons",      HTTP_GET,  handleButtons);
  server.on("/audit",        HTTP_GET,  handleAudit);
  server.on("/test/recycle", HTTP_POST, handleTestRecycle);
  server.on("/test/redeem",  HTTP_POST, handleTestRedeem);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("[HTTP] Server started on port 80");

  // MDNS
  if (MDNS.begin("resiklo-backend")) {
    MDNS.setInstanceName("ReSiklo Orchestrator");
    MDNS.addService("http", "tcp", 80);
    Serial.println("[MDNS] resiklo-backend.local advertised on port 80");

    // Resolve the Flask backend
    IPAddress backend = MDNS.queryHost(BACKEND_MDNS_HOST);
    if (backend != IPAddress(0, 0, 0, 0)) {
      MOSIP_URL   = "http://" + backend.toString() + ":" + String(MOSIP_PORT);
      REWARDS_URL = "http://" + backend.toString() + ":" + String(REWARDS_PORT);
      Serial.println("[MDNS] " + String(BACKEND_MDNS_HOST) + ".local -> " + backend.toString());
      Serial.println("[MDNS]   MOSIP   " + MOSIP_URL);
      Serial.println("[MDNS]   REWARDS " + REWARDS_URL);
    } else {
      Serial.println("[MDNS] WARN: could not resolve " + String(BACKEND_MDNS_HOST) + ".local");
      Serial.println("[MDNS]   Is avahi-daemon up on the backend host? Reboot the ESP32 after starting it.");
    }
  } else {
    Serial.println("[MDNS] FAILED to start");
  }

  // Drain UART noise emitted while Serial2's TX pin was being configured
  delay(50);
  while (Serial2.available() > 0) {
    Serial2.read();
  }

  Serial.println("[READY] System ready");
  Serial.println("[READY] Try http://resiklo-backend.local/ or http://" + WiFi.localIP().toString() + "/");
  setupComplete = true;
  beep();
}

// Main Loop
void loop() {
  server.handleClient();
  
  // Display idle screen
  if (currentState == "IDLE" && setupComplete) {
    if (WiFi.status() == WL_CONNECTED) {
      displayMessage(WELCOME_MSG, "Scan your ID...");
    } else {
      display.clearDisplay();
      display.display();
    }
  }

  // Smoke-test: paste UIN into Serial Monitor + Enter
  if (Serial.available()) {
    String dummy = Serial.readStringUntil('\n');
    dummy.trim();
    if (dummy.length() >= 1) {
      Serial.println("Serial UIN: " + dummy);
      handleScannedData(dummy);
    }
  }

  // Check for QR data.
  static String qrBuffer = "";
  while (Serial2.available()) {
    char c = (char)Serial2.read();
    if (c == '\r' || c == '\n') {
      if (qrBuffer.length() > 0) {
        // Only accept payloads that start with '{'. Anything else is a
        // truncated tail left over from a buffer overrun while the loop
        // was blocked, and parsing it would yield a junk UIN.
        if (qrBuffer.indexOf('{') == 0) {
          Serial.println("[RAW] " + qrBuffer);
          handleScannedData(qrBuffer);
        } else {
          Serial.println("[DROP] partial frame: " + qrBuffer);
        }
        qrBuffer = "";
      }
    } else if (c >= 0x20 && c < 0x7F) {
      qrBuffer += c;
      if (qrBuffer.length() > 512) qrBuffer = "";  // runaway guard
    }
    // else: drop non-printable byte (UART framing noise)
  }

  // Short delay for loop
  delay(50); 
}

// QR Data Intake
void handleScannedData(String raw) {
  if (sessionInProgress) {
    Serial.println("[TEST] Scan ignored - session already in progress");
    return;
  }
  String uin = extractUin(raw);
  if (uin.length() == 0) {
    Serial.println("[QR] No UIN found in scan");
    return;
  }
  currentName = extractName(raw);  // optional; "" if the payload omits it
  Serial.println("[QR] uin=" + uin + " name=" + currentName);
  handleAuthentication(uin);
}

// Extract "name" field from the QR JSON payload. Returns "" if absent.
String extractName(String raw) {
  int jsonStart = raw.indexOf('{');
  if (jsonStart < 0) return "";
  String json = raw.substring(jsonStart);

  int key = json.indexOf("\"name\"");
  if (key < 0) key = json.indexOf("\"NAME\"");
  if (key < 0) key = json.indexOf("\"Name\"");
  if (key < 0) return "";

  int colon = json.indexOf(':', key);
  if (colon < 0) return "";

  int i = colon + 1;
  while (i < (int)json.length() && (json[i] == ' ' || json[i] == '\t')) i++;
  if (i >= (int)json.length() || json[i] != '"') return "";

  int q2 = json.indexOf('"', i + 1);
  if (q2 < 0) return "";
  return json.substring(i + 1, q2);
}

// Extract "uin"
String extractUin(String raw) {
  // 1) Skip any non-JSON prefix by finding the start of the object
  int jsonStart = raw.indexOf('{');
  if (jsonStart < 0) {
    // Strip non-digits
    String digits = "";
    for (unsigned int i = 0; i < raw.length(); i++) {
      char c = raw[i];
      if (c >= '0' && c <= '9') digits += c;
    }
    return digits;
  }
  String json = raw.substring(jsonStart);

  // 2) Find the "uin" key
  int key = json.indexOf("\"uin\"");
  if (key < 0) key = json.indexOf("\"UIN\"");
  if (key < 0) key = json.indexOf("\"Uin\"");
  if (key < 0) return "";

  // 3) Find the colon after the key
  int colon = json.indexOf(':', key);
  if (colon < 0) return "";

  // 4) Skip whitespace after the colon
  int i = colon + 1;
  while (i < (int)json.length() && (json[i] == ' ' || json[i] == '\t')) i++;
  if (i >= (int)json.length()) return "";

  // 5) Read the value -- "string" or unquoted number
  if (json[i] == '"') {
    int q2 = json.indexOf('"', i + 1);
    if (q2 < 0) return "";
    return json.substring(i + 1, q2);
  } else {
    int j = i;
    while (j < (int)json.length()) {
      char c = json[j];
      if (c == ',' || c == '}' || c == ' ' || c == '\r' || c == '\n' || c == '\t') break;
      j++;
    }
    return json.substring(i, j);
  }
}

// PHASE 1: USER AUTHENTICATION
void handleAuthentication(String uin) {
  sessionInProgress = true;
  currentUin = uin;
  currentState = "VERIFYING";
  // Show the scanned identity on the OLED so the user can confirm we
  // read the right ID before authentication completes.
  String nameLine = currentName.length() ? currentName : String("(unknown name)");
  displayMessage(nameLine, "UIN: " + uin);
  Serial.println("Verifying ID: " + uin);
  delay(3000);
  
  String response = postRequest(String(MOSIP_URL) + "/authenticate", "id=" + uin);
  response.trim();

  if (response == "VALID") {
    currentState = "VALID";
    digitalWrite(greenLed, HIGH);
    beep();  // <-- success beep on valid QR
    displayMessage("ACCESS GRANTED", "Authenticated User.");
    delay(3000);
    digitalWrite(greenLed, LOW);

    // RECYLE or REDEEM user selection
    currentState = "AWAITING_SELECTION";
    displayMessage("Press BLUE: Recycle", "Press RED: Redeem");
    Serial.printf("[SELECT] Waiting up to %lu ms for BLUE/RED\n", SELECTION_WINDOW_MS);

    unsigned long startTime = millis();
    unsigned long blueLowSince = 0, redLowSince = 0;
    unsigned long lastDbg = 0;
    bool selectionMade = false;

    // Wait up to timeout for a button press
    while (millis() - startTime < SELECTION_WINDOW_MS) {
      server.handleClient();

      int blueState = digitalRead(BLUE_BUTTON);
      int redState  = digitalRead(RED_BUTTON);

      // Periodic Serial diagnostic so we can confirm what the buttons read
      // during the wait without flooding the log.
      if (millis() - lastDbg > 500) {
        lastDbg = millis();
        Serial.printf("[SELECT] t=%lums BLUE=%d RED=%d\n",
                      millis() - startTime, blueState, redState);
      }

      if (blueState == LOW) {
        if (blueLowSince == 0) blueLowSince = millis();
        if (millis() - blueLowSince >= DEBOUNCE_MS) {
          Serial.println("[SELECT] BLUE -> RECYCLE_ACTIVE");
          selectionMade = true;
          currentState = "RECYCLE_ACTIVE";
          displayMessage("Chosen: RECYCLE", "Deposit bottle...");
          dispatchTrigger(TRASHCAN_URL);
          postRequest(String(REWARDS_URL) + "/recycle", "uin=" + uin);
          smartDelay(5000);
          break;
        }
      } else {
        blueLowSince = 0;
      }

      if (redState == LOW) {
        if (redLowSince == 0) redLowSince = millis();
        if (millis() - redLowSince >= DEBOUNCE_MS) {
          Serial.println("[SELECT] RED -> REDEEM_ACTIVE");
          selectionMade = true;
          currentState = "REDEEM_ACTIVE";
          displayMessage("Chosen: REDEEM", "Dispensing reward...");
          dispatchTrigger(DISPENSER_URL);
          postRequest(String(REWARDS_URL) + "/redeem", "uin=" + uin);
          smartDelay(5000);
          break;
        }
      } else {
        redLowSince = 0;
      }

      delay(10);
    }

    if (!selectionMade) {
      Serial.println("[SELECT] TIMEOUT");
      currentState = "TIMEOUT";
      displayMessage("Session timeout", "System restarting...");
      delay(2000);
    }
  } else {
    currentState = "INVALID";
    digitalWrite(redLed, HIGH);
    displayMessage("ACCESS DENIED", "Invalid ID.");
    delay(2000);
    digitalWrite(redLed, LOW);
  }

  // Drain the UART RX buffer so we don't immediately re-trigger
  // on stale repeats the moment we return to the main loop
  while (Serial2.available()) Serial2.read();

  currentState = "IDLE";
  currentUin = "";
  currentName = "";
  sessionInProgress = false;
}

// Sends HTTP POST to let another module start now
void dispatchTrigger(const char* baseUrl) {
  Serial.printf("[DISPATCH] POST %s/trigger\n", baseUrl);
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[DISPATCH] Skipped - no WiFi");
    return;
  }
  HTTPClient http;
  http.begin(String(baseUrl) + "/trigger");
  http.setTimeout(1000);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  int code = http.POST("");
  Serial.printf("[DISPATCH] HTTP %d\n", code);
  http.end();
}

// Allows us to wait for a period of time without
// freezing the web server.
// Keeps ESP32 responsive during delays.
// Some notes:
// when we write delay(5000), the ESP32 does nothing and 
// our server stops responding. parang nasa spinlock
// tas hindi makakapoll ng status kasi umiikot lang haha
// eto yung better-ish version ng delay()
// ----> useful para sa orchestrator natin
// Example use case:
//  >> currentState = "RECYCLE_ACTIVE"
//  >> smartDelay(5000);
//  >>> state remains "RECYCLE_ACTIVE" and other systems
//      can see this kasi pwede mapoll yung ESP32 natin!
void smartDelay(unsigned long ms) {
  unsigned long t0 = millis();    // timeout
  while (millis() - t0 < ms) {    // runs until timeout
    server.handleClient();        // process incoming requests
    delay(10);                    // prevents CPU from continuously spinning
  }
}

// Helpers
// bee bee beeeeeeeeeep
void beep() {
  tone(BUZZER_PIN, 1000);
  delay(BEEP_MS);
  noTone(BUZZER_PIN);
}

// Helper for displaying messages
void displayMessage(String line1, String line2) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 10);
  display.println(line1);
  display.setCursor(0, 35);
  display.println(line2);
  display.display();
}

// Helper for POST message to intermediary server.
// On success returns the backend's response body
// On failure returns "ERROR_*: <reason>" so the dashboard can show the cause
String postRequest(String url, String postData) {
  if (url.length() == 0 || !url.startsWith("http")) {
    Serial.println("[POST] aborted: backend URL not resolved");
    return "ERROR_NO_URL: backend mDNS unresolved (reboot ESP32 after avahi is up)";
  }
  if (WiFi.status() != WL_CONNECTED) {
    return "ERROR_NO_WIFI";
  }

  HTTPClient http;
  http.begin(url);
  http.setTimeout(10000);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  int httpResponseCode = http.POST(postData);

  if (httpResponseCode > 0) {
    String payload = http.getString();
    Serial.printf("[POST %s] %d -> %s\n", url.c_str(), httpResponseCode, payload.c_str());
    http.end();
    return payload;
  }
  Serial.printf("[POST %s] failed code=%d\n", url.c_str(), httpResponseCode);
  http.end();
  return "ERROR_HTTP " + String(httpResponseCode) + " (is the Flask backend listening on " + url + "?)";
}
