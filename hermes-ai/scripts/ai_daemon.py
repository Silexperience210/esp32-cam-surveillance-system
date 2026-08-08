#!/usr/bin/env python3
"""Hermes AI Eye Daemon — YOLO only, rapide et fiable"""
import time, os, sys, json, urllib.request
from pathlib import Path

HOME = Path.home()
BASE = HOME / ".hermes" / "camera"
FLAG = BASE / "ai_enabled"
CONFIG_FILE = BASE / "cameras.json"
SYS_PATH = str(HOME / ".hermes" / "scripts")
if SYS_PATH not in sys.path: sys.path.insert(0, SYS_PATH)
exec(open(HOME / ".hermes" / "scripts" / "ai_eye.py").read().split("if __name__")[0])

FR = {'person':'👤 personne','dog':'🐕 chien','cat':'🐱 chat','bird':'🐦 oiseau',
      'car':'🚗 voiture','truck':'🚛 camion','motorcycle':'🏍️ moto','bicycle':'🚲 vélo',
      'horse':'🐴 cheval','sheep':'🐑 mouton','cow':'🐄 vache'}

def load_cameras():
    if CONFIG_FILE.exists():
        with open(CONFIG_FILE) as f:
            return [c for c in json.load(f).get("cameras",[]) if c.get("enabled",True)]
    return [{"name":"Camera","url":CAMERA_URL,"interval":15}]

def telegram_send(text):
    try:
        env = HOME / ".hermes" / ".env"
        token = ""
        chat_ids = ["540124594", "7686686467"]
        with open(env) as f:
            for line in f:
                if line.startswith("TELEGRAM_BOT_TOKEN="):
                    token = line.split("=",1)[1].strip().strip('"').strip("'")
        if token:
            for cid in chat_ids:
                data = json.dumps({"chat_id":cid,"text":text}).encode()
                req = urllib.request.Request(f"https://api.telegram.org/bot{token}/sendMessage",
                    data=data, headers={"Content-Type":"application/json"})
                urllib.request.urlopen(req, timeout=5)
    except: pass

cameras = load_cameras()
print(f"👁 AI Daemon — {len(cameras)} camera(s) [YOLO only]")
last_key = {}

while True:
    if not FLAG.exists():
        time.sleep(5); continue
    for cam in cameras:
        name = cam.get("name","Cam")
        url = cam.get("url",CAMERA_URL)
        interval = cam.get("interval",15)
        try:
            os.environ["CAMERA_URL"] = url
            r = analyze(require_motion=False, skip_ollama=True, save=False)
            dets = r.get('detections',[])
            alert = r.get('alert',False)
            count = r.get('count',0)
            has_person = alert or 'person' in str(dets).lower()

            # Description YOLO
            if not dets:
                desc = "RAS"
            else:
                counts = {}
                for d in dets: counts[d['label']] = counts.get(d['label'],0)+1
                parts = []
                for l in ['person','dog','cat','bird','car','truck','motorcycle','bicycle','horse','sheep','cow']:
                    if l in counts:
                        n = counts[l]
                        parts.append(f"{FR.get(l,l)}" if n==1 else f"{n} {FR.get(l,l)}s")
                if not parts: parts = [f"{list(counts.keys())[0]} ({list(counts.values())[0]})"]
                desc = ", ".join(parts)
                if count > len(parts):
                    desc += f" +{count-len(parts)} objets"

            if has_person:
                r2 = analyze(require_motion=False, skip_ollama=True, save=True)
                dets2 = r2.get('detections',dets)
                alert = r2.get('alert',alert)
                counts = {}
                for d in dets2: counts[d['label']] = counts.get(d['label'],0)+1
                parts = []
                for l in ['person','dog','cat','bird','car','truck','motorcycle','bicycle','horse','sheep','cow']:
                    if l in counts:
                        n = counts[l]
                        parts.append(f"{FR.get(l,l)}" if n==1 else f"{n} {FR.get(l,l)}s")
                desc = ", ".join(parts)

            key = f"{name}:{desc}"
            if (alert or has_person) and key != last_key.get(name,''):
                last_key[name] = key
                emoji = "🚨" if alert else "👤"
                msg = f"{emoji} [{name}] {desc}"
                telegram_send(msg)
                print(msg)
        except Exception as e:
            pass
        time.sleep(interval)
    time.sleep(1)
