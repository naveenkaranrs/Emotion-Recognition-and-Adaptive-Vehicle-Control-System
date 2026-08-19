# Wiring Guide — Emotion-Aware Vehicle Control System

## System Overview (ESP32 Standalone — No STM32)

```
Phone (IP Webcam) ──WiFi──► Laptop (Python + DeepFace)
                                    │ HTTP POST /api/emotion
                                    ▼
┌───────────────┐    PWM/GPIO    ┌─────────┐
│     ESP32     │──────────────►│  L298N  │──► 4 Motors
│  (Dashboard   │  GPIO 25,26,  │ Driver  │
│  + Motor Ctrl)│  27,32,33,14  └─────────┘
└───────────────┘                    │
     WiFi AP                    12V Battery
  Dashboard at
  ESP32's IP
```

---

## 1. ESP32 → L298N Motor Driver

### Channel A — Left Motors (front-left + rear-left wired in parallel)

| ESP32 Pin | L298N Pin | Function |
|-----------|-----------|----------|
| **GPIO 25** | **IN1** | Left motor direction 1 |
| **GPIO 26** | **IN2** | Left motor direction 2 |
| **GPIO 27** | **ENA** | Left motor speed (PWM) |

### Channel B — Right Motors (front-right + rear-right wired in parallel)

| ESP32 Pin | L298N Pin | Function |
|-----------|-----------|----------|
| **GPIO 32** | **IN3** | Right motor direction 1 |
| **GPIO 33** | **IN4** | Right motor direction 2 |
| **GPIO 14** | **ENB** | Right motor speed (PWM) |

> ⚠️ **Remove ENA and ENB jumpers** on the L298N board to enable PWM speed control!

---

## 2. L298N Power & Motor Connections

| L298N Pin | Connection |
|-----------|------------|
| **+12V** | Battery positive (7–12V) |
| **GND** | Battery negative + ESP32 GND |
| **5V** | Output (leave as-is if 12V jumper is on) |

### Motor Wiring (parallel per side):
```
L298N OUT1/OUT2 ──┬── Front-Left Motor
                  └── Rear-Left Motor      (both in parallel)

L298N OUT3/OUT4 ──┬── Front-Right Motor
                  └── Rear-Right Motor     (both in parallel)
```

> 💡 If a motor spins backward, swap its two wires at the L298N screw terminal.

---

## 3. Common Ground (CRITICAL!)

**ESP32 and L298N must share the same GND:**
```
ESP32 GND ─────┐
L298N GND ─────┤── All connected together
Battery GND ───┘
```

---

## 4. Power Supply

| Component | Power Source | Voltage |
|-----------|-------------|---------|
| ESP32 | USB from laptop | 5V |
| L298N + Motors | External battery | 7–12V |

**Recommended battery:** 2× 18650 Li-ion in series (7.4V)

> ⚠️ NEVER power motors from USB. Use external battery only.

---

## 5. ESP32 Pin Summary

| GPIO | Function | L298N Pin |
|------|----------|-----------|
| 25 | Left motor dir 1 | IN1 |
| 26 | Left motor dir 2 | IN2 |
| 27 | Left motor PWM | ENA |
| 32 | Right motor dir 1 | IN3 |
| 33 | Right motor dir 2 | IN4 |
| 14 | Right motor PWM | ENB |

> ✅ All pins are safe ESP32 output pins that support LEDC PWM. No conflicts with WiFi, boot strapping, or UART0 (debug).

---

## 6. Troubleshooting

| Problem | Fix |
|---------|-----|
| Motors don't spin | Check L298N power. Check ENA/ENB jumpers removed. Check motor wire connections |
| Motors spin wrong direction | Swap the two wires for that motor at L298N terminal |
| Dashboard not loading | Check WiFi connection. Try the IP from ESP32 Serial Monitor |
| No emotion detection | Install: `pip install deepface tf-keras` |
| IP Webcam not connecting | Phone and laptop must be on same WiFi network |
| Motors spin when they shouldn't | Check GND connection between ESP32 and L298N |
| ESP32 won't boot | GPIO 14 can sometimes cause issues on some boards — try GPIO 12 instead for ENB |
