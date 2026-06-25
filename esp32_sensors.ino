/*
  Smart Water Cooler — ESP32 Sensor Node
  Sensors: pH, TDS, Turbidity, Temperature (DS18B20), Chlorine, Pressure
  Protocol: MQTT over Wi-Fi
  Broker: HiveMQ Cloud (or any MQTT broker)

  Libraries needed (install via Arduino Library Manager):
  - PubSubClient by Nick O'Leary
  - ArduinoJson by Benoit Blanchon
  - OneWire by Paul Stoffregen
  - DallasTemperature by Miles Burton
*/

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// ─── Wi-Fi credentials ───────────────────────────────────────────────────────
const char* WIFI_SSID     = "YOUR_WIFI_NAME";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// ─── MQTT broker settings ────────────────────────────────────────────────────
// For HiveMQ Cloud: get these from your HiveMQ Cloud dashboard
const char* MQTT_BROKER   = "YOUR_HIVEMQ_BROKER_URL";  // e.g. abc123.s1.eu.hivemq.cloud
const int   MQTT_PORT     = 8883;                        // 8883 for TLS, 1883 for plain
const char* MQTT_USER     = "YOUR_MQTT_USERNAME";
const char* MQTT_PASSWORD = "YOUR_MQTT_PASSWORD";
const char* MQTT_TOPIC    = "watercooler/sensors";
const char* MQTT_CLIENT_ID = "ESP32_WaterCooler_01";

// ─── Pin definitions ─────────────────────────────────────────────────────────
#define PH_PIN          34    // Analog pin for pH sensor
#define TDS_PIN         35    // Analog pin for TDS sensor
#define TURBIDITY_PIN   32    // Analog pin for turbidity sensor
#define CHLORINE_PIN    33    // Analog pin for chlorine sensor
#define PRESSURE_PIN    36    // Analog pin for pressure sensor
#define TEMP_PIN        4     // Digital pin for DS18B20 temperature sensor

// ─── Sensor calibration constants ────────────────────────────────────────────
// pH sensor (adjust based on your calibration)
#define PH_OFFSET       0.0   // Calibration offset
#define VREF            3.3   // ESP32 ADC reference voltage

// TDS sensor
#define TDS_FACTOR      0.5   // Conversion factor

// Publish interval (milliseconds)
#define PUBLISH_INTERVAL 2000  // Send data every 2 seconds

// ─── Objects ─────────────────────────────────────────────────────────────────
OneWire oneWire(TEMP_PIN);
DallasTemperature tempSensor(&oneWire);
WiFiClient espClient;
PubSubClient mqttClient(espClient);

unsigned long lastPublishTime = 0;

// ─── Wi-Fi connection ─────────────────────────────────────────────────────────
void connectWiFi() {
  Serial.print("Connecting to Wi-Fi: ");
  Serial.println(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWi-Fi connected! IP: " + WiFi.localIP().toString());
}

// ─── MQTT connection ──────────────────────────────────────────────────────────
void connectMQTT() {
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  while (!mqttClient.connected()) {
    Serial.print("Connecting to MQTT broker...");
    if (mqttClient.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD)) {
      Serial.println(" connected!");
    } else {
      Serial.print(" failed (rc=");
      Serial.print(mqttClient.state());
      Serial.println("). Retrying in 3s...");
      delay(3000);
    }
  }
}

// ─── Read pH ─────────────────────────────────────────────────────────────────
float readPH() {
  int raw = analogRead(PH_PIN);
  float voltage = (raw / 4095.0) * VREF;
  // Standard pH sensor formula: pH = -5.70 * voltage + 21.34
  // Adjust slope/intercept based on your calibration buffer solutions
  float pH = -5.70 * voltage + 21.34 + PH_OFFSET;
  return constrain(pH, 0.0, 14.0);
}

