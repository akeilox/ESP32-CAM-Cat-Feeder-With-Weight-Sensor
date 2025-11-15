/*
  ESP32-CAM Cat feeder with Motion detection, Motor control, Telegram & Home Assistant
  - PIR on GPIO13
  - Motor control on GPIO12 (via relay/MOSFET)
  - SD: saves as /catcam/YYYY-MM-DD_HH-MM-SS.jpg 
  - NTP time sync (Pacific Time with DST)
  - Live stream: http://<IP>:81/stream
  - Manual feed: http://<IP>/feed
  - Telegram commands: /feed, /photo, /status
  - Home Assistant MQTT integration with auto-discovery (read HA_INTEGRATION.md)
  
*/

#include <Arduino.h>
#include <WiFi.h>
#include "esp_camera.h"
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include "FS.h"
#include "SD_MMC.h"
#include <time.h>
#include <ArduinoJson.h>

// ======== USER CONFIG ========
#define WIFI_SSID       "ENTER YOUR WIFI SSID"
#define WIFI_PASS       "ENTER YOU WIFI PASSWORD"
#define TELEGRAM_TOKEN  "ENTER YOUR TELEGRAM TOKEN"
#define TELEGRAM_CHATID "ENTER YOUR CHAT ID"

// MQTT / Home Assistant
#define MQTT_ENABLED        1
#define MQTT_SERVER         "192.168.1.100"  // Your MQTT broker IP
#define MQTT_PORT           1883
#define MQTT_USER           ""               // Leave empty if no auth
#define MQTT_PASS           ""
#define DEVICE_NAME         "cat_feeder"     // Unique device name
#define HA_DISCOVERY_PREFIX "homeassistant"  // Default HA discovery prefix

#define ENABLE_TELEGRAM  1    // 1=send to Telegram, 0=disable
#define ENABLE_SD        1    // 1=save to SD, 0=disable
#define ENABLE_AUTO_FEED 1    // 1=auto feed on motion, 0=motion detection only

// Motor settings
#define MOTOR_PIN               12
#define MOTOR_RUN_TIME_MS       3000
#define MOTOR_COOLDOWN_MS       30000

// =============================

// Pins / camera options
#define PIR_PIN                   13
#define FLASH_PIN                 4
#define PRE_CAPTURE_SETTLE_MS     900
#define FLASH_PULSE_MS            120
#define FRAME_SIZE                FRAMESIZE_SVGA
#define JPEG_QUALITY              12

#define PIR_STABLE_HIGH_MS        40
#define PIR_WARMUP_MS             15000
#define PIR_COOLDOWN_MS           5000

#define WIFI_RETRY_MAX            30
#define TG_SEND_RETRIES           3
#define TG_SOCKET_TIMEOUT_MS      8000
#define FAILS_BEFORE_RESTART      5
#define SD_RETRY_MOUNT            3

// AI Thinker ESP32-CAM pin map
#define PWDN_GPIO_NUM             32
#define RESET_GPIO_NUM            -1
#define XCLK_GPIO_NUM              0
#define SIOD_GPIO_NUM             26
#define SIOC_GPIO_NUM             27
#define Y9_GPIO_NUM               35
#define Y8_GPIO_NUM               34
#define Y7_GPIO_NUM               39
#define Y6_GPIO_NUM               36
#define Y5_GPIO_NUM               21
#define Y4_GPIO_NUM               19
#define Y3_GPIO_NUM               18
#define Y2_GPIO_NUM                5
#define VSYNC_GPIO_NUM            25
#define HREF_GPIO_NUM             23
#define PCLK_GPIO_NUM             22

// Globals
WiFiServer streamServer(81);
WiFiServer webServer(80);
const char* TG_HOST = "api.telegram.org";
WiFiClientSecure tls;
WiFiClient espClient;
PubSubClient mqtt(espClient);

