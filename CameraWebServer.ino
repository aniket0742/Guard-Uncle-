#include "esp_camera.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <base64.h>
#include "esp_timer.h"
#include "img_converters.h"
#include "fb_gfx.h"
#include "esp32-hal-ledc.h"
#include "esp_http_server.h"

// ───── WiFi Credentials ─────
const char* ssid     = "xxxxx";
const char* password = "xxxxx";

// ───── Telegram Bot ─────
const char* botToken = "xxxxxx";
const char* chatID   = "xxxxxx";

// ───── Imagga API ─────
String imagga_user   = "xxxxxxxxx";
String imagga_secret = "xxxxxxxxx";

// ───── Pins ─────
#define PIR_PIN         13
#define FLASH_PIN       4
#define DOOR_SENSOR_PIN 12

WiFiClientSecure clientTCP;
unsigned long lastMotionTime = 0;
const unsigned long motionCooldown = 15000;
#define DEBUG true

// ───── Forward Declarations ─────
String captureAndLabel(camera_fb_t* fb);
void sendPhotoTelegram(camera_fb_t* fb, String caption);
void startLiveCameraServer();

// ───── Setup ─────
void setup() {
  Serial.begin(115200);
  pinMode(PIR_PIN, INPUT);
  pinMode(DOOR_SENSOR_PIN, INPUT_PULLUP); // Magnetic switch
  pinMode(FLASH_PIN, OUTPUT);
  digitalWrite(FLASH_PIN, LOW);

  WiFi.begin(ssid, password);
  WiFi.setSleep(false);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n✅ WiFi Connected: " + WiFi.localIP().toString());

  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = 5;
  config.pin_d1       = 18;
  config.pin_d2       = 19;
  config.pin_d3       = 21;
  config.pin_d4       = 36;
  config.pin_d5       = 39;
  config.pin_d6       = 34;
  config.pin_d7       = 35;
  config.pin_xclk     = 0;
  config.pin_pclk     = 22;
  config.pin_vsync    = 25;
  config.pin_href     = 23;
  config.pin_sscb_sda = 26;
  config.pin_sscb_scl = 27;
  config.pin_pwdn     = 32;
  config.pin_reset    = -1;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size   = FRAMESIZE_QVGA;
  config.jpeg_quality = 12;
  config.fb_count     = 2;
  config.fb_location  = CAMERA_FB_IN_PSRAM;

  if (esp_camera_init(&config) != ESP_OK) {
    Serial.println("❌ Camera init failed");
    return;
  }

  Serial.println("📷 Camera Ready");
  startLiveCameraServer();
}

// ───── Loop ─────
void loop() {
  static bool lastDoorState = HIGH;
  bool doorState = digitalRead(DOOR_SENSOR_PIN);

  // Door Sensor Trigger
  if (doorState != lastDoorState) {
    lastDoorState = doorState;

    if (doorState == HIGH) {
      Serial.println("🚪 Door/Window OPENED!");

      camera_fb_t* fb = esp_camera_fb_get();
      if (fb) {
        sendPhotoTelegram(fb, "🚪 Door/Window OPENED!");
        esp_camera_fb_return(fb);
      }
    } else {
      Serial.println("🔒 Door/Window CLOSED");
    }

    delay(100); // Debounce
  }

  // PIR Motion Detection
  if (digitalRead(PIR_PIN) == HIGH && millis() - lastMotionTime > motionCooldown) {
    lastMotionTime = millis();
    Serial.println("🚨 Motion detected!");

    digitalWrite(FLASH_PIN, HIGH);
    delay(300);

    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("❌ Camera capture failed");
      digitalWrite(FLASH_PIN, LOW);
      return;
    }

    digitalWrite(FLASH_PIN, LOW);

    String label = captureAndLabel(fb);
    String caption;

    if (label == "person") caption = "🚶 Person Detected!";
    else if (label == "animal") caption = "🐾 Animal Detected!";
    else if (label == "imagga_fail" || label == "wifi_fail" || label == "json_parse_fail")
      caption = "⚠ Motion detected, but classification failed!";
    else
      caption = "⚠ Motion Detected (Unknown)";

    sendPhotoTelegram(fb, caption);
    esp_camera_fb_return(fb);
  }
}

