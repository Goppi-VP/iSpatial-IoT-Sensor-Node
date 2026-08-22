#include <WiFi.h>
#include <AsyncMqttClient.h>
#include <LittleFS.h>
#include <time.h>
#include <esp_task_wdt.h>

// =========================
// Wi-Fi
// =========================

const char* WIFI_SSID = "YOUR_WIFI_NAME";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// =========================
// MQTT
// =========================

const char* MQTT_SERVER = "broker.hivemq.com";
const uint16_t MQTT_PORT = 1883;

AsyncMqttClient mqttClient;

String deviceID;
String mqttTopic;

// =========================
// ADC / TELEMETRY
// =========================

const int ADC_PIN = 34;

const unsigned long SAMPLE_INTERVAL = 10000;  // 10 seconds
const unsigned long WINDOW_TIME = 60000;      // 60 seconds

unsigned long lastSampleTime = 0;
unsigned long windowStartTime = 0;

int sampleCount = 0;
long sampleSum = 0;
int minimumValue = 4095;
int maximumValue = 0;

// =========================
// MQTT QUEUE STATE
// =========================

bool waitingForAck = false;

uint16_t pendingPacketId = 0;
String pendingFilename = "";

// =========================
// TELEMETRY STRUCT
// =========================

struct TelemetryRecord {
  String timestamp;
  float average;
  int minimum;
  int maximum;
  int count;
};

// =========================
// WIFI / MQTT RECONNECT
// =========================

unsigned long lastWiFiAttempt = 0;
unsigned long lastMQTTAttempt = 0;

const unsigned long RETRY_INITIAL_DELAY = 1000;
const unsigned long RETRY_MAX_DELAY = 30000;

unsigned long wifiRetryDelay = RETRY_INITIAL_DELAY;
unsigned long mqttRetryDelay = RETRY_INITIAL_DELAY;

// =========================
// WATCHDOG
// =========================

const unsigned long WATCHDOG_TIMEOUT_MS = 10000;

// =========================
// NTP
// =========================

bool timeSynchronized = false;

// India = UTC + 5:30
const long GMT_OFFSET_SEC = 19800;
const int DAYLIGHT_OFFSET_SEC = 0;

// =====================================================
// GET TIMESTAMP
// =====================================================

String getTimestamp() {

  struct tm timeinfo;

  if (!getLocalTime(&timeinfo, 1000)) {
    return "TIME_NOT_SYNCED";
  }

  char buffer[25];

  strftime(
    buffer,
    sizeof(buffer),
    "%Y-%m-%d %H:%M:%S",
    &timeinfo
  );

  return String(buffer);
}

// =====================================================
// FIND OLDEST RECORD and Next record
// =====================================================

void findQueueNumbers(int &oldestNumber, int &nextNumber) {

  File root = LittleFS.open("/");

  oldestNumber = -1;
  int highestNumber = 0;

  if (!root || !root.isDirectory()) {
    nextNumber = 1;
    return;
  }

  File file = root.openNextFile();

  while (file) {
    Serial.print("Found file: [");
Serial.print(file.name());
Serial.println("]");

    String filename = file.name();

    if (filename.startsWith("record_") &&
        filename.endsWith(".json")) {

      int start = filename.indexOf("_") + 1;
      int end = filename.indexOf(".json");

      int number =
        filename.substring(start, end).toInt();

      // Lowest number = oldest record
      if (oldestNumber == -1 ||
          number < oldestNumber) {

        oldestNumber = number;
      }

      // Highest number = newest record
      if (number > highestNumber) {

        highestNumber = number;
      }
    }

    file = root.openNextFile();
  }

  // Next filename number
  nextNumber = highestNumber + 1;
}
// =====================================================
// SAVE TELEMETRY TO LITTLEFS
// =====================================================

String saveTelemetry(String json) {

  int oldestNumber;
  int nextNumber;

  findQueueNumbers(oldestNumber, nextNumber);

  char filename[40];

  sprintf(
    filename,
    "/record_%03d.json",
    nextNumber
  );

  File file = LittleFS.open(filename, "w");

  if (!file) {

    Serial.println("Failed to create telemetry file");

    return "";
  }

  file.print(json);
  file.close();

  Serial.print("Stored: ");
  Serial.println(filename);

  return String(filename);
}

// =====================================================
// SEND OLDEST RECORD
// =====================================================