RTC_DATA_ATTR uint32_t photo_counter = 0;
RTC_DATA_ATTR uint32_t feed_counter = 0;
int consecutiveFails = 0;
uint32_t lastFeedTime = 0;
uint32_t lastTelegramCheck = 0;
uint32_t lastMqttPublish = 0;

// MQTT Topics
String mqttBaseTopic = String("homeassistant/") + DEVICE_NAME;
String mqttCommandTopic = mqttBaseTopic + "/feed/set";
String mqttStateTopic = mqttBaseTopic + "/state";
String mqttMotionTopic = mqttBaseTopic + "/motion";
String mqttAvailabilityTopic = mqttBaseTopic + "/availability";

// ---------- MQTT / Home Assistant ----------
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message = "";
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  
  Serial.begin(115200); delay(200);
  proofBlink(2);

  if (!initCamera()) { delay(1000); ESP.restart(); }
  if (!ensureSD()) { Serial.println("[SD] Not mounted; will retry on capture."); }

  ensureWiFi();
  syncTimeOnce();

  // Setup MQTT
  if (MQTT_ENABLED) {
    mqtt.setServer(MQTT_SERVER, MQTT_PORT);
    mqtt.setCallback(mqttCallback);
    mqtt.setBufferSize(1024);
    mqttConnect();
  }

  startStreamServer();
  startWebServer();
  
  Serial.println("\n=== Cat Feeder Ready ===");
  Serial.printf("Web UI: http://%s/\n", WiFi.localIP().toString().c_str());
  Serial.printf("Stream: http://%s:81/stream\n", WiFi.localIP().toString().c_str());
  Serial.println("Telegram: /feed /photo /status");
  if (MQTT_ENABLED) {
    Serial.printf("MQTT: %s:%d\n", MQTT_SERVER, MQTT_PORT);
    Serial.println("Home Assistant: Auto-discovery enabled");
  }
}

void loop() {
  static uint32_t bootStart = millis();
  static uint32_t lastShot  = 0;

  // Serve stream, web interface, MQTT
  streamLoop();
  handleWebClient();
  checkTelegramCommands();
  mqttLoop();

  // Warm-up ignore
  if (millis() - bootStart < PIR_WARMUP_MS) { delay(5); return; }

  // Cooldown
  if (millis() - lastShot < PIR_COOLDOWN_MS) { delay(5); return; }

  // Motion?
  if (motionRisingEdgeStable(PIR_STABLE_HIGH_MS)) {
    lastShot = millis();
    Serial.println("[PIR] Motion → capture");
    
    // Publish motion to Home Assistant
    publishMotionDetected();

    if (!timeIsValid()) syncTimeOnce();

    // Capture
    camera_fb_t *fb = takeFrame();
    if (!fb) {
      Serial.println("[CAP] fb=NULL, reinit camera");
      esp_camera_deinit();
      if (!initCamera()) { Serial.println("[CAM] Reinit failed"); consecutiveFails++; }
      if (consecutiveFails >= FAILS_BEFORE_RESTART) { Serial.println("[SYS] Restarting"); ESP.restart(); }
      delay(5);
      return;
    }

    bool sdOk=false, tgOk=false;

    // Save to SD
    if (ENABLE_SD) {
      if (SD_MMC.cardType()==CARD_NONE) { Serial.println("[SD] Not mounted; trying mount..."); ensureSD(); }
      String path;
      sdOk = saveFrameToSD(fb, path);
    }

    // Send to Telegram
    if (ENABLE_TELEGRAM) {
      if (WiFi.status() != WL_CONNECTED) ensureWiFi();
      tgOk = sendPhotoToTelegram(fb->buf, fb->len);
    }

    esp_camera_fb_return(fb);
    photo_counter++;

    // Auto-feed on motion detection
    if (ENABLE_AUTO_FEED && canFeedNow()) {
      Serial.println("[AUTO] Motion detected - auto feeding");
      runMotor(MOTOR_RUN_TIME_MS);
      if (ENABLE_TELEGRAM) {
        sendTextToTelegram("Auto-fed on motion! 🐱 Feed #" + String(feed_counter));
      }
    }

    if (!sdOk)  Serial.println("[SD] Save failed (ignored if SD disabled)");
    if (!tgOk && ENABLE_TELEGRAM) Serial.println("[TG] Send failed");

    if (sdOk || tgOk) {
      consecutiveFails = 0;
    } else {
      consecutiveFails++;
      if (consecutiveFails >= FAILS_BEFORE_RESTART) {
        Serial.println("[SYS] Too many consecutive failures → restart");
        ESP.restart();
      }
    }
    
    // Update MQTT state after motion event
    publishMqttState();
  }

  delay(5);
}.printf("[MQTT] Received on %s: %s\n", topic, message.c_str());
  
  if (String(topic) == mqttCommandTopic) {
    if (message == "ON" || message == "FEED") {
      if (canFeedNow()) {
        Serial.println("[MQTT] Feed command received");
        runMotor(MOTOR_RUN_TIME_MS);
        publishMqttState();
      } else {
        Serial.println("[MQTT] Feed rejected - cooldown active");
      }
    }
  }
}

