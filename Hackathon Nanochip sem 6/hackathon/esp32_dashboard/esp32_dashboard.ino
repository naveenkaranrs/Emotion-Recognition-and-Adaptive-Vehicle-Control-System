/*
 * ==========================================================
 *  EMOTION-AWARE VEHICLE CONTROL SYSTEM
 *  ESP32 Standalone — Dashboard + Motor Controller
 * ==========================================================
 *
 *  NO STM32 REQUIRED — ESP32 drives L298N directly!
 *
 *  L298N Connections:
 *    ESP32 GPIO25 --> IN1  (Left motor dir 1)
 *    ESP32 GPIO26 --> IN2  (Left motor dir 2)
 *    ESP32 GPIO27 --> ENA  (Left motor PWM)
 *    ESP32 GPIO32 --> IN3  (Right motor dir 1)
 *    ESP32 GPIO33 --> IN4  (Right motor dir 2)
 *    ESP32 GPIO14 --> ENB  (Right motor PWM)
 *    GND <--> GND (ESP32 + L298N + Battery)
 *
 *  Features:
 *    - Web dashboard with joystick, speed control, emotion display
 *    - Receives emotion data from Python script via HTTP POST
 *    - Directly controls motors via L298N driver
 *    - Differential steering, smooth PWM ramping
 *    - Emotion-based speed limiting & safety policies
 *    - Emergency stop, watchdog, real-time alerts
 *
 *  Libraries Required (Arduino Library Manager):
 *    - ArduinoJson by Benoit Blanchon (v7.x)
 *
 *  Board: ESP32 Dev Module
 * ==========================================================
 */

#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>

// ===== WiFi Configuration =====
const char* ssid = "Harish";
const char* password = "123456799";

// ===== L298N Motor Driver Pins (ESP32 GPIO) =====
#define IN1   25    // Left motor direction 1
#define IN2   26    // Left motor direction 2
#define ENA   27    // Left motor PWM speed

#define IN3   32    // Right motor direction 1
#define IN4   33    // Right motor direction 2
#define ENB   14    // Right motor PWM speed

// ===== LEDC PWM Configuration =====
#define PWM_FREQ      1000   // 1 kHz
#define PWM_RESOLUTION 8     // 8-bit (0-255)

// ===== Web Server =====
WebServer server(80);

// ===== Motor State =====
int currentLeftPWM = 0;
int currentRightPWM = 0;
int targetLeftPWM = 0;
int targetRightPWM = 0;
const int RAMP_STEP = 12;
unsigned long lastRampTime = 0;

// ===== System State =====
struct VehicleState {
  String emotion = "calm";
  int speedLimit = 255;
  int currentSpeed = 0;
  int joyX = 0;
  int joyY = 0;
  bool emergencyStop = false;
  int safetyScore = 100;
  String lastAlert = "System OK";
  unsigned long lastEmotionUpdate = 0;
  unsigned long lastJoystickUpdate = 0;
  int aggressiveCount = 0;
  int fatiguedCount = 0;
  float emotionConfidence = 0.0;
} state;

// ===== Watchdog =====
const unsigned long WATCHDOG_MS = 3000;
bool watchdogActive = false;

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
void computeMotors();
void rampMotors();
void driveMotor(int pin1, int pin2, int pwmChannel, int speed);
void stopMotors();
void applyEmotionPolicy();
void addAlert(String msg);
void updateHistory();

void setup() {
  Serial.begin(115200);
  Serial.println(F("\n============================================"));
  Serial.println(F("  EMOTION-AWARE VEHICLE CONTROL SYSTEM"));
  Serial.println(F("  ESP32 Standalone — Dashboard + Motors"));
  Serial.println(F("============================================"));

  // ----- Motor Pin Setup -----
  pinMode(IN1, OUTPUT);
  pinMode(IN2, OUTPUT);
  pinMode(IN3, OUTPUT);
  pinMode(IN4, OUTPUT);

  // LEDC PWM setup for ENA and ENB (ESP32 Arduino Core v3.x API)
  ledcAttach(ENA, PWM_FREQ, PWM_RESOLUTION);
  ledcAttach(ENB, PWM_FREQ, PWM_RESOLUTION);

  stopMotors();
  Serial.println(F("[OK] Motor pins initialized (GPIO 25,26,27,32,33,14)"));
  Serial.println(F("[OK] L298N: Left(IN1=25,IN2=26,ENA=27) Right(IN3=32,IN4=33,ENB=14)"));

  // ----- WiFi -----
  WiFi.begin(ssid, password);
  Serial.print(F("[WiFi] Connecting to "));
  Serial.print(ssid);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(F("\n[WiFi] Connected!"));
    Serial.print(F("[WiFi] Dashboard: http://"));
    Serial.println(WiFi.localIP());
  } else {
    Serial.println(F("\n[WiFi] STA failed — starting AP mode"));
    WiFi.softAP("EmotionVehicle", "12345678");
    Serial.print(F("[WiFi] AP Dashboard: http://"));
    Serial.println(WiFi.softAPIP());
  }

  // ----- Web Routes -----
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
  Serial.println(F("[OK] Web server started"));
  addAlert("System initialized — ESP32 standalone mode");

  state.lastJoystickUpdate = millis();
  Serial.println(F("\n[READY] Dashboard is live. Open in browser."));
}

