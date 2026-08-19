/*
 * ESP32 - Emotion-Aware Vehicle Dashboard & Communication Hub
 *
 * Connections:
 *   ESP32 TX2 (GPIO17) --> STM32 RX (PA10)
 *   ESP32 RX2 (GPIO16) <-- STM32 TX (PA9)
 *   GND <--> GND
 *
 * Features:
 *   - Web dashboard with joystick, speed control, emotion display
 *   - Receives emotion data from Python YOLO script via HTTP API
 *   - Sends motor commands to STM32 via UART
 *   - Displays real-time status, alerts, safety score
 */

#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>

// ===== WiFi Configuration =====
const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

// ===== UART to STM32 =====
#define STM32_SERIAL Serial2
#define STM32_BAUD 115200
#define STM32_TX 17
#define STM32_RX 16

// ===== Web Server =====
WebServer server(80);

// ===== System State =====
struct VehicleState {
  String emotion = "calm";
  int speedLimit = 255;       // PWM max (0-255)
  int currentSpeed = 0;
  int joyX = 0;               // -100 to 100
  int joyY = 0;               // -100 to 100
  bool emergencyStop = false;
  int safetyScore = 100;
  String lastAlert = "System OK";
  unsigned long lastEmotionUpdate = 0;
  unsigned long lastCommandSent = 0;
  int aggressiveCount = 0;
  int fatiguedCount = 0;
  float emotionConfidence = 0.0;
} state;

// ===== Emotion History for Timeline =====
#define HISTORY_SIZE 60
struct EmotionEntry {
  String emotion;
  int speed;
  unsigned long timestamp;
};
EmotionEntry emotionHistory[HISTORY_SIZE];
int historyIndex = 0;

// ===== Alert Log =====
#define MAX_ALERTS 20
String alertLog[MAX_ALERTS];
int alertIndex = 0;

// ===== Forward Declarations =====
void handleRoot();
void handleApi();
void handleEmotion();
void handleJoystick();
void handleSpeed();
void handleEmergency();
void handleHistory();
void handleAlerts();
void sendCommandToSTM32();
void applyEmotionPolicy();
void addAlert(String msg);
void updateHistory();

void setup() {
  Serial.begin(115200);
  Serial.println("\n[ESP32] Emotion-Aware Vehicle Control Starting...");

  // UART to STM32
  STM32_SERIAL.begin(STM32_BAUD, SERIAL_8N1, STM32_RX, STM32_TX);
  Serial.println("[ESP32] UART to STM32 initialized");

  // WiFi
  WiFi.begin(ssid, password);
  Serial.print("[ESP32] Connecting to WiFi");
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[ESP32] WiFi Connected!");
    Serial.print("[ESP32] Dashboard: http://");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\n[ESP32] WiFi failed - starting AP mode");
    WiFi.softAP("EmotionVehicle", "12345678");
    Serial.print("[ESP32] AP Dashboard: http://");
    Serial.println(WiFi.softAPIP());
  }

  // Routes
  server.on("/", handleRoot);
  server.on("/api/status", handleApi);
  server.on("/api/emotion", HTTP_POST, handleEmotion);
  server.on("/api/joystick", HTTP_POST, handleJoystick);
  server.on("/api/speed", HTTP_POST, handleSpeed);
  server.on("/api/emergency", HTTP_POST, handleEmergency);
  server.on("/api/history", handleHistory);
  server.on("/api/alerts", handleAlerts);

  server.enableCORS(true);
  server.begin();
  Serial.println("[ESP32] Web server started");

  addAlert("System initialized");
}

