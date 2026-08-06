#!/usr/bin/env python3
"""Proxy Flask - Mini App Telegram + API Dashboard multi-cameras"""
from flask import Flask, request, Response, send_from_directory, send_file
import urllib.request, json, os, subprocess
from datetime import datetime, timedelta
from pathlib import Path

app = Flask(__name__)
CAMERA = "http://192.168.1.178"
HOME = Path.home()
DASHBOARD_DIR = HOME / ".hermes" / "dashboards"
BASE_DIR = HOME / ".hermes" / "camera"
AI_LOG = BASE_DIR / "ai_events.jsonl"
FLAG_FILE = BASE_DIR / "ai_enabled"

def proxy(url, timeout=8):
    try:
        req = urllib.request.Request(url)
        for h in ['Accept', 'Content-Type']:
            if h in request.headers: req.add_header(h, request.headers[h])
        resp = urllib.request.urlopen(req, timeout=timeout)
        return Response(resp.read(), status=resp.status,
                       content_type=resp.headers.get('Content-Type','application/octet-stream'))
    except Exception as e:
        return Response(json.dumps({"error": str(e)}), status=502, content_type="application/json")

# --- Pages ---
@app.route("/")
def mini_app(): return send_from_directory(DASHBOARD_DIR, "eye_telegram.html")

@app.route("/dashboard")
def dashboard(): return send_from_directory(DASHBOARD_DIR, "ai_dashboard.html")

@app.route("/gallery")
def gallery_page(): return send_from_directory(DASHBOARD_DIR, "gallery.html")

# --- Camera API ---
@app.route("/api/capture")
def api_capture(): return proxy(f"{CAMERA}/capture")

@app.route("/api/info")
def api_info(): return proxy(f"{CAMERA}/info")

@app.route("/api/led")
def api_led():
    return proxy(f"{CAMERA}/led?state={request.args.get('state','toggle')}")

# --- AI API ---
@app.route("/api/ai/status")
def ai_status():
    events_total = persons = vehicles = 0
    if AI_LOG.exists():
        with open(AI_LOG) as f:
            for line in f:
                try:
                    e = json.loads(line); events_total += 1
                    if "person" in str(e.get("detections","")).lower(): persons += 1
                    if any(v in str(e.get("detections","")).lower() for v in ["car","truck","motorcycle"]): vehicles += 1
                except: pass
    gpu = "N/A"
    try:
        r = subprocess.run(["nvidia-smi","--query-gpu=memory.used","--format=csv,noheader"],
                          capture_output=True, text=True, timeout=3)
        if r.returncode == 0: gpu = r.stdout.strip()
    except: pass
    return {"ai_enabled": FLAG_FILE.exists(), "total_events": events_total,
            "persons": persons, "vehicles": vehicles, "gpu": gpu,
            "snapshots": len(list(BASE_DIR.glob("snapshots/*.jpg"))) if (BASE_DIR/"snapshots").exists() else 0,
            "updated": datetime.now().isoformat()}

@app.route("/api/ai/events")
def ai_events():
    events = []
    if AI_LOG.exists():
        with open(AI_LOG) as f:
            for line in f.readlines()[-int(request.args.get("limit",30)):]:
                try:
                    e = json.loads(line)
                    events.append({"time": e.get("timestamp",""), "desc": e.get("description_full","?")[:150],
                                   "count": e.get("count",0)})
                except: pass
    return list(reversed(events))

@app.route("/api/ai/toggle")
def ai_toggle():
    if FLAG_FILE.exists(): FLAG_FILE.unlink(); return {"enabled": False}
    else: FLAG_FILE.touch(); return {"enabled": True}

@app.route("/api/ai/analyze")
def ai_analyze():
    try:
        r = subprocess.run(["python3", str(HOME/".hermes"/"scripts"/"ai_eye.py")],
                          capture_output=True, text=True, timeout=60)
        return {"ok": True, "output": r.stdout[-200:]}
    except Exception as e:
        return {"ok": False, "error": str(e)}

@app.route("/api/ai/counter")
def ai_counter():
    cf = BASE_DIR / "daily_counter.json"
    if not cf.exists(): return {"today": {"persons":0,"vehicles":0}, "history": {}}
    with open(cf) as f: data = json.load(f)
    return {"today": data.get(datetime.now().strftime("%Y-%m-%d"), {"persons":0,"vehicles":0}), "history": data}

@app.route("/api/ai/report")
def ai_report():
    now = datetime.now(); cutoff = now - timedelta(hours=24)
    persons = vehicles = total = 0
    if AI_LOG.exists():
        with open(AI_LOG) as f:
            for line in f:
                try:
                    e = json.loads(line)
                    if datetime.fromisoformat(e["timestamp"]) >= cutoff:
                        total += 1
                        if "person" in str(e.get("detections","")).lower(): persons += 1
                        if any(v in str(e.get("detections","")).lower() for v in ["car","truck","motorcycle"]): vehicles += 1
                except: pass
    cf = BASE_DIR / "daily_counter.json"
    counter = {}
    if cf.exists():
        with open(cf) as f: counter = json.load(f)
    return {"date": now.strftime("%Y-%m-%d"), "total_events": total,
            "person_events": persons, "vehicle_events": vehicles,
            "counter": counter.get(now.strftime("%Y-%m-%d"),{"persons":0,"vehicles":0}),
            "anomalies": [], "generated": now.isoformat()}

@app.route("/api/ai/heatmap")
def ai_heatmap():
    cutoff = datetime.now() - timedelta(hours=24); points = []
    if AI_LOG.exists():
        with open(AI_LOG) as f:
            for line in f:
                try:
                    e = json.loads(line)
                    if datetime.fromisoformat(e["timestamp"]) < cutoff: continue
                    for d in e.get("detections",[]):
                        if "bbox" in d:
                            bbox = d["bbox"]
                            points.append({"x": round((bbox[0]+bbox[2])/2/1600,3),
                                          "y": round((bbox[1]+bbox[3])/2/1200,3),
                                          "label": d.get("label","?"), "conf": d.get("confidence",0)})
                except: pass
    return {"points": points, "image_size": [1600,1200]}

# --- Gallery API ---
@app.route("/api/ai/gallery")
def ai_gallery():
    limit = int(request.args.get("limit", 50)); items = []
    if AI_LOG.exists():
        with open(AI_LOG) as f:
            for line in reversed(f.readlines()[-500:]):
                try:
                    e = json.loads(line)
                    if "person" not in str(e.get("detections","")).lower(): continue
                    img = e.get("image","")
                    if img and os.path.exists(img):
                        items.append({"time": e.get("timestamp",""), "desc": e.get("description_full","")[:150],
                                     "count": e.get("count",0), "image": img.replace(str(HOME), ""),
                                     "alert": e.get("alert",False)})
                    if len(items) >= limit: break
                except: pass
    return items

@app.route("/snapshots/<path:filename>")
def serve_snapshot(filename):
    return send_file(str(BASE_DIR / "snapshots" / filename))

# --- Cameras config ---
@app.route("/api/cameras")
def api_cameras():
    cf = BASE_DIR / "cameras.json"
    if cf.exists():
        with open(cf) as f: return json.load(f)
    return {"cameras": [{"name": "Camera", "url": CAMERA}]}

if __name__ == "__main__":
    app.run(host="127.0.0.1", port=8084, debug=False)