bool mqttConnect() {
  if (!MQTT_ENABLED) return false;
  if (mqtt.connected()) return true;
  
  Serial.print("[MQTT] Connecting to broker...");
  
  String clientId = String(DEVICE_NAME) + "_" + String(random(0xffff), HEX);
  
  bool connected;
  if (strlen(MQTT_USER) > 0) {
    connected = mqtt.connect(clientId.c_str(), MQTT_USER, MQTT_PASS, 
                            mqttAvailabilityTopic.c_str(), 0, true, "offline");
  } else {
    connected = mqtt.connect(clientId.c_str(), mqttAvailabilityTopic.c_str(), 
                            0, true, "offline");
  }
  
  if (connected) {
    Serial.println(" OK");
    mqtt.publish(mqttAvailabilityTopic.c_str(), "online", true);
    mqtt.subscribe(mqttCommandTopic.c_str());
    publishHaDiscovery();
    publishMqttState();
    return true;
  } else {
    Serial.printf(" Failed, rc=%d\n", mqtt.state());
    return false;
  }
}

void publishHaDiscovery() {
  if (!MQTT_ENABLED) return;
  
  // Discovery topic: homeassistant/button/cat_feeder/feed/config
  String buttonDiscoveryTopic = String(HA_DISCOVERY_PREFIX) + "/button/" + 
                                DEVICE_NAME + "/feed/config";
  
  StaticJsonDocument<768> doc;
  doc["name"] = "Feed Cat";
  doc["unique_id"] = String(DEVICE_NAME) + "_feed_button";
  doc["command_topic"] = mqttCommandTopic;
  doc["payload_press"] = "FEED";
  doc["availability_topic"] = mqttAvailabilityTopic;
  doc["device"]["identifiers"][0] = DEVICE_NAME;
  doc["device"]["name"] = "Cat Feeder Camera";
  doc["device"]["manufacturer"] = "ESP32-CAM";
  doc["device"]["model"] = "AI-Thinker";
  doc["icon"] = "mdi:food-drumstick";
  
  String discoveryPayload;
  serializeJson(doc, discoveryPayload);
  mqtt.publish(buttonDiscoveryTopic.c_str(), discoveryPayload.c_str(), true);
  
  // Motion sensor discovery
  String motionDiscoveryTopic = String(HA_DISCOVERY_PREFIX) + "/binary_sensor/" + 
                                DEVICE_NAME + "/motion/config";
  
  StaticJsonDocument<768> motionDoc;
  motionDoc["name"] = "Cat Feeder Motion";
  motionDoc["unique_id"] = String(DEVICE_NAME) + "_motion";
  motionDoc["state_topic"] = mqttMotionTopic;
  motionDoc["device_class"] = "motion";
  motionDoc["availability_topic"] = mqttAvailabilityTopic;
  motionDoc["device"]["identifiers"][0] = DEVICE_NAME;
  motionDoc["icon"] = "mdi:motion-sensor";
  
  String motionPayload;
  serializeJson(motionDoc, motionPayload);
  mqtt.publish(motionDiscoveryTopic.c_str(), motionPayload.c_str(), true);
  
  // Feed count sensor discovery
  String countDiscoveryTopic = String(HA_DISCOVERY_PREFIX) + "/sensor/" + 
                               DEVICE_NAME + "/feed_count/config";
  
  StaticJsonDocument<768> countDoc;
  countDoc["name"] = "Cat Feeder Count";
  countDoc["unique_id"] = String(DEVICE_NAME) + "_feed_count";
  countDoc["state_topic"] = mqttStateTopic;
  countDoc["value_template"] = "{{ value_json.feed_count }}";
  countDoc["availability_topic"] = mqttAvailabilityTopic;
  countDoc["device"]["identifiers"][0] = DEVICE_NAME;
  countDoc["icon"] = "mdi:counter";
  
  String countPayload;
  serializeJson(countDoc, countPayload);
  mqtt.publish(countDiscoveryTopic.c_str(), countPayload.c_str(), true);
  
  // Photo count sensor
  String photoDiscoveryTopic = String(HA_DISCOVERY_PREFIX) + "/sensor/" + 
                               DEVICE_NAME + "/photo_count/config";
  
  StaticJsonDocument<768> photoDoc;
  photoDoc["name"] = "Cat Feeder Photos";
  photoDoc["unique_id"] = String(DEVICE_NAME) + "_photo_count";
  photoDoc["state_topic"] = mqttStateTopic;
  photoDoc["value_template"] = "{{ value_json.photo_count }}";
  photoDoc["availability_topic"] = mqttAvailabilityTopic;
  photoDoc["device"]["identifiers"][0] = DEVICE_NAME;
  photoDoc["icon"] = "mdi:camera";
  
  String photoPayload;
  serializeJson(photoDoc, photoPayload);
  mqtt.publish(photoDiscoveryTopic.c_str(), photoPayload.c_str(), true);
  
  Serial.println("[MQTT] Home Assistant discovery published");
}