void sendOldestRecord() {

  if (!mqttClient.connected()) {
    return;
  }

  if (waitingForAck) {
    return;
  }

  int oldestNumber;
  int nextNumber;

  findQueueNumbers(oldestNumber, nextNumber);

  if (oldestNumber == -1) {
    return;  // Queue empty
  }

  char filename[40];

  sprintf(
    filename,
    "/record_%03d.json",
    oldestNumber
  );

  File file = LittleFS.open(filename, "r");

  if (!file) {

    Serial.println("Failed to open oldest record");

    return;
  }

  String json = file.readString();

  file.close();

  Serial.println();
  Serial.println("----- SENDING QUEUED RECORD -----");

  Serial.print("File: ");
  Serial.println(filename);

  Serial.print("JSON: ");
  Serial.println(json);

  uint16_t packetId = mqttClient.publish(
    mqttTopic.c_str(),
    1,
    false,
    json.c_str()
  );

  if (packetId == 0) {

    Serial.println("MQTT publish failed");

    return;
  }

  pendingPacketId = packetId;
  pendingFilename = String(filename);

  waitingForAck = true;

  Serial.print("Packet ID: ");
  Serial.println(packetId);

  Serial.println("Waiting for PUBACK...");
}
// =====================================================
// MQTT CONNECTED
// =====================================================

void onMqttConnect(bool sessionPresent) {

  Serial.println();
  Serial.println("MQTT connected");

  Serial.print("Session present: ");
  Serial.println(sessionPresent);

  // Immediately try to send any stored records
  sendOldestRecord();
}

// =====================================================
// MQTT DISCONNECTED
// =====================================================

void onMqttDisconnect(
  AsyncMqttClientDisconnectReason reason
) {

  Serial.println();
  Serial.println("MQTT disconnected");

  // The old in-flight record must remain in LittleFS.
  waitingForAck = false;

  pendingPacketId = 0;
  pendingFilename = "";

  Serial.println("Queued records will be kept.");
}

// =====================================================
// MQTT PUBLISH ACKNOWLEDGEMENT
// =====================================================

void onMqttPublish(
  const uint16_t& packetId
) {

  Serial.println();
  Serial.println("Publish acknowledged!");

  Serial.print("ACK Packet ID: ");
  Serial.println(packetId);

  // VERY IMPORTANT:
  // Delete only the record belonging to this ACK.

  if (waitingForAck &&
      packetId == pendingPacketId) {

    Serial.print("ACK matches: ");
    Serial.println(pendingFilename);

    if (LittleFS.remove(pendingFilename)) {

      Serial.println("Record deleted from LittleFS.");

    } else {

      Serial.println("ERROR: Failed to delete record.");
    }

    pendingFilename = "";

    pendingPacketId = 0;

    waitingForAck = false;

    // Send the next oldest record
    sendOldestRecord();
  }
  else {

    Serial.println("ACK does not match pending record.");
  }
}

// =====================================================
// CONNECT TO MQTT
// =====================================================

void connectToMQTT() {

  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  if (mqttClient.connected()) {
    mqttRetryDelay = RETRY_INITIAL_DELAY;
    return;
  }

  unsigned long now = millis();

  if (now - lastMQTTAttempt < mqttRetryDelay) {
    return;
  }

  lastMQTTAttempt = now;

  Serial.print("MQTT reconnect attempt (delay ");
  Serial.print(mqttRetryDelay);
  Serial.println(" ms)");

  mqttClient.connect();

  if (mqttRetryDelay < RETRY_MAX_DELAY) {
    mqttRetryDelay *= 2;
    if (mqttRetryDelay > RETRY_MAX_DELAY) {
      mqttRetryDelay = RETRY_MAX_DELAY;
    }
  }
}

// =====================================================
// CREATE TELEMETRY RECORD
// =====================================================

void finishTelemetryWindow() {

  if (sampleCount == 0) {
    return;
  }

  TelemetryRecord record;

  record.timestamp = getTimestamp();

  record.average =
    (float)sampleSum / sampleCount;

  record.minimum = minimumValue;

  record.maximum = maximumValue;

  record.count = sampleCount;

  // =========================
  // DISPLAY RECORD
  // =========================

  Serial.println();
  Serial.println("----- TELEMETRY RECORD -----");

  Serial.print("Timestamp: ");
  Serial.println(record.timestamp);

  Serial.print("Average: ");
  Serial.println(record.average);

  Serial.print("Minimum: ");
  Serial.println(record.minimum);

  Serial.print("Maximum: ");
  Serial.println(record.maximum);

  Serial.print("Count: ");
  Serial.println(record.count);

  // =========================
  // CREATE JSON
  // =========================

  String json = "{";

  json += "\"device\":\"";
  json += deviceID;
  json += "\",";

  json += "\"timestamp\":\"";
  json += record.timestamp;
  json += "\",";

  json += "\"average\":";
  json += String(record.average, 3);
  json += ",";

  json += "\"minimum\":";
  json += String(record.minimum);
  json += ",";

  json += "\"maximum\":";
  json += String(record.maximum);
  json += ",";

  json += "\"count\":";
  json += String(record.count);

  json += "}";

  Serial.println(json);

  // =========================
  // ALWAYS STORE FIRST
  // =========================

  String filename = saveTelemetry(json);

  if (filename == "") {
    Serial.println("Telemetry storage failed.");
  }

  // =========================
  // TRY MQTT
  // =========================

  sendOldestRecord();

  // =========================
  // RESET WINDOW
  // =========================

  sampleCount = 0;
  sampleSum = 0;

  minimumValue = 4095;
  maximumValue = 0;

  windowStartTime = millis();

  Serial.println("----------------------------");
}