// ───── Send Photo to Telegram ─────
void sendPhotoTelegram(camera_fb_t* fb, String caption) {
  if (WiFi.status() != WL_CONNECTED || !fb) return;

  clientTCP.stop();
  clientTCP.setInsecure();
  if (!clientTCP.connect("api.telegram.org", 443)) {
    Serial.println("❌ Telegram connection failed");
    return;
  }

  String boundary = "ESP32CAMBOUNDARY";
  String startRequest = "--" + boundary + "\r\n";
  startRequest += "Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n";
  startRequest += String(chatID) + "\r\n--" + boundary + "\r\n";
  startRequest += "Content-Disposition: form-data; name=\"caption\"\r\n\r\n";
  startRequest += caption + "\r\n--" + boundary + "\r\n";
  startRequest += "Content-Disposition: form-data; name=\"photo\"; filename=\"image.jpg\"\r\n";
  startRequest += "Content-Type: image/jpeg\r\n\r\n";
  String endRequest = "\r\n--" + boundary + "--\r\n";

  int contentLength = startRequest.length() + fb->len + endRequest.length();

  String headers = "POST /bot" + String(botToken) + "/sendPhoto HTTP/1.1\r\n";
  headers += "Host: api.telegram.org\r\n";
  headers += "Content-Type: multipart/form-data; boundary=" + boundary + "\r\n";
  headers += "Content-Length: " + String(contentLength) + "\r\n\r\n";

  clientTCP.print(headers);
  clientTCP.print(startRequest);
  clientTCP.write(fb->buf, fb->len);
  clientTCP.print(endRequest);

  delay(500);
  clientTCP.stop();
  Serial.println("✅ Sent to Telegram");
}

// ───── Image Labeling ─────
String captureAndLabel(camera_fb_t* fb) {
  if (!fb || fb->len == 0) return "no_image";

  String encoded = base64::encode(fb->buf, fb->len);

  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin("https://api.imagga.com/v2/tags");
    String auth = base64::encode((imagga_user + ":" + imagga_secret).c_str());
    http.addHeader("Authorization", "Basic " + auth);
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    String body = "image_base64=" + encoded;

    int code = http.POST(body);
    String response = http.getString();
    http.end();

    if (DEBUG) {
      Serial.println("[IMAGGA] Code: " + String(code));
      Serial.println("[IMAGGA] Response: " + response);
    }

    if (code != 200) return "imagga_fail";

    DynamicJsonDocument doc(2048);
    if (deserializeJson(doc, response)) return "json_parse_fail";

    JsonArray tags = doc["result"]["tags"];
    for (JsonObject tag : tags) {
      String label = tag["tag"]["en"];
      label.toLowerCase();
      if (label.indexOf("person") != -1 || label.indexOf("man") != -1 || label.indexOf("woman") != -1)
        return "person";
      if (label.indexOf("dog") != -1 || label.indexOf("cat") != -1 || label.indexOf("animal") != -1)
        return "animal";
    }

    return "no_person";
  }

  return "wifi_fail";
}

// ───── Live Stream Server ─────
static esp_err_t stream_handler(httpd_req_t* req) {
  camera_fb_t* fb = NULL;
  esp_err_t res = ESP_OK;
  char* part_buf[64];

  static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=frame";
  static const char* _STREAM_BOUNDARY = "\r\n--frame\r\n";
  static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

  res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
  if (res != ESP_OK) return res;

  while (true) {
    fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("Camera capture failed");
      res = ESP_FAIL;
    } else {
      size_t hlen = snprintf((char*)part_buf, 64, _STREAM_PART, fb->len);
      res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
      res |= httpd_resp_send_chunk(req, (const char*)part_buf, hlen);
      res |= httpd_resp_send_chunk(req, (const char*)fb->buf, fb->len);
      res |= httpd_resp_send_chunk(req, NULL, 0);
      esp_camera_fb_return(fb);
    }
    if (res != ESP_OK) break;
    delay(100);
  }

  return res;
}

void startLiveCameraServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;

  httpd_uri_t stream_uri = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = stream_handler,
    .user_ctx  = NULL
  };

  httpd_handle_t stream_httpd = NULL;
  if (httpd_start(&stream_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(stream_httpd, &stream_uri);
    Serial.println("🌐 Streaming ready: http://" + WiFi.localIP().toString());
  }
}