void publishMqttState() {
  if (!MQTT_ENABLED || !mqtt.connected()) return;
  
  StaticJsonDocument<256> doc;
  doc["feed_count"] = feed_counter;
  doc["photo_count"] = photo_counter;
  doc["uptime"] = millis() / 1000;
  doc["ip"] = WiFi.localIP().toString();
  doc["rssi"] = WiFi.RSSI();
  
  String payload;
  serializeJson(doc, payload);
  mqtt.publish(mqttStateTopic.c_str(), payload.c_str(), true);
}

void publishMotionDetected() {
  if (!MQTT_ENABLED || !mqtt.connected()) return;
  mqtt.publish(mqttMotionTopic.c_str(), "ON", false);
  // Auto clear after 5 seconds (binary sensor)
  delay(100);
  mqtt.publish(mqttMotionTopic.c_str(), "OFF", false);
}

void mqttLoop() {
  if (!MQTT_ENABLED) return;
  
  if (!mqtt.connected()) {
    static uint32_t lastReconnect = 0;
    if (millis() - lastReconnect > 5000) {
      lastReconnect = millis();
      if (ensureWiFi()) {
        mqttConnect();
      }
    }
  } else {
    mqtt.loop();
    
    // Publish state every 30 seconds
    if (millis() - lastMqttPublish > 30000) {
      lastMqttPublish = millis();
      publishMqttState();
    }
  }
}