void loop() {
  server.handleClient();

  // Compute motor targets from joystick (continuous)
  computeMotors();

  // Ramp motors smoothly every 20ms
  if (millis() - lastRampTime >= 20) {
    rampMotors();
    lastRampTime = millis();
  }

  // Watchdog: stop motors if no joystick input for 3s
  if (millis() - state.lastJoystickUpdate > WATCHDOG_MS && !watchdogActive) {
    watchdogActive = true;
    stopMotors();
    state.joyX = 0;
    state.joyY = 0;
    state.currentSpeed = 0;
    Serial.println(F("[WATCHDOG] No joystick input for 3s — motors stopped"));
    addAlert("Watchdog: motors stopped (no input)");
  }

  // Update history every 2 seconds
  static unsigned long lastHistory = 0;
  if (millis() - lastHistory > 2000) {
    updateHistory();
    lastHistory = millis();
  }

  // Emotion confidence decay
  if (millis() - state.lastEmotionUpdate > 5000 && state.lastEmotionUpdate > 0) {
    state.emotionConfidence *= 0.95;
  }

  // Safety score recovery when calm
  static unsigned long lastScoreUpdate = 0;
  if (millis() - lastScoreUpdate > 1000) {
    if (state.emotion == "calm" && state.safetyScore < 100) {
      state.safetyScore = min(100, state.safetyScore + 1);
    }
    lastScoreUpdate = millis();
  }

  // Debug motor output every 500ms
  static unsigned long lastDebug = 0;
  if (millis() - lastDebug > 500) {
    if (currentLeftPWM != 0 || currentRightPWM != 0) {
      Serial.printf("[MOTORS] L=%d R=%d | Joy(%d,%d) | Limit=%d | Emotion=%s\n",
        currentLeftPWM, currentRightPWM,
        state.joyX, state.joyY,
        state.speedLimit, state.emotion.c_str());
    }
    lastDebug = millis();
  }
}

// ===== Compute Motor Targets from Joystick =====
void computeMotors() {
  if (state.emergencyStop || watchdogActive) {
    targetLeftPWM = 0;
    targetRightPWM = 0;
    return;
  }

  int jx = state.joyX;
  int jy = state.joyY;

  // Dead zone
  if (abs(jx) < 8) jx = 0;
  if (abs(jy) < 8) jy = 0;

  if (jx == 0 && jy == 0) {
    targetLeftPWM = 0;
    targetRightPWM = 0;
    return;
  }

  // Differential steering
  float throttle = jy / 100.0;   // -1.0 to 1.0
  float steering = jx / 100.0;

  float leftF  = throttle + steering;
  float rightF = throttle - steering;

  // Normalize to [-1, 1]
  float maxV = max(abs(leftF), abs(rightF));
  if (maxV > 1.0) { leftF /= maxV; rightF /= maxV; }

  // Scale to speed limit, convert to PWM (-255 to 255)
  targetLeftPWM  = (int)(leftF  * state.speedLimit);
  targetRightPWM = (int)(rightF * state.speedLimit);
  targetLeftPWM  = constrain(targetLeftPWM,  -255, 255);
  targetRightPWM = constrain(targetRightPWM, -255, 255);

  // Motor dead zone (prevent whine at very low PWM)
  if (abs(targetLeftPWM)  > 0 && abs(targetLeftPWM)  < 25) targetLeftPWM = 0;
  if (abs(targetRightPWM) > 0 && abs(targetRightPWM) < 25) targetRightPWM = 0;
}

// ===== Ramp Motors Smoothly =====
void rampMotors() {
  currentLeftPWM  = rampVal(currentLeftPWM,  targetLeftPWM,  RAMP_STEP);
  currentRightPWM = rampVal(currentRightPWM, targetRightPWM, RAMP_STEP);

  driveMotor(IN1, IN2, ENA, currentLeftPWM);
  driveMotor(IN3, IN4, ENB, currentRightPWM);
}

int rampVal(int cur, int tgt, int step) {
  if (cur < tgt) return min(cur + step, tgt);
  if (cur > tgt) return max(cur - step, tgt);
  return cur;
}

// ===== Drive a Single Motor =====
void driveMotor(int pin1, int pin2, int pwmPin, int speed) {
  if (speed > 0) {
    digitalWrite(pin1, HIGH);
    digitalWrite(pin2, LOW);
    ledcWrite(pwmPin, speed);
  } else if (speed < 0) {
    digitalWrite(pin1, LOW);
    digitalWrite(pin2, HIGH);
    ledcWrite(pwmPin, -speed);
  } else {
    digitalWrite(pin1, LOW);
    digitalWrite(pin2, LOW);
    ledcWrite(pwmPin, 0);
  }
}

