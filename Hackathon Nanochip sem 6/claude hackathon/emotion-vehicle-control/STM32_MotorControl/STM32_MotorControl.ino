/*
 * STM32F103RB (Nucleo-64) - Motor Control via L298N
 * Arduino IDE with STM32duino core
 *
 * UART Wiring:
 *   STM32 PB10 (USART3 TX) --> ESP32 GPIO16 (RX2)
 *   STM32 PB11 (USART3 RX) <-- ESP32 GPIO17 (TX2)
 *   GND <--> GND
 *
 * L298N Wiring:
 *   IN1  <- PA8   | IN3  <- PB5
 *   IN2  <- PA4   | IN4  <- PB6
 *   ENA  <- PA7   | ENB  <- PB1
 *   OUT1 -> Front-Left  (+)  & Front-Right (+)
 *   OUT2 -> Front-Left  (-)  & Front-Right (-)
 *   OUT3 -> Rear-Left   (+)  & Rear-Right  (+)
 *   OUT4 -> Rear-Left   (-)  & Rear-Right  (-)
 *
 * IMPORTANT (Nucleo F103RB):
 *   "Serial"  = USART2 (PA2/PA3) routed to ST-Link USB (your serial monitor)
 *   "Serial3" = USART3 (PB10/PB11) used for ESP32 communication
 *
 *   Remove ENA & ENB jumper caps on L298N for PWM control!
 */

// ===== USART3 for ESP32 Communication =====
// On Nucleo F103RB: Serial = USART2 (ST-Link USB), we need USART3 (PB10/PB11)
HardwareSerial Serial3(PB11, PB10);  // RX=PB11, TX=PB10

// ===== Pin Definitions =====
// Front Motors (L298N Channel A: OUT1/OUT2)
#define FRONT_IN1   PA8    // Direction pin 1
#define FRONT_IN2   PA4    // Direction pin 2
#define FRONT_ENA   PA7    // PWM speed (TIM3_CH2)

// Rear Motors (L298N Channel B: OUT3/OUT4)
#define REAR_IN3    PB5    // Direction pin 1
#define REAR_IN4    PB6    // Direction pin 2
#define REAR_ENB    PB1    // PWM speed (TIM3_CH4)

// On-board LED for status
#define STATUS_LED  LED_BUILTIN  // PA5 on Nucleo F103RB

// ===== Motor State =====
int joyX = 0;
int joyY = 0;
int speedLimit = 255;
bool emergency = false;
unsigned long lastCommand = 0;
bool motorActive = false;

// ===== Safety Timeout =====
#define COMMAND_TIMEOUT 500  // Stop motors if no command for 500ms

// ===== Receive Buffer =====
char rxBuffer[64];
int rxIndex = 0;

void setup() {
  // USB Serial Monitor (through ST-Link, USART2, PA2/PA3)
  Serial.begin(115200);
  delay(1000);  // Wait for serial monitor to connect

  Serial.println(F("========================================"));
  Serial.println(F("  STM32F103RB - Motor Control System"));
  Serial.println(F("  USART3: PB10(TX) / PB11(RX)"));
  Serial.println(F("========================================"));

  // USART3 for ESP32 (PB10=TX, PB11=RX)
  Serial3.begin(115200);

  Serial.println(F("[UART] Serial3 (USART3) initialized at 115200"));
  Serial.println(F("[UART] PB10=TX -> ESP32 GPIO16"));
  Serial.println(F("[UART] PB11=RX <- ESP32 GPIO17"));

  // Motor pins
  pinMode(FRONT_IN1, OUTPUT);
  pinMode(FRONT_IN2, OUTPUT);
  pinMode(FRONT_ENA, OUTPUT);
  pinMode(REAR_IN3, OUTPUT);
  pinMode(REAR_IN4, OUTPUT);
  pinMode(REAR_ENB, OUTPUT);

  // Status LED
  pinMode(STATUS_LED, OUTPUT);

  // Motors OFF
  stopAllMotors();

  Serial.println(F("[MOTOR] All pins configured, motors stopped"));
  Serial.println(F("[READY] Waiting for ESP32 commands..."));
  Serial.println(F(""));

  // Blink LED 3 times to show ready
  for (int i = 0; i < 3; i++) {
    digitalWrite(STATUS_LED, HIGH);
    delay(150);
    digitalWrite(STATUS_LED, LOW);
    delay(150);
  }

  // Send ready message to ESP32
  Serial3.println("STM32_READY");
}

