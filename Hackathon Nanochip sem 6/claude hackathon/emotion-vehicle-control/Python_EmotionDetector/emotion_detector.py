"""
==========================================================
 Driver Emotion Detection — Multi-Model High Accuracy
==========================================================

Uses multiple AI models for robust emotion classification:
  1. FER (Facial Expression Recognition CNN) — primary emotion classifier
  2. MediaPipe Face Mesh — Eye Aspect Ratio for drowsiness/fatigue
  3. DeepFace — secondary emotion classifier (ensemble voting)
  4. Haar Cascade + Heuristics — final fallback

Emotion mapping:
  happy, neutral, surprise → CALM
  angry, disgust           → AGGRESSIVE
  sad, fear + drowsy eyes  → FATIGUED

Requirements:
    pip install fer mediapipe deepface opencv-python requests numpy tf-keras

Usage:
    python emotion_detector.py                            # IP Webcam
    python emotion_detector.py --local-cam                # Laptop webcam
    python emotion_detector.py --simulate                 # Keyboard mode
    python emotion_detector.py --esp32-ip 192.168.1.100   # Custom ESP32 IP

==========================================================
"""

import cv2
import numpy as np
import requests
import time
import argparse
import sys
from collections import deque, Counter

# ===== Configuration =====
DEFAULT_PHONE_IP = "192.168.137.224"
DEFAULT_ESP32_IP = "192.168.137.248"

ANALYSIS_INTERVAL = 0.3       # Seconds between full analysis
POST_INTERVAL = 0.5       # Seconds between ESP32 POST
SHOW_GUI = True

# ===== Model Loading =====
# We try to load multiple models; each one that loads adds accuracy

HAS_FER = False
HAS_MEDIAPIPE = False
HAS_DEEPFACE = False

# --- FER (primary emotion model) ---
try:
    from fer import FER
    HAS_FER = True
    print("[OK] FER (Facial Expression Recognition) loaded")
except ImportError:
    print("[--] FER not installed. Run: pip install fer")

# --- MediaPipe (face mesh for eye aspect ratio) ---
try:
    import mediapipe as mp
    import mediapipe.python.solutions  # Force load solutions submodule
    HAS_MEDIAPIPE = True
    print("[OK] MediaPipe Face Mesh loaded")
except ImportError:
    print("[--] MediaPipe not installed. Run: pip install mediapipe")

# --- DeepFace (secondary emotion model) ---
try:
    from deepface import DeepFace
    HAS_DEEPFACE = True
    print("[OK] DeepFace loaded")
except ImportError:
    print("[--] DeepFace not installed. Run: pip install deepface tf-keras")


# ===== Emotion Mapping =====
# Map raw emotions from models → driver states
EMOTION_TO_STATE = {
    # FER / DeepFace emotions
    "happy":    "calm",
    "neutral":  "calm",
    "surprise": "calm",
    "angry":    "aggressive",
    "disgust":  "aggressive",
    "sad":      "fatigued",
    "fear":     "fatigued",
}

STATE_COLORS = {
    "calm":       (0, 220, 0),      # Green
    "aggressive": (0, 0, 255),      # Red
    "fatigued":   (0, 180, 255),    # Orange
    "emergency":  (255, 0, 255),    # Purple (Stop)
}