// ---------- Motor Control ----------
void runMotor(int duration_ms) {
  Serial.printf("[MOTOR] Running for %d ms\n", duration_ms);
  digitalWrite(MOTOR_PIN, HIGH);
  delay(duration_ms);
  digitalWrite(MOTOR_PIN, LOW);
  feed_counter++;
  lastFeedTime = millis();
  Serial.println("[MOTOR] Stopped");
  
  publishMqttState();
}

bool canFeedNow() {
  if (millis() - lastFeedTime < MOTOR_COOLDOWN_MS) {
    Serial.printf("[MOTOR] Cooldown: %lu ms remaining\n", 
                  MOTOR_COOLDOWN_MS - (millis() - lastFeedTime));
    return false;
  }
  return true;
}

// ---------- Helpers ----------
void proofBlink(int n=2,int on=80,int off=120){
  for(int i=0;i<n;i++){ digitalWrite(FLASH_PIN,HIGH); delay(on); digitalWrite(FLASH_PIN,LOW); delay(off); }
}

bool ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  Serial.println("[WiFi] Connecting...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  for (int i=0;i<WIFI_RETRY_MAX;i++){
    if (WiFi.status()==WL_CONNECTED){ 
      Serial.print("[WiFi] IP: "); 
      Serial.println(WiFi.localIP()); 
      return true; 
    }
    delay(250);
  }
  Serial.println("[WiFi] Connect timeout");
  return false;
}

void configureTimezone() {
  setenv("TZ", "PST8PDT,M3.2.0/2,M11.1.0/2", 1);
  tzset();
}

bool timeIsValid() {
  time_t now = time(nullptr);
  return now > 1609459200;
}

void syncTimeOnce() {
  if (timeIsValid()) return; 
  if (!ensureWiFi()) return;
  configureTimezone();
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  Serial.print("[NTP] Syncing");
  for (int i=0; i<30 && !timeIsValid(); i++) { delay(500); Serial.print("."); }
  Serial.println();
  if (timeIsValid()) {
    time_t now = time(nullptr);
    Serial.print("[NTP] Time set: "); Serial.println(ctime(&now));
  }
}

bool initCamera() {
  camera_config_t c;
  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer   = LEDC_TIMER_0;
  c.pin_d0 = Y2_GPIO_NUM;  c.pin_d1 = Y3_GPIO_NUM;  c.pin_d2 = Y4_GPIO_NUM;  c.pin_d3 = Y5_GPIO_NUM;
  c.pin_d4 = Y6_GPIO_NUM;  c.pin_d5 = Y7_GPIO_NUM;  c.pin_d6 = Y8_GPIO_NUM;  c.pin_d7 = Y9_GPIO_NUM;
  c.pin_xclk = XCLK_GPIO_NUM; c.pin_pclk = PCLK_GPIO_NUM; c.pin_vsync = VSYNC_GPIO_NUM; c.pin_href = HREF_GPIO_NUM;
  c.pin_sscb_sda = SIOD_GPIO_NUM; c.pin_sscb_scl = SIOC_GPIO_NUM; c.pin_pwdn = PWDN_GPIO_NUM; c.pin_reset = RESET_GPIO_NUM;
  c.xclk_freq_hz = 20000000;
  c.frame_size   = FRAME_SIZE;
  c.pixel_format = PIXFORMAT_JPEG;
  c.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;
  c.fb_location  = CAMERA_FB_IN_PSRAM;
  c.jpeg_quality = JPEG_QUALITY;
  c.fb_count     = 2;

  Serial.println("[CAM] Init...");
  if (esp_camera_init(&c) != ESP_OK) { Serial.println("[CAM] Init FAILED"); return false; }
  Serial.println("[CAM] Ready");
  return true;
}

bool ensureSD() {
  if (!ENABLE_SD) return true;
  for (int i=0;i<SD_RETRY_MOUNT;i++){
    if (SD_MMC.begin("/sdcard", true)) {
      if (!SD_MMC.exists("/catcam")) SD_MMC.mkdir("/catcam");
      Serial.println("[SD] Mounted (1-bit)");
      return true;
    }
    Serial.println("[SD] Mount failed, retrying...");
    delay(300);
  }
  Serial.println("[SD] Mount FAILED");
  return false;
}