void loop() {
  // ===== Read UART byte-by-byte (more reliable than readStringUntil) =====
  while (Serial3.available()) {
    char c = Serial3.read();

    // Toggle LED on any received byte (visual confirmation)
    digitalWrite(STATUS_LED, !digitalRead(STATUS_LED));

    if (c == '\n' || c == '\r') {
      if (rxIndex > 0) {
        rxBuffer[rxIndex] = '\0';  // Null terminate
        String cmd = String(rxBuffer);
        cmd.trim();

        if (cmd.length() > 0) {
          Serial.print(F("[RX] Received: "));
          Serial.println(cmd);
          parseCommand(cmd);
        }
        rxIndex = 0;  // Reset buffer
      }
    } else {
      if (rxIndex < 63) {
        rxBuffer[rxIndex++] = c;
      } else {
        rxIndex = 0;  // Buffer overflow, reset
      }
    }
  }

  // ===== Safety Timeout =====
  if (motorActive && (millis() - lastCommand > COMMAND_TIMEOUT)) {
    Serial.println(F("[SAFETY] Command timeout - stopping motors"));
    stopAllMotors();
    motorActive = false;
    Serial3.println("ACK,timeout");
  }

  // ===== Apply Motor Control at 50Hz =====
  static unsigned long lastMotorUpdate = 0;
  if (millis() - lastMotorUpdate > 20) {
    if (motorActive && !emergency) {
      applyMotorControl();
    }
    lastMotorUpdate = millis();
  }

  // ===== Heartbeat: send status to ESP32 every 2 seconds =====
  static unsigned long lastHeartbeat = 0;
  if (millis() - lastHeartbeat > 2000) {
    Serial3.println("HB,ok");
    Serial.println(F("[HB] Heartbeat sent"));
    lastHeartbeat = millis();
  }

  // ===== Debug: Show status every 5 seconds =====
  static unsigned long lastStatus = 0;
  if (millis() - lastStatus > 5000) {
    Serial.print(F("[STATUS] Active="));
    Serial.print(motorActive);
    Serial.print(F(" Emergency="));
    Serial.print(emergency);
    Serial.print(F(" X="));
    Serial.print(joyX);
    Serial.print(F(" Y="));
    Serial.print(joyY);
    Serial.print(F(" Limit="));
    Serial.println(speedLimit);
    lastStatus = millis();
  }
}

// ===== Parse Command from ESP32 =====
// Format: CMD,joyX,joyY,speedLimit,emergency
// Example: CMD,50,-30,200,0
void parseCommand(String cmd) {
  if (!cmd.startsWith("CMD,")) {
    Serial.print(F("[PARSE] Unknown command: "));
    Serial.println(cmd);
    return;
  }

  // Parse CSV: CMD,joyX,joyY,speedLimit,emergency
  int values[4];
  int valueIndex = 0;
  int startPos = 4;  // After "CMD,"

  for (int i = 4; i <= (int)cmd.length() && valueIndex < 4; i++) {
    if (i == (int)cmd.length() || cmd.charAt(i) == ',') {
      values[valueIndex] = cmd.substring(startPos, i).toInt();
      valueIndex++;
      startPos = i + 1;
    }
  }

  if (valueIndex < 4) {
    Serial.println(F("[PARSE] Incomplete command, need 4 values"));
    return;
  }

  joyX = constrain(values[0], -100, 100);
  joyY = constrain(values[1], -100, 100);
  speedLimit = constrain(values[2], 0, 255);
  emergency = (values[3] == 1);

  lastCommand = millis();
  motorActive = true;

  // Handle emergency
  if (emergency) {
    stopAllMotors();
    Serial3.println("ACK,emergency");
    Serial.println(F("[EMERGENCY] Motors stopped!"));
    return;
  }

  // Send ACK back to ESP32
  Serial3.println("ACK,ok");

  // Debug (throttled)
  static unsigned long lastDebug = 0;
  if (millis() - lastDebug > 500) {
    Serial.print(F("[CMD] X="));
    Serial.print(joyX);
    Serial.print(F(" Y="));
    Serial.print(joyY);
    Serial.print(F(" Limit="));
    Serial.print(speedLimit);
    Serial.println();
    lastDebug = millis();
  }
}

// ===== Differential Steering Motor Control =====
// joyY = throttle (forward/backward)
// joyX = steering (mix into left/right differential)
void applyMotorControl() {
  int throttle = joyY;   // -100 to 100
  int steering = joyX;   // -100 to 100

  // Differential drive mixing
  int leftSpeed  = throttle + steering;   // Front motors
  int rightSpeed = throttle - steering;   // Rear motors

  leftSpeed  = constrain(leftSpeed, -100, 100);
  rightSpeed = constrain(rightSpeed, -100, 100);

  // Map to PWM with speed limit
  int leftPWM  = map(abs(leftSpeed), 0, 100, 0, speedLimit);
  int rightPWM = map(abs(rightSpeed), 0, 100, 0, speedLimit);

  // Dead zone to prevent motor whine
  if (leftPWM < 30)  leftPWM = 0;
  if (rightPWM < 30) rightPWM = 0;

  // === Front Motors (Channel A: OUT1/OUT2) ===
  if (leftSpeed > 0) {
    digitalWrite(FRONT_IN1, HIGH);
    digitalWrite(FRONT_IN2, LOW);
  } else if (leftSpeed < 0) {
    digitalWrite(FRONT_IN1, LOW);
    digitalWrite(FRONT_IN2, HIGH);
  } else {
    digitalWrite(FRONT_IN1, LOW);
    digitalWrite(FRONT_IN2, LOW);
  }
  analogWrite(FRONT_ENA, leftPWM);

  // === Rear Motors (Channel B: OUT3/OUT4) ===
  if (rightSpeed > 0) {
    digitalWrite(REAR_IN3, HIGH);
    digitalWrite(REAR_IN4, LOW);
  } else if (rightSpeed < 0) {
    digitalWrite(REAR_IN3, LOW);
    digitalWrite(REAR_IN4, HIGH);
  } else {
    digitalWrite(REAR_IN3, LOW);
    digitalWrite(REAR_IN4, LOW);
  }
  analogWrite(REAR_ENB, rightPWM);
}

// ===== Stop All Motors =====
void stopAllMotors() {
  digitalWrite(FRONT_IN1, LOW);
  digitalWrite(FRONT_IN2, LOW);
  analogWrite(FRONT_ENA, 0);

  digitalWrite(REAR_IN3, LOW);
  digitalWrite(REAR_IN4, LOW);
  analogWrite(REAR_ENB, 0);
}
