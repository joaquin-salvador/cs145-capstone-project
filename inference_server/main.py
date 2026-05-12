# ReSiklo: Inference Server
#
# Consumes feed produced by camera_server, runs YOLO
# continuously on each frame, and pushes bottle-presence
# signals to trashcan_module via HTTP /update endpoint.
#
# Recycling session is only started when user presses
# the RECYCLE button on the orchestrator. The trashcan_module
# stays in WAITING_FOR_BOTTLE until /update turns bottleVisible
# to 'true'.
#
# Signals:
#   label   = BOTTLE | NO_OBJECT
#   led     = BOTTLE_DETECTED | NO_BOTTLE
#
# References
#   - YOLOv8 for real-time waste classification (Rastari et al., 2024):
#     https://ieeexplore.ieee.org/document/10730703
#   - Bufferless OpenCV capture pattern (Keuning, 2024):
#     https://spin.atomicobject.com/frame-buffering-opencv/
#   - Non-blocking HTTP POST pattern:
#     https://www.w3tutorials.net/blog/sending-a-non-blocking-http-post-request/
#   - Ultralytics YOLO API:
#     https://docs.ultralytics.com/

from ultralytics import YOLO
import cv2
import requests
import threading
import queue
import time

# Network Configuration
ESP32_CAM_HOST   = "resiklo-cam.local"
ESP32_CAM_STREAM = f"http://{ESP32_CAM_HOST}:81/stream"
ESP32_CAM_CTRL   = f"http://{ESP32_CAM_HOST}/control"

TRASHCAN_HOST    = "http://resiklo-bin.local:1145"
TRASHCAN_UPDATE  = f"{TRASHCAN_HOST}/update"

# Detection Tuning
BOTTLE_CLASS_ID = 39 # 'bottle' in COCO
BOTTLE_DETECTION_THRESHOLD = 0.30

model = YOLO('yolo11n.pt') # Pretrained Ultralytics YOLOv11

# Bufferless CV Capture
# This was a neat solution by Keuning (2024)!
# The "view" of the feed, seen from the stream, lagged
# far behind real-time.
#
# This fix decouples the reading of frames from the stream
# from the actual logical feed via the usage of threads!
#
# _reader() is in charge of getting frames from the stream
# read() is in charge of the control logic
# ---------------------------
class FrameGrabber:
    def __init__(self, src):
        self.src = src
        self.cap = cv2.VideoCapture(src)
        self.cap.set(cv2.CAP_PROP_BUFFERSIZE, 1)
        self.ret, self.frame = self.cap.read()
        self.lock = threading.Lock()
        self.running = True
        self.thread = threading.Thread(target=self._reader, daemon=True)
        self.thread.start()

    def _reader(self):
        fail_count = 0
        while self.running:
            ret, frame = self.cap.read()
            if not ret:
                fail_count += 1
                if fail_count > 30:
                    print("[STREAM] Connection lost, reconnecting...")
                    self.cap.release()
                    time.sleep(1)
                    self.cap = cv2.VideoCapture(self.src)
                    self.cap.set(cv2.CAP_PROP_BUFFERSIZE, 1)
                    fail_count = 0
                time.sleep(0.01)
                continue
            fail_count = 0
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


# Non-blocking HTTP POST
post_queue = queue.Queue(maxsize=8)

def poster():
    while True:
        payload = post_queue.get()
        if payload is None:
            break
        try:
            response = requests.post(payload["url"], data=payload["data"], timeout=2.0)
            if response.status_code < 400:
                print(f"[POST OK] {payload['url'].split('/')[-1]}")
            else:
                print(f"[POST ERROR] Status {response.status_code}")
        except requests.exceptions.Timeout:
            print(f"[POST TIMEOUT] {payload['url'].split('/')[-1]} - ESP32 busy")
        except requests.exceptions.ConnectionError as e:
            print(f"[POST CONNECTION ERROR] {str(e)}")
        except Exception as e:
            print(f"[POST ERROR] {str(e)}")

# Main loop
grabber = None
last_state = None # (label, led) -- only POST /update when it changes

poster_thread = threading.Thread(target=poster, daemon=True)
poster_thread.start()

try:
    grabber = FrameGrabber(ESP32_CAM_STREAM)
    cv2.namedWindow("frame", cv2.WINDOW_NORMAL)
    cv2.resizeWindow("frame", 1280, 720)

    while True:
        ret, frame = grabber.read()
        if not ret or frame is None:
            if cv2.waitKey(1) & 0xFF == ord("q"):
                break
            continue

        detections = model(frame, imgsz=416, verbose=False)

        label = "NO_OBJECT"
        led   = "NO_BOTTLE"

        for box in detections[0].boxes:
            cls_id = int(box.cls[0])
            if cls_id == BOTTLE_CLASS_ID:
                conf = float(box.conf[0])
                x1, y1, x2, y2 = box.xyxy[0]
                if conf >= BOTTLE_DETECTION_THRESHOLD:
                    led   = "BOTTLE_DETECTED"
                    label = "BOTTLE"

                cv2.rectangle(frame, (int(x1), int(y1)), (int(x2), int(y2)), (0, 255, 0), 3)
                cv2.putText(frame, f"Bottle {conf:.2f}", (int(x1), int(y1) - 10),
                            cv2.FONT_HERSHEY_SIMPLEX, 0.9, (0, 255, 0), 2)

        # Post status update only on a change in (label, led).
        state = (label, led)
        if state != last_state:
            try:
                post_queue.put_nowait({
                    "url": TRASHCAN_UPDATE,
                    "data": {"label": label, "led": led},
                })
                print(f"[UPDATE] {label}")
            except queue.Full:
                pass
            except Exception as e:
                print(f"[QUEUE ERROR] {str(e)}")
            last_state = state

        cv2.imshow("frame", frame)
        if cv2.waitKey(1) & 0xFF == ord("q"):
            break
finally:
    if grabber is not None:
        grabber.release()
    post_queue.put(None)
    poster_thread.join(timeout=1)
    cv2.destroyAllWindows()

