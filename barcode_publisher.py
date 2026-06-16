#!/usr/bin/env python3
"""
Smart Billing - Barcode Scanner Publisher (Raspberry Pi 4)
----------------------------------------------------------
Reads barcode IDs typed by a USB barcode scanner (HID keyboard)
or from a watched file, looks up product info, then publishes
to MQTT for the ESP32 display and web dashboard.

Usage:
    python3 barcode_publisher.py

    # Manual test (no scanner):
    python3 barcode_publisher.py --test
    # Then type a barcode ID and press Enter

MQTT Broker: runs on this Pi (localhost) via Mosquitto
"""

import sys
import json
import time
import threading
import argparse
import os
import paho.mqtt.client as mqtt

# ─── MQTT Configuration ────────────────────────────────
MQTT_BROKER   = "localhost"
MQTT_PORT     = 1883
MQTT_CLIENT   = "Pi4_BarcodePublisher"

TOPIC_BILLING_SCAN  = "billing/scan"
TOPIC_BILLING_ITEM  = "billing/item"
TOPIC_BILLING_CART  = "billing/cart"
TOPIC_BILLING_CLEAR = "billing/clear"

# ─── Product Database ─────────────────────────────────
# Barcode ID → (name, price in ₹)
PRODUCTS = {
    "001": {"name": "Soap",        "price": 50},
    "002": {"name": "Brush",       "price": 30},
    "003": {"name": "Shampoo",     "price": 80},
    "004": {"name": "Toothpaste",  "price": 45},
    "005": {"name": "Comb",        "price": 25},
}

# ─── Watched file (for Notepad-style input) ────────────
WATCH_FILE = "/tmp/barcode_input.txt"

# ─── Cart State ────────────────────────────────────────
cart = []          # list of {id, name, price}
cart_total = 0

# ─── MQTT Client Setup ────────────────────────────────
client = mqtt.Client(client_id=MQTT_CLIENT)

def on_connect(c, userdata, flags, rc):
    if rc == 0:
        print(f"[MQTT] Connected to broker at {MQTT_BROKER}:{MQTT_PORT}")
        c.subscribe(TOPIC_BILLING_CLEAR)
    else:
        print(f"[MQTT] Connection failed, rc={rc}")

def on_message(c, userdata, msg):
    global cart, cart_total
    topic   = msg.topic
    payload = msg.payload.decode().strip()

    if topic == TOPIC_BILLING_CLEAR:
        cart.clear()
        cart_total = 0
        print("[CART] Cart cleared")
        publish_cart()

client.on_connect = on_connect
client.on_message = on_message

# ─── Core Logic ───────────────────────────────────────
def process_barcode(barcode_id: str):
    global cart, cart_total

    barcode_id = barcode_id.strip()
    if not barcode_id:
        return

    print(f"\n[SCAN] Barcode ID: {barcode_id}")

    # Publish raw scan
    client.publish(TOPIC_BILLING_SCAN, barcode_id)

    # Look up product
    if barcode_id not in PRODUCTS:
        print(f"[WARN] Unknown barcode: {barcode_id}")
        unknown_payload = json.dumps({
            "id":    barcode_id,
            "name":  "Unknown Item",
            "price": 0,
            "error": True
        })
        client.publish(TOPIC_BILLING_ITEM, unknown_payload)
        return

    product = PRODUCTS[barcode_id]
    item = {
        "id":    barcode_id,
        "name":  product["name"],
        "price": product["price"]
    }

    # Publish item info → ESP32 + Dashboard
    item_payload = json.dumps(item)
    client.publish(TOPIC_BILLING_ITEM, item_payload)
    print(f"[MQTT] Published billing/item: {item_payload}")

    # Update cart
    cart.append(item)
    cart_total += item["price"]
    publish_cart()


def publish_cart():
    payload = json.dumps({
        "items": cart,
        "total": cart_total,
        "count": len(cart)
    })
    client.publish(TOPIC_BILLING_CART, payload)
    print(f"[CART] Total: ₹{cart_total}  Items: {len(cart)}")


# ─── Input Modes ──────────────────────────────────────
def read_stdin_loop():
    """Reads barcodes typed by USB HID barcode scanner (keyboard mode)."""
    print("[INPUT] Reading from stdin (barcode scanner / keyboard).")
    print("        Scan a barcode or type an ID and press Enter.")
    print("        IDs: 001=Soap  002=Brush  003=Shampoo  004=Toothpaste  005=Comb")
    print()
    try:
        for line in sys.stdin:
            process_barcode(line.strip())
    except KeyboardInterrupt:
        print("\n[EXIT] Stopped by user.")


def watch_file_loop():
    """
    Watches WATCH_FILE for new barcode IDs.
    External tools (Notepad, scripts) can write a barcode ID + newline to this file.
    The file is cleared after reading.
    """
    print(f"[INPUT] Watching file: {WATCH_FILE}")
    # Ensure file exists
    open(WATCH_FILE, "a").close()
    last_mtime = os.path.getmtime(WATCH_FILE)

    while True:
        try:
            mtime = os.path.getmtime(WATCH_FILE)
            if mtime != last_mtime:
                last_mtime = mtime
                with open(WATCH_FILE, "r") as f:
                    lines = f.readlines()
                # Clear file after reading
                open(WATCH_FILE, "w").close()
                for line in lines:
                    process_barcode(line.strip())
        except Exception as e:
            print(f"[FILE] Error: {e}")
        time.sleep(0.3)


# ─── Main ─────────────────────────────────────────────
def main():
    parser = argparse.ArgumentParser(description="Smart Billing Barcode Publisher")
    parser.add_argument("--test",      action="store_true", help="Interactive test mode (stdin)")
    parser.add_argument("--file",      action="store_true", help="Watch file mode instead of stdin")
    parser.add_argument("--broker",    default=MQTT_BROKER, help="MQTT broker IP")
    parser.add_argument("--port",      default=MQTT_PORT,  type=int, help="MQTT broker port")
    args = parser.parse_args()

    # Connect MQTT
    client.connect(args.broker, args.port, keepalive=60)
    client.loop_start()

    time.sleep(1)  # wait for connection

    try:
        if args.file:
            # Watch file mode (separate thread) + stdin as fallback
            t = threading.Thread(target=watch_file_loop, daemon=True)
            t.start()
            print("[INFO] File watcher running. Also accepting stdin input.")
            read_stdin_loop()
        else:
            # Default: stdin (barcode scanner HID)
            read_stdin_loop()
    finally:
        client.loop_stop()
        client.disconnect()
        print("[MQTT] Disconnected.")


if __name__ == "__main__":
    main()
