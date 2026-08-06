#!/usr/bin/env python3
"""Hermes AI Eye Daemon HARDCORE — YOLO + DeepFace + Pose + OWL-ViT"""
import time, os, sys, json, urllib.request
from pathlib import Path

HOME = Path.home()
BASE = HOME / ".hermes" / "camera"
FLAG = BASE / "ai_enabled"
CONFIG = BASE / "cameras.json"
SCRIPTS = HOME / ".hermes" / "scripts"
sys.path.insert(0, str(SCRIPTS))

# Load hardcore pipeline
pipeline = {}
exec(open(SCRIPTS / "ai_eye_hardcore.py").read(), pipeline)
analyze = pipeline['analyze']

def load_cameras():
    if CONFIG.exists():
        with open(CONFIG) as f:
            return [c for c in json.load(f).get("cameras",[]) if c.get("enabled",True)]
    return [{"name":"Camera","url":"http://192.168.1.178","interval":30}]

def telegram_send(text):
    try:
        with open(HOME / ".hermes" / ".env") as f: env = f.read()
        token = ""
        for line in env.split('\n'):
            if line.startswith("TELEGRAM_BOT_TOKEN="):
                token = line.split("=",1)[1].strip().strip('"').strip("'")
        if token:
            for cid in ["540124594", "7686686467"]:
                data = json.dumps({"chat_id":cid,"text":text}).encode()
                req = urllib.request.Request(f"https://api.telegram.org/bot{token}/sendMessage",
                    data=data, headers={"Content-Type":"application/json"})
                urllib.request.urlopen(req, timeout=5)
    except: pass

cameras = load_cameras()
print(f"👁 HARDCORE Daemon — {len(cameras)} camera(s) [YOLO+DeepFace+Pose+OWL-ViT]")
last_key = {}

while True:
    if not FLAG.exists():
        time.sleep(5); continue
    for cam in cameras:
        name = cam.get("name","Cam")
        url = cam.get("url","http://192.168.1.178")
        interval = cam.get("interval", 30)  # 30s pour ne pas saturer
        try:
            os.environ["CAMERA_URL"] = url
            r = analyze(save=False)  # YOLO rapide d'abord
            desc = r.get('description','')
            alert = r.get('alert',False)
            has_person = alert or 'personne' in desc

            if has_person:
                r = analyze(save=True)  # Sauvegarde complète
                desc = r.get('description','')
                alert = r.get('alert',alert)

            key = f"{name}:{desc}"
            timing = r.get('timing',{})
            tag = timing.get('total',0)

            if (alert or has_person) and key != last_key.get(name,''):
                last_key[name] = key
                emoji = "🚨" if alert else "👤"
                msg = f"{emoji} [{name}] {desc}"
                if tag > 10: msg += f" (⚡{tag:.0f}s)"
                telegram_send(msg)
                print(f"{datetime.now().strftime('%H:%M:%S')} {msg[:120]}")
        except Exception as e:
            pass
        time.sleep(interval)
    time.sleep(1)
