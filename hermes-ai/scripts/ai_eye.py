#!/usr/bin/env python3
"""
Hermes AI Eye — Pipeline IA pour ESP32-CAM
YOLO détection objets + logs + toggle on/off

Utilisation :
  python3 ai_eye.py            → analyse une fois
  python3 ai_eye.py --loop     → boucle continue (cron-friendly)
  python3 ai_eye.py --on       → active l'IA
  python3 ai_eye.py --off      → désactive l'IA
  python3 ai_eye.py --status   → état actuel
  python3 ai_eye.py --cleanup N → nettoie les fichiers de + de N jours
"""
import os, sys, json, time, shutil, subprocess
from datetime import datetime, timedelta
from pathlib import Path

# ─── CONFIG ────────────────────────────────────────────
BASE_DIR = Path.home() / ".hermes" / "camera"
CAMERA_URL = os.environ.get("CAMERA_URL", "http://192.168.1.178")  # override via env
CAMERAS_CONFIG = BASE_DIR / "cameras.json"
SNAPSHOT_DIR = BASE_DIR / "snapshots"
DIFF_DIR = BASE_DIR / "diffs"
AI_LOG = BASE_DIR / "ai_events.jsonl"
FLAG_FILE = BASE_DIR / "ai_enabled"
ZONES_FILE = BASE_DIR / "zones.json"       # zones d'interet
ALERT_FILE = BASE_DIR / "alert_config.json" # config alertes
MODEL_CACHE = BASE_DIR / "models"
CLEANUP_DAYS = 7

# Alertes intelligentes: quels objets declenchent une alerte?
ALERT_CLASSES = {"person", "car", "truck", "motorcycle", "bicycle", "dog", "cat"}
COUNTER_FILE = BASE_DIR / "daily_counter.json"
PASSAGE_COOLDOWN = 300  # 5 minutes entre 2 passages comptes comme distincts

os.makedirs(SNAPSHOT_DIR, exist_ok=True)
os.makedirs(DIFF_DIR, exist_ok=True)
os.makedirs(MODEL_CACHE, exist_ok=True)


# ─── FLAG TOGGLE ────────────────────────────────────────
def ai_enabled() -> bool:
    return FLAG_FILE.exists()

def ai_toggle(on: bool):
    if on:
        FLAG_FILE.touch()
        print("✅ IA activée")
    else:
        FLAG_FILE.unlink(missing_ok=True)
        print("⏸️ IA suspendue")


# ─── CAPTURE ───────────────────────────────────────────
def capture():
    import urllib.request
    try:
        req = urllib.request.Request(f"{CAMERA_URL}/capture")
        req.add_header("X-Auth-Token", "hermes2024")
        data = urllib.request.urlopen(req, timeout=8).read()
        ts = datetime.now()
        path = SNAPSHOT_DIR / f"snap_{ts.strftime('%Y%m%d_%H%M%S')}.jpg"
        with open(path, "wb") as f:
            f.write(data)
        return data, path, ts
    except Exception as e:
        print(f"ERREUR capture: {e}", file=sys.stderr)
        return None, None, None


# ─── YOLO DETECTION ────────────────────────────────────
_model = None

def get_model():
    global _model
    if _model is None:
        from ultralytics import YOLO
        _model = YOLO("yolov8n.pt")  # nano model (~6MB, le plus rapide)
        if hasattr(_model, 'to'):
            _model.to('cuda')  # GPU
    return _model

def detect_objects(image_path) -> list:
    """Retourne une liste de {label, confidence, bbox}."""
    try:
        model = get_model()
        results = model(image_path, verbose=False)
        detections = []
        for r in results:
            for box in r.boxes:
                cls_id = int(box.cls[0])
                label = model.names.get(cls_id, f"cls_{cls_id}")
                conf = float(box.conf[0])
                bbox = box.xyxy[0].tolist()
                detections.append({
                    "label": label,
                    "confidence": round(conf, 2),
                    "bbox": [round(x) for x in bbox]
                })
        return detections
    except Exception as e:
        return [{"error": str(e)}]


