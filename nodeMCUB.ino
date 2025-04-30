#include <DHT.h>
#include <DHT_U.h>
#include <Wire.h>
#include "Adafruit_TCS34725.h"
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <EEPROM.h>
#include <ArduinoJson.h>

#define DHTPIN D6
#define DHTTYPE DHT22
#define minHumidity 60
#define maxHumidity 80
#define PWM_PIN 2

const char* ssid = "Hidroponik";           // Replace with your Wi-Fi network name
const char* password = "hidrounhas07";   // Replace with your Wi-Fi password
const char* mqtt_server = "103.195.142.180";   // Replace with the IP of the machine running Mosquitto broker
const char* mqtt_topic = "dataPPFDHumidity";   // MQTT topic to publish TDS values
const int mqtt_port = 1883;

float PAR = 0;
int minThreshold; // Variabel global untuk minThreshold
int maxThreshold; // Variabel global untuk maxThreshold
float humidity = 0;

Adafruit_TCS34725 tcs = Adafruit_TCS34725(TCS34725_INTEGRATIONTIME_300MS, TCS34725_GAIN_1X);
DHT dht(DHTPIN, DHTTYPE);
WiFiClient espClient;
PubSubClient client(espClient);

void setup_wifi() {
  Serial.println(" Connecting to Wi-Fi...");
  WiFi.begin(ssid, password);
  unsigned long startAttemptTime = millis();

  while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 10000) { // Timeout 10 detik
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n WiFi terhubung");
  } else {
    Serial.println("\nWiFi gagal terhubung");
  }
}

void reconnect() {
  if (!client.connected()) {
    Serial.println(" Menghubungkan ke MQTT...");
    if (client.connect("NodeMCUClient")) {
      Serial.println(" MQTT terhubung");
      client.subscribe("setThresholdLight");
    } else {
      Serial.print(" MQTT gagal terhubung, rc=");
      Serial.println(client.state());
      delay(2000); // Tunggu 2 detik sebelum mencoba lagi
    }
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  // Buat buffer untuk menyimpan payload
  char buffer[length + 1];
  strncpy(buffer, (char*)payload, length);
  buffer[length] = '\0'; // Tambahkan null terminator

  // Parse JSON menggunakan ArduinoJson
  StaticJsonDocument<200> doc;
  DeserializationError error = deserializeJson(doc, buffer);

  if (error) {
    Serial.print(" JSON Parsing failed: ");
    Serial.println(error.c_str());
    return;
  }

  // Ambil nilai minThreshold dan maxThreshold dari JSON
  if (doc.containsKey("minThreshold") && doc.containsKey("maxThreshold")) {
    int minThreshold = doc["minThreshold"];
    int maxThreshold = doc["maxThreshold"];
    
    Serial.print(" minThreshold: ");
    Serial.println(minThreshold);
    Serial.print(" maxThreshold: ");
    Serial.println(maxThreshold);

    // Simpan ke EEPROM
    EEPROM.put(0, minThreshold);  // Simpan minThreshold di alamat 0
    EEPROM.put(sizeof(minThreshold), maxThreshold); // Simpan maxThreshold setelahnya
    EEPROM.commit();
    Serial.println(" Thresholds saved to EEPROM.");
  } else {
    Serial.println(" Keys 'minThreshold' or 'maxThreshold' not found in payload.");
  }
}

void setup() {
  // put your setup code here, to run once:
  Serial.begin(115200);
  EEPROM.begin(512);

  setup_wifi();
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(mqttCallback);

  EEPROM.get(0, minThreshold);
  EEPROM.get(sizeof(minThreshold), maxThreshold);

  if (tcs.begin()) {
    Serial.println("Sensor TCS34725 terdeteksi.");
  } else {
    Serial.println("Sensor TCS34725 tidak terdeteksi. Cek Kabel.");
    while (1); // Hentikan program
  }
  dht.begin();
}

float readPAR() {
  uint16_t r, g, b, c;
  float white, PAR;
  tcs.getRawData(&r, &g, &b, &c);
  white = (r + g + b) / 3.0;
  PAR = (white * 0.65847) + (r * 1.60537) + (g * 2.30216) + (b * 0.50019);
  return PAR;
}

float readHumidity() {
  return dht.readHumidity();
}

// Fuzzy Mamdani
int fuzzyPWM(float humidity, float light, int minThreshold, int maxThreshold) {
  // Fuzzifikasi Kelembapan
  float lowHumidity = constrain((maxHumidity - humidity) / (maxHumidity - minHumidity), 0, 1);
  float highHumidity = constrain((humidity - minHumidity) / (maxHumidity - minHumidity), 0, 1);

  // Fuzzifikasi PAR
  float dimLight = constrain((maxThreshold - light) / (maxThreshold - minThreshold), 0, 1);
  float brightLight = constrain((light - minThreshold) / (maxThreshold - minThreshold), 0, 1);

  // aturan Inferensi
  // PAR Tinggi & Kelembapan Rendah -> Output Rendah
  float pwmLow = min(brightLight, lowHumidity) * 85; 
   // PAR Rendah & Kelembapan Rendah / PAR Tinggi & Kelembapan Tinggi -> Output Sedang
  float pwmMed = min(dimLight, lowHumidity) * 170 + min(brightLight, highHumidity) * 170;
  // PAR Rendah & Kelembapan Tinggi -> Output Tinggi
  float pwmHigh = min(dimLight, highHumidity) * 255; 

  // Aggregasi
  float numerator = pwmLow + pwmMed + pwmHigh;
  float denominator = 
    (min(brightLight, lowHumidity)) +  // Aturan: PAR Tinggi & Kelembapan Rendah
    (min(dimLight, lowHumidity)) +    // Aturan: PAR Rendah & Kelembapan Rendah
    (min(brightLight, highHumidity)) + // Aturan: PAR Tinggi & Kelembapan Tinggi
    (min(dimLight, highHumidity));     // Aturan: PAR Rendah & Kelembapan Tinggi

  if (denominator == 0) return 0;

  return (int)(numerator / denominator);
}

void loop() {
  // Periksa dan hubungkan Wi-Fi jika tidak terhubung
  if (WiFi.status() != WL_CONNECTED) {
    setup_wifi(); // Coba sambungkan kembali ke Wi-Fi
  }

  // Periksa dan hubungkan MQTT jika tidak terhubung
  if (WiFi.status() == WL_CONNECTED && !client.connected()) {
    reconnect(); // Coba sambungkan kembali ke MQTT
  }

  // Loop MQTT jika terhubung
  if (client.connected()) {
    client.loop();
  }

  float humidity = dht.readHumidity();
  PAR = readPAR();
  int pwmValue = fuzzyPWM(humidity, PAR, minThreshold, maxThreshold);

  analogWrite(PWM_PIN, pwmValue);

  Serial.print(" PAR: "); Serial.print(PAR, 2);
  Serial.println(" µmol/m²/s"); // Satuan PAR
  Serial.print(" Kelembapan: "); Serial.print(humidity, 1);
  Serial.println(" %");

  String data = String(PAR) + "|" + String(humidity);
  client.publish(mqtt_topic,String(data).c_str(), true);
  delay(1000); // Delay 1 detik
}