void loop() {
  server.handleClient();

  // Send commands to STM32 at 20Hz
  if (millis() - state.lastCommandSent > 50) {
    sendCommandToSTM32();
    state.lastCommandSent = millis();
  }

  // Update history every 2 seconds
  static unsigned long lastHistory = 0;
  if (millis() - lastHistory > 2000) {
    updateHistory();
    lastHistory = millis();
  }

  // Check for emotion timeout (no update in 5 seconds)
  if (millis() - state.lastEmotionUpdate > 5000 && state.lastEmotionUpdate > 0) {
    // Keep last known emotion but reduce confidence
    state.emotionConfidence *= 0.95;
  }

  // Read responses from STM32
  if (STM32_SERIAL.available()) {
    String response = STM32_SERIAL.readStringUntil('\n');
    Serial.println("[STM32 Response] " + response);
  }

  // Safety score decay/recovery
  static unsigned long lastScoreUpdate = 0;
  if (millis() - lastScoreUpdate > 1000) {
    if (state.emotion == "calm" && state.safetyScore < 100) {
      state.safetyScore = min(100, state.safetyScore + 1);
    }
    lastScoreUpdate = millis();
  }
}

// ===== Apply Emotion-Based Driving Policy =====
void applyEmotionPolicy() {
  if (state.emotion == "aggressive") {
    state.speedLimit = 120;  // ~47% max
    state.safetyScore = max(0, state.safetyScore - 5);
    state.aggressiveCount++;
    if (state.aggressiveCount >= 3) {
      addAlert("WARNING: Repeated aggressive driving detected!");
      state.speedLimit = 80;
    }
    addAlert("Aggressive state - Speed limited to " + String(state.speedLimit));
  }
  else if (state.emotion == "fatigued") {
    state.speedLimit = 100;  // ~39% max
    state.safetyScore = max(0, state.safetyScore - 3);
    state.fatiguedCount++;
    if (state.fatiguedCount >= 5) {
      addAlert("CRITICAL: Driver fatigue detected! Please stop.");
      state.speedLimit = 60;
    }
    addAlert("Fatigue detected - Speed reduced, alarm triggered");
  }
  else if (state.emotion == "calm") {
    state.speedLimit = 255;
    state.aggressiveCount = max(0, state.aggressiveCount - 1);
    state.fatiguedCount = max(0, state.fatiguedCount - 1);
  }
}

// ===== Send Command to STM32 =====
void sendCommandToSTM32() {
  // Protocol: CMD,joyX,joyY,speedLimit,emergency\n
  // joyX: -100 to 100 (left/right)
  // joyY: -100 to 100 (backward/forward)

  int effectiveSpeed = state.speedLimit;

  if (state.emergencyStop) {
    // Emergency: stop everything
    STM32_SERIAL.println("CMD,0,0,0,1");
    return;
  }

  String cmd = "CMD,";
  cmd += String(state.joyX) + ",";
  cmd += String(state.joyY) + ",";
  cmd += String(effectiveSpeed) + ",";
  cmd += "0";
  STM32_SERIAL.println(cmd);
}

// ===== API: POST /api/emotion (from Python YOLO script) =====
void handleEmotion() {
  if (server.hasArg("plain")) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, server.arg("plain"));
    if (!err) {
      String newEmotion = doc["emotion"].as<String>();
      state.emotionConfidence = doc["confidence"] | 0.5;

      if (newEmotion == "calm" || newEmotion == "aggressive" || newEmotion == "fatigued") {
        if (newEmotion != state.emotion) {
          Serial.println("[ESP32] Emotion changed: " + state.emotion + " -> " + newEmotion);
          state.emotion = newEmotion;
          applyEmotionPolicy();
        }
        state.lastEmotionUpdate = millis();
        server.send(200, "application/json", "{\"status\":\"ok\"}");
        return;
      }
    }
  }
  server.send(400, "application/json", "{\"error\":\"invalid\"}");
}

// ===== API: POST /api/joystick =====
void handleJoystick() {
  if (server.hasArg("plain")) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, server.arg("plain"));
    if (!err) {
      state.joyX = constrain(doc["x"] | 0, -100, 100);
      state.joyY = constrain(doc["y"] | 0, -100, 100);

      // Calculate display speed
      int magnitude = sqrt(state.joyX * state.joyX + state.joyY * state.joyY);
      state.currentSpeed = map(constrain(magnitude, 0, 100), 0, 100, 0, state.speedLimit);

      server.send(200, "application/json", "{\"status\":\"ok\"}");
      return;
    }
  }
  server.send(400, "application/json", "{\"error\":\"invalid\"}");
}

