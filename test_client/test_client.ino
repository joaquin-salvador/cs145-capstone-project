// This is a sample client, based off CS 145 Lab Exercises
// We display the results from the inference server here!

#include <WiFi.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include <ESPmDNS.h>

#ifndef STASSID
#define STASSID "netnetnet"
#define STAPSK "hRkh777r"
#endif

const char * ssid = STASSID;
const char * password = STAPSK;

WebServer server(1145);
const int LDR = 34;
int reading = 888;
String lastClassification = "No detection yet";
String ledStatus = "sarado pa...";

void handleRoot() {
  reading = analogRead(LDR);
  String page = R"(
<html>
  <head>
    <title>Bottle Classification</title>
    <script>
      setInterval(function() {
        fetch('/status')
          .then(r => r.json())
          .then(data => {
            document.getElementById('detection').innerText = data.label;
            document.getElementById('led').innerText = data.led;  // ADD THIS
          });
      }, 1000);
    </script>
  </head>
  <body>
    <p>YOLO Detection: <strong><span id="detection">)" + lastClassification + R"(</span></strong></p>
    <p>kunwari LED ito: <strong><span id="led">)" + ledStatus + R"(</span></strong></p>
  </body>
</html>
)";
  server.send(200, "text/html", page);
}

void handleStatus() {
  String json = "{\"label\":\"" + lastClassification + "\",\"led\":\"" + ledStatus + "\"}";
  server.send(200, "application/json", json);
}

void handleUpdate() {
  if (server.hasArg("label")) {
    lastClassification = server.arg("label");
    Serial.println("Classification updated: " + lastClassification);
  }
  if (server.hasArg("led")) {
    ledStatus = server.arg("led");
    Serial.println("LED status updated: " + ledStatus);
  }
  server.send(200, "text/plain", "OK");
}

void handleNotFound() {
  server.send(404, "text/plain", "PAGE NOT FOUND\n\n");
}

void setup(void) {
  Serial.begin(115200);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println("\nConnected! IP: " + WiFi.localIP().toString());

  MDNS.begin("esp32");
  server.on("/", handleRoot);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/update", HTTP_POST, handleUpdate);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("HTTP server started");
}

void loop(void) {
  server.handleClient();
}