class MultiModelEmotionDetector:
    """High-accuracy emotion detector using multiple AI models."""

    def __init__(self):
        # ----- FER Model -----
        self.fer_detector = None
        if HAS_FER:
            try:
                self.fer_detector = FER(mtcnn=True)
                print("[FER] MTCNN + CNN emotion model ready")
            except Exception as e:
                print(f"[FER] MTCNN failed, trying OpenCV backend: {e}")
                try:
                    self.fer_detector = FER(mtcnn=False)
                    print("[FER] OpenCV backend emotion model ready")
                except Exception as e2:
                    print(f"[FER] Failed completely: {e2}")
                    self.fer_detector = None

        # ----- MediaPipe Face Mesh -----
        self.face_mesh = None
        self.mp_face_mesh = None
        if HAS_MEDIAPIPE:
            try:
                self.mp_face_mesh = mp.solutions.face_mesh
                self.face_mesh = self.mp_face_mesh.FaceMesh(
                    static_image_mode=False,
                    max_num_faces=1,
                    refine_landmarks=True,
                    min_detection_confidence=0.5,
                    min_tracking_confidence=0.5
                )
                print("[MediaPipe] Face Mesh ready (468 landmarks)")
            except Exception as e:
                print(f"[MediaPipe] Failed: {e}")
                self.face_mesh = None

        # ----- Haar Cascade (fallback) -----
        self.face_cascade = cv2.CascadeClassifier(
            cv2.data.haarcascades + 'haarcascade_frontalface_default.xml'
        )
        self.eye_cascade = cv2.CascadeClassifier(
            cv2.data.haarcascades + 'haarcascade_eye.xml'
        )

        # ----- State tracking -----
        self.emotion_history = deque(maxlen=20)
        self.ear_history = deque(maxlen=30)         # Eye Aspect Ratio
        self.mar_history = deque(maxlen=15)          # Mouth Aspect Ratio
        self.movement_history = deque(maxlen=15)
        self.prev_face_center = None

        # Current state
        self.current_emotion = "calm"
        self.current_confidence = 0.5
        self.current_raw_emotion = "neutral"
        self.blink_counter = 0
        self.total_blinks = 0
        self.eyes_closed_frames = 0
        self.no_face_start_time = 0

        # Thresholds
        self.EAR_THRESHOLD = 0.28         # Below this = eyes closed (tuned up for more sensitivity)
        self.EAR_CONSEC_FRAMES = 2        # Frames eyes must be closed for blink
        self.FATIGUE_EAR_RATIO = 0.15     # Ratio of closed-eye frames → fatigue
        self.YAWN_MAR_THRESHOLD = 0.45    # Above this = yawning
        self.MOVEMENT_THRESHOLD = 25      # Face movement → aggression

        # MediaPipe landmark indices for eyes
        # Left eye: [362, 385, 387, 263, 373, 380]
        # Right eye: [33, 160, 158, 133, 153, 144]
        self.LEFT_EYE = [362, 385, 387, 263, 373, 380]
        self.RIGHT_EYE = [33, 160, 158, 133, 153, 144]
        # Mouth landmarks
        self.UPPER_LIP = [13]
        self.LOWER_LIP = [14]
        self.LEFT_MOUTH = [78]
        self.RIGHT_MOUTH = [308]

        self.analysis_count = 0
        self.last_analysis_time = 0

        model_count = sum([
            self.fer_detector is not None,
            self.face_mesh is not None,
            HAS_DEEPFACE
        ])
        print(f"\n[Detector] Ready with {model_count} AI model(s) + Haar cascade fallback")

    def _eye_aspect_ratio(self, landmarks, eye_indices, img_w, img_h):
        """Calculate Eye Aspect Ratio (EAR) from MediaPipe landmarks.
        EAR = (|p2-p6| + |p3-p5|) / (2 * |p1-p4|)
        Low EAR = closed eyes = fatigue indicator
        """
        pts = []
        for idx in eye_indices:
            lm = landmarks[idx]
            pts.append((lm.x * img_w, lm.y * img_h))

        # Vertical distances
        v1 = np.linalg.norm(np.array(pts[1]) - np.array(pts[5]))
        v2 = np.linalg.norm(np.array(pts[2]) - np.array(pts[4]))
        # Horizontal distance
        h = np.linalg.norm(np.array(pts[0]) - np.array(pts[3]))

        if h == 0:
            return 0.3
        ear = (v1 + v2) / (2.0 * h)
        return ear

    def _mouth_aspect_ratio(self, landmarks, img_w, img_h):
        """Calculate Mouth Aspect Ratio (MAR) for yawn detection."""
        upper = landmarks[self.UPPER_LIP[0]]
        lower = landmarks[self.LOWER_LIP[0]]
        left = landmarks[self.LEFT_MOUTH[0]]
        right = landmarks[self.RIGHT_MOUTH[0]]

        vertical = abs(upper.y - lower.y) * img_h
        horizontal = abs(left.x - right.x) * img_w

        if horizontal == 0:
            return 0
        return vertical / horizontal

    def analyze_frame(self, frame):
        """Run all available models on the frame and fuse results."""
        self.analysis_count += 1
        h, w = frame.shape[:2]

        # Scores: each model contributes weighted votes
        scores = {"calm": 0.0, "aggressive": 0.0, "fatigued": 0.0}
        total_weight = 0.0
        face_rect = None
        raw_emotion = "neutral"
        force_fatigue = False

        # ===== 1. FER Model (weight: 0.45) =====
        if self.fer_detector is not None:
            try:
                fer_results = self.fer_detector.detect_emotions(frame)
                if fer_results and len(fer_results) > 0:
                    # Use the result with highest total score (largest/most confident face)
                    best = max(fer_results, key=lambda r: max(r['emotions'].values()))
                    emotions = best['emotions']
                    box = best['box']
                    face_rect = (box[0], box[1], box[2], box[3])

                    # Map FER emotions to driver states
                    fer_raw = max(emotions, key=emotions.get)
                    raw_emotion = fer_raw
                    fer_conf = emotions[fer_raw]

                    # Weighted contribution per emotion
                    calm_score = emotions.get('happy', 0) + emotions.get('neutral', 0) + emotions.get('surprise', 0)
                    aggr_score = emotions.get('angry', 0) + emotions.get('disgust', 0)
                    fatg_score = emotions.get('sad', 0) + emotions.get('fear', 0)

                    weight = 0.45
                    scores["calm"] += calm_score * weight
                    scores["aggressive"] += aggr_score * weight
                    scores["fatigued"] += fatg_score * weight
                    total_weight += weight

            except Exception as e:
                pass  # FER failed silently

        # ===== 2. MediaPipe Face Mesh — EAR + MAR (weight: 0.35) =====
        if self.face_mesh is not None:
            try:
                rgb = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
                mp_results = self.face_mesh.process(rgb)

                if mp_results.face_landmarks:
                    landmarks = mp_results.face_landmarks.landmark

                    # Eye Aspect Ratio
                    left_ear = self._eye_aspect_ratio(landmarks, self.LEFT_EYE, w, h)
                    right_ear = self._eye_aspect_ratio(landmarks, self.RIGHT_EYE, w, h)
                    avg_ear = (left_ear + right_ear) / 2.0
                    self.ear_history.append(avg_ear)

                    # Mouth Aspect Ratio (yawn detection)
                    mar = self._mouth_aspect_ratio(landmarks, w, h)
                    self.mar_history.append(mar)

                    # Blink / eye closure detection
                    if avg_ear < self.EAR_THRESHOLD:
                        self.eyes_closed_frames += 1
                        force_fatigue = True  # Instant fatigue override if eyes currently closed
                    else:
                        if self.eyes_closed_frames >= self.EAR_CONSEC_FRAMES:
                            self.total_blinks += 1
                        self.eyes_closed_frames = 0

                    weight = 0.35

                    # Fatigue scoring from EAR
                    if len(self.ear_history) >= 10:
                        closed_ratio = sum(1 for e in self.ear_history if e < self.EAR_THRESHOLD) / len(self.ear_history)
                        if closed_ratio > self.FATIGUE_EAR_RATIO:
                            scores["fatigued"] += 0.9 * weight
                        elif closed_ratio > 0.2:
                            scores["fatigued"] += 0.5 * weight
                        else:
                            scores["calm"] += 0.3 * weight

                    # Prolonged eye closure (strongest fatigue signal)
                    if self.eyes_closed_frames > 5:
                        scores["fatigued"] += 3.0 * weight  # Dominates the score

                    # Yawn detection from MAR
                    if len(self.mar_history) >= 5:
                        avg_mar = sum(self.mar_history) / len(self.mar_history)
                        if avg_mar > self.YAWN_MAR_THRESHOLD:
                            scores["fatigued"] += 0.6 * weight

                    # Face bounding box from landmarks (if FER missed it)
                    if face_rect is None:
                        xs = [lm.x * w for lm in landmarks]
                        ys = [lm.y * h for lm in landmarks]
                        x1, y1, x2, y2 = int(min(xs)), int(min(ys)), int(max(xs)), int(max(ys))
                        margin = 20
                        face_rect = (max(0, x1 - margin), max(0, y1 - margin),
                                     x2 - x1 + 2*margin, y2 - y1 + 2*margin)

                    total_weight += weight

            except Exception as e:
                pass  # MediaPipe failed silently

        # ===== 3. DeepFace (weight: 0.30, run less frequently for speed) =====
        if HAS_DEEPFACE and (self.analysis_count % 3 == 0):  # Every 3rd frame
            try:
                df_results = DeepFace.analyze(
                    frame, actions=['emotion'],
                    enforce_detection=False,
                    detector_backend='opencv',
                    silent=True
                )
                if df_results and len(df_results) > 0:
                    result = df_results[0]
                    df_emotions = result.get('emotion', {})
                    df_raw = result.get('dominant_emotion', 'neutral')

                    calm_score = (df_emotions.get('happy', 0) + df_emotions.get('neutral', 0) +
                                  df_emotions.get('surprise', 0)) / 100.0
                    aggr_score = (df_emotions.get('angry', 0) + df_emotions.get('disgust', 0)) / 100.0
                    fatg_score = (df_emotions.get('sad', 0) + df_emotions.get('fear', 0)) / 100.0

                    weight = 0.30
                    scores["calm"] += calm_score * weight
                    scores["aggressive"] += aggr_score * weight
                    scores["fatigued"] += fatg_score * weight
                    total_weight += weight

                    # Use DeepFace face region if no face_rect yet
                    if face_rect is None:
                        region = result.get('region', {})
                        if region:
                            face_rect = (region.get('x', 0), region.get('y', 0),
                                         region.get('w', 0), region.get('h', 0))

            except Exception:
                pass

        # ===== 4. Movement Analysis (weight: 0.15) =====
        if face_rect is not None:
            cx, cy = face_rect[0] + face_rect[2]//2, face_rect[1] + face_rect[3]//2
            if self.prev_face_center is not None:
                dx = cx - self.prev_face_center[0]
                dy = cy - self.prev_face_center[1]
                movement = np.sqrt(dx*dx + dy*dy)
                self.movement_history.append(movement)

                if len(self.movement_history) >= 5:
                    avg_move = sum(self.movement_history) / len(self.movement_history)
                    weight = 0.15
                    if avg_move > self.MOVEMENT_THRESHOLD:
                        scores["aggressive"] += 0.7 * weight
                    elif avg_move > self.MOVEMENT_THRESHOLD * 0.5:
                        scores["aggressive"] += 0.3 * weight
                    else:
                        scores["calm"] += 0.2 * weight
                    total_weight += weight

            self.prev_face_center = (cx, cy)

        # ===== 5. Haar Cascade Fallback (if no other model detected a face) =====
        if total_weight == 0:
            gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
            faces = self.face_cascade.detectMultiScale(gray, 1.1, 5, minSize=(80, 80))
            if len(faces) > 0:
                face = max(faces, key=lambda f: f[2]*f[3])
                x, y, fw, fh = face
                face_rect = (x, y, fw, fh)
                face_roi = gray[y:y+fh, x:x+fw]

                # Eye detection
                eyes = self.eye_cascade.detectMultiScale(
                    face_roi[0:int(fh*0.6), :], 1.1, 3, minSize=(20, 20))
                eyes_open = len(eyes) >= 2

                scores["calm"] = 0.5 if eyes_open else 0.2
                scores["fatigued"] = 0.1 if eyes_open else 0.6
                scores["aggressive"] = 0.1
                total_weight = 0.5

        # ===== Fusion: determine final emotion =====
        if total_weight == 0:
            # No face detected at all
            if self.no_face_start_time == 0:
                self.no_face_start_time = time.time()
                
            elapsed = time.time() - self.no_face_start_time
            if elapsed >= 5.0:
                self.current_emotion = "emergency"
                self.current_confidence = 0.99
                self.current_raw_emotion = "camera_lost"
                return "emergency", 0.99, None
            elif elapsed >= 3.0:
                self.current_emotion = "fatigued"
                self.current_confidence = 0.99
                self.current_raw_emotion = "no_face_3s"
                return "fatigued", 0.99, None
            else:
                self.emotion_history.append("calm")
                self.current_emotion = "calm"
                self.current_confidence = 0.3
                self.current_raw_emotion = "searching"
                return "calm", 0.3, None
        else:
            self.no_face_start_time = 0

        # Normalize scores
        total = sum(scores.values())
        if total > 0:
            for k in scores:
                scores[k] /= total

        detected = max(scores, key=scores.get)
        confidence = scores[detected]

        # Instant safety override
        if force_fatigue:
            detected = "fatigued"
            confidence = 0.99
            raw_emotion = "eyes_closed"

        # Temporal smoothing (majority vote over recent frames)
        self.emotion_history.append(detected)
        
        # Bypass smoothing if fatigued for immediate safety response
        if detected != "fatigued" and len(self.emotion_history) >= 5:
            recent = list(self.emotion_history)[-12:]
            counts = Counter(recent)
            smoothed = counts.most_common(1)[0][0]
            # Only switch if the new emotion has enough votes
            if counts[smoothed] >= len(recent) * 0.4:
                detected = smoothed

        self.current_emotion = detected
        self.current_confidence = confidence
        self.current_raw_emotion = raw_emotion

        return detected, confidence, face_rect

    def get_debug_info(self):
        """Return debug info string for overlay."""
        info = []
        if self.fer_detector:
            info.append("FER:ON")
        if self.face_mesh:
            avg_ear = sum(self.ear_history) / len(self.ear_history) if self.ear_history else 0
            info.append(f"EAR:{avg_ear:.2f}")
            info.append(f"Blinks:{self.total_blinks}")
        if HAS_DEEPFACE:
            info.append("DF:ON")
        return " | ".join(info)