String buildPhotoPath() {
  if (timeIsValid()) {
    time_t now = time(nullptr);
    struct tm t; localtime_r(&now, &t);
    char namebuf[64];
    strftime(namebuf, sizeof(namebuf), "/catcam/%Y-%m-%d_%H-%M-%S.jpg", &t);
    return String(namebuf);
  } else {
    char namebuf[64];
    snprintf(namebuf, sizeof(namebuf), "/catcam/%08lu.jpg", (unsigned long)photo_counter);
    return String(namebuf);
  }
}

bool saveFrameToSD(camera_fb_t* fb, String &outPath) {
  if (!ENABLE_SD) return false;
  if (!fb) return false;

  outPath = buildPhotoPath();

  File f = SD_MMC.open(outPath, FILE_WRITE);
  if (!f) { Serial.println("[SD] Open file failed"); return false; }
  size_t w = f.write(fb->buf, fb->len);
  f.close();
  if (w != fb->len) { Serial.println("[SD] Write incomplete"); return false; }
  Serial.print("[SD] Saved: "); Serial.println(outPath);
  return true;
}

bool sendPhotoToTelegram(uint8_t* buf, size_t len, String caption="") {
  if (!ENABLE_TELEGRAM) return false;
  for (int attempt=1; attempt<=TG_SEND_RETRIES; attempt++){
    if (!ensureWiFi()){ delay(300); continue; }
    tls.setInsecure();
    tls.setTimeout(TG_SOCKET_TIMEOUT_MS);
    Serial.printf("[TG] Connect attempt %d...\n", attempt);
    if (!tls.connect(TG_HOST, 443)) { Serial.println("[TG] TLS connect failed"); tls.stop(); delay(300); continue; }

    if (caption == "") {
      caption = String("Motion from ") + WiFi.localIP().toString() + " • Live: http://" + WiFi.localIP().toString() + ":81/stream";
    }

    String boundary = "----ESP32CAMFormBoundary";
    String head =
      "--" + boundary + "\r\n"
      "Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n" + String(TELEGRAM_CHATID) + "\r\n" +
      "--" + boundary + "\r\n"
      "Content-Disposition: form-data; name=\"caption\"\r\n\r\n" + caption + "\r\n" +
      "--" + boundary + "\r\n"
      "Content-Disposition: form-data; name=\"photo\"; filename=\"cat.jpg\"\r\n"
      "Content-Type: image/jpeg\r\n\r\n";
    String tail = "\r\n--" + boundary + "--\r\n";
    String url = String("/bot") + TELEGRAM_TOKEN + "/sendPhoto";
    String req =
      "POST " + url + " HTTP/1.1\r\n"
      "Host: " + String(TG_HOST) + "\r\n"
      "Content-Type: multipart/form-data; boundary=" + boundary + "\r\n"
      "Content-Length: " + String(head.length() + len + tail.length()) + "\r\n"
      "Connection: close\r\n\r\n";

    if (tls.print(req) == 0 || tls.print(head) == 0) { tls.stop(); delay(200); continue; }
    
    size_t sent = 0, chunk = 1024;
    while (sent < len) {
      size_t n = min(chunk, len - sent);
      int w = tls.write(buf + sent, n);
      if (w <= 0) { tls.stop(); delay(200); goto retry; }
      sent += w;
      yield();
    }
    if (tls.print(tail) == 0) { tls.stop(); delay(200); continue; }

    {
      unsigned long t0 = millis();
      bool ok = false;
      while (millis() - t0 < TG_SOCKET_TIMEOUT_MS) {
        String line = tls.readStringUntil('\n');
        if (line.startsWith("HTTP/1.1 200")) { ok = true; break; }
        if (line.length()==0) break;
      }
      tls.stop();
      if (ok) { Serial.println("[TG] Sent OK"); return true; }
      Serial.println("[TG] No 200; assuming fail");
    }
retry:
    tls.stop(); delay(250);
  }
  Serial.println("[TG] All attempts failed");
  return false;
}

