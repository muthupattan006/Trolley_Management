# Smart Billing + Motor Control IoT System

**ESP32 · 3.5" TFT LCD · L298N · Raspberry Pi 4 · MQTT · Web Dashboard**

---

## System Overview

```
[Barcode Scanner]──USB──[Raspberry Pi 4]
                              │ WiFi / MQTT (port 1883)
                     [Mosquitto Broker] (Pi 4)
                         │            │
                    [ESP32]      [Web Browser]
                  3.5" TFT         Dashboard
                  L298N           MQTT WS :9001
```

---

## Product Database

| ID  | Product    | Price |
|-----|------------|-------|
| 001 | Soap       | ₹50   |
| 002 | Brush      | ₹30   |
| 003 | Shampoo    | ₹80   |
| 004 | Toothpaste | ₹45   |
| 005 | Comb       | ₹25   |

---

## Wiring

### ESP32 ↔ 3.5" TFT LCD (ILI9488, SPI)

| TFT Pin | ESP32 GPIO |
|---------|------------|
| VCC     | 3.3V       |
| GND     | GND        |
| CS      | GPIO 5     |
| RESET   | GPIO 4     |
| DC/RS   | GPIO 2     |
| SDI/MOSI| GPIO 23    |
| SCK     | GPIO 18    |
| LED/BL  | GPIO 15    |
| MISO    | GPIO 19    |

### ESP32 ↔ L298N Motor Driver

| L298N Pin | ESP32 GPIO |
|-----------|------------|
| ENA       | GPIO 25 (PWM) |
| IN1       | GPIO 26    |
| IN2       | GPIO 27    |
| IN3       | GPIO 14    |
| IN4       | GPIO 12    |
| ENB       | GPIO 13 (PWM) |
| VCC       | 5V / 12V (motor power) |
| GND       | GND (common) |

---

## Setup

### 1. Raspberry Pi 4 — Mosquitto Broker

```bash
sudo apt update && sudo apt install -y mosquitto mosquitto-clients

# Copy config
sudo cp /home/muthu/ccp/mosquitto/mosquitto.conf /etc/mosquitto/conf.d/smart_billing.conf

# Restart broker
sudo systemctl restart mosquitto
sudo systemctl enable mosquitto

# Verify
mosquitto_sub -h localhost -t "#" -v
```

### 2. Raspberry Pi 4 — Barcode Publisher

```bash
cd /home/muthu/ccp/pi4
pip3 install -r requirements.txt

# Run (scanner types barcodes via USB HID):
python3 barcode_publisher.py

# Run (watch file mode — write IDs to /tmp/barcode_input.txt):
python3 barcode_publisher.py --file

# Quick test (type ID manually):
python3 barcode_publisher.py --test
```

### 3. ESP32 — Arduino IDE

1. Install libraries via **Library Manager**:
   - `TFT_eSPI` by Bodmer
   - `PubSubClient` by Nick O'Leary
   - `ArduinoJson` by Benoit Blanchon

2. Configure `TFT_eSPI`:
   - Open `Documents/Arduino/libraries/TFT_eSPI/User_Setup.h`
   - Set driver: `#define ILI9488_DRIVER`
   - Set pins as shown in wiring table above

3. Edit `esp32/smart_billing_motor.ino`:
   ```cpp
   const char* WIFI_SSID     = "YOUR_WIFI_SSID";
   const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
   const char* MQTT_BROKER   = "192.168.1.100"; // Pi 4 IP
   ```

4. Flash to ESP32 (`Tools → Board → ESP32 Dev Module`)

### 4. Web Dashboard

```bash
# Serve from Pi 4 (recommended):
cd /home/muthu/ccp/dashboard
python3 -m http.server 8080

# Open in browser:
# http://<Pi4_IP>:8080
```

Or simply open `dashboard/index.html` directly in a browser on the same WiFi network.

Set the **Broker IP** field to your Pi 4's IP and click **Connect**.

---

## MQTT Topics

| Topic           | Direction        | Payload Example |
|----------------|------------------|-----------------|
| `billing/scan`  | Pi → broker      | `"001"` |
| `billing/item`  | Pi → all         | `{"id":"001","name":"Soap","price":50}` |
| `billing/cart`  | Pi → all         | `{"items":[...],"total":50,"count":1}` |
| `billing/clear` | Dashboard → all  | `"clear"` |
| `motor/control` | Dashboard → ESP32| `{"speed":200,"direction":"forward"}` |
| `motor/status`  | ESP32 → all      | `{"speed":200,"direction":"forward","pwm":200}` |

---

## File Structure

```
ccp/
├── esp32/
│   └── smart_billing_motor.ino   # ESP32 firmware
├── pi4/
│   ├── barcode_publisher.py      # Barcode → MQTT
│   └── requirements.txt
├── mosquitto/
│   └── mosquitto.conf            # Broker config
├── dashboard/
│   ├── index.html                # Web UI
│   ├── style.css                 # Dark premium styles
│   └── app.js                   # MQTT + UI logic
└── README.md
```

---

## Quick Test (No Hardware)

```bash
# Terminal 1 — Subscribe to all topics:
mosquitto_sub -h localhost -t "#" -v

# Terminal 2 — Simulate a barcode scan:
mosquitto_pub -h localhost -t "billing/scan" -m "001"
# (barcode_publisher.py picks this up and publishes billing/item + billing/cart)

# Terminal 3 — Simulate motor status from ESP32:
mosquitto_pub -h localhost -t "motor/status" \
  -m '{"speed":180,"direction":"forward","pwm":180}'
```