// =====================================================
// TAKE ADC SAMPLE
// =====================================================

void takeSample() {

  int reading = analogRead(ADC_PIN);

  Serial.print("Reading: ");
  Serial.println(reading);

  sampleSum += reading;

  sampleCount++;

  if (reading < minimumValue) {
    minimumValue = reading;
  }

  if (reading > maximumValue) {
    maximumValue = reading;
  }
}

// =====================================================
// SETUP
// =====================================================

void setup() {

  Serial.begin(115200);

  delay(500);

  Serial.println();
  Serial.println("================================");
  Serial.println("iSpatial IoT Sensor Node");
  Serial.println("================================");

  // =========================
  // DEVICE ID FROM MAC
  // =========================

  String mac = WiFi.macAddress();

  mac.replace(":", "");

  deviceID = "ESP32_" + mac;

  mqttTopic =
    "devices/" +
    deviceID +
    "/telemetry";

  Serial.print("Device ID: ");
  Serial.println(deviceID);

  Serial.print("MQTT Topic: ");
  Serial.println(mqttTopic);

  // =========================
  // LITTLEFS
  // =========================

  if (!LittleFS.begin(true)) {

    Serial.println("LittleFS mount failed");

    return;
  }

  Serial.println("LittleFS mounted");

  // =========================
  // MQTT CALLBACKS
  // =========================

  mqttClient.onConnect(onMqttConnect);

  mqttClient.onDisconnect(onMqttDisconnect);

  mqttClient.onPublish(onMqttPublish);

  mqttClient.setServer(
    MQTT_SERVER,
    MQTT_PORT
  );

  // =========================
  // WIFI
  // =========================

  Serial.print("Connecting to Wi-Fi");

  WiFi.begin(
    WIFI_SSID,
    WIFI_PASSWORD
  );

  // Wait only for initial connection
  while (WiFi.status() != WL_CONNECTED) {

    delay(500);

    Serial.print(".");
  }

  Serial.println();

  Serial.println("Wi-Fi connected");

  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  // =========================
  // NTP
  // =========================

  Serial.println(
    "Waiting for NTP synchronization..."
  );

  configTime(
    GMT_OFFSET_SEC,
    DAYLIGHT_OFFSET_SEC,
    "pool.ntp.org",
    "time.nist.gov"
  );

  struct tm timeinfo;

  if (getLocalTime(&timeinfo, 10000)) {

    timeSynchronized = true;

    Serial.println("NTP synchronized!");

    Serial.println(
      getTimestamp()
    );

  } else {

    Serial.println(
      "NTP synchronization failed"
    );
  }

  // =========================
  // MQTT
  // =========================

  Serial.println(
    "Connecting to MQTT..."
  );

  mqttClient.connect();

  // =========================
  // WATCHDOG
  // =========================

  #if ESP_ARDUINO_VERSION_MAJOR >= 3
    esp_task_wdt_config_t watchdogConfig = {
      .timeout_ms = WATCHDOG_TIMEOUT_MS,
      .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
      .trigger_panic = true
    };

    esp_task_wdt_init(&watchdogConfig);
  #else
    esp_task_wdt_init(WATCHDOG_TIMEOUT_MS / 1000, true);
  #endif

  esp_task_wdt_add(NULL);

  Serial.println("Watchdog enabled (10 seconds)");

  // =========================
  // START TELEMETRY WINDOW
  // =========================

  windowStartTime = millis();

  lastSampleTime = millis();

  Serial.println();
  Serial.println("Combined test started");
}

// =====================================================
// LOOP
// =====================================================

void loop() {

  // Feed watchdog because the main loop is still healthy.
  esp_task_wdt_reset();

  unsigned long now = millis();

  // =========================
  // WIFI RECONNECT
  // =========================

if (WiFi.status() != WL_CONNECTED) {

  if (now - lastWiFiAttempt >= wifiRetryDelay) {

    lastWiFiAttempt = now;

    Serial.print("Wi-Fi reconnect attempt (delay ");
    Serial.print(wifiRetryDelay);
    Serial.println(" ms)");

    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    if (wifiRetryDelay < RETRY_MAX_DELAY) {
      wifiRetryDelay *= 2;

      if (wifiRetryDelay > RETRY_MAX_DELAY) {
        wifiRetryDelay = RETRY_MAX_DELAY;
      }
    }
  }

}

  // =========================
  // MQTT RECONNECT
  // =========================

  connectToMQTT();

  // =========================
  // ADC SAMPLE
  // =========================

  if (now - lastSampleTime >=
      SAMPLE_INTERVAL) {

    lastSampleTime = now;

    takeSample();
  }

  // =========================
  // 60-SECOND WINDOW
  // =========================

  if (now - windowStartTime >=
      WINDOW_TIME) {

    finishTelemetryWindow();
  }
}