def send_to_esp32(esp32_url, emotion, confidence):
    """Send emotion to ESP32 via HTTP POST."""
    try:
        data = {"emotion": emotion, "confidence": round(float(confidence), 2)}
        response = requests.post(esp32_url, json=data, timeout=1)
        return response.status_code == 200
    except requests.exceptions.RequestException:
        return False


def draw_overlay(frame, emotion, confidence, face_rect, fps, debug_info, esp_ok):
    """Draw rich HUD overlay."""
    h, w = frame.shape[:2]
    color = STATE_COLORS.get(emotion, (200, 200, 200))

    # Top status bar
    overlay = frame.copy()
    cv2.rectangle(overlay, (0, 0), (w, 50), (20, 20, 20), -1)
    cv2.addWeighted(overlay, 0.7, frame, 0.3, 0, frame)

    cv2.putText(frame, "EMOTION-AWARE VEHICLE CONTROL",
                (10, 20), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (180, 180, 180), 1)
    cv2.putText(frame, f"State: {emotion.upper()} ({confidence:.0%})",
                (10, 42), cv2.FONT_HERSHEY_SIMPLEX, 0.65, color, 2)
    cv2.putText(frame, f"FPS: {fps:.0f}",
                (w - 100, 20), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (180, 180, 180), 1)

    # Status indicator circle
    cv2.circle(frame, (w - 30, 35), 15, color, -1)
    cv2.circle(frame, (w - 30, 35), 15, (255, 255, 255), 2)

    # Face rectangle
    if face_rect is not None:
        x, y, fw, fh = face_rect
        cv2.rectangle(frame, (x, y), (x + fw, y + fh), color, 2)
        # Emotion label above face
        label = f"{emotion.upper()} {confidence:.0%}"
        (tw, th), _ = cv2.getTextSize(label, cv2.FONT_HERSHEY_SIMPLEX, 0.6, 2)
        cv2.rectangle(frame, (x, y - th - 10), (x + tw + 8, y), color, -1)
        cv2.putText(frame, label, (x + 4, y - 6),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.6, (255, 255, 255), 2)

    # Bottom bar: debug info + ESP32 status
    overlay2 = frame.copy()
    cv2.rectangle(overlay2, (0, h - 45), (w, h), (20, 20, 20), -1)
    cv2.addWeighted(overlay2, 0.7, frame, 0.3, 0, frame)

    cv2.putText(frame, debug_info, (10, h - 28),
                cv2.FONT_HERSHEY_SIMPLEX, 0.4, (150, 150, 150), 1)

    esp_text = "ESP32: Connected" if esp_ok else "ESP32: Disconnected"
    esp_color = (0, 200, 0) if esp_ok else (0, 0, 255)
    cv2.putText(frame, esp_text, (10, h - 8),
                cv2.FONT_HERSHEY_SIMPLEX, 0.45, esp_color, 1)

    cv2.putText(frame, "Q=quit | 0=auto 1=calm 2=aggr 3=fatigue",
                (w - 350, h - 8), cv2.FONT_HERSHEY_SIMPLEX, 0.35, (120, 120, 120), 1)

    # Confidence bar
    bar_w = 180
    bar_x = w - bar_w - 10
    bar_y = h - 40
    cv2.rectangle(frame, (bar_x, bar_y), (bar_x + bar_w, bar_y + 12), (50, 50, 50), -1)
    cv2.rectangle(frame, (bar_x, bar_y), (bar_x + int(bar_w * confidence), bar_y + 12), color, -1)

    return frame