// ─── Read TDS ────────────────────────────────────────────────────────────────
float readTDS(float temperature) {
  int raw = analogRead(TDS_PIN);
  float voltage = (raw / 4095.0) * VREF;
  // Temperature compensation
  float compensationCoeff = 1.0 + 0.02 * (temperature - 25.0);
  float compVoltage = voltage / compensationCoeff;
  // TDS formula
  float tds = (133.42 * pow(compVoltage, 3)
             - 255.86 * pow(compVoltage, 2)
             + 857.39 * compVoltage) * TDS_FACTOR;
  return max(0.0f, tds);
}

// ─── Read turbidity ───────────────────────────────────────────────────────────
float readTurbidity() {
  int raw = analogRead(TURBIDITY_PIN);
  float voltage = (raw / 4095.0) * VREF;
  // Turbidity sensor (SEN0189): lower voltage = more turbid
  // Typical: 4.1V = clear (0 NTU), lower = turbid
  float ntu = -1120.4 * pow(voltage, 2) + 5742.3 * voltage - 4353.8;
  return max(0.0f, ntu);
}

// ─── Read temperature ─────────────────────────────────────────────────────────
float readTemperature() {
  tempSensor.requestTemperatures();
  float temp = tempSensor.getTempCByIndex(0);
  if (temp == DEVICE_DISCONNECTED_C) return -999.0;
  return temp;
}

// ─── Read chlorine ────────────────────────────────────────────────────────────
float readChlorine() {
  int raw = analogRead(CHLORINE_PIN);
  float voltage = (raw / 4095.0) * VREF;
  // Map voltage to chlorine concentration (mg/L)
  // Adjust based on your specific sensor datasheet
  float chlorine = voltage * 2.0;
  return max(0.0f, chlorine);
}

// ─── Read pressure ────────────────────────────────────────────────────────────
float readPressure() {
  int raw = analogRead(PRESSURE_PIN);
  float voltage = (raw / 4095.0) * VREF;
  // Map 0.5V–4.5V to 0–1.2 MPa (adjust for your sensor range)
  float pressure = (voltage - 0.5) / (4.5 - 0.5) * 1.2;
  return max(0.0f, pressure);
}

// ─── Determine water quality status ──────────────────────────────────────────
String getStatus(float pH, float tds, float turbidity, float chlorine) {
  if (pH < 6.5 || pH > 8.5)     return "WARNING";
  if (tds > 500)                  return "WARNING";
  if (turbidity > 4.0)            return "WARNING";
  if (chlorine < 0.2 || chlorine > 4.0) return "WARNING";
  return "SAFE";
}

// ─── Publish sensor data via MQTT ─────────────────────────────────────────────
void publishData() {
  float temperature = readTemperature();
  float pH          = readPH();
  float tds         = readTDS(temperature);
  float turbidity   = readTurbidity();
  float chlorine    = readChlorine();
  float pressure    = readPressure();
  String status     = getStatus(pH, tds, turbidity, chlorine);

  // Build JSON payload
  StaticJsonDocument<256> doc;
  doc["device_id"]    = MQTT_CLIENT_ID;
  doc["timestamp"]    = millis();
  doc["pH"]           = round(pH * 100.0) / 100.0;
  doc["tds"]          = round(tds * 10.0) / 10.0;
  doc["turbidity"]    = round(turbidity * 100.0) / 100.0;
  doc["temperature"]  = round(temperature * 10.0) / 10.0;
  doc["chlorine"]     = round(chlorine * 100.0) / 100.0;
  doc["pressure"]     = round(pressure * 100.0) / 100.0;
  doc["status"]       = status;

  char payload[256];
  serializeJson(doc, payload);

  if (mqttClient.publish(MQTT_TOPIC, payload)) {
    Serial.println("Published: " + String(payload));
  } else {
    Serial.println("Publish failed!");
  }
}

// ─── Setup ────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  tempSensor.begin();
  connectWiFi();
  connectMQTT();
  Serial.println("Smart Water Cooler sensor node started.");
}

// ─── Loop ─────────────────────────────────────────────────────────────────────
void loop() {
  if (!mqttClient.connected()) connectMQTT();
  mqttClient.loop();

  unsigned long now = millis();
  if (now - lastPublishTime >= PUBLISH_INTERVAL) {
    lastPublishTime = now;
    publishData();
  }
}