// ===== API: POST /api/speed =====
void handleSpeed() {
  if (server.hasArg("plain")) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, server.arg("plain"));
    if (!err) {
      int requestedLimit = doc["limit"] | 255;
      // Don't allow exceeding emotion-based limit
      int emotionMax = 255;
      if (state.emotion == "aggressive") emotionMax = 120;
      else if (state.emotion == "fatigued") emotionMax = 100;

      state.speedLimit = min(requestedLimit, emotionMax);
      server.send(200, "application/json", "{\"status\":\"ok\",\"effective\":" + String(state.speedLimit) + "}");
      return;
    }
  }
  server.send(400, "application/json", "{\"error\":\"invalid\"}");
}

// ===== API: POST /api/emergency =====
void handleEmergency() {
  state.emergencyStop = !state.emergencyStop;
  if (state.emergencyStop) {
    addAlert("EMERGENCY STOP ACTIVATED");
    state.joyX = 0;
    state.joyY = 0;
    state.currentSpeed = 0;
  } else {
    addAlert("Emergency stop released");
  }
  server.send(200, "application/json", "{\"emergency\":" + String(state.emergencyStop ? "true" : "false") + "}");
}

// ===== API: GET /api/status =====
void handleApi() {
  JsonDocument doc;
  doc["emotion"] = state.emotion;
  doc["confidence"] = state.emotionConfidence;
  doc["speedLimit"] = state.speedLimit;
  doc["currentSpeed"] = state.currentSpeed;
  doc["joyX"] = state.joyX;
  doc["joyY"] = state.joyY;
  doc["emergency"] = state.emergencyStop;
  doc["safetyScore"] = state.safetyScore;
  doc["lastAlert"] = state.lastAlert;
  doc["uptime"] = millis() / 1000;

  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}

// ===== API: GET /api/history =====
void handleHistory() {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();

  for (int i = 0; i < HISTORY_SIZE; i++) {
    int idx = (historyIndex + i) % HISTORY_SIZE;
    if (emotionHistory[idx].timestamp > 0) {
      JsonObject entry = arr.add<JsonObject>();
      entry["e"] = emotionHistory[idx].emotion;
      entry["s"] = emotionHistory[idx].speed;
      entry["t"] = emotionHistory[idx].timestamp;
    }
  }

  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}

// ===== API: GET /api/alerts =====
void handleAlerts() {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();

  for (int i = 0; i < MAX_ALERTS; i++) {
    int idx = (alertIndex - 1 - i + MAX_ALERTS) % MAX_ALERTS;
    if (alertLog[idx].length() > 0) {
      arr.add(alertLog[idx]);
    }
  }

  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}

void addAlert(String msg) {
  String timestamped = String(millis() / 1000) + "s: " + msg;
  alertLog[alertIndex % MAX_ALERTS] = timestamped;
  alertIndex++;
  state.lastAlert = msg;
  Serial.println("[ALERT] " + msg);
}

void updateHistory() {
  emotionHistory[historyIndex % HISTORY_SIZE] = {
    state.emotion,
    state.currentSpeed,
    millis() / 1000
  };
  historyIndex++;
}

