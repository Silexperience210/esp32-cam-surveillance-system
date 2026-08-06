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

# ─── Telegram direct ────────────────────────────────────
import urllib.request

def telegram_send(text: str):
    """Envoie un message direct Telegram."""
    try:
        env = Path.home() / ".hermes" / ".env"
        token = chat_id = ""
        with open(env) as f:
            for line in f:
                if line.startswith("TELEGRAM_BOT_TOKEN="):
                    token = line.split("=",1)[1].strip().strip('"').strip("'")
                if line.startswith("TELEGRAM_CHAT_ID="):
                    chat_id = line.split("=",1)[1].strip().strip('"').strip("'")
        if not chat_id:
            chat_id = "540124594"  # fallback
        if token and chat_id:
            data = json.dumps({"chat_id": chat_id, "text": text}).encode()
            req = urllib.request.Request(
                f"https://api.telegram.org/bot{token}/sendMessage",
                data=data, headers={"Content-Type": "application/json"}
            )
            urllib.request.urlopen(req, timeout=5)
    except Exception as e:
        print(f"⚠️ Telegram send error: {e}")

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
            os.environ["CAMERA_URL"] = url

            # YOLO rapide d'abord (~3s, sans sauvegarde)
            r = analyze(require_motion=False, skip_ollama=True, save=False)
            desc = r.get('description_full', '')
            alert = r.get('alert', False)
            count = r.get('count', 0)

            has_person = alert or 'person' in str(r.get('detections','')).lower()

            # Sauvegarder le snapshot UNIQUEMENT si personne detectee
            if has_person:
                # Relancer avec Ollama + sauvegarde image
                r2 = analyze(require_motion=False, skip_ollama=False, save=True)
                desc = r2.get('description_full', desc)
                alert = r2.get('alert', alert)
                count = r2.get('count', count)
            else:
                count = r.get('count', 0)
            key = f"{name}:{desc}"

            if (alert or has_person) and key != last_desc.get(name, ''):
                last_desc[name] = key
                emoji = "🚨" if alert else "👤"
                msg = f"{emoji} [{name}] {desc} ({count} objets)"
                # Envoyer direct sur Telegram
                telegram_send(msg)
                print(msg[:120])

        except Exception as e:
            print(f"⚠️ [{name}] {e}")

        time.sleep(interval)

    time.sleep(1)
