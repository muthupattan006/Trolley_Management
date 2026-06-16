/*
 * Smart Billing + Motor Control - ESP32 Firmware
 * ------------------------------------------------
 * Hardware:
 *   - 3.5" TFT LCD (ILI9488, SPI)  via TFT_eSPI
 *   - L298N Motor Driver
 *   - WiFi + MQTT (PubSubClient)
 *
 * Libraries required (install via Arduino Library Manager):
 *   TFT_eSPI, PubSubClient, ArduinoJson
 *
 * TFT_eSPI User_Setup.h must be configured:
 *   #define ILI9488_DRIVER
 *   #define TFT_MOSI 23
 *   #define TFT_SCLK 18
 *   #define TFT_CS   5
 *   #define TFT_DC   2
 *   #define TFT_RST  4
 *   #define TFT_BL   15
 *   #define TFT_BACKLIGHT_ON HIGH
 *
 * L298N Wiring:
 *   ENA → ESP32 GPIO25 (PWM channel 0)
 *   IN1 → ESP32 GPIO26
 *   IN2 → ESP32 GPIO27
 *   IN3 → ESP32 GPIO14
 *   IN4 → ESP32 GPIO12
 *   ENB → ESP32 GPIO13 (PWM channel 1)
 *
 * MQTT Broker: Raspberry Pi 4 IP (port 1883)
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <SPI.h>

// ─── WiFi Configuration ────────────────────────────────
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// ─── MQTT Configuration ────────────────────────────────
const char* MQTT_BROKER   = "192.168.1.100";  // <- Pi 4 IP address
const int   MQTT_PORT     = 1883;
const char* MQTT_CLIENT   = "ESP32_SmartBilling";

// MQTT Topics
const char* TOPIC_BILLING_ITEM    = "billing/item";
const char* TOPIC_BILLING_CLEAR   = "billing/clear";
const char* TOPIC_MOTOR_CONTROL   = "motor/control";
const char* TOPIC_MOTOR_STATUS    = "motor/status";

// ─── L298N Motor Driver Pins ───────────────────────────
#define ENA_PIN  25
#define IN1_PIN  26
#define IN2_PIN  27
#define IN3_PIN  14
#define IN4_PIN  12
#define ENB_PIN  13

// PWM channels
#define PWM_CH_A  0
#define PWM_CH_B  1
#define PWM_FREQ  1000   // 1 kHz
#define PWM_RES   8      // 8-bit resolution (0-255)

// ─── TFT Display ──────────────────────────────────────
TFT_eSPI tft = TFT_eSPI();
#define TFT_BG_COLOR   0x1082   // Dark grey
#define TFT_HDR_COLOR  0x041F   // Deep blue
#define TFT_GREEN      0x07E0
#define TFT_RED        0xF800
#define TFT_WHITE      0xFFFF
#define TFT_YELLOW     0xFFE0
#define TFT_CYAN       0x07FF

// ─── State ────────────────────────────────────────────
struct CartItem {
  String name;
  int    price;
};

CartItem cart[20];
int      cartCount  = 0;
int      cartTotal  = 0;

int  motorSpeed     = 0;
bool motorForward   = true;
bool motorRunning   = false;

String lastItemName  = "";
int    lastItemPrice = 0;

WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);

unsigned long lastStatusPublish = 0;
unsigned long lastDisplayUpdate = 0;

// ─── Forward Declarations ─────────────────────────────
void setupMotor();
void setMotor(int speed, bool forward, bool run);
void connectWiFi();
void connectMQTT();
void mqttCallback(char* topic, byte* payload, unsigned int length);
void drawScreen();
void drawHeader();
void drawBillingSection();
void drawMotorSection();
void drawStatus(String msg, uint16_t color);

// ──────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  // TFT init
  tft.init();
  tft.setRotation(1);  // Landscape 480x320
  tft.fillScreen(TFT_BG_COLOR);
  pinMode(15, OUTPUT);
  digitalWrite(15, HIGH);  // Backlight ON

  drawHeader();
  drawStatus("Connecting to WiFi...", TFT_YELLOW);

  // Motor init
  setupMotor();

  // WiFi
  connectWiFi();
  drawStatus("WiFi OK. Connecting MQTT...", TFT_YELLOW);

  // MQTT
  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(512);
  connectMQTT();

  // Full screen draw
  drawScreen();
}

// ──────────────────────────────────────────────────────
void loop() {
  if (!mqtt.connected()) connectMQTT();
  mqtt.loop();

  // Publish motor status every 2 seconds
  if (millis() - lastStatusPublish > 2000) {
    publishMotorStatus();
    lastStatusPublish = millis();
  }

  // Refresh display every second
  if (millis() - lastDisplayUpdate > 1000) {
    drawScreen();
    lastDisplayUpdate = millis();
  }
}

// ─── WiFi ─────────────────────────────────────────────
void connectWiFi() {
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected: " + WiFi.localIP().toString());
  } else {
    Serial.println("\nWiFi FAILED");
  }
}

// ─── MQTT ─────────────────────────────────────────────
void connectMQTT() {
  int retries = 0;
  while (!mqtt.connected() && retries < 5) {
    Serial.print("MQTT connecting...");
    if (mqtt.connect(MQTT_CLIENT)) {
      Serial.println("connected");
      mqtt.subscribe(TOPIC_BILLING_ITEM);
      mqtt.subscribe(TOPIC_BILLING_CLEAR);
      mqtt.subscribe(TOPIC_MOTOR_CONTROL);
    } else {
      Serial.printf("failed rc=%d, retry in 2s\n", mqtt.state());
      delay(2000);
      retries++;
    }
  }
}

// ─── MQTT Callback ────────────────────────────────────
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String topicStr = String(topic);
  String msg      = "";
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];

  Serial.printf("MQTT [%s]: %s\n", topic, msg.c_str());

  // ── Billing Item ──────────────────────────────────
  if (topicStr == TOPIC_BILLING_ITEM) {
    StaticJsonDocument<256> doc;
    if (!deserializeJson(doc, msg)) {
      lastItemName  = doc["name"].as<String>();
      lastItemPrice = doc["price"].as<int>();
      // Add to cart
      if (cartCount < 20) {
        cart[cartCount].name  = lastItemName;
        cart[cartCount].price = lastItemPrice;
        cartCount++;
        cartTotal += lastItemPrice;
      }
    }
  }

  // ── Clear Cart ────────────────────────────────────
  else if (topicStr == TOPIC_BILLING_CLEAR) {
    cartCount   = 0;
    cartTotal   = 0;
    lastItemName  = "";
    lastItemPrice = 0;
  }

  // ── Motor Control ─────────────────────────────────
  else if (topicStr == TOPIC_MOTOR_CONTROL) {
    StaticJsonDocument<128> doc;
    if (!deserializeJson(doc, msg)) {
      int speed       = doc["speed"] | 0;
      String direction = doc["direction"] | "stop";
      if (direction == "stop") {
        setMotor(0, true, false);
      } else {
        bool fwd = (direction == "forward");
        setMotor(speed, fwd, true);
      }
    }
  }
}

// ─── Motor Control ────────────────────────────────────
void setupMotor() {
  ledcSetup(PWM_CH_A, PWM_FREQ, PWM_RES);
  ledcAttachPin(ENA_PIN, PWM_CH_A);
  ledcSetup(PWM_CH_B, PWM_FREQ, PWM_RES);
  ledcAttachPin(ENB_PIN, PWM_CH_B);

  pinMode(IN1_PIN, OUTPUT);
  pinMode(IN2_PIN, OUTPUT);
  pinMode(IN3_PIN, OUTPUT);
  pinMode(IN4_PIN, OUTPUT);

  setMotor(0, true, false);
}

void setMotor(int speed, bool forward, bool run) {
  motorSpeed   = speed;
  motorForward = forward;
  motorRunning = run;

  if (!run || speed == 0) {
    // Stop
    digitalWrite(IN1_PIN, LOW);
    digitalWrite(IN2_PIN, LOW);
    digitalWrite(IN3_PIN, LOW);
    digitalWrite(IN4_PIN, LOW);
    ledcWrite(PWM_CH_A, 0);
    ledcWrite(PWM_CH_B, 0);
  } else if (forward) {
    digitalWrite(IN1_PIN, HIGH);
    digitalWrite(IN2_PIN, LOW);
    digitalWrite(IN3_PIN, HIGH);
    digitalWrite(IN4_PIN, LOW);
    ledcWrite(PWM_CH_A, speed);
    ledcWrite(PWM_CH_B, speed);
  } else {
    digitalWrite(IN1_PIN, LOW);
    digitalWrite(IN2_PIN, HIGH);
    digitalWrite(IN3_PIN, LOW);
    digitalWrite(IN4_PIN, HIGH);
    ledcWrite(PWM_CH_A, speed);
    ledcWrite(PWM_CH_B, speed);
  }
}

void publishMotorStatus() {
  if (!mqtt.connected()) return;
  StaticJsonDocument<128> doc;
  doc["speed"]     = motorSpeed;
  doc["direction"] = motorRunning ? (motorForward ? "forward" : "reverse") : "stop";
  doc["pwm"]       = motorSpeed;
  char buf[128];
  serializeJson(doc, buf);
  mqtt.publish(TOPIC_MOTOR_STATUS, buf);
}

// ─── TFT Display ──────────────────────────────────────
void drawHeader() {
  tft.fillRect(0, 0, 480, 40, TFT_HDR_COLOR);
  tft.setTextColor(TFT_WHITE, TFT_HDR_COLOR);
  tft.setTextSize(2);
  tft.setCursor(10, 12);
  tft.print("  SMART BILLING SYSTEM");
  // WiFi IP
  tft.setTextSize(1);
  tft.setCursor(330, 30);
  if (WiFi.status() == WL_CONNECTED)
    tft.print(WiFi.localIP().toString());
  else
    tft.print("No WiFi");
}

void drawBillingSection() {
  // Section header
  tft.fillRect(0, 42, 300, 16, 0x2945);
  tft.setTextColor(TFT_CYAN, 0x2945);
  tft.setTextSize(1);
  tft.setCursor(4, 46);
  tft.print("CART ITEMS");

  // Item rows (max 5 visible)
  int startRow = max(0, cartCount - 5);
  for (int i = 0; i < 5; i++) {
    int y = 60 + i * 22;
    tft.fillRect(0, y, 300, 20, (i % 2 == 0) ? 0x1082 : 0x18C3);
    int idx = startRow + i;
    if (idx < cartCount) {
      tft.setTextColor(TFT_WHITE, (i % 2 == 0) ? 0x1082 : 0x18C3);
      tft.setTextSize(1);
      tft.setCursor(6, y + 6);
      String line = String(idx + 1) + ". " + cart[idx].name;
      tft.print(line.substring(0, 22));
      tft.setCursor(220, y + 6);
      tft.print("Rs." + String(cart[idx].price));
    }
  }

  // Last scanned item highlight
  tft.fillRect(0, 172, 300, 26, 0x0329);
  tft.setTextColor(TFT_YELLOW, 0x0329);
  tft.setTextSize(1);
  tft.setCursor(4, 176);
  tft.print("Last: ");
  tft.setTextColor(TFT_GREEN, 0x0329);
  if (lastItemName != "") {
    tft.print(lastItemName + "  Rs." + String(lastItemPrice));
  } else {
    tft.print("Scan a barcode...");
  }

  // Total
  tft.fillRect(0, 200, 300, 30, 0x0010);
  tft.setTextColor(TFT_GREEN, 0x0010);
  tft.setTextSize(2);
  tft.setCursor(6, 208);
  tft.print("TOTAL: Rs." + String(cartTotal));

  // Item count
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE, 0x0010);
  tft.setCursor(6, 228);
  tft.print("Items: " + String(cartCount));
}

void drawMotorSection() {
  // Motor panel (right side)
  tft.fillRect(305, 42, 175, 200, 0x2124);
  tft.setTextColor(TFT_CYAN, 0x2124);
  tft.setTextSize(1);
  tft.setCursor(310, 46);
  tft.print("MOTOR CONTROL");

  // Direction
  uint16_t dirColor = TFT_RED;
  String dirText = "STOPPED";
  if (motorRunning) {
    dirColor = motorForward ? TFT_GREEN : TFT_YELLOW;
    dirText  = motorForward ? "FORWARD" : "REVERSE";
  }
  tft.fillRect(310, 62, 160, 28, 0x1082);
  tft.setTextColor(dirColor, 0x1082);
  tft.setTextSize(2);
  tft.setCursor(318, 70);
  tft.print(dirText);

  // Speed bar
  tft.setTextColor(TFT_WHITE, 0x2124);
  tft.setTextSize(1);
  tft.setCursor(310, 100);
  tft.print("Speed: " + String(motorSpeed) + "/255");

  // Speed bar background
  tft.fillRect(310, 115, 160, 14, 0x1082);
  int barW = (motorSpeed * 160) / 255;
  uint16_t barColor = motorForward ? TFT_GREEN : TFT_YELLOW;
  if (!motorRunning) barColor = TFT_RED;
  tft.fillRect(310, 115, barW, 14, barColor);

  // PWM %
  int pct = (motorSpeed * 100) / 255;
  tft.setTextColor(TFT_WHITE, 0x2124);
  tft.setCursor(310, 134);
  tft.print("PWM: " + String(pct) + "%");

  // WiFi / MQTT status
  tft.fillRect(305, 248, 175, 24, 0x1082);
  tft.setTextSize(1);
  tft.setTextColor(WiFi.status() == WL_CONNECTED ? TFT_GREEN : TFT_RED, 0x1082);
  tft.setCursor(310, 252);
  tft.print(WiFi.status() == WL_CONNECTED ? "WiFi: Connected" : "WiFi: Offline");
  tft.setTextColor(mqtt.connected() ? TFT_GREEN : TFT_RED, 0x1082);
  tft.setCursor(310, 262);
  tft.print(mqtt.connected() ? "MQTT: Connected" : "MQTT: Offline");
}

void drawStatus(String msg, uint16_t color) {
  tft.fillRect(0, 285, 480, 35, TFT_BG_COLOR);
  tft.setTextColor(color, TFT_BG_COLOR);
  tft.setTextSize(1);
  tft.setCursor(6, 296);
  tft.print(msg);
}

void drawScreen() {
  drawHeader();
  drawBillingSection();
  drawMotorSection();
  tft.fillRect(0, 235, 480, 48, 0x0841);
  drawStatus("IP: " + (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "No WiFi") +
             "  MQTT: " + (mqtt.connected() ? "OK" : "ERR") +
             "  Items: " + String(cartCount) + "  Total: Rs." + String(cartTotal), TFT_WHITE);
}
