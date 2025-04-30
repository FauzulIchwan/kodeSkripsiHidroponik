#include <Arduino.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <stdlib.h>
#include <EEPROM.h>
#include <ArduinoJson.h>

#define ONE_WIRE_BUS D0
#define VREF 3.3
#define SCOUNT  30

const int tdsPin = A0; // Pin analog NodeMCU (input dari multiplexer)

// Pin untuk mengontrol multiplexer
const int s0 = D3;
const int s1 = D4;
const int s2 = D5;
const int s3 = D6;
const int LIQUID_LEVEL_PIN_A = D7;
const int LIQUID_LEVEL_PIN_B = D8;

const int relayPompa1 = D1; // Relay pompa 1
const int relayPompa2 = D2; // Relay pompa 2

int minThreshold = 800; // Nilai ambang batas bawah PPM
int maxThreshold = 1000; // Nilai ambang batas bawah PPM

float temperature = 25;
int liquidLevelA = 0; //deteksi nutrisi A
int liquidLevelB = 0; //deteksi nutrisi B
int tdsValue = 0; // nilai tds
float slope = -5.7;  // kalibrasi pH
float offset = 16.0; // kalibrasi pH
int statusPompa = 0; // 1=menyala

const char* ssid = "Hidroponik";          
const char* password = "hidrounhas07";
const char* mqtt_server = "103.195.142.180";
const char* mqtt_topic = "dataNodeMCUA";
const int mqtt_port = 1883;

WiFiClient espClient;
PubSubClient client(espClient);
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

// Konfigurasi TDS
int analogBuffer[SCOUNT];     // store the analog value in the array, read from ADC
int analogBufferTemp[SCOUNT];
int analogBufferIndex = 0;
int copyIndex = 0;

float averageVoltage = 0;

// median filtering algorithm
int getMedianNum(int bArray[], int iFilterLen){
  int bTab[iFilterLen];
  for (byte i = 0; i<iFilterLen; i++)
  bTab[i] = bArray[i];
  int i, j, bTemp;
  for (j = 0; j < iFilterLen - 1; j++) {
    for (i = 0; i < iFilterLen - j - 1; i++) {
      if (bTab[i] > bTab[i + 1]) {
        bTemp = bTab[i];
        bTab[i] = bTab[i + 1];
        bTab[i + 1] = bTemp;
      }
    }
  }
  if ((iFilterLen & 1) > 0){
    bTemp = bTab[(iFilterLen - 1) / 2];
  }
  else {
    bTemp = (bTab[iFilterLen / 2] + bTab[iFilterLen / 2 - 1]) / 2;
  }
  return bTemp;
}

void setup_wifi() {
  Serial.println("Connecting to Wi-Fi...");
  WiFi.begin(ssid, password);
  unsigned long startAttemptTime = millis();

  while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 10000) { // Timeout 10 detik
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi terhubung");
  } else {
    Serial.println("\nWiFi gagal terhubung");
  }
}

void reconnect() {
  if (!client.connected()) {
    Serial.println("Menghubungkan ke MQTT...");
    if (client.connect("NodeMCUClient")) {
      Serial.println("MQTT terhubung");
    } else {
      Serial.print("MQTT gagal terhubung, rc=");
      Serial.println(client.state());
    }
  }
}

void setup() {
  Serial.begin(115200);
  // Connect to Wi-Fi
  setup_wifi();

  // Set up MQTT
  client.setServer(mqtt_server, mqtt_port);

  // Konfigurasi pin multiplexer
  pinMode(s0, OUTPUT);
  pinMode(s1, OUTPUT);
  pinMode(s2, OUTPUT);
  pinMode(s3, OUTPUT);
  pinMode(LIQUID_LEVEL_PIN_A, INPUT_PULLUP);
  pinMode(LIQUID_LEVEL_PIN_B, INPUT_PULLUP);

  // Konfigurasi pin relay
  pinMode(relayPompa1, OUTPUT);
  pinMode(relayPompa2, OUTPUT);

  // Matikan pompa saat startup
  digitalWrite(relayPompa1, HIGH); // HIGH untuk relay optocoupler dalam mode default (NO)
  digitalWrite(relayPompa2, HIGH);
}