// ===== Dashboard HTML/CSS/JS =====
void handleRoot() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Emotion-Aware Vehicle Control</title>
<style>
  * { margin: 0; padding: 0; box-sizing: border-box; }
  body {
    font-family: 'Segoe UI', sans-serif;
    background: #0a0e17;
    color: #e0e6f0;
    min-height: 100vh;
  }
  .header {
    background: linear-gradient(135deg, #1a1f35, #0d1225);
    padding: 15px 25px;
    display: flex;
    justify-content: space-between;
    align-items: center;
    border-bottom: 2px solid #2a3050;
  }
  .header h1 { font-size: 1.3em; color: #7eb8ff; }
  .header .status-dot {
    width: 12px; height: 12px; border-radius: 50%;
    background: #4caf50; display: inline-block;
    animation: pulse 2s infinite;
  }
  @keyframes pulse { 0%,100% { opacity: 1; } 50% { opacity: 0.4; } }

  .grid {
    display: grid;
    grid-template-columns: 1fr 1fr 1fr;
    gap: 15px;
    padding: 15px;
    max-width: 1400px;
    margin: 0 auto;
  }
  .card {
    background: #131829;
    border-radius: 12px;
    padding: 20px;
    border: 1px solid #1e2740;
  }
  .card h3 {
    color: #8899bb;
    font-size: 0.85em;
    text-transform: uppercase;
    letter-spacing: 1px;
    margin-bottom: 12px;
  }

  /* Emotion Display */
  .emotion-box {
    text-align: center;
    padding: 15px;
  }
  .emotion-icon { font-size: 3.5em; margin-bottom: 10px; }
  .emotion-label {
    font-size: 1.6em;
    font-weight: bold;
    text-transform: uppercase;
  }
  .emotion-calm { color: #4caf50; }
  .emotion-aggressive { color: #f44336; }
  .emotion-fatigued { color: #ff9800; }
  .confidence-bar {
    width: 100%; height: 6px; background: #1e2740;
    border-radius: 3px; margin-top: 10px; overflow: hidden;
  }
  .confidence-fill {
    height: 100%; border-radius: 3px;
    transition: width 0.5s, background 0.5s;
  }

  /* Safety Score */
  .safety-ring {
    width: 130px; height: 130px;
    margin: 10px auto;
    position: relative;
  }
  .safety-ring svg { transform: rotate(-90deg); }
  .safety-ring .value {
    position: absolute;
    top: 50%; left: 50%;
    transform: translate(-50%, -50%);
    font-size: 2em; font-weight: bold;
  }

  /* Joystick */
  .joystick-container {
    display: flex;
    flex-direction: column;
    align-items: center;
  }
  .joystick-area {
    width: 200px; height: 200px;
    background: radial-gradient(circle, #1e2740, #131829);
    border-radius: 50%;
    border: 2px solid #2a3555;
    position: relative;
    touch-action: none;
    cursor: pointer;
  }
  .joystick-knob {
    width: 50px; height: 50px;
    background: radial-gradient(circle, #5588cc, #3366aa);
    border-radius: 50%;
    position: absolute;
    top: 50%; left: 50%;
    transform: translate(-50%, -50%);
    transition: none;
    box-shadow: 0 0 15px rgba(85,136,204,0.5);
  }
  .joystick-crosshair {
    position: absolute;
    top: 50%; left: 50%;
    transform: translate(-50%, -50%);
    width: 180px; height: 180px;
    pointer-events: none;
  }
  .joystick-crosshair::before, .joystick-crosshair::after {
    content: '';
    position: absolute;
    background: #2a3555;
  }
  .joystick-crosshair::before {
    width: 1px; height: 100%; left: 50%; top: 0;
  }
  .joystick-crosshair::after {
    height: 1px; width: 100%; top: 50%; left: 0;
  }
  .joy-values {
    margin-top: 10px;
    font-family: monospace;
    color: #6688aa;
  }

  /* Speed Control */
  .speed-display {
    text-align: center;
    margin-bottom: 15px;
  }
  .speed-value { font-size: 2.5em; font-weight: bold; color: #7eb8ff; }
  .speed-unit { color: #556688; font-size: 0.9em; }
  .speed-slider {
    width: 100%;
    -webkit-appearance: none;
    height: 8px;
    border-radius: 4px;
    background: #1e2740;
    outline: none;
  }
  .speed-slider::-webkit-slider-thumb {
    -webkit-appearance: none;
    width: 22px; height: 22px;
    border-radius: 50%;
    background: #5588cc;
    cursor: pointer;
  }
  .limit-info {
    text-align: center;
    margin-top: 8px;
    color: #667799;
    font-size: 0.85em;
  }

  /* Emergency Button */
  .emergency-btn {
    width: 100%; padding: 18px;
    font-size: 1.2em; font-weight: bold;
    border: none; border-radius: 10px;
    cursor: pointer;
    transition: all 0.3s;
    text-transform: uppercase;
    letter-spacing: 2px;
  }
  .emergency-btn.inactive {
    background: linear-gradient(135deg, #d32f2f, #b71c1c);
    color: white;
  }
  .emergency-btn.active {
    background: linear-gradient(135deg, #ff6659, #d32f2f);
    color: white;
    animation: flash 0.5s infinite;
  }
  @keyframes flash { 0%,100% { opacity: 1; } 50% { opacity: 0.6; } }

  /* Timeline Chart */
  .timeline-canvas { width: 100%; height: 120px; }

  /* Alert Log */
  .alert-list {
    max-height: 180px;
    overflow-y: auto;
    font-size: 0.85em;
  }
  .alert-item {
    padding: 6px 10px;
    border-left: 3px solid #2a3555;
    margin-bottom: 4px;
    background: #0d1225;
    border-radius: 0 4px 4px 0;
  }
  .alert-item.warn { border-left-color: #ff9800; }
  .alert-item.danger { border-left-color: #f44336; }

  /* Gauges Row */
  .gauges { display: flex; gap: 15px; justify-content: center; flex-wrap: wrap; }
  .gauge {
    text-align: center;
    padding: 10px 20px;
    background: #0d1225;
    border-radius: 8px;
    min-width: 100px;
  }
  .gauge .val { font-size: 1.4em; font-weight: bold; color: #7eb8ff; }
  .gauge .lbl { font-size: 0.75em; color: #556688; margin-top: 4px; }

  .full-width { grid-column: 1 / -1; }

  @media (max-width: 900px) {
    .grid { grid-template-columns: 1fr 1fr; }
  }
  @media (max-width: 600px) {
    .grid { grid-template-columns: 1fr; }
  }
</style>
</head>
<body>
  <div class="header">
    <h1>Emotion-Aware Vehicle Control</h1>
    <div><span class="status-dot" id="statusDot"></span> <span id="connStatus">Connected</span></div>
  </div>

  <div class="grid">
    <!-- Emotion Display -->
    <div class="card">
      <h3>Driver Emotion</h3>
      <div class="emotion-box">
        <div class="emotion-icon" id="emotionIcon">&#128522;</div>
        <div class="emotion-label emotion-calm" id="emotionLabel">CALM</div>
        <div class="confidence-bar">
          <div class="confidence-fill" id="confidenceFill" style="width:80%; background:#4caf50;"></div>
        </div>
        <div style="color:#556688; font-size:0.8em; margin-top:5px;">
          Confidence: <span id="confidenceVal">80%</span>
        </div>
      </div>
    </div>

    <!-- Safety Score -->
    <div class="card">
      <h3>Safety Score</h3>
      <div class="safety-ring">
        <svg width="130" height="130" viewBox="0 0 130 130">
          <circle cx="65" cy="65" r="55" fill="none" stroke="#1e2740" stroke-width="10"/>
          <circle cx="65" cy="65" r="55" fill="none" stroke="#4caf50" stroke-width="10"
            stroke-dasharray="345.6" stroke-dashoffset="0" stroke-linecap="round" id="safetyArc"/>
        </svg>
        <div class="value" id="safetyValue">100</div>
      </div>
      <div style="text-align:center; color:#556688; font-size:0.85em;">
        Aggressive: <span id="aggrCount">0</span> | Fatigued: <span id="fatCount">0</span>
      </div>
    </div>

    <!-- Speed Control -->
    <div class="card">
      <h3>Speed Control</h3>
      <div class="speed-display">
        <span class="speed-value" id="speedValue">0</span>
        <span class="speed-unit">/ <span id="speedMax">255</span></span>
      </div>
      <input type="range" class="speed-slider" id="speedSlider" min="0" max="255" value="255">
      <div class="limit-info">Manual limit: <span id="manualLimit">255</span> | Effective: <span id="effectiveLimit">255</span></div>
    </div>

    <!-- Joystick -->
    <div class="card">
      <h3>Joystick Control</h3>
      <div class="joystick-container">
        <div class="joystick-area" id="joystickArea">
          <div class="joystick-crosshair"></div>
          <div class="joystick-knob" id="joystickKnob"></div>
        </div>
        <div class="joy-values">
          X: <span id="joyXVal">0</span> | Y: <span id="joyYVal">0</span>
        </div>
      </div>
    </div>

    <!-- Emergency Stop -->
    <div class="card" style="display:flex; flex-direction:column; justify-content:center;">
      <h3>Emergency Override</h3>
      <button class="emergency-btn inactive" id="emergencyBtn" onclick="toggleEmergency()">
        EMERGENCY STOP
      </button>
      <div style="text-align:center; margin-top:10px; color:#556688; font-size:0.8em;">
        Click to activate emergency brake
      </div>
    </div>

    <!-- Gauges -->
    <div class="card">
      <h3>Vehicle Status</h3>
      <div class="gauges">
        <div class="gauge">
          <div class="val" id="gUptime">0s</div>
          <div class="lbl">Uptime</div>
        </div>
        <div class="gauge">
          <div class="val" id="gJoyMag">0</div>
          <div class="lbl">Joy Mag</div>
        </div>
        <div class="gauge">
          <div class="val" id="gDirection">--</div>
          <div class="lbl">Direction</div>
        </div>
      </div>
    </div>

    <!-- Emotion Timeline -->
    <div class="card full-width">
      <h3>Emotion & Speed Timeline</h3>
      <canvas class="timeline-canvas" id="timelineCanvas"></canvas>
    </div>

    <!-- Alert Log -->
    <div class="card full-width">
      <h3>Alert Log</h3>
      <div class="alert-list" id="alertList">
        <div class="alert-item">System ready</div>
      </div>
    </div>
  </div>

<script>
  const ESP_URL = '';  // Same origin

  // ===== Joystick Logic =====
  const joyArea = document.getElementById('joystickArea');
  const joyKnob = document.getElementById('joystickKnob');
  let joyActive = false;
  let joyX = 0, joyY = 0;

  function updateKnob(clientX, clientY) {
    const rect = joyArea.getBoundingClientRect();
    const cx = rect.left + rect.width / 2;
    const cy = rect.top + rect.height / 2;
    const maxR = rect.width / 2 - 25;

    let dx = clientX - cx;
    let dy = cy - clientY; // Invert Y so up = positive
    let dist = Math.sqrt(dx*dx + dy*dy);

    if (dist > maxR) {
      dx = dx / dist * maxR;
      dy = dy / dist * maxR;
      dist = maxR;
    }

    joyX = Math.round(dx / maxR * 100);
    joyY = Math.round(dy / maxR * 100);

    joyKnob.style.left = (50 + dx / (rect.width/2) * 50) + '%';
    joyKnob.style.top = (50 - dy / (rect.height/2) * 50) + '%';

    document.getElementById('joyXVal').textContent = joyX;
    document.getElementById('joyYVal').textContent = joyY;
  }

  function sendJoystick() {
    fetch(ESP_URL + '/api/joystick', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({x: joyX, y: joyY})
    }).catch(() => {});
  }

  // Mouse events
  joyArea.addEventListener('mousedown', (e) => { joyActive = true; updateKnob(e.clientX, e.clientY); });
  document.addEventListener('mousemove', (e) => { if (joyActive) updateKnob(e.clientX, e.clientY); });
  document.addEventListener('mouseup', () => {
    if (joyActive) {
      joyActive = false;
      joyX = 0; joyY = 0;
      joyKnob.style.left = '50%';
      joyKnob.style.top = '50%';
      document.getElementById('joyXVal').textContent = 0;
      document.getElementById('joyYVal').textContent = 0;
      sendJoystick();
    }
  });

  // Touch events
  joyArea.addEventListener('touchstart', (e) => {
    joyActive = true;
    const t = e.touches[0];
    updateKnob(t.clientX, t.clientY);
    e.preventDefault();
  });
  document.addEventListener('touchmove', (e) => {
    if (joyActive) {
      const t = e.touches[0];
      updateKnob(t.clientX, t.clientY);
    }
  });
  document.addEventListener('touchend', () => {
    if (joyActive) {
      joyActive = false;
      joyX = 0; joyY = 0;
      joyKnob.style.left = '50%';
      joyKnob.style.top = '50%';
      document.getElementById('joyXVal').textContent = 0;
      document.getElementById('joyYVal').textContent = 0;
      sendJoystick();
    }
  });

  // Send joystick at 10Hz when active
  setInterval(() => { if (joyActive) sendJoystick(); }, 100);

  // ===== Speed Slider =====
  document.getElementById('speedSlider').addEventListener('input', function() {
    const val = this.value;
    document.getElementById('manualLimit').textContent = val;
    fetch(ESP_URL + '/api/speed', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({limit: parseInt(val)})
    }).catch(() => {});
  });

  // ===== Emergency Stop =====
  function toggleEmergency() {
    fetch(ESP_URL + '/api/emergency', {method: 'POST'})
      .then(r => r.json())
      .then(d => {
        const btn = document.getElementById('emergencyBtn');
        if (d.emergency) {
          btn.className = 'emergency-btn active';
          btn.textContent = 'RELEASE EMERGENCY STOP';
        } else {
          btn.className = 'emergency-btn inactive';
          btn.textContent = 'EMERGENCY STOP';
        }
      }).catch(() => {});
  }

  // ===== Status Polling =====
  const emotionIcons = { calm: '&#128522;', aggressive: '&#128545;', fatigued: '&#128564;' };
  const emotionColors = { calm: '#4caf50', aggressive: '#f44336', fatigued: '#ff9800' };

  function pollStatus() {
    fetch(ESP_URL + '/api/status')
      .then(r => r.json())
      .then(d => {
        // Emotion
        document.getElementById('emotionIcon').innerHTML = emotionIcons[d.emotion] || '&#10067;';
        const lbl = document.getElementById('emotionLabel');
        lbl.textContent = d.emotion.toUpperCase();
        lbl.className = 'emotion-label emotion-' + d.emotion;

        const conf = Math.round((d.confidence || 0) * 100);
        document.getElementById('confidenceVal').textContent = conf + '%';
        const fill = document.getElementById('confidenceFill');
        fill.style.width = conf + '%';
        fill.style.background = emotionColors[d.emotion] || '#555';

        // Safety Score
        const score = d.safetyScore || 0;
        document.getElementById('safetyValue').textContent = score;
        const arc = document.getElementById('safetyArc');
        arc.style.strokeDashoffset = 345.6 * (1 - score / 100);
        arc.style.stroke = score > 70 ? '#4caf50' : score > 40 ? '#ff9800' : '#f44336';

        // Speed
        document.getElementById('speedValue').textContent = d.currentSpeed;
        document.getElementById('speedMax').textContent = d.speedLimit;
        document.getElementById('effectiveLimit').textContent = d.speedLimit;

        // Emergency
        const btn = document.getElementById('emergencyBtn');
        if (d.emergency) {
          btn.className = 'emergency-btn active';
          btn.textContent = 'RELEASE EMERGENCY STOP';
        } else {
          btn.className = 'emergency-btn inactive';
          btn.textContent = 'EMERGENCY STOP';
        }

        // Gauges
        document.getElementById('gUptime').textContent = d.uptime + 's';
        const mag = Math.round(Math.sqrt(d.joyX*d.joyX + d.joyY*d.joyY));
        document.getElementById('gJoyMag').textContent = mag;

        let dir = 'STOP';
        if (mag > 10) {
          if (Math.abs(d.joyY) > Math.abs(d.joyX)) {
            dir = d.joyY > 0 ? 'FWD' : 'REV';
          } else {
            dir = d.joyX > 0 ? 'RIGHT' : 'LEFT';
          }
        }
        document.getElementById('gDirection').textContent = dir;

        // Connection status
        document.getElementById('statusDot').style.background = '#4caf50';
        document.getElementById('connStatus').textContent = 'Connected';
      })
      .catch(() => {
        document.getElementById('statusDot').style.background = '#f44336';
        document.getElementById('connStatus').textContent = 'Disconnected';
      });
  }

  // ===== Timeline Chart =====
  let timelineData = [];

  function drawTimeline() {
    const canvas = document.getElementById('timelineCanvas');
    const ctx = canvas.getContext('2d');
    canvas.width = canvas.offsetWidth;
    canvas.height = canvas.offsetHeight;

    const w = canvas.width, h = canvas.height;
    ctx.clearRect(0, 0, w, h);

    if (timelineData.length < 2) return;

    // Draw emotion background bands
    const bandH = h / 2;
    timelineData.forEach((d, i) => {
      if (i === 0) return;
      const x1 = (i - 1) / (timelineData.length - 1) * w;
      const x2 = i / (timelineData.length - 1) * w;
      const col = d.e === 'calm' ? 'rgba(76,175,80,0.15)' :
                  d.e === 'aggressive' ? 'rgba(244,67,54,0.15)' :
                  'rgba(255,152,0,0.15)';
      ctx.fillStyle = col;
      ctx.fillRect(x1, 0, x2 - x1, bandH);
    });

    // Draw speed line
    ctx.beginPath();
    ctx.strokeStyle = '#7eb8ff';
    ctx.lineWidth = 2;
    timelineData.forEach((d, i) => {
      const x = i / (timelineData.length - 1) * w;
      const y = h - (d.s / 255 * h * 0.9) - 5;
      if (i === 0) ctx.moveTo(x, y);
      else ctx.lineTo(x, y);
    });
    ctx.stroke();

    // Labels
    ctx.fillStyle = '#556688';
    ctx.font = '11px monospace';
    ctx.fillText('Speed', 5, h - 5);
    ctx.fillText('Emotion', 5, 14);
  }

  function pollHistory() {
    fetch(ESP_URL + '/api/history')
      .then(r => r.json())
      .then(data => {
        timelineData = data;
        drawTimeline();
      }).catch(() => {});
  }

  function pollAlerts() {
    fetch(ESP_URL + '/api/alerts')
      .then(r => r.json())
      .then(alerts => {
        const list = document.getElementById('alertList');
        list.innerHTML = '';
        alerts.forEach(a => {
          const div = document.createElement('div');
          div.className = 'alert-item' +
            (a.includes('WARNING') ? ' warn' : '') +
            (a.includes('CRITICAL') || a.includes('EMERGENCY') ? ' danger' : '');
          div.textContent = a;
          list.appendChild(div);
        });
      }).catch(() => {});
  }

  // ===== Keyboard Controls =====
  const keyState = {};
  document.addEventListener('keydown', (e) => {
    if (keyState[e.key]) return;
    keyState[e.key] = true;
    updateKeyboardJoystick();
    if (e.key === ' ') { e.preventDefault(); toggleEmergency(); }
  });
  document.addEventListener('keyup', (e) => {
    delete keyState[e.key];
    updateKeyboardJoystick();
  });

  function updateKeyboardJoystick() {
    let kx = 0, ky = 0;
    if (keyState['ArrowUp'] || keyState['w'] || keyState['W']) ky += 100;
    if (keyState['ArrowDown'] || keyState['s'] || keyState['S']) ky -= 100;
    if (keyState['ArrowLeft'] || keyState['a'] || keyState['A']) kx -= 100;
    if (keyState['ArrowRight'] || keyState['d'] || keyState['D']) kx += 100;

    if (kx !== 0 || ky !== 0 || (joyX !== 0 || joyY !== 0)) {
      joyX = kx; joyY = ky;
      document.getElementById('joyXVal').textContent = joyX;
      document.getElementById('joyYVal').textContent = joyY;

      // Update knob visual
      joyKnob.style.left = (50 + kx/2) + '%';
      joyKnob.style.top = (50 - ky/2) + '%';

      sendJoystick();
    }
  }

  // ===== Start Polling =====
  setInterval(pollStatus, 500);
  setInterval(pollHistory, 3000);
  setInterval(pollAlerts, 2000);
  pollStatus();
  pollHistory();
  pollAlerts();
</script>
</body>
</html>
)rawliteral";

  server.send(200, "text/html", html);
}
