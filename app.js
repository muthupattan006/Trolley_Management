/**
 * Smart Billing Dashboard — app.js
 * MQTT over WebSocket using mqtt.js
 * Connects to Mosquitto broker on Pi 4 (port 9001)
 */

// ─── Configuration ────────────────────────────────────
const MQTT_WS_PORT = 9001;
const TOPICS = {
  BILLING_ITEM:  "billing/item",
  BILLING_CART:  "billing/cart",
  BILLING_CLEAR: "billing/clear",
  BILLING_SCAN:  "billing/scan",
  MOTOR_CONTROL: "motor/control",
  MOTOR_STATUS:  "motor/status",
};

// Products (for quick-send chips)
const PRODUCTS = {
  "001": { name: "Soap",       price: 50 },
  "002": { name: "Brush",      price: 30 },
  "003": { name: "Shampoo",    price: 80 },
  "004": { name: "Toothpaste", price: 45 },
  "005": { name: "Comb",       price: 25 },
};

// ─── State ────────────────────────────────────────────
let mqttClient   = null;
let cart         = [];
let cartTotal    = 0;
let motorSpeed   = 0;
let motorDir     = "stop";

// ─── DOM refs ─────────────────────────────────────────
const statusDot      = document.getElementById("status-dot");
const statusText     = document.getElementById("status-text");
const lastItemName   = document.getElementById("last-item-name");
const lastItemPrice  = document.getElementById("last-item-price");
const lastScanBox    = document.getElementById("last-scan-box");
const scanBadge      = document.getElementById("scan-badge");
const cartTbody      = document.getElementById("cart-tbody");
const totalCount     = document.getElementById("total-count");
const grandTotal     = document.getElementById("grand-total");
const logBox         = document.getElementById("log-box");
const speedSlider    = document.getElementById("speed-slider");
const speedDisplay   = document.getElementById("speed-display");
const motorRing      = document.getElementById("motor-ring");
const ringDirection  = document.getElementById("ring-direction");
const ringSpeed      = document.getElementById("ring-speed");
const infoDirection  = document.getElementById("info-direction");
const infoPwm        = document.getElementById("info-pwm");
const infoSpeed      = document.getElementById("info-speed");
const infoTime       = document.getElementById("info-time");

// ─── MQTT Connect ─────────────────────────────────────
function reconnectMQTT() {
  const ip = document.getElementById("broker-ip").value.trim();
  if (!ip) { alert("Enter broker IP address"); return; }

  if (mqttClient && mqttClient.connected) {
    mqttClient.end(true);
  }

  const brokerUrl = `ws://${ip}:${MQTT_WS_PORT}/mqtt`;
  setStatus("connecting");
  addLog("sys", `Connecting to ${brokerUrl}…`);

  mqttClient = mqtt.connect(brokerUrl, {
    clientId: "Dashboard_" + Math.random().toString(16).substr(2, 6),
    connectTimeout: 8000,
    reconnectPeriod: 5000,
    keepalive: 60,
  });

  mqttClient.on("connect", () => {
    setStatus("connected");
    addLog("sys", `Connected to MQTT broker at ${ip}:${MQTT_WS_PORT}`);
    // Subscribe to all inbound topics
    mqttClient.subscribe(TOPICS.BILLING_ITEM);
    mqttClient.subscribe(TOPICS.BILLING_CART);
    mqttClient.subscribe(TOPICS.BILLING_SCAN);
    mqttClient.subscribe(TOPICS.MOTOR_STATUS);
    addLog("sys", "Subscribed to: billing/item, billing/cart, billing/scan, motor/status");
  });

  mqttClient.on("error", (err) => {
    setStatus("error");
    addLog("sys", `Error: ${err.message}`);
  });

  mqttClient.on("close", () => {
    setStatus("disconnected");
    addLog("sys", "Connection closed.");
  });

  mqttClient.on("message", handleMessage);
}

// Auto-connect on page load
window.addEventListener("load", () => {
  reconnectMQTT();
});

// ─── MQTT Message Handler ──────────────────────────────
function handleMessage(topic, message) {
  const payload = message.toString();
  addLog(topic, payload);

  try {
    if (topic === TOPICS.BILLING_SCAN) {
      // Raw scan — just log (item info comes separately)
      scanBadge.classList.add("flash");
      setTimeout(() => scanBadge.classList.remove("flash"), 500);
    }

    else if (topic === TOPICS.BILLING_ITEM) {
      const item = JSON.parse(payload);
      if (!item.error) {
        updateLastScan(item.name, item.price);
      } else {
        updateLastScan("Unknown Item (" + item.id + ")", 0);
      }
    }

    else if (topic === TOPICS.BILLING_CART) {
      const data = JSON.parse(payload);
      cart      = data.items  || [];
      cartTotal = data.total  || 0;
      renderCart();
    }

    else if (topic === TOPICS.MOTOR_STATUS) {
      const data = JSON.parse(payload);
      updateMotorDisplay(data.speed, data.direction, data.pwm);
    }

  } catch (e) {
    console.warn("Message parse error:", e);
  }
}

// ─── Billing UI ───────────────────────────────────────
function updateLastScan(name, price) {
  lastItemName.textContent  = name;
  lastItemPrice.textContent = price > 0 ? `₹${price}` : "—";
  lastScanBox.classList.add("flash");
  setTimeout(() => lastScanBox.classList.remove("flash"), 800);
}

