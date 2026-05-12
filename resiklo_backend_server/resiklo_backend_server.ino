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

// Intermediary PythonAnywhere URL
const char* serverUrl = "http://resiklo.pythonanywhere.com";

// Local backend URLs (Flask servers in backend/)
const char* MOSIP_URL   = "http://192.168.1.42:5001";
const char* REWARDS_URL = "http://192.168.1.42:5000";

// Module URLs
const char* TRASHCAN_URL  = "http://resiklo-bin.local:1145";
const char* DISPENSER_URL = "http://resiklo-dispenser.local:1145";

// Scanner Settings (GM861S)
#define RXD2 4 
#define TXD2 2
// HardwareSerial qrScanner(2); // Use UART2
// String qrData = "";

// OLED Settings
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// Hardware Pins
const int greenLed = 18;
const int redLed = 19;
const int BLUE_BUTTON = 23;
const int RED_BUTTON = 5;

// States
String currentState = "IDLE";
String currentUin   = "";

const char* WELCOME_MSG = "ReSiklo";

// Tuning
const unsigned long SELECTION_WINDOW_MS = 30000;  // Selection window
const unsigned long BEEP_MS = 150;                // Buzzer beep
const unsigned long DEBOUNCE_MS = 30;             // Debounce

WebServer server(80);

// Re-entry guard
volatile bool sessionInProgress = false;

// HTTP Handlers
void handleRoot() {
  String page = "<!doctype html><html><head><title>ReSiklo Identity Verification (TEST)</title></head><body>";
  page += "<h1>ReSiklo Identity Verification</h1>";
  page += "<p>State: <b>" + currentState + "</b></p>";
  page += "<p>Last UIN: " + (currentUin.length() ? currentUin : String("(none)")) + "</p>";
  page += "<p>Backend: MOSIP @ " + String(MOSIP_URL) + " | REWARDS @ " + String(REWARDS_URL) + "</p>";

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
  page += "<p><a href='" + String(REWARDS_URL) + "/audit'>View audit trail (HTML)</a></p>";

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
  String resp = postRequest(String(REWARDS_URL) + "/recycle", body);
  server.send(200, "text/plain",
              "POST /recycle " + body + "\n\n" + resp + "\n\nClick back to return.");
}

// Proxy /test/redeem -> rewards backend /redeem
void handleTestRedeem() {
  String uin    = server.hasArg("uin")    ? server.arg("uin")    : "";
  String points = server.hasArg("points") ? server.arg("points") : "50";
  String body = "uin=" + uin + "&points=" + points;
  String resp = postRequest(String(REWARDS_URL) + "/redeem", body);
  server.send(200, "text/plain",
              "POST /redeem " + body + "\n\n" + resp + "\n\nClick back to return.");
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
  Serial2.begin(9600, SERIAL_8N1, RXD2, TXD2);
  Serial.println("[INIT] QR scanner UART2 ready (9600 baud)");

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

  // MDNS
  if (MDNS.begin("resiklo-backend")) {
    MDNS.addService("http", "tcp", 80);
    Serial.println("[MDNS] resiklo-backend.local advertised on port 80");
  } else {
    Serial.println("[MDNS] FAILED to start");
  }
  
  // Clear buffered data
  while (Serial2.available() > 0) { 
    Serial2.read(); 
  }

  server.on("/",             HTTP_GET,  handleRoot);
  server.on("/state",        HTTP_GET,  handleState);
  server.on("/buttons",      HTTP_GET,  handleButtons);
  server.on("/test/recycle", HTTP_POST, handleTestRecycle);
  server.on("/test/redeem",  HTTP_POST, handleTestRedeem);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("[HTTP] Server started on port 80");
  Serial.println("[READY] System ready");
}

// Main Loop
void loop() {
  // Display idle screen
  displayMessage(serverWelcomeMsg, "Scan your ID...");

  // Smoke-test: paste UIN into Serial Monitor + Enter
  if (Serial.available()) {
    String dummy = Serial.readStringUntil('\n');
    dummy.trim();
    if (dummy.length() >= 1) {
      Serial.println("Serial UIN: " + dummy);
      handleScannedData(dummy);
    }
  }

  // Check for QR data
  if (Serial2.available()) {
    String qrData = Serial2.readStringUntil('\n');
    qrData.trim();
    if (qrData.length() >= 1) {
      handleScannedData(qrData);
    }
  }

  // Short delay for loop
  delay(100); 
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
  handleAuthentication(uin);
}

// Extract "uin"
String extractUin(String raw) {
  int key = raw.indexOf("\"uin\"");
  if (key < 0) {
    String trimmed = raw;
    trimmed.trim();
    return trimmed;
  }
  int colon = raw.indexOf(':', key);
  if (colon < 0) return "";
  int q1 = raw.indexOf('"', colon + 1);
  if (q1 < 0) return "";
  int q2 = raw.indexOf('"', q1 + 1);
  if (q2 < 0) return "";
  return raw.substring(q1 + 1, q2);
}

// PHASE 1: USER AUTHENTICATION
void handleAuthentication(String data) {
  sessionInProgress = true;
  currentUin = uin;
  currentState = "VERIFYING";
  displayMessage("Verifying user...", "Please wait...");
  Serial.println("Verifying ID: " + uin);
  displayMessage("Verifying user...", "Please wait..."); 
  Serial.println("Verifying ID: " + data);
  
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
    bool selectionMade = false;

    // Wait up to timeout for a button press
    while (millis() - startTime < SELECTION_WINDOW_MS) {
      server.handleClient();

      int blueState = digitalRead(BLUE_BUTTON);
      int redState  = digitalRead(RED_BUTTON);

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

  currentState = "IDLE";
  currentUin = "";
  sessionInProgress = false;
}

// Sends HTTP POST to let another module start now
// For testing
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
  digitalWrite(BUZZER_PIN, HIGH);
  delay(BEEP_MS);
  digitalWrite(BUZZER_PIN, LOW);
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

// Helper for POST message to intermediary server
String postRequest(String endpoint, String postData) {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    
    // URL now points to /authenticate
    http.begin(String(serverUrl) + endpoint);
 
    // Set timeout to 35 seconds
    // 5 sec longer than flask_app.py
    http.setTimeout(35000);
    
    // Set the content type for form data
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");

    // Send the POST request with the data in the body
    int httpResponseCode = http.POST(postData);

    if (httpResponseCode > 0) {
      String payload = http.getString();
      http.end();
      // Returns "VALID" or "INVALID"
      return payload;
    }
    http.end();
  }
  return "ERROR";
}