def run_simulation(esp32_url):
    """Keyboard-based simulation mode."""
    print("\n" + "=" * 50)
    print(" SIMULATION MODE (No Camera)")
    print("=" * 50)
    print(" 1 → CALM    2 → AGGRESSIVE    3 → FATIGUED    Q → Quit")
    print("=" * 50 + "\n")

    states = {"1": ("calm", 0.92), "2": ("aggressive", 0.85), "3": ("fatigued", 0.78)}

    try:
        while True:
            key = input("Emotion (1/2/3/Q): ").strip().upper()
            if key == 'Q':
                break
            if key in states:
                emotion, conf = states[key]
                ok = send_to_esp32(esp32_url, emotion, conf)
                status = "OK" if ok else "FAIL"
                print(f"  → Sent '{emotion}' (conf={conf}) → ESP32: {status}")
            else:
                print("  Invalid. Use 1, 2, 3, or Q.")
    except KeyboardInterrupt:
        pass

    send_to_esp32(esp32_url, "calm", 1.0)
    print("[Done] Exited simulation mode.")


def run_camera(camera_source, esp32_url):
    """Camera-based emotion detection with multi-model fusion."""
    detector = MultiModelEmotionDetector()

    print(f"\n[Camera] Connecting to {camera_source}...")
    cap = cv2.VideoCapture(camera_source)

    if not cap.isOpened():
        print(f"[Camera] Could not open: {camera_source}")
        if isinstance(camera_source, str):
            print("[Camera] Trying local webcam (index 0)...")
            cap = cv2.VideoCapture(0)
            if not cap.isOpened():
                print("[ERROR] No camera available!")
                print("  1. Check IP Webcam app is running")
                print("  2. Verify the IP address")
                print("  3. Phone and PC must be on same WiFi")
                return
        else:
            print("[ERROR] No camera available!")
            return

    print("[Camera] Connected successfully!\n")
    print("Controls:")
    print("  Q - Quit")
    print("  1 - Force CALM")
    print("  2 - Force AGGRESSIVE")
    print("  3 - Force FATIGUED")
    print("  0 - Auto detect mode\n")

    fps = 0
    frame_count = 0
    start_time = time.time()
    last_esp_send = 0
    last_analysis = 0
    esp_connected = False
    force_emotion = None

    # Cache last result for frames between analyses
    cached_emotion = "calm"
    cached_confidence = 0.5
    cached_face_rect = None

    try:
        while True:
            ret, frame = cap.read()
            if not ret:
                print("[Camera] Frame read failed, reconnecting...")
                time.sleep(1)
                cap = cv2.VideoCapture(camera_source)
                continue

            # Resize for performance
            frame = cv2.resize(frame, (640, 480))

            # Run analysis at set interval (not every frame — saves CPU)
            now = time.time()
            if force_emotion:
                cached_emotion = force_emotion
                cached_confidence = 0.95
                cached_face_rect = None
            elif now - last_analysis >= ANALYSIS_INTERVAL:
                cached_emotion, cached_confidence, cached_face_rect = detector.analyze_frame(frame)
                last_analysis = now

            # Send to ESP32
            if now - last_esp_send > POST_INTERVAL:
                esp_connected = send_to_esp32(esp32_url, cached_emotion, cached_confidence)
                last_esp_send = now
                status = "OK" if esp_connected else "FAIL"
                print(f"[{time.strftime('%H:%M:%S')}] {cached_emotion:12s} "
                      f"Conf:{cached_confidence:.2f}  Raw:{detector.current_raw_emotion:10s}  "
                      f"ESP32:{status}  {detector.get_debug_info()}")

            # FPS
            frame_count += 1
            elapsed = now - start_time
            if elapsed > 1:
                fps = frame_count / elapsed
                frame_count = 0
                start_time = now

            # Display
            if SHOW_GUI:
                debug = detector.get_debug_info()
                frame = draw_overlay(frame, cached_emotion, cached_confidence,
                                     cached_face_rect, fps, debug, esp_connected)
                cv2.imshow("Driver Emotion Detection (Multi-Model)", frame)

                key = cv2.waitKey(1) & 0xFF
                if key == ord('q'):
                    break
                elif key == ord('1'):
                    force_emotion = "calm"
                    print("[Manual] → CALM")
                elif key == ord('2'):
                    force_emotion = "aggressive"
                    print("[Manual] → AGGRESSIVE")
                elif key == ord('3'):
                    force_emotion = "fatigued"
                    print("[Manual] → FATIGUED")
                elif key == ord('0'):
                    force_emotion = None
                    print("[Manual] → Auto-detect")

    except KeyboardInterrupt:
        print("\n[System] Shutting down...")
    finally:
        cap.release()
        cv2.destroyAllWindows()
        send_to_esp32(esp32_url, "calm", 1.0)
        print("[System] Goodbye!")