# ─── DESCRIPTION ────────────────────────────────────────
def describe(detections: list) -> str:
    """Génère une description texte des objets détectés."""
    if not detections:
        return "Rien de notable."
    
    if any("error" in d for d in detections):
        return f"⚠️ Erreur modèle: {detections[0].get('error', 'inconnue')}"

    # Regrouper par label
    from collections import Counter
    counts = Counter(d["label"] for d in detections)
    
    # Priorités
    priority = ["person", "car", "truck", "motorcycle", "bicycle",
                "dog", "cat", "bird", "backpack", "suitcase", "umbrella"]
    
    parts = []
    for label in priority:
        if label in counts:
            n = counts[label]
            label_fr = {
                "person": "personne", "car": "voiture", "truck": "camion",
                "motorcycle": "moto", "bicycle": "vélo", "dog": "chien",
                "cat": "chat", "bird": "oiseau", "backpack": "sac à dos",
                "suitcase": "valise", "umbrella": "parapluie",
                "cell phone": "téléphone", "laptop": "ordinateur",
                "chair": "chaise", "bottle": "bouteille", "cup": "tasse",
                "book": "livre", "clock": "horloge", "vase": "vase",
                "potted plant": "plante", "tv": "télévision",
                "couch": "canapé", "bed": "lit", "dining table": "table",
                "toilet": "toilettes", "sink": "évier",
                "refrigerator": "frigo", "oven": "four",
                "microwave": "micro-ondes", "sports ball": "ballon",
            }.get(label, label)
            parts.append(f"{n} {label_fr}" + ("s" if n > 1 and not label_fr.endswith("s") else ""))
            del counts[label]

    # Reste des objets
    for label, n in counts.most_common(5):
        parts.append(f"{n} {label}" + ("s" if n > 1 else ""))

    if parts:
        return "Detecte: " + ", ".join(parts) + "."
    return "Objets non identifies."


def enrich_description(detections: list, yolo_desc: str) -> str:
    """Enrichit la description YOLO avec du contexte temporel et spatial."""
    if not detections:
        return "Rien de notable."
    
    if any("error" in d for d in detections):
        return yolo_desc

    from collections import Counter
    counts = Counter(d["label"] for d in detections)
    
    has_person = counts.get("person", 0) > 0
    has_vehicle = counts.get("car", 0) + counts.get("truck", 0) + counts.get("motorcycle", 0) > 0
    has_animal = counts.get("dog", 0) + counts.get("cat", 0) + counts.get("bird", 0) > 0
    total = len(detections)
    count_persons = counts.get("person", 0)

    # Construction contextuelle
    now = datetime.now()
    hour = now.hour
    moment = "la nuit" if hour < 7 or hour >= 21 else ("le matin" if hour < 12 else ("l'apres-midi" if hour < 18 else "le soir"))

    parts = []
    if has_person and has_vehicle:
        parts.append(f"Personne pres d'un vehicule, {moment}")
    elif has_person:
        parts.append(f"{count_persons} personne(s) detectee(s), {moment}")
    elif has_vehicle:
        parts.append(f"Vehicule detecte, {moment}")
    elif has_animal:
        parts.append(f"Animal visible, {moment}")
    elif total > 0:
        parts.append(f"{total} objet(s) detecte(s), {moment}")
    else:
        parts.append(f"Scene vide, {moment}")

    # Ajouter details YOLO
    detail = yolo_desc.replace("Detecte: ", "").replace(".", "")
    if detail and detail != "Rien de notable" and detail != "Objets non identifies":
        parts.append(f"({detail})")

    return " ".join(parts) + "."


