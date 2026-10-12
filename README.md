# 🚗 Emotion-Aware Adaptive Vehicle Control System

> **Driver Emotion-Aware Adaptive Vehicle Control using ESP32 + Edge AI**

A real-time embedded system that detects driver emotional state and automatically adjusts vehicle behavior for enhanced safety. The ESP32 handles everything — web dashboard, emotion API, and direct motor control via L298N. Built for hackathon demonstration.

---

## 🧠 Core Concept

| Emotion Detected | Vehicle Action | Safety Response |
|-----------------|---------------|-----------------|
| 😊 **Calm** | Normal operation | Full speed available |
| 😡 **Aggressive** | Speed limited to 60% | Dashboard warning |
| 😴 **Fatigued** | Speed limited to 40% | Alert + gradual slowdown |
| 🔴 **Emergency** | Full stop | All motors disabled |

---

## 🏗️ System Architecture

```
📱 Phone (IP Webcam) ──WiFi──► 💻 Laptop (Python + DeepFace)
                                        │
                                   HTTP POST /api/emotion
                                        │
                                        ▼
                              ┌──────────────────┐
                              │      ESP32       │
                              │  Web Dashboard   │◄───── 🌐 Browser (Joystick)
                              │  Motor Control   │       HTTP API
                              │  WiFi AP Mode    │
                              └────────┬─────────┘
                              GPIO PWM │ (25,26,27,32,33,14)
                              ┌────────┴─────────┐
                              │   L298N Driver   │
                              ├──────┬──────┤
                              │ Left │Right │
                              │Motors│Motors│
                              └──────┴──────┘
```

---

## 📁 Project Structure

```
hackathon/
├── esp32_dashboard/
│   └── esp32_dashboard.ino           # ESP32 code (dashboard + motor control)
├── python_emotion/
│   ├── emotion_detector.py           # Emotion detection script
│   └── requirements.txt              # Python dependencies
├── WIRING_GUIDE.md                   # Complete wiring reference
└── README.md                         # This file
```

---

## 🔧 Hardware Required

| Component | Quantity | Purpose |
|-----------|----------|---------|
| ESP32 Dev Module | 1 | WiFi dashboard + motor control |
| L298N Motor Driver | 1 | H-bridge motor driver |
| DC Motors | 4 | Vehicle wheels (2 left, 2 right) |
| Car chassis | 1 | Vehicle body |
| Battery (7–12V) | 1 | Motor power supply |
| USB cable | 1 | Programming ESP32 |
| Jumper wires | ~10 | Connections |
| Phone with IP Webcam | 1 | Camera source |

---

## 🚀 Setup Instructions

### Step 1: Arduino IDE Setup

1. **Install Arduino IDE** (v2.x recommended)

2. **Add ESP32 board support:**
   - File → Preferences → Additional Board Manager URLs:
   ```
   https://dl.espressif.com/dl/package_esp32_index.json
   ```
   - Tools → Board → Boards Manager → Search "ESP32" → Install

3. **Install Libraries** (Sketch → Include Library → Manage Libraries):
   - `ArduinoJson` by Benoit Blanchon (v7.x)

### Step 2: Wire Everything

See **[WIRING_GUIDE.md](WIRING_GUIDE.md)** for complete pin connections.

**Quick reference — ESP32 → L298N:**
| ESP32 GPIO | L298N | Function |
|------------|-------|----------|
| 25 | IN1 | Left motor dir 1 |
| 26 | IN2 | Left motor dir 2 |
| 27 | ENA | Left motor PWM |
| 32 | IN3 | Right motor dir 1 |
| 33 | IN4 | Right motor dir 2 |
| 14 | ENB | Right motor PWM |
| GND | GND | Common ground |

### Step 3: Upload ESP32 Code

1. Connect ESP32 via USB
2. Select: Tools → Board → ESP32 Dev Module
3. Open `esp32_dashboard/esp32_dashboard.ino`
4. Click Upload
5. Open Serial Monitor (115200 baud) — note the IP address

### Step 4: Setup Python Emotion Detection

