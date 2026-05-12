// ReSiklo: ESP32-CAM Camera Server
// 
// Based off the Espressif "CameraWebServer" example
// (app_httpd.cpp, board_config.h, camera_index.h, 
// and camera_pins.h come from there).
// 
// Endpoints:
//  /stream                           - stream consumed by inference_server
//  /control                          - sensor controls
//
// References:
//  - Espressif CameraWebServer example
//    https://github.com/espressif/arduino-esp32/tree/master/libraries/ESP32/examples/Camera/CameraWebServer
//  - ESP32-CAM + YOLO end-to-end
//    https://www.youtube.com/watch?v=npJsmbFZiMg
//  - ESP32-CAM flash LED control
//    https://www.makerguides.com/control-esp32-cam-flash-led/
//  - OV2640 settings reference
//    https://randomnerdtutorials.com/esp32-cam-ov2640-camera-settings/
//  - mDNS
//    https://randomnerdtutorials.com/esp32-mdns-arduino/

#include "esp_camera.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <SPI.h>
#include <Wire.h>

// WiFi credentials
const char *ssid = "netnetnet";
const char *password = "hRkh777r";

// Select camera model in board_config.h
#include "board_config.h"

// mDNS
// Advertise as resiklo-cam.local on the LAN
const char *mdnsHostname = "resiklo-cam";

void startCameraServer();
void setupLedFlash();

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println();
  Serial.println("ReSiklo - Camera Server");

  // CAMERA INIT
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size   = FRAMESIZE_QVGA;       // keep JPEG at a small file size
  config.grab_mode    = CAMERA_GRAB_LATEST;   // never get stale frames
  config.fb_location  = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 25;                   // lower quality
  config.fb_count     = 1;                    // One frame at a time

  // PSRAM settings
  if (config.pixel_format == PIXFORMAT_JPEG) {
    if (psramFound()) {
      config.fb_count = 2;                    // Allow two frames
    } else {
      // Stay in DRAM
      config.fb_location = CAMERA_FB_IN_DRAM;
    }
  } else {
    config.frame_size = FRAMESIZE_240X240;
#if CONFIG_IDF_TARGET_ESP32S3
    config.fb_count = 2;
#endif
  }

#if defined(CAMERA_MODEL_ESP_EYE)
  pinMode(13, INPUT_PULLUP);
  pinMode(14, INPUT_PULLUP);
#endif

  // camera init
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[INIT] Camera init FAILED: 0x%x\n", err);
    return;
  }
  Serial.println("[INIT] Camera ready (QVGA, jpeg_quality=25)");
  
  sensor_t *s = esp_camera_sensor_get();
  // initial sensors are flipped vertically and colors are a bit saturated
  if (s->id.PID == OV3660_PID) {
    s->set_vflip(s, 1);        // flip it back
    s->set_brightness(s, 1);   // up the brightness just a bit
    s->set_saturation(s, -2);  // lower the saturation
  }
  // Lock runtime sensor settings to low-bandwidth defaults
  if (config.pixel_format == PIXFORMAT_JPEG) {
    s->set_framesize(s, FRAMESIZE_QVGA);
    s->set_quality(s, 25);
  }

#if defined(CAMERA_MODEL_M5STACK_WIDE) || defined(CAMERA_MODEL_M5STACK_ESP32CAM)
  s->set_vflip(s, 1);
  s->set_hmirror(s, 1);
#endif

#if defined(CAMERA_MODEL_ESP32S3_EYE)
  s->set_vflip(s, 1);
#endif

// Setup LED FLash if LED pin is defined in camera_pins.h
#if defined(LED_GPIO_NUM)
  setupLedFlash();
#endif

  Serial.printf("[WIFI] Connecting to '%s'...\n", ssid);
  WiFi.begin(ssid, password);
  WiFi.setSleep(false);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.println("[WIFI] Connected, IP: " + WiFi.localIP().toString());

  // Start mDNS
  if (MDNS.begin(mdnsHostname)) {
    MDNS.addService("http", "tcp", 80);
    MDNS.addService("http", "tcp", 81);
    Serial.printf("[MDNS] %s.local advertised on ports 80 (control) and 81 (stream)\n",
                  mdnsHostname);
  } else {
    Serial.println("[MDNS] FAILED to start");
  }

  startCameraServer();
  Serial.println("[HTTP] Camera server started on ports 80 (control) and 81 (stream)");
  Serial.println("[READY] System ready");
}

void loop() {

}