def should_alert(detections: list) -> tuple:
    """Determine si une alerte doit etre envoyee. Retourne (alerte: bool, raison: str)."""
    if not detections:
        return False, ""
    
    alert_objects = [d for d in detections if d.get("label") in ALERT_CLASSES]
    if not alert_objects:
        return False, ""
    
    from collections import Counter
    counts = Counter(d["label"] for d in alert_objects)
    reasons = []
    for label, n in counts.most_common(3):
        reasons.append(f"{n} {label}" + ("s" if n > 1 else ""))
    
    return True, "Alerte: " + ", ".join(reasons)


def ollama_describe(image_path: str) -> str:
    """Description via Ollama + LLaVA. Necessite ollama et llava-llama3."""
    now = datetime.now()
    hour = now.hour
    moment = "la nuit (il fait noir dehors)" if hour < 7 or hour >= 21 else (
        "le matin" if hour < 12 else ("l'apres-midi" if hour < 17 else "le soir (la luminosite baisse)"))

    prompt = (
        f"Decris UNIQUEMENT les personnes et animaux visibles dans cette image de surveillance. "
        f"Ne decris PAS le decor, les murs, les objets, les meubles ou l'arriere-plan. "
        f"Pour chaque personne: position dans la piece, ce qu'elle fait, vetements si visibles. "
        f"Il est {now.strftime('%Hh%M')}, {moment}. "
        f"Si personne n'est visible, dis juste \"Personne\". "
        f"Une phrase maximum. Image: {image_path}"
    )
    try:
        r = subprocess.run(
            ["ollama", "run", "llava-llama3:8b", prompt],
            capture_output=True, text=True, timeout=30
        )
        if r.returncode == 0 and r.stdout.strip():
            return r.stdout.strip()
        return ""
    except Exception:
        return ""


def check_anomaly(ts: datetime, detections: list) -> str:
    """
    Detecte des patterns anormaux base sur l'historique.
    - Personne detectee a des heures inhabituelles (3h du mat)
    - Absence prolongee d'activite (>6h alors que normalement actif)
    - Pic d'activite anormal
    Retourne description de l'anomalie ou ''.
    """
    if not AI_LOG.exists():
        return ""

    hour = ts.hour
    has_person = any(d.get("label") == "person" for d in detections)

    # Analyser les dernieres 24h d'evenements
    cutoff = ts - timedelta(hours=24)
    recent_persons = 0
    recent_total = 0
    hour_persons = 0
    night_events = 0

    with open(AI_LOG) as f:
        for line in f:
            try:
                e = json.loads(line)
                et = datetime.fromisoformat(e["timestamp"])
                if et < cutoff:
                    continue
                recent_total += 1
                if "person" in str(e.get("detections", "")).lower():
                    recent_persons += 1
                    if et.hour == hour:
                        hour_persons += 1
                    if et.hour < 6 or et.hour >= 23:
                        night_events += 1
            except:
                pass

    # Regles d'anomalie
    anomalies = []

    # 1. Personne la nuit (23h-6h)
    if has_person and (hour < 6 or hour >= 23):
        anomalies.append(f"Personne detectee a {hour}h (nuit) — inhabituel")

    # 2. Premiere activite depuis longtemps
    if recent_total <= 1 and recent_persons == 1:
        anomalies.append("Premiere activite en 24h")

    # 3. Pic d'activite par rapport a la normale
    if has_person and hour_persons > 3:
        anomalies.append(f"Activite anormale: {hour_persons} detections cette heure")

    return " | ".join(anomalies) if anomalies else ""