```bash
cd python_emotion
pip install -r requirements.txt
```

### Step 5: Run the System

1. **Power on** the car (battery for motors, USB for ESP32)
2. **Connect phone** to "EmotionVehicle" WiFi (password: 12345678) — or your home WiFi
3. **Start IP Webcam** app on phone
4. **Open dashboard** in browser using the IP from Serial Monitor
5. **Run emotion detector**:
   ```bash
   # With IP Webcam
   python emotion_detector.py --camera-url http://192.168.1.5:8080/video

   # With laptop webcam
   python emotion_detector.py --local-cam

   # Simulation mode (no camera needed)
   python emotion_detector.py --simulate
   ```

---

## 🎮 Dashboard Features

| Feature | Description |
|---------|-------------|
| 🕹️ Virtual Joystick | Touch/mouse/keyboard control for car direction |
| 🔄 Speed Slider | Adjustable max speed (0–255) |
| 😊 Emotion Display | Real-time driver emotion state with confidence |
| 🛡️ Safety Score | 0–100 score based on driving behavior |
| ⚠️ Alert Panel | Warnings when emotion triggers intervention |
| 📈 Emotion Timeline | Visual history of detected emotions & speed |
| 🔴 Emergency Stop | Instant motor shutdown button (also SPACE key) |
| ⚙️ Motor Output | Real-time left/right motor PWM visualization |

---

## 📡 Communication Protocol

### Python → ESP32 (HTTP POST)
```json
POST http://<ESP32_IP>/api/emotion
Body: {"emotion": "calm", "confidence": 0.92}
```
Emotions: `calm`, `aggressive`, `fatigued`

### Dashboard → ESP32 (HTTP POST)
```json
POST /api/joystick   Body: {"x": 50, "y": -30}     // -100 to 100
POST /api/speed      Body: {"limit": 200}            // 0 to 255
POST /api/emergency                                   // Toggle emergency stop
```

### ESP32 → Dashboard (HTTP GET)
```json
GET /api/status      // Returns current state, motor PWM, emotion, etc.
GET /api/history     // Emotion + speed timeline data
GET /api/alerts      // Alert log
```

---

## 🛡️ Safety Features

1. **Emotion-based speed limiting** — Automatic throttle reduction for aggressive/fatigued states
2. **Smooth PWM ramping** — Gradual acceleration/deceleration (step=12, 20ms interval)
3. **Watchdog timer** — Motors stop if no joystick input for 3 seconds
4. **Emergency stop** — Dashboard button or SPACE key for instant shutdown
5. **Dead zone filtering** — Prevents motor whine at very low PWM (<25)
6. **Differential steering** — Proper tank-style turning from a single joystick
7. **Safety score tracking** — Cumulative score that degrades with aggressive/fatigued driving

---

## 🏆 Hackathon Recognition

Developed and demonstrated during the **KARE Student Innovation Hackathon 2026** (Kalasalingam Academy of Research and Education).

---

## ⚠️ System Limitations & Engineering Constraints

For technical accuracy and interview review, please note the following design characteristics of this prototype:

1. **Inter-MCU Communication Bus:**
   The production prototype utilizes a dedicated **Hardware UART (USART3 at 115200 baud)** serial link between the ESP32 wireless hub and the STM32F103RB motor controller with CSV-delimited packets (`CMD,joyX,joyY,speedLimit,emergency`). While CAN bus transceivers (MCP2515) were evaluated in initial architecture planning, physical bus integration was constrained by hackathon prototyping time.
2. **AI Processing Location:**
   Driver facial emotion classification is executed on a host workstation utilizing OpenCV and DeepFace (`VGG-Face` backend) processing frames from an IP camera stream, with parsed emotional states (`calm`, `aggressive`, `fatigued`) dispatched to the ESP32 gateway over HTTP POST.
3. **Safety Watchdog:**
   Motor actuation contains an automatic 500 ms command timeout watchdog on the STM32 to ensure failsafe motor shutdown if serial communication is interrupted.

---

## 📄 License

This project is for educational/hackathon purposes.
