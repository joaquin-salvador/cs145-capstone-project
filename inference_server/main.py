# ===========================
# References
# ===========================
# ESP32-CAM & YOLO - Computer Vision
# https://www.youtube.com/watch?v=npJsmbFZiMg

# Threaded Frame Buffering OpenCV
# https://spin.atomicobject.com/frame-buffering-opencv/

# Non-blocking HTTP POST Request
# https://www.w3tutorials.net/blog/sending-a-non-blocking-http-post-request/

# ===========================
# Setup
# ===========================
from ultralytics import YOLO
import cv2
import requests
import threading
import queue
import time

ESP32_URL = "http://10.49.26.204:1145/update"
ESP32_CAM_URL = "http://10.49.26.31:81/stream"
ESP32_CAM_IP = "http://10.49.26.31"

# framesize: 4=VGA(640x480), 5=SVGA, 6=XGA(1024x768), 8=SXGA, 9=UXGA.
try:
    requests.get(f"{ESP32_CAM_IP}/control?var=framesize&val=6", timeout=2)
    print("Resolution set")
except Exception as e:
    print(f"Could not set resolution: {e}")

model = YOLO('yolo11n.pt')

# ===========================
# Helpers
# ===========================
# -- Bufferless CV Capture --
class FrameGrabber:
    def __init__(self, src):
        self.cap = cv2.VideoCapture(src)
        self.cap.set(cv2.CAP_PROP_BUFFERSIZE, 1) # Keep only 1 frame buffered
        self.ret, self.frame = self.cap.read()
        self.lock = threading.Lock()
        self.running = True
        self.thread = threading.Thread(target=self._reader, daemon=True)
        self.thread.start()

    # read frames as asap
    def _reader(self):
        while self.running:
            ret, frame = self.cap.read()
            if not ret:
                time.sleep(0.01)
                continue
            with self.lock:
                self.ret, self.frame = ret, frame

    def read(self):
        with self.lock:
            if self.frame is None:
                return False, None
            return self.ret, self.frame.copy()

    def release(self):
        self.running = False
        self.cap.release()
        self.thread.join(timeout=1)

# -- Non-blocking HTTP POST --
post_queue = queue.Queue(maxsize=2)

def poster():
    while True:
        payload = post_queue.get()
        if payload is None:
            break
        try:
            requests.post(ESP32_URL, data=payload, timeout=0.5)
        except Exception as e:
            print(f"POST failed: {e}")

threading.Thread(target=poster, daemon=True).start()

# ===========================
# Main loop
# ===========================
grabber = None
last_state = None  # (label, led)
                   # Only POST when this changes

poster_thread = threading.Thread(target=poster, daemon=True)
poster_thread.start()

try:
    grabber = FrameGrabber(ESP32_CAM_URL)
    cv2.namedWindow('frame', cv2.WINDOW_NORMAL)
    cv2.resizeWindow('frame', 1280, 720)

    while True:
        ret, frame = grabber.read()
        if not ret or frame is None:
            if cv2.waitKey(1) & 0xFF == ord('q'):
                break
            continue

        detections = model(frame, imgsz=416, verbose=False)

        label = "Nothing detected"
        led = "..."

        for box in detections[0].boxes:
            cls_id = int(box.cls[0])
            if cls_id == 39:  # bottle in COCO
                conf = float(box.conf[0])
                x1, y1, x2, y2 = box.xyxy[0]
                if conf > 0.3:
                    led = "DINGDINGDING"
                label = f"Bottle ({conf:.2f})"
                cv2.rectangle(frame, (int(x1), int(y1)), (int(x2), int(y2)), (0, 255, 0), 3)
                cv2.putText(frame, f'Bottle {conf:.2f}', (int(x1), int(y1) - 10),
                            cv2.FONT_HERSHEY_SIMPLEX, 0.9, (0, 255, 0), 2)

        state = (label, led)
        if state != last_state:
            try:
                post_queue.put_nowait({"label": label, "led": led})
            except queue.Full:
                pass
            last_state = state

        cv2.imshow('frame', frame)
        if cv2.waitKey(1) & 0xFF == ord('q'):
            break
finally:
    if grabber is not None:
        grabber.release()
    post_queue.put(None)
    poster_thread.join(timeout=1)
    cv2.destroyAllWindows()
