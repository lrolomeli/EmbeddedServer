#include <WiFi.h>
#include <WebServer.h>
#include <Adafruit_AHTX0.h>
#include <ArduinoJson.h>

// Set your Access Point credentials
const char* ssid = "HermososPA";
const char* password = "1823LomeliPlascencia";

WebServer server(80);
Adafruit_AHTX0 aht;

void handleGetSensorData() {
  sensors_event_t humidity, temp;
  aht.getEvent(&humidity, &temp);

  // Create a JSON document (200 bytes is plenty for this)
  StaticJsonDocument<200> doc;
  doc["temperature"] = temp.temperature;
  doc["humidity"] = humidity.relative_humidity;
  doc["unit"] = "celsius";
  doc["status"] = "success";

  // Serialize to string
  String jsonResponse;
  serializeJson(doc, jsonResponse);

  // Send response with application/json header
  server.send(200, "application/json", jsonResponse);
}

void setup() {
  Serial.begin(115200);
  delay(1000); // Small delay to allow the serial hardware to initialize
  Serial.println("Starting ESP32-C3...");

  if (!aht.begin()) {
    Serial.println("AHT10 not found");
    while (1) delay(10);
  }

// 1. Start Wi-Fi in Station Mode
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  
  Serial.print("Connecting to Wi-Fi");
  
  // 2. Wait for connection (DHCP happens automatically here)
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  // 3. Print the IP assigned by your DHCP server
  Serial.println("\nConnected!");
  Serial.print("IP Address assigned by DHCP: ");
  Serial.println(WiFi.localIP());

  // ... rest of your setup

  // Define your API endpoint
  server.on("/api/sensors", HTTP_GET, handleGetSensorData);

  server.begin();
  Serial.println("API Server started at /api/sensors");
}

void loop() {
  server.handleClient();
}