bool sendTextToTelegram(String message) {
  if (!ENABLE_TELEGRAM) return false;
  if (!ensureWiFi()) return false;
  
  tls.setInsecure();
  tls.setTimeout(TG_SOCKET_TIMEOUT_MS);
  
  if (!tls.connect(TG_HOST, 443)) {
    Serial.println("[TG] Text connect failed");
    return false;
  }

  String url = String("/bot") + TELEGRAM_TOKEN + "/sendMessage?chat_id=" + 
               TELEGRAM_CHATID + "&text=" + urlEncode(message);
  
  String req = "GET " + url + " HTTP/1.1\r\n"
               "Host: " + String(TG_HOST) + "\r\n"
               "Connection: close\r\n\r\n";
  
  tls.print(req);
  
  unsigned long t0 = millis();
  bool ok = false;
  while (millis() - t0 < TG_SOCKET_TIMEOUT_MS) {
    String line = tls.readStringUntil('\n');
    if (line.startsWith("HTTP/1.1 200")) { ok = true; break; }
    if (line.length() == 0) break;
  }
  tls.stop();
  return ok;
}

String urlEncode(String str) {
  String encoded = "";
  char c;
  for (int i = 0; i < str.length(); i++) {
    c = str.charAt(i);
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      encoded += c;
    } else if (c == ' ') {
      encoded += '+';
    } else {
      encoded += '%';
      char hex[3];
      sprintf(hex, "%02X", c);
      encoded += hex;
    }
  }
  return encoded;
}

void checkTelegramCommands() {
  if (!ENABLE_TELEGRAM) return;
  if (millis() - lastTelegramCheck < 2000) return;
  lastTelegramCheck = millis();
  
  if (!ensureWiFi()) return;
  
  tls.setInsecure();
  if (!tls.connect(TG_HOST, 443)) return;
  
  String url = String("/bot") + TELEGRAM_TOKEN + "/getUpdates?offset=-1&limit=1&timeout=0";
  String req = "GET " + url + " HTTP/1.1\r\n"
               "Host: " + String(TG_HOST) + "\r\n"
               "Connection: close\r\n\r\n";
  
  tls.print(req);
  
  String response = "";
  unsigned long t0 = millis();
  while (tls.connected() && millis() - t0 < 3000) {
    if (tls.available()) {
      response += tls.readString();
      break;
    }
  }
  tls.stop();
  
  if (response.indexOf("/feed") > 0) {
    Serial.println("[TG] Received /feed command");
    if (canFeedNow()) {
      sendTextToTelegram("Feeding cat... 🐱");
      runMotor(MOTOR_RUN_TIME_MS);
      sendTextToTelegram("Fed! Total feeds: " + String(feed_counter));
    } else {
      sendTextToTelegram("Please wait - cooldown active");
    }
  } 
  else if (response.indexOf("/photo") > 0) {
    Serial.println("[TG] Received /photo command");
    camera_fb_t *fb = takeFrame();
    if (fb) {
      sendPhotoToTelegram(fb->buf, fb->len, "Photo on request");
      esp_camera_fb_return(fb);
    }
  }
  else if (response.indexOf("/status") > 0) {
    Serial.println("[TG] Received /status command");
    String status = "Cat Feeder Status:\n";
    status += "Photos: " + String(photo_counter) + "\n";
    status += "Feeds: " + String(feed_counter) + "\n";
    status += "IP: " + WiFi.localIP().toString() + "\n";
    status += "Stream: http://" + WiFi.localIP().toString() + ":81/stream\n";
    status += "Uptime: " + String(millis()/1000) + "s\n";
    if (MQTT_ENABLED) {
      status += "MQTT: " + String(mqtt.connected() ? "Connected" : "Disconnected");
    }
    sendTextToTelegram(status);
  }
}