def check_counter(detections: list, ts: datetime) -> dict:
    """Compteur de passages avec cooldown 5min. Retourne stats du jour."""
    today = ts.strftime("%Y-%m-%d")
    has_person = any(d.get("label") == "person" for d in detections)

    counter = {}
    if COUNTER_FILE.exists():
        with open(COUNTER_FILE) as f:
            counter = json.load(f)

    day_data = counter.get(today, {
        "persons": 0, "vehicles": 0,
        "last_person_ts": None, "last_vehicle_ts": None
    })

    if has_person:
        last = day_data.get("last_person_ts")
        if not last or (ts - datetime.fromisoformat(last)).total_seconds() > PASSAGE_COOLDOWN:
            day_data["persons"] += 1
            day_data["last_person_ts"] = ts.isoformat()

    has_vehicle = any(d.get("label") in ("car", "truck", "motorcycle") for d in detections)
    if has_vehicle:
        last = day_data.get("last_vehicle_ts")
        if not last or (ts - datetime.fromisoformat(last)).total_seconds() > PASSAGE_COOLDOWN:
            day_data["vehicles"] += 1
            day_data["last_vehicle_ts"] = ts.isoformat()

    counter[today] = day_data
    with open(COUNTER_FILE, "w") as f:
        json.dump(counter, f)
    return day_data


MOTION_THRESHOLD = 0.005  # 0.5% — ultra sensible pour faible luminosité

def motion_detected(threshold: float = MOTION_THRESHOLD) -> tuple:
    """Compare deux frames a 1s d'intervalle. CPU uniquement, 0 GPU."""
    import urllib.request, io
    try:
        from PIL import Image
        import numpy as np

        # Frame 1
        req1 = urllib.request.Request(f"{CAMERA_URL}/capture")
        req1.add_header("X-Auth-Token", "hermes2024")
        data1 = urllib.request.urlopen(req1, timeout=8).read()
        time.sleep(0.8)
        # Frame 2
        req2 = urllib.request.Request(f"{CAMERA_URL}/capture")
        req2.add_header("X-Auth-Token", "hermes2024")
        data2 = urllib.request.urlopen(req2, timeout=8).read()

        # Comparer en basse resolution pour la vitesse
        img1 = Image.open(io.BytesIO(data1)).convert("L").resize((160, 120))
        img2 = Image.open(io.BytesIO(data2)).convert("L").resize((160, 120))

        arr1 = np.array(img1, dtype=np.float32)
        arr2 = np.array(img2, dtype=np.float32)

        diff = np.abs(arr1 - arr2)
        diff_pct = float(np.mean(diff > 25))

        motion = diff_pct > threshold
        return motion, round(diff_pct * 100, 2)

    except ImportError:
        return True, 0.0  # pas de PIL -> on analyse
    except Exception:
        return True, 0.0  # erreur -> on analyse


# ─── PIPELINE ───────────────────────────────────────────
def analyze(save=True, require_motion: bool = True, skip_ollama: bool = False) -> dict:
    """Pipeline intelligent: motion -> capture -> YOLO -> Ollama -> alerte.
    Si require_motion=True, ne lance YOLO que si mouvement detecte.
    Si skip_ollama=True, saute Ollama (plus rapide, ~3s vs ~8s)."""

    if not ai_enabled():
        return {"status": "disabled", "reason": "IA suspendue"}

    # Etape 0: detection de mouvement (CPU, zero GPU)
    diff_pct = None
    if require_motion:
        has_motion, diff_pct = motion_detected(MOTION_THRESHOLD)
        if not has_motion:
            return {
                "status": "static",
                "reason": f"Pas de mouvement ({diff_pct}% < {int(MOTION_THRESHOLD*100)}%)",
                "description": "RAS - scene statique",
                "count": 0
            }

    data, path, ts = capture()
    if not data:
        return {"status": "error", "reason": "Echec capture"}

    detections = detect_objects(str(path))
    yolo_desc = describe(detections)

    # Description: Ollama (si dispo et pas skip) sinon YOLO enrichi
    if not skip_ollama:
        ollama_desc = ollama_describe(str(path))
        if ollama_desc:
            full_desc = ollama_desc
        else:
            full_desc = enrich_description(detections, yolo_desc)
    else:
        full_desc = enrich_description(detections, yolo_desc)

    # Verifier si alerte + patterns + compteur
    alert, alert_reason = should_alert(detections)
    anomaly = check_anomaly(ts, detections)
    counter = check_counter(detections, ts)

    result = {
        "timestamp": ts.isoformat(),
        "image": str(path) if save else None,
        "detections": detections,
        "description_full": full_desc,
        "count": len(detections),
        "alert": alert,
        "alert_reason": alert_reason,
        "anomaly": anomaly,
        "counter": counter,
        "motion_diff_pct": diff_pct
    }

    # Logger
    with open(AI_LOG, "a") as f:
        f.write(json.dumps(result, ensure_ascii=False) + "\n")

    return result


