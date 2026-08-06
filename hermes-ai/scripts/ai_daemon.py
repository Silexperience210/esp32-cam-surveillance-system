#!/usr/bin/env python3
"""Hermes AI Eye Daemon — multi-cameras, YOLO 15s, silencieux si RAS"""
import time, os, sys, json
from pathlib import Path
from datetime import datetime

HOME = Path.home()
BASE = HOME / ".hermes" / "camera"
FLAG = BASE / "ai_enabled"
SIGNAL = BASE / "last_alert.txt"
CONFIG_FILE = BASE / "cameras.json"
SYS_PATH = str(HOME / ".hermes" / "scripts")
if SYS_PATH not in sys.path:
    sys.path.insert(0, SYS_PATH)
exec(open(HOME / ".hermes" / "scripts" / "ai_eye.py").read().split("if __name__")[0])

# Charger config cameras
def load_cameras():
    if CONFIG_FILE.exists():
        with open(CONFIG_FILE) as f:
            cfg = json.load(f)
        return [c for c in cfg.get("cameras", []) if c.get("enabled", True)]
    return [{"name": "Camera", "url": CAMERA_URL, "interval": 15}]

cameras = load_cameras()
print(f"👁️ AI Eye Daemon — {len(cameras)} camera(s)")

last_desc = {}  # par caméra

while True:
    if not FLAG.exists():
        time.sleep(5)
        continue

    for cam in cameras:
        name = cam.get("name", "Cam")
        url = cam.get("url", CAMERA_URL)
        interval = cam.get("interval", 15)

        try:
            # Override CAMERA_URL pour cette caméra
            os.environ["CAMERA_URL"] = url

            r = analyze(require_motion=False, skip_ollama=True)
            desc = r.get('description_full', '')
            alert = r.get('alert', False)
            count = r.get('count', 0)

            has_person = 'personne' in desc.lower() or 'person' in str(r.get('detections','')).lower()
            key = f"{name}:{desc}"

            if (alert or has_person) and key != last_desc.get(name, ''):
                last_desc[name] = key
                emoji = "🚨" if alert else "👤"
                msg = f"{emoji} [{name}] {desc} ({count} objets)"
                with open(SIGNAL, "w") as f:
                    f.write(msg)
                print(msg[:120])

        except Exception as e:
            print(f"⚠️ [{name}] {e}")

        time.sleep(interval)

    time.sleep(1)