void selectChannel(int channel) {
  digitalWrite(s0, channel & 0x01);
  digitalWrite(s1, (channel >> 1) & 0x01);
  digitalWrite(s2, (channel >> 2) & 0x01);
  digitalWrite(s3, (channel >> 3) & 0x01);
}

float readTDS(float temperature) {
  selectChannel(15);
  delay(10);

  static unsigned long analogSampleTimepoint = millis();

  // Pembacaan sensor setiap 40 ms
  if (millis() - analogSampleTimepoint > 40U) {
    analogSampleTimepoint = millis();
    analogBuffer[analogBufferIndex] = analogRead(tdsPin); // Baca nilai analog
    analogBufferIndex++;
    if (analogBufferIndex == SCOUNT) {
      analogBufferIndex = 0;
    }
  }

  // Pengolahan data setiap 800 ms
  static unsigned long processTimepoint = millis();
  if (millis() - processTimepoint > 800U) {
    processTimepoint = millis();
    averageVoltage = getMedianNum(analogBuffer, SCOUNT) * (float)VREF / 1024.0;
    float compensationCoefficient = 1.0 + 0.02 * (temperature - 25.0);
    float compensationVoltage = averageVoltage / compensationCoefficient;
    float tdsValueRaw = (133.42 * compensationVoltage * compensationVoltage * compensationVoltage
                         - 255.86 * compensationVoltage * compensationVoltage
                         + 857.39 * compensationVoltage) * 0.5;
    tdsValue = tdsValueRaw; 
  }
  return tdsValue; // Kembalikan nilai TDS yang telah disesuaikan
}

float readPH(float temperature) {
  selectChannel(1); // Pilih channel 1 untuk sensor pH
  delay(10); // Tunggu stabilisasi multiplexer
  int analogValue = analogRead(tdsPin);
  float voltage = (analogValue / 1024.0) * 3.3; // Konversi nilai ADC ke voltase
  float slope_T = slope * (298.15 / (temperature + 273.15));
  float phValue = slope_T * voltage + offset;
  return phValue;
}

int readWaterLevelA() {
  liquidLevelA = digitalRead(LIQUID_LEVEL_PIN_A);
  return liquidLevelA ;
}

int readWaterLevelB() {
  liquidLevelB = digitalRead(LIQUID_LEVEL_PIN_B);
  return liquidLevelB ;
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

  sensors.requestTemperatures(); 
  float temperature = sensors.getTempCByIndex(0); // Baca suhu dalam Celsius

  float tds = readTDS(temperature);
  float ph = readPH(temperature);
  int waterLevelA = readWaterLevelA();
  int waterLevelB = readWaterLevelB();

  Serial.print("Threshold: ");
  Serial.println(threshold);
  Serial.print("PPM: ");
  Serial.println(tds);
  Serial.print("pH: ");
  Serial.println(ph);
  Serial.print("Suhu: ");
  Serial.println(temperature);
  Serial.print("Nutrisi A: ");
  Serial.println(waterLevelA);
  Serial.print("Nutrisi B: ");
  Serial.println(waterLevelB);

 // Kontrol pompa
  if (tds < minThreshold) {
    digitalWrite(relayPompa1, LOW); // Nyalakan pompa 1
    digitalWrite(relayPompa2, LOW); // Nyalakan pompa 2
    statusPompa = 1;
    Serial.println("Pompa ON");
  } else if (tds > maxThreshold){
    digitalWrite(relayPompa1, HIGH); // Matikan pompa 1
    digitalWrite(relayPompa2, HIGH); // Matikan pompa 2
    statusPompa = 0;
    Serial.println("Pompa OFF");
  }

 // Publikasi data ke MQTT jika terhubung
  if (client.connected()) {
    String data = String(tds) + "|" + String(temperature) + "|" + String(waterLevelA) + "|" + String(waterLevelB) + "|" + String(ph) + "|" + String(statusPompa);
    client.publish(mqtt_topic, data.c_str(), true);
  }

  delay(1000); // Delay untuk stabilitas pembacaan
}