// ===== Stop All Motors =====
void stopMotors() {
  currentLeftPWM = 0;
  currentRightPWM = 0;
  targetLeftPWM = 0;
  targetRightPWM = 0;
  driveMotor(IN1, IN2, ENA, 0);
  driveMotor(IN3, IN4, ENB, 0);
}

// ===== Apply Emotion-Based Driving Policy =====
void applyEmotionPolicy() {
  if (state.emotion == "emergency") {
    state.speedLimit = 0;
    addAlert("EMERGENCY (Camera Lost)");
    stopMotors();
  }
  else if (state.emotion == "aggressive") {
    state.speedLimit = 0;
    state.safetyScore = max(0, state.safetyScore - 5);
    state.aggressiveCount++;
    addAlert("AGGRESSIVE (Vehicle Stopped)");
    stopMotors();
  }
  else if (state.emotion == "fatigued") {
    state.speedLimit = 100;
    state.safetyScore = max(0, state.safetyScore - 3);
    state.fatiguedCount++;
    if (state.fatiguedCount >= 5) {
      addAlert("CRITICAL: Severe fatigue! Please stop driving.");
      state.speedLimit = 60;
    }
    addAlert("Fatigue detected — Speed reduced to " + String(state.speedLimit));
  }
  else if (state.emotion == "calm") {
    state.speedLimit = 255;
    state.aggressiveCount = max(0, state.aggressiveCount - 1);
    state.fatiguedCount = max(0, state.fatiguedCount - 1);
  }
}

// ===== API: POST /api/emotion =====
void handleEmotion() {
  if (server.hasArg("plain")) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, server.arg("plain"));
    if (!err) {
      String newEmotion = doc["emotion"].as<String>();
      state.emotionConfidence = doc["confidence"] | 0.5;

      if (newEmotion == "calm" || newEmotion == "aggressive" || newEmotion == "fatigued" || newEmotion == "emergency") {
        if (newEmotion != state.emotion) {
          Serial.println("[EMO] " + state.emotion + " -> " + newEmotion);
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
      int magnitude = sqrt(state.joyX * state.joyX + state.joyY * state.joyY);
      state.currentSpeed = map(constrain(magnitude, 0, 100), 0, 100, 0, state.speedLimit);

      // Reset watchdog on joystick input
      state.lastJoystickUpdate = millis();
      watchdogActive = false;

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
      int emotionMax = 255;
      if (state.emotion == "aggressive") emotionMax = 120;
      else if (state.emotion == "fatigued") emotionMax = 100;
      state.speedLimit = min(requestedLimit, emotionMax);
      server.send(200, "application/json",
                  "{\"status\":\"ok\",\"effective\":" + String(state.speedLimit) + "}");
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
    stopMotors();
    state.joyX = 0;
    state.joyY = 0;
    state.currentSpeed = 0;
  } else {
    addAlert("Emergency stop released");
    watchdogActive = false;
    state.lastJoystickUpdate = millis();
  }
  server.send(200, "application/json",
              "{\"emergency\":" + String(state.emergencyStop ? "true" : "false") + "}");
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
  doc["leftPWM"] = currentLeftPWM;
  doc["rightPWM"] = currentRightPWM;
  doc["motorsActive"] = (currentLeftPWM != 0 || currentRightPWM != 0);

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
    if (alertLog[idx].length() > 0) arr.add(alertLog[idx]);
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
    state.emotion, state.currentSpeed, millis() / 1000
  };
  historyIndex++;
}