def main():
    parser = argparse.ArgumentParser(description="Multi-Model Driver Emotion Detection")
    parser.add_argument('--simulate', action='store_true', help='Keyboard simulation mode')
    parser.add_argument('--local-cam', action='store_true', help='Use laptop webcam')
    parser.add_argument('--phone-ip', type=str, default=DEFAULT_PHONE_IP, help='Phone IP Webcam IP')
    parser.add_argument('--esp32-ip', type=str, default=DEFAULT_ESP32_IP, help='ESP32 IP address')
    parser.add_argument('--no-gui', action='store_true', help='Disable OpenCV window')
    args = parser.parse_args()

    global SHOW_GUI
    if args.no_gui:
        SHOW_GUI = False

    esp32_url = f"http://{args.esp32_ip}/api/emotion"

    print("\n" + "=" * 55)
    print("  EMOTION-AWARE VEHICLE CONTROL")
    print("  Multi-Model Emotion Detection v3.0")
    print("=" * 55)
    print(f"  FER:       {'✓ Loaded' if HAS_FER else '✗ Not installed'}")
    print(f"  MediaPipe: {'✓ Loaded' if HAS_MEDIAPIPE else '✗ Not installed'}")
    print(f"  DeepFace:  {'✓ Loaded' if HAS_DEEPFACE else '✗ Not installed'}")
    print(f"  ESP32:     {args.esp32_ip}")
    print(f"  Mode:      {'Simulation' if args.simulate else 'Local Cam' if args.local_cam else 'IP Webcam'}")
    print("=" * 55)

    models_loaded = sum([HAS_FER, HAS_MEDIAPIPE, HAS_DEEPFACE])
    if models_loaded == 0:
        print("\n[WARNING] No AI models installed! Using basic Haar cascade only.")
        print("  For best accuracy, install at least FER + MediaPipe:")
        print("    pip install fer mediapipe")
        print()

    if args.simulate:
        run_simulation(esp32_url)
    else:
        if args.local_cam:
            camera = 0
        else:
            camera = f"http://{args.phone_ip}:8080/video"
        run_camera(camera, esp32_url)


if __name__ == "__main__":
    main()