# ─── CLEANUP ────────────────────────────────────────────
def cleanup(max_days: int = CLEANUP_DAYS):
    """Supprime les snapshots et diffs vieux de plus de N jours."""
    cutoff = datetime.now() - timedelta(days=max_days)
    cleaned = 0

    for d in [SNAPSHOT_DIR, DIFF_DIR]:
        if not d.exists():
            continue
        for f in d.iterdir():
            if f.is_file():
                mtime = datetime.fromtimestamp(f.stat().st_mtime)
                if mtime < cutoff:
                    f.unlink()
                    cleaned += 1

    print(f"🧹 Nettoyage: {cleaned} fichiers supprimés (>{max_days} jours)")
    return cleaned


# ─── STATUS ─────────────────────────────────────────────
def status() -> dict:
    enabled = ai_enabled()
    gpu_info = "N/A"
    try:
        import subprocess
        r = subprocess.run(
            ["nvidia-smi", "--query-gpu=memory.used,utilization.gpu,temperature.gpu",
             "--format=csv,noheader,nounits"],
            capture_output=True, text=True, timeout=5
        )
        if r.returncode == 0:
            gpu_info = r.stdout.strip()
    except:
        pass

    snap_count = len(list(SNAPSHOT_DIR.glob("*.jpg"))) if SNAPSHOT_DIR.exists() else 0
    log_lines = 0
    if AI_LOG.exists():
        with open(AI_LOG) as f:
            log_lines = sum(1 for _ in f)

    return {
        "ai_enabled": enabled,
        "gpu": gpu_info,
        "snapshots": snap_count,
        "ai_events_logged": log_lines,
        "model": "yolov8n",
        "cleanup_days": CLEANUP_DAYS
    }


# ─── MAIN ───────────────────────────────────────────────
if __name__ == "__main__":
    if len(sys.argv) < 2:
        # Mode par défaut: analyse unique
        if not ai_enabled():
            print("⏸️ IA désactivée. Utilise --on pour activer.")
            sys.exit(0)
        print("🔍 Analyse en cours...")
        import time as _time
        t0 = _time.time()
        r = analyze()
        dt = _time.time() - t0
        print(json.dumps(r, indent=2, ensure_ascii=False))
        print(f"\n⚡ Inférence: {dt:.1f}s")
    
    elif sys.argv[1] == "--on":
        ai_toggle(True)
    elif sys.argv[1] == "--off":
        ai_toggle(False)
    elif sys.argv[1] == "--status":
        print(json.dumps(status(), indent=2, ensure_ascii=False))
    elif sys.argv[1] == "--cleanup":
        days = int(sys.argv[2]) if len(sys.argv) > 2 else CLEANUP_DAYS
        cleanup(days)
    elif sys.argv[1] == "--loop":
        # Pour usage cron: analyse si active
        if not ai_enabled():
            sys.exit(0)
        r = analyze()
        print(f"{r.get('description_full', r.get('description','erreur'))} ({r.get('count',0)} objets)" if r.get('description_full') else "erreur")
    elif sys.argv[1] == "--yolo-only":
        # YOLO direct, sans filtre motion (pour faible luminosite)
        if not ai_enabled():
            sys.exit(0)
        r = analyze(require_motion=False)
        desc = r.get('description_full','')
        alert = r.get('alert', False)
        if alert or 'personne' in desc.lower() or 'person' in str(r.get('detections','')).lower():
            print(f"{'🚨' if alert else '👤'} {desc}")
    else:
        print(__doc__)