function renderCart() {
  if (cart.length === 0) {
    cartTbody.innerHTML = `<tr class="empty-row"><td colspan="4">No items yet. Scan a barcode to begin.</td></tr>`;
  } else {
    cartTbody.innerHTML = cart.map((item, i) => `
      <tr>
        <td>${i + 1}</td>
        <td>${escHtml(item.name)}</td>
        <td style="font-family:'JetBrains Mono',monospace;color:var(--accent-blue)">${escHtml(item.id)}</td>
        <td class="price-cell">₹${item.price}</td>
      </tr>
    `).join("");
    // Scroll to bottom
    const wrap = document.querySelector(".cart-table-wrap");
    wrap.scrollTop = wrap.scrollHeight;
  }
  totalCount.textContent = cart.length;
  grandTotal.textContent = `₹${cartTotal}`;
}

function clearCart() {
  if (!mqttClient || !mqttClient.connected) {
    alert("Not connected to MQTT broker");
    return;
  }
  mqttClient.publish(TOPICS.BILLING_CLEAR, "clear");
  cart      = [];
  cartTotal = 0;
  renderCart();
  updateLastScan("Waiting for barcode…", 0);
  lastItemPrice.textContent = "—";
  addLog(TOPICS.BILLING_CLEAR, "clear");
}

// Test scan from product chip click
function testScan(id) {
  if (!mqttClient || !mqttClient.connected) {
    alert("Not connected to MQTT broker");
    return;
  }
  // Simulate what Pi4 barcode_publisher.py does:
  const product = PRODUCTS[id];
  if (!product) return;

  // Directly publish item (bypasses Pi4 scanner, useful for dashboard testing)
  const itemPayload = JSON.stringify({ id, name: product.name, price: product.price });
  mqttClient.publish(TOPICS.BILLING_ITEM, itemPayload);
  addLog(TOPICS.BILLING_ITEM + " [test]", itemPayload);

  // Manually update local cart for instant feedback
  cart.push({ id, name: product.name, price: product.price });
  cartTotal += product.price;
  renderCart();
  updateLastScan(product.name, product.price);
}

// ─── Motor Control UI ─────────────────────────────────
function onSpeedChange(val) {
  motorSpeed = parseInt(val);
  const pct  = Math.round((motorSpeed / 255) * 100);
  speedDisplay.textContent = `${motorSpeed} / 255`;
  // Update slider fill background
  speedSlider.style.background =
    `linear-gradient(to right, var(--accent-blue) ${pct}%, var(--border) ${pct}%)`;
}

function sendMotor(direction) {
  if (!mqttClient || !mqttClient.connected) {
    alert("Not connected to MQTT broker");
    return;
  }
  motorDir = direction;
  const payload = JSON.stringify({
    speed:     direction === "stop" ? 0 : motorSpeed,
    direction: direction
  });
  mqttClient.publish(TOPICS.MOTOR_CONTROL, payload);
  addLog(TOPICS.MOTOR_CONTROL, payload);

  // Update button active states immediately
  updateMotorButtons(direction);
}

function updateMotorButtons(dir) {
  document.getElementById("btn-forward").classList.toggle("active", dir === "forward");
  document.getElementById("btn-reverse").classList.toggle("active", dir === "reverse");
}

function updateMotorDisplay(speed, direction, pwm) {
  const s = parseInt(speed) || 0;
  const p = parseInt(pwm)   || 0;
  const pct = Math.round((p / 255) * 100);

  ringSpeed.textContent = s;
  ringDirection.textContent = direction.toUpperCase();

  motorRing.className = "motor-status-ring";
  if (direction === "forward") {
    motorRing.classList.add("forward");
    ringDirection.style.color = "var(--accent-green)";
  } else if (direction === "reverse") {
    motorRing.classList.add("reverse");
    ringDirection.style.color = "var(--accent-yellow)";
  } else {
    motorRing.classList.add("stopped");
    ringDirection.style.color = "var(--text-secondary)";
  }

  infoDirection.textContent = direction;
  infoPwm.textContent       = `${p} (${pct}%)`;
  infoSpeed.textContent     = `${s}/255`;
  infoTime.textContent      = new Date().toLocaleTimeString();

  updateMotorButtons(direction);
}

// ─── MQTT Log ─────────────────────────────────────────
function addLog(topic, payload) {
  const ts    = new Date().toLocaleTimeString();
  const cls   = topic.startsWith("billing") ? "log-billing"
               : topic.startsWith("motor")  ? "log-motor"
               : "";
  const entry = document.createElement("span");
  entry.className = `log-entry ${cls}`;
  // Truncate long payloads
  const pShort = payload.length > 80 ? payload.substring(0, 80) + "…" : payload;
  entry.innerHTML =
    `<span class="ts">[${ts}]</span> ` +
    `<span class="topic">${escHtml(topic)}</span> ` +
    `<span class="payload">${escHtml(pShort)}</span>`;
  logBox.appendChild(entry);
  logBox.appendChild(document.createTextNode("\n"));
  logBox.scrollTop = logBox.scrollHeight;
}

function clearLog() {
  logBox.innerHTML = "";
}

// ─── Connection Status ────────────────────────────────
function setStatus(state) {
  statusDot.className = "status-dot";
  if (state === "connected") {
    statusDot.classList.add("connected");
    statusText.textContent = "Connected";
  } else if (state === "connecting") {
    statusText.textContent = "Connecting…";
  } else if (state === "error") {
    statusText.textContent = "Error";
  } else {
    statusText.textContent = "Disconnected";
  }
}

// ─── Utility ──────────────────────────────────────────
function escHtml(str) {
  return String(str)
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;");
}