// ===== Dashboard HTML =====
void handleRoot() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Emotion-Aware Vehicle Control</title>
<style>
  *{margin:0;padding:0;box-sizing:border-box}
  body{
    font-family:'Segoe UI',system-ui,sans-serif;
    background:#0a0e17;color:#e0e6f0;min-height:100vh;
    background-image:radial-gradient(ellipse at 20% 80%,rgba(0,150,255,0.04) 0%,transparent 50%),
                     radial-gradient(ellipse at 80% 20%,rgba(0,230,118,0.03) 0%,transparent 50%);
  }
  .header{
    background:linear-gradient(135deg,#1a1f35,#0d1225);
    padding:14px 20px;display:flex;justify-content:space-between;align-items:center;
    border-bottom:2px solid #2a3050;
  }
  .header h1{font-size:1.15em;color:#7eb8ff;letter-spacing:1px}
  .conn-info{display:flex;align-items:center;gap:8px;font-size:0.8em}
  .dot{width:10px;height:10px;border-radius:50%;display:inline-block}
  .dot.on{background:#4caf50;box-shadow:0 0 8px rgba(76,175,80,0.6)}
  .dot.off{background:#f44336;box-shadow:0 0 8px rgba(244,67,54,0.4)}

  .grid{
    display:grid;grid-template-columns:1fr 1fr 1fr;gap:14px;
    padding:14px;max-width:1400px;margin:0 auto;
  }
  .card{
    background:#131829;border-radius:14px;padding:18px;
    border:1px solid #1e2740;transition:border-color 0.3s;
  }
  .card:hover{border-color:#2a3555}
  .card h3{
    color:#7799bb;font-size:0.78em;text-transform:uppercase;
    letter-spacing:1.5px;margin-bottom:12px;
  }
  .full{grid-column:1/-1}

  /* Emotion */
  .emo-box{text-align:center;padding:10px}
  .emo-icon{font-size:3.2em;margin-bottom:8px;transition:transform 0.3s}
  .emo-label{font-size:1.5em;font-weight:700;text-transform:uppercase;letter-spacing:2px}
  .emo-calm{color:#4caf50}.emo-aggressive{color:#f44336}.emo-fatigued{color:#ff9800}
  .conf-bar{width:100%;height:5px;background:#1e2740;border-radius:3px;margin-top:8px;overflow:hidden}
  .conf-fill{height:100%;border-radius:3px;transition:width 0.5s,background 0.5s}
  .conf-text{color:#556688;font-size:0.75em;margin-top:4px}

  /* Safety Ring */
  .safety-wrap{text-align:center}
  .safety-ring{width:120px;height:120px;margin:8px auto;position:relative}
  .safety-ring svg{transform:rotate(-90deg)}
  .safety-ring circle{fill:none;stroke-linecap:round}
  .safety-ring .bg{stroke:#1e2740;stroke-width:10}
  .safety-ring .fg{stroke-width:10;transition:stroke-dashoffset 0.6s,stroke 0.3s}
  .safety-val{
    position:absolute;top:50%;left:50%;transform:translate(-50%,-50%);
    font-size:1.8em;font-weight:700;
  }
  .safety-sub{color:#556688;font-size:0.8em;margin-top:4px}

  /* Joystick */
  .joy-center{display:flex;flex-direction:column;align-items:center}
  .joy-area{
    width:190px;height:190px;
    background:radial-gradient(circle,#1e2740,#131829);
    border-radius:50%;border:2px solid #2a3555;
    position:relative;touch-action:none;cursor:pointer;
  }
  .joy-knob{
    width:48px;height:48px;
    background:radial-gradient(circle,#5588cc,#3366aa);
    border-radius:50%;position:absolute;
    top:50%;left:50%;transform:translate(-50%,-50%);
    box-shadow:0 0 18px rgba(85,136,204,0.4);
    transition:box-shadow 0.2s;
  }
  .joy-knob.active{box-shadow:0 0 25px rgba(85,136,204,0.7)}
  .joy-cross{
    position:absolute;top:50%;left:50%;transform:translate(-50%,-50%);
    width:170px;height:170px;pointer-events:none;
  }
  .joy-cross::before,.joy-cross::after{content:'';position:absolute;background:#2a3555}
  .joy-cross::before{width:1px;height:100%;left:50%;top:0}
  .joy-cross::after{height:1px;width:100%;top:50%;left:0}
  .joy-labels{
    position:absolute;top:50%;left:50%;transform:translate(-50%,-50%);
    width:180px;height:180px;pointer-events:none;
    font-size:0.6em;color:#3a4a6a;
  }
  .joy-labels span{position:absolute}
  .joy-labels .jl-u{top:4px;left:50%;transform:translateX(-50%)}
  .joy-labels .jl-d{bottom:4px;left:50%;transform:translateX(-50%)}
  .joy-labels .jl-l{left:4px;top:50%;transform:translateY(-50%)}
  .joy-labels .jl-r{right:4px;top:50%;transform:translateY(-50%)}
  .joy-vals{margin-top:8px;font-family:monospace;color:#6688aa;font-size:0.85em}

  /* Speed */
  .spd-display{text-align:center;margin-bottom:12px}
  .spd-val{font-size:2.3em;font-weight:700;color:#7eb8ff}
  .spd-unit{color:#556688;font-size:0.85em}
  .spd-slider{
    width:100%;-webkit-appearance:none;height:8px;
    border-radius:4px;background:#1e2740;outline:none;
  }
  .spd-slider::-webkit-slider-thumb{
    -webkit-appearance:none;width:22px;height:22px;
    border-radius:50%;background:#5588cc;cursor:pointer;
    box-shadow:0 0 10px rgba(85,136,204,0.4);
  }
  .spd-info{text-align:center;margin-top:8px;color:#556688;font-size:0.8em}

  /* Emergency */
  .emg-btn{
    width:100%;padding:16px;font-size:1.1em;font-weight:700;
    border:none;border-radius:10px;cursor:pointer;
    transition:all 0.3s;text-transform:uppercase;letter-spacing:2px;
  }
  .emg-off{background:linear-gradient(135deg,#c62828,#b71c1c);color:#fff;
           box-shadow:0 4px 20px rgba(198,40,40,0.4)}
  .emg-off:hover{transform:translateY(-2px);box-shadow:0 6px 28px rgba(198,40,40,0.6)}
  .emg-on{background:linear-gradient(135deg,#ff6659,#d32f2f);color:#fff;
          animation:flash 0.5s infinite}
  @keyframes flash{0%,100%{opacity:1}50%{opacity:0.6}}

  /* Motor bars */
  .motor-row{display:flex;gap:20px;justify-content:center;margin-top:12px}
  .motor-col{text-align:center}
  .motor-col label{font-size:0.7em;color:#556688;letter-spacing:1px}
  .m-track{
    width:30px;height:100px;background:#0d1225;
    border-radius:6px;margin:4px auto;position:relative;
    overflow:hidden;border:1px solid #1e2740;
  }
  .m-fill{
    position:absolute;left:0;right:0;bottom:50%;
    background:linear-gradient(0deg,#5588cc,#7eb8ff);
    border-radius:0 0 6px 6px;transition:height 0.15s;
  }
  .m-fill.rev{bottom:auto;top:50%;background:linear-gradient(180deg,#ff9800,#ffb74d);
              border-radius:6px 6px 0 0}
  .m-center{position:absolute;left:0;right:0;top:50%;height:1px;background:#3a4a6a}
  .m-val{font-family:monospace;font-size:0.75em;color:#7eb8ff;margin-top:2px}

  /* Status gauges */
  .gauges{display:flex;gap:12px;justify-content:center;flex-wrap:wrap}
  .gauge{
    text-align:center;padding:10px 16px;
    background:#0d1225;border-radius:8px;min-width:85px;
  }
  .gauge .val{font-size:1.2em;font-weight:700;color:#7eb8ff}
  .gauge .lbl{font-size:0.7em;color:#556688;margin-top:3px}

  /* Timeline canvas */
  .tl-canvas{width:100%;height:120px}

  /* Alerts */
  .alert-list{max-height:180px;overflow-y:auto;font-size:0.82em}
  .alert-item{
    padding:6px 10px;border-left:3px solid #2a3555;
    margin-bottom:3px;background:#0d1225;border-radius:0 4px 4px 0;
  }
  .alert-item.warn{border-left-color:#ff9800}
  .alert-item.danger{border-left-color:#f44336}

  @media(max-width:900px){.grid{grid-template-columns:1fr 1fr}}
  @media(max-width:600px){.grid{grid-template-columns:1fr}}
</style>
</head>
<body>
<div class="header">
  <h1>&#128663; Emotion-Aware Vehicle Control</h1>
  <div class="conn-info">
    <span class="dot on" id="wifiDot"></span><span id="wifiText">WiFi</span>
    &nbsp;|&nbsp;
    <span class="dot off" id="motorDot"></span><span id="motorText">Motors</span>
  </div>
</div>

<div class="grid">
  <!-- Emotion -->
  <div class="card">
    <h3>&#129504; Driver Emotion</h3>
    <div class="emo-box">
      <div class="emo-icon" id="emoIcon">&#128522;</div>
      <div class="emo-label emo-calm" id="emoLabel">CALM</div>
      <div class="conf-bar"><div class="conf-fill" id="confFill" style="width:80%;background:#4caf50"></div></div>
      <div class="conf-text">Confidence: <span id="confVal">80%</span></div>
    </div>
  </div>

  <!-- Safety -->
  <div class="card">
    <h3>&#128737;&#65039; Safety Score</h3>
    <div class="safety-wrap">
      <div class="safety-ring">
        <svg width="120" height="120" viewBox="0 0 120 120">
          <circle class="bg" cx="60" cy="60" r="50"/>
          <circle class="fg" id="safetyArc" cx="60" cy="60" r="50"
            stroke-dasharray="314.2" stroke-dashoffset="0" stroke="#4caf50"/>
        </svg>
        <div class="safety-val" id="safetyVal">100</div>
      </div>
      <div class="safety-sub">
        Aggressive: <span id="aggrCnt">0</span> | Fatigued: <span id="fatCnt">0</span>
      </div>
    </div>
  </div>

  <!-- Speed -->
  <div class="card">
    <h3>&#9889; Speed Control</h3>
    <div class="spd-display">
      <span class="spd-val" id="spdVal">0</span>
      <span class="spd-unit">/ <span id="spdMax">255</span></span>
    </div>
    <input type="range" class="spd-slider" id="spdSlider" min="0" max="255" value="255">
    <div class="spd-info">Manual: <span id="manLim">255</span> | Effective: <span id="effLim">255</span></div>
  </div>

  <!-- Joystick -->
  <div class="card">
    <h3>&#127918; Joystick Control</h3>
    <div class="joy-center">
      <div class="joy-area" id="joyArea">
        <div class="joy-cross"></div>
        <div class="joy-labels">
          <span class="jl-u">FWD</span><span class="jl-d">REV</span>
          <span class="jl-l">L</span><span class="jl-r">R</span>
        </div>
        <div class="joy-knob" id="joyKnob"></div>
      </div>
      <div class="joy-vals">X: <span id="jxV">0</span> &nbsp;|&nbsp; Y: <span id="jyV">0</span></div>
    </div>
  </div>

  <!-- Emergency + Motors -->
  <div class="card" style="display:flex;flex-direction:column;justify-content:space-between">
    <div>
      <h3>&#128680; Emergency Override</h3>
      <button class="emg-btn emg-off" id="emgBtn" onclick="toggleEmg()">EMERGENCY STOP</button>
      <div style="text-align:center;margin-top:8px;color:#556688;font-size:0.75em">Click or press SPACE</div>
    </div>
    <div style="margin-top:auto">
      <h3>&#9881;&#65039; Motor Output</h3>
      <div class="motor-row">
        <div class="motor-col">
          <label>LEFT</label>
          <div class="m-track"><div class="m-center"></div><div class="m-fill" id="mL"></div></div>
          <div class="m-val" id="mLv">0</div>
        </div>
        <div class="motor-col">
          <label>RIGHT</label>
          <div class="m-track"><div class="m-center"></div><div class="m-fill" id="mR"></div></div>
          <div class="m-val" id="mRv">0</div>
        </div>
      </div>
    </div>
  </div>

  <!-- Motor Status -->
  <div class="card">
    <h3>&#128268; Motor Status</h3>
    <div class="gauges">
      <div class="gauge"><div class="val" id="gUp">0s</div><div class="lbl">Uptime</div></div>
      <div class="gauge"><div class="val" id="gMag">0</div><div class="lbl">Joy Mag</div></div>
      <div class="gauge"><div class="val" id="gDir">STOP</div><div class="lbl">Direction</div></div>
      <div class="gauge"><div class="val" id="gMotor">OFF</div><div class="lbl">Motors</div></div>
    </div>
  </div>

  <!-- Camera Feed -->
  <div class="card full" style="display:flex;flex-direction:column">
    <div style="display:flex;justify-content:space-between;align-items:center;margin-bottom:10px">
      <h3>&#128247; Live Camera Feed</h3>
      <div style="display:flex;gap:8px">
        <button onclick="toggleVoice()" id="voiceBtn" style="background:#2a3555;border:none;color:#fff;padding:6px 12px;border-radius:6px;cursor:pointer;font-weight:bold">&#128226; Voice ON</button>
        <input type="text" id="camIp" placeholder="Phone IP (e.g. 192.168.1.5)" style="background:#1e2740;border:1px solid #2a3555;color:#e0e6f0;padding:6px 10px;border-radius:6px;width:180px">
        <button onclick="updateCam()" style="background:#5588cc;border:none;color:#fff;padding:6px 12px;border-radius:6px;cursor:pointer;font-weight:bold">Connect</button>
      </div>
    </div>
    <div style="background:#000;border-radius:8px;overflow:hidden;display:flex;justify-content:center;align-items:center;min-height:360px;border:1px solid #1e2740">
      <img id="camImg" src="" alt="Waiting for camera connection..." style="max-width:100%;max-height:480px;color:#556688;font-family:monospace">
    </div>
  </div>

  <!-- Timeline -->
  <div class="card full">
    <h3>&#128200; Emotion & Speed Timeline</h3>
    <canvas class="tl-canvas" id="tlCanvas"></canvas>
  </div>

  <!-- Alerts -->
  <div class="card full">
    <h3>&#128276; Alert Log</h3>
    <div class="alert-list" id="alertList"><div class="alert-item">System ready</div></div>
  </div>
</div>

<script>
const URL='';
const jA=document.getElementById('joyArea'),jK=document.getElementById('joyKnob');
let jActive=false,jX=0,jY=0;

function updKnob(cx,cy){
  const r=jA.getBoundingClientRect(),
        mx=r.left+r.width/2,my=r.top+r.height/2,
        mR=r.width/2-24;
  let dx=cx-mx,dy=my-cy,d=Math.sqrt(dx*dx+dy*dy);
  if(d>mR){dx=dx/d*mR;dy=dy/d*mR}
  jX=Math.round(dx/mR*100);jY=Math.round(dy/mR*100);
  jK.style.left=(50+dx/(r.width/2)*50)+'%';
  jK.style.top=(50-dy/(r.height/2)*50)+'%';
  document.getElementById('jxV').textContent=jX;
  document.getElementById('jyV').textContent=jY;
}
function sendJoy(){
  fetch(URL+'/api/joystick',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({x:jX,y:jY})}).catch(()=>{});
}
jA.addEventListener('mousedown',e=>{jActive=true;jK.classList.add('active');updKnob(e.clientX,e.clientY)});
document.addEventListener('mousemove',e=>{if(jActive)updKnob(e.clientX,e.clientY)});
document.addEventListener('mouseup',()=>{
  if(jActive){jActive=false;jX=0;jY=0;jK.classList.remove('active');
    jK.style.left='50%';jK.style.top='50%';
    document.getElementById('jxV').textContent=0;
    document.getElementById('jyV').textContent=0;sendJoy()}
});
jA.addEventListener('touchstart',e=>{jActive=true;jK.classList.add('active');
  updKnob(e.touches[0].clientX,e.touches[0].clientY);e.preventDefault()},{passive:false});
document.addEventListener('touchmove',e=>{if(jActive)updKnob(e.touches[0].clientX,e.touches[0].clientY)});
document.addEventListener('touchend',()=>{
  if(jActive){jActive=false;jX=0;jY=0;jK.classList.remove('active');
    jK.style.left='50%';jK.style.top='50%';
    document.getElementById('jxV').textContent=0;
    document.getElementById('jyV').textContent=0;sendJoy()}
});
setInterval(()=>{if(jActive)sendJoy()},100);

document.getElementById('spdSlider').addEventListener('input',function(){
  document.getElementById('manLim').textContent=this.value;
  fetch(URL+'/api/speed',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({limit:parseInt(this.value)})}).catch(()=>{});
});

function toggleEmg(){
  fetch(URL+'/api/emergency',{method:'POST'}).then(r=>r.json()).then(d=>{
    const b=document.getElementById('emgBtn');
    b.className='emg-btn '+(d.emergency?'emg-on':'emg-off');
    b.textContent=d.emergency?'RELEASE EMERGENCY STOP':'EMERGENCY STOP';
  }).catch(()=>{});
}

const eIcons={calm:'\u{1F60A}',aggressive:'\u{1F621}',fatigued:'\u{1F634}'};
const eColors={calm:'#4caf50',aggressive:'#f44336',fatigued:'#ff9800'};

function poll(){
  fetch(URL+'/api/status').then(r=>r.json()).then(d=>{
    if (d.emotion && d.emotion !== lastSpokenEmo) {
      lastSpokenEmo = d.emotion;
      if (phrases[d.emotion]) speak(phrases[d.emotion]);
    }
    document.getElementById('emoIcon').innerHTML=eIcons[d.emotion]||'\u2753';
    const el=document.getElementById('emoLabel');
    el.textContent=d.emotion.toUpperCase();el.className='emo-label emo-'+d.emotion;
    const cf=Math.round((d.confidence||0)*100);
    document.getElementById('confVal').textContent=cf+'%';
    const fl=document.getElementById('confFill');fl.style.width=cf+'%';fl.style.background=eColors[d.emotion]||'#555';
    const sc=d.safetyScore||0;
    document.getElementById('safetyVal').textContent=sc;
    const arc=document.getElementById('safetyArc');
    arc.style.strokeDashoffset=314.2*(1-sc/100);
    arc.style.stroke=sc>70?'#4caf50':sc>40?'#ff9800':'#f44336';
    document.getElementById('spdVal').textContent=d.currentSpeed;
    document.getElementById('spdMax').textContent=d.speedLimit;
    document.getElementById('effLim').textContent=d.speedLimit;
    const b=document.getElementById('emgBtn');
    b.className='emg-btn '+(d.emergency?'emg-on':'emg-off');
    b.textContent=d.emergency?'RELEASE EMERGENCY STOP':'EMERGENCY STOP';
    document.getElementById('gUp').textContent=d.uptime+'s';
    const mg=Math.round(Math.sqrt(d.joyX*d.joyX+d.joyY*d.joyY));
    document.getElementById('gMag').textContent=mg;
    let dir='STOP';
    if(mg>10){if(Math.abs(d.joyY)>Math.abs(d.joyX))dir=d.joyY>0?'FWD':'REV';else dir=d.joyX>0?'RIGHT':'LEFT'}
    document.getElementById('gDir').textContent=dir;

    // Motor status
    const mActive=d.motorsActive;
    document.getElementById('gMotor').textContent=mActive?'RUNNING':'OFF';
    document.getElementById('gMotor').style.color=mActive?'#4caf50':'#556688';
    const mDot=document.getElementById('motorDot');
    mDot.className='dot '+(mActive?'on':'off');
    document.getElementById('motorText').textContent=mActive?'Running':'Idle';

    // Motor bars
    updMotor('mL','mLv',d.leftPWM||0);
    updMotor('mR','mRv',d.rightPWM||0);
    document.getElementById('wifiDot').className='dot on';
    document.getElementById('wifiText').textContent='WiFi';
  }).catch(()=>{
    document.getElementById('wifiDot').className='dot off';
    document.getElementById('wifiText').textContent='Offline';
  });
}
function updMotor(bId,vId,spd){
  const b=document.getElementById(bId);
  const pct=Math.abs(spd)/255*50;
  b.style.height=pct+'%';
  if(spd>=0){b.className='m-fill';b.style.bottom='50%';b.style.top='auto'}
  else{b.className='m-fill rev';b.style.top='50%';b.style.bottom='auto'}
  document.getElementById(vId).textContent=spd;
}

let tlData=[];
function drawTL(){
  const c=document.getElementById('tlCanvas'),ctx=c.getContext('2d');
  c.width=c.offsetWidth;c.height=c.offsetHeight;
  const w=c.width,h=c.height;ctx.clearRect(0,0,w,h);
  if(tlData.length<2)return;
  const bH=h/2;
  tlData.forEach((d,i)=>{if(!i)return;
    const x1=(i-1)/(tlData.length-1)*w,x2=i/(tlData.length-1)*w;
    ctx.fillStyle=d.e==='calm'?'rgba(76,175,80,0.12)':d.e==='aggressive'?'rgba(244,67,54,0.12)':'rgba(255,152,0,0.12)';
    ctx.fillRect(x1,0,x2-x1,bH)});
  ctx.beginPath();ctx.strokeStyle='#7eb8ff';ctx.lineWidth=2;
  tlData.forEach((d,i)=>{const x=i/(tlData.length-1)*w,y=h-(d.s/255*h*0.9)-5;
    if(!i)ctx.moveTo(x,y);else ctx.lineTo(x,y)});ctx.stroke();
  ctx.fillStyle='#556688';ctx.font='11px monospace';
  ctx.fillText('Speed',5,h-5);ctx.fillText('Emotion',5,14);
}
function pollHist(){fetch(URL+'/api/history').then(r=>r.json()).then(d=>{tlData=d;drawTL()}).catch(()=>{})}
function pollAlerts(){
  fetch(URL+'/api/alerts').then(r=>r.json()).then(alerts=>{
    const l=document.getElementById('alertList');l.innerHTML='';
    alerts.forEach(a=>{const d=document.createElement('div');
      d.className='alert-item'+(a.includes('WARNING')?' warn':'')+(a.includes('CRITICAL')||a.includes('EMERGENCY')?' danger':'');
      d.textContent=a;l.appendChild(d)});
  }).catch(()=>{});
}

// Keyboard controls
const ks={};
document.addEventListener('keydown',e=>{
  if(ks[e.key])return;ks[e.key]=true;updKeyJoy();
  if(e.key===' '){e.preventDefault();toggleEmg()}
});
document.addEventListener('keyup',e=>{delete ks[e.key];updKeyJoy()});
function updKeyJoy(){
  let kx=0,ky=0;
  if(ks['ArrowUp']||ks['w']||ks['W'])ky+=100;
  if(ks['ArrowDown']||ks['s']||ks['S'])ky-=100;
  if(ks['ArrowLeft']||ks['a']||ks['A'])kx-=100;
  if(ks['ArrowRight']||ks['d']||ks['D'])kx+=100;
  if(kx||ky||jX||jY){
    jX=kx;jY=ky;
    document.getElementById('jxV').textContent=jX;
    document.getElementById('jyV').textContent=jY;
    jK.style.left=(50+kx/2)+'%';jK.style.top=(50-ky/2)+'%';sendJoy()}
}

setInterval(poll,500);setInterval(pollHist,3000);setInterval(pollAlerts,2000);
poll();pollHist();pollAlerts();

// --- Camera & TTS Logic ---
function updateCam() {
  const ip = document.getElementById('camIp').value.trim();
  if(ip) {
    document.getElementById('camImg').src = 'http://' + ip + ':8080/video';
    localStorage.setItem('camIp', ip);
  }
}
const savedIp = localStorage.getItem('camIp');
if(savedIp) {
  document.getElementById('camIp').value = savedIp;
  updateCam();
}

let voiceEnabled = true;
let lastSpokenEmo = '';
function toggleVoice() {
  voiceEnabled = !voiceEnabled;
  document.getElementById('voiceBtn').innerHTML = voiceEnabled ? '&#128226; Voice ON' : '&#128263; Voice OFF';
}
function speak(text) {
  if(!voiceEnabled || !window.speechSynthesis) return;
  const ut = new SpeechSynthesisUtterance(text);
  speechSynthesis.cancel(); // Stop current speech
  speechSynthesis.speak(ut);
}

const phrases = {
  calm: 'You are calm.',
  fatigued: 'You are fatigued. Vehicle is slowing down.',
  aggressive: 'Aggressive driving detected. Vehicle stopped.',
  emergency: 'Emergency stop activated.'
};
</script>
</body>
</html>
)rawliteral";
  server.send(200, "text/html", html);
}
