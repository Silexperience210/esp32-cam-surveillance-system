#!/usr/bin/env python3
"""Proxy Flask pour la Mini App Telegram - relaye les requetes vers l'ESP32-CAM"""
from flask import Flask, request, Response, send_from_directory
import urllib.request
import json
import os

app = Flask(__name__)
CAMERA = "http://192.168.1.178"
DASHBOARD_DIR = os.path.expanduser("~/.hermes/dashboards")


def proxy(url, timeout=8):
    try:
        req = urllib.request.Request(url)
        for h in ['Accept', 'Content-Type']:
            if h in request.headers:
                req.add_header(h, request.headers[h])
        resp = urllib.request.urlopen(req, timeout=timeout)
        data = resp.read()
        content_type = resp.headers.get('Content-Type', 'application/octet-stream')
        return Response(data, status=resp.status, content_type=content_type)
    except urllib.error.HTTPError as e:
        return Response(str(e), status=e.code)
    except Exception as e:
        return Response(json.dumps({"error": str(e)}), status=502, content_type="application/json")


@app.route("/")
def mini_app():
    return send_from_directory(DASHBOARD_DIR, "eye_telegram.html")

@app.route("/api/capture")
def api_capture():
    return proxy(f"{CAMERA}/capture")

@app.route("/api/info")
def api_info():
    return proxy(f"{CAMERA}/info")

@app.route("/api/led")
def api_led():
    state = request.args.get("state", "toggle")
    return proxy(f"{CAMERA}/led?state={state}")


# --- AI DASHBOARD API ---
import subprocess
from datetime import datetime, timedelta
from pathlib import Path

BASE_DIR = Path.home() / ".hermes" / "camera"
AI_LOG = BASE_DIR / "ai_events.jsonl"
FLAG_FILE = BASE_DIR / "ai_enabled"

@app.route("/dashboard")
def ai_dashboard():
    return send_from_directory(DASHBOARD_DIR, "ai_dashboard.html")

@app.route("/api/ai/status")
def ai_status():
    events_total = 0; persons = 0; vehicles = 0
    if AI_LOG.exists():
        with open(AI_LOG) as f:
            for line in f:
                try:
                    e = json.loads(line)
                    events_total += 1
                    desc = e.get("description_full", "")
                    if "personne" in desc.lower() or "person" in str(e.get("detections","")).lower():
                        persons += 1
                    if "vehicule" in desc.lower() or "car" in str(e.get("detections","")).lower():
                        vehicles += 1
                except: pass
    gpu = "N/A"
    try:
        r = subprocess.run(["nvidia-smi","--query-gpu=memory.used","--format=csv,noheader"],
                          capture_output=True, text=True, timeout=3)
        if r.returncode == 0: gpu = r.stdout.strip()
    except: pass
    return {
        "ai_enabled": FLAG_FILE.exists(),
        "total_events": events_total, "persons": persons, "vehicles": vehicles,
        "snapshots": len(list(BASE_DIR.glob("snapshots/*.jpg"))) if (BASE_DIR/"snapshots").exists() else 0,
        "gpu": gpu, "updated": datetime.now().isoformat()
    }

@app.route("/api/ai/events")
def ai_events():
    limit = int(request.args.get("limit", 30))
    events = []
    if AI_LOG.exists():
        with open(AI_LOG) as f:
            lines = f.readlines()
            for line in lines[-limit:]:
                try:
                    e = json.loads(line)
                    events.append({"time": e.get("timestamp",""), "desc": e.get("description_full","?"), "count": e.get("count",0)})
                except: pass
    return list(reversed(events))

@app.route("/api/ai/toggle")
def ai_toggle():
    if FLAG_FILE.exists():
        FLAG_FILE.unlink(); return {"enabled": False}
    else:
        FLAG_FILE.touch(); return {"enabled": True}

@app.route("/api/ai/analyze")
def ai_analyze():
    try:
        r = subprocess.run(["python3", str(Path.home()/".hermes"/"scripts"/"ai_eye.py")],
                          capture_output=True, text=True, timeout=60)
        return {"ok": True, "output": r.stdout[-200:]}
    except Exception as e:
        return {"ok": False, "error": str(e)}

@app.route("/api/ai/counter")
def ai_counter():
    cf = BASE_DIR / "daily_counter.json"
    if not cf.exists():
        return {"today": {"persons":0,"vehicles":0}, "history": {}}
    with open(cf) as f: data = json.load(f)
    today = datetime.now().strftime("%Y-%m-%d")
    return {"today": data.get(today, {"persons":0,"vehicles":0}), "history": data}

@app.route("/api/ai/report")
def ai_report():
    now = datetime.now(); cutoff = now - timedelta(hours=24)
    events = []; persons = 0; vehicles = 0
    if AI_LOG.exists():
        with open(AI_LOG) as f:
            for line in f:
                try:
                    e = json.loads(line)
                    if datetime.fromisoformat(e["timestamp"]) >= cutoff:
                        events.append(e)
                        if "person" in str(e.get("detections","")).lower(): persons += 1
                        if any(v in str(e.get("detections","")).lower() for v in ["car","truck","motorcycle"]): vehicles += 1
                except: pass
    counter = {}
    cf = BASE_DIR / "daily_counter.json"
    if cf.exists():
        with open(cf) as f: counter = json.load(f)
    return {
        "date": now.strftime("%Y-%m-%d"), "total_events": len(events),
        "person_events": persons, "vehicle_events": vehicles,
        "counter": counter.get(now.strftime("%Y-%m-%d"), {"persons":0,"vehicles":0}),
        "anomalies": [], "generated": now.isoformat()
    }

@app.route("/api/ai/heatmap")
def ai_heatmap():
    cutoff = datetime.now() - timedelta(hours=24)
    points = []
    if AI_LOG.exists():
        with open(AI_LOG) as f:
            for line in f:
                try:
                    e = json.loads(line)
                    if datetime.fromisoformat(e["timestamp"]) < cutoff: continue
                    for d in e.get("detections", []):
                        if "bbox" in d:
                            bbox = d["bbox"]
                            cx = (bbox[0] + bbox[2]) / 2
                            cy = (bbox[1] + bbox[3]) / 2
                            points.append({"x": round(cx/1600,3), "y": round(cy/1200,3),
                                          "label": d.get("label","?"), "conf": d.get("confidence",0)})
                except: pass
    return {"points": points, "image_size": [1600, 1200]}

@app.route("/api/cameras")
def api_cameras():
    """Liste des cameras configurees."""
    cf = BASE_DIR / "cameras.json"
    if cf.exists():
        with open(cf) as f: return json.load(f)
    return {"cameras": [{"name": "Camera", "url": CAMERA}]}


if __name__ == "__main__":
    app.run(host="127.0.0.1", port=8084, debug=False)