void startStreamServer() { streamServer.begin(); Serial.println("[HTTP] Stream on :81/stream"); }

void handleStreamClient(WiFiClient &client) {
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: multipart/x-mixed-replace; boundary=frame");
  client.println("Cache-Control: no-cache");
  client.println("Pragma: no-cache");
  client.println();
  while (client.connected() && WiFi.status()==WL_CONNECTED) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) break;
    client.println("--frame");
    client.println("Content-Type: image/jpeg");
    client.print("Content-Length: "); client.println(fb->len);
    client.println();
    client.write(fb->buf, fb->len);
    client.println();
    esp_camera_fb_return(fb);
    delay(50);
    yield();
  }
  client.stop();
}

void streamLoop() { 
  WiFiClient client = streamServer.available(); 
  if (client) handleStreamClient(client); 
}

void startWebServer() { 
  webServer.begin(); 
  Serial.println("[HTTP] Web server on :80"); 
}

void handleWebClient() {
  WiFiClient client = webServer.available();
  if (!client) return;
  
  String request = client.readStringUntil('\r');
  client.flush();
  
  String response = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n";
  
  if (request.indexOf("/feed") > 0) {
    if (canFeedNow()) {
      runMotor(MOTOR_RUN_TIME_MS);
      response += "<html><body><h1>Fed!</h1><p>Total feeds: " + String(feed_counter) + "</p>";
      response += "<a href='/'>Back</a></body></html>";
    } else {
      response += "<html><body><h1>Please wait</h1><p>Cooldown active</p>";
      response += "<a href='/'>Back</a></body></html>";
    }
  } else {
    response += "<html><head><style>body{font-family:Arial;margin:40px;}</style></head>";
    response += "<body><h1>ESP32-CAM Cat Feeder</h1>";
    response += "<p><strong>Photos:</strong> " + String(photo_counter) + "</p>";
    response += "<p><strong>Feeds:</strong> " + String(feed_counter) + "</p>";
    response += "<p><strong>MQTT:</strong> " + String(mqtt.connected() ? "Connected ✓" : "Disconnected ✗") + "</p>";
    response += "<p><a href='/feed'><button style='padding:20px;font-size:18px;'>Feed Cat Now</button></a></p>";
    response += "<p><a href='http://" + WiFi.localIP().toString() + ":81/stream'>Live Stream</a></p>";
    response += "<p><small>Telegram: /feed /photo /status</small></p>";
    response += "</body></html>";
  }
  
  client.print(response);
  client.stop();
}

camera_fb_t* takeFrame() {
  delay(PRE_CAPTURE_SETTLE_MS);
  digitalWrite(FLASH_PIN, HIGH);
  delay(FLASH_PULSE_MS);
  camera_fb_t *fb = esp_camera_fb_get();
  digitalWrite(FLASH_PIN, LOW);
  return fb;
}

bool motionRisingEdgeStable(uint32_t stable_ms) {
  static bool prevHigh = false;
  bool nowHigh = (digitalRead(PIR_PIN) == HIGH);
  if (!prevHigh && nowHigh) {
    uint32_t t0 = millis();
    while (millis() - t0 < stable_ms) {
      if (digitalRead(PIR_PIN) == LOW) { prevHigh = false; return false; }
      delay(1);
    }
    prevHigh = true; return true;
  }
  if (!nowHigh) prevHigh = false;
  return false;
}

void setup() {
  pinMode(FLASH_PIN, OUTPUT); digitalWrite(FLASH_PIN, LOW);
  pinMode(PIR_PIN, INPUT_PULLDOWN);
  pinMode(MOTOR_PIN, OUTPUT); digitalWrite(MOTOR_PIN, LOW);

  Serial
