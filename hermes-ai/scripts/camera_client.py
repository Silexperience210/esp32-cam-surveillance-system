#!/usr/bin/env python3
"""
Hermes Camera Client — pont entre l'ESP32-CAM et Hermes
Caméra : ESP32-CAM OV3660 @ 1600x1200, 192.168.1.178
"""
import urllib.request
import json
import time
import sys
import os
from datetime import datetime

CAMERA_URL = "http://192.168.1.178"
SNAPSHOT_DIR = os.path.expanduser("~/.hermes/camera/snapshots")
DIFF_DIR = os.path.expanduser("~/.hermes/camera/diffs")
LAST_FRAME = os.path.expanduser("~/.hermes/camera/last_frame.jpg")
HEALTH_FILE = os.path.expanduser("~/.hermes/camera/health.json")

os.makedirs(SNAPSHOT_DIR, exist_ok=True)
os.makedirs(DIFF_DIR, exist_ok=True)


def capture(save=True):
    """Prend une photo. Retourne (bytes, path)."""
    try:
        req = urllib.request.urlopen(f"{CAMERA_URL}/capture", timeout=10)
        data = req.read()
        if save:
            ts = datetime.now().strftime("%Y%m%d_%H%M%S")
            path = os.path.join(SNAPSHOT_DIR, f"snap_{ts}.jpg")
            with open(path, "wb") as f:
                f.write(data)
            return data, path
        return data, None
    except Exception as e:
        print(f"ERREUR capture: {e}", file=sys.stderr)
        return None, None


def info():
    """Récupère les infos de la caméra."""
    try:
        req = urllib.request.urlopen(f"{CAMERA_URL}/info", timeout=5)
        return json.loads(req.read())
    except Exception as e:
        return {"error": str(e), "online": False}


def health_check():
    """Vérifie si la caméra est en ligne et stable."""
    try:
        data = info()
        data["checked_at"] = datetime.now().isoformat()
        data["online"] = "error" not in data
        with open(HEALTH_FILE, "w") as f:
            json.dump(data, f, indent=2)
        return data
    except Exception as e:
        data = {"online": False, "error": str(e), "checked_at": datetime.now().isoformat()}
        with open(HEALTH_FILE, "w") as f:
            json.dump(data, f, indent=2)
        return data


def motion_detect(threshold=0.05, debug=False):
    """
    Détection de mouvement par différence entre deux frames.
    Retourne (motion, diff_pct, diff_path).
    threshold: pourcentage de pixels différents pour déclencher (0.0-1.0)
    """
    try:
        from PIL import Image
        import numpy as np

        # Prendre 2 frames à 1s d'intervalle
        data1, _ = capture(save=False)
        if not data1:
            return False, 0, None
        time.sleep(1.0)
        data2, _ = capture(save=False)
        if not data2:
            return False, 0, None

        # Comparer
        with open("/tmp/cam_frame1.jpg", "wb") as f:
            f.write(data1)
        with open("/tmp/cam_frame2.jpg", "wb") as f:
            f.write(data2)

        img1 = Image.open("/tmp/cam_frame1.jpg").convert("L").resize((320, 240))
        img2 = Image.open("/tmp/cam_frame2.jpg").convert("L").resize((320, 240))

        arr1 = np.array(img1, dtype=np.float32)
        arr2 = np.array(img2, dtype=np.float32)

        diff = np.abs(arr1 - arr2)
        diff_pct = np.mean(diff > 25)  # pixels avec >25 niveaux d'écart

        motion = diff_pct > threshold

        if motion or debug:
            # Sauvegarder l'image de différence
            ts = datetime.now().strftime("%Y%m%d_%H%M%S")
            diff_img = Image.fromarray((diff * 4).clip(0, 255).astype(np.uint8))
            diff_path = os.path.join(DIFF_DIR, f"diff_{ts}.jpg")
            diff_img.save(diff_path)
        else:
            diff_path = None

        return motion, round(diff_pct * 100, 2), diff_path

    except ImportError:
        return False, 0, "PIL/numpy non installés"
    except Exception as e:
        return False, 0, str(e)


def status_report():
    """Rapport complet pour le dashboard."""
    cam = info()
    cam["snapshot_count"] = len(os.listdir(SNAPSHOT_DIR)) if os.path.exists(SNAPSHOT_DIR) else 0
    cam["last_snapshot"] = None
    if os.path.exists(SNAPSHOT_DIR):
        files = sorted(os.listdir(SNAPSHOT_DIR))
        if files:
            cam["last_snapshot"] = files[-1]
    return cam


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "status"
    if cmd == "capture":
        _, path = capture()
        print(path if path else "ERREUR")
    elif cmd == "info":
        print(json.dumps(info(), indent=2))
    elif cmd == "motion":
        motion, pct, diff_path = motion_detect()
        print(json.dumps({"motion": bool(motion), "diff_pct": pct, "diff": diff_path}))
    elif cmd == "health":
        data = health_check()
        if data.get("online"):
            print(f"✅ ONLINE — IP: {data.get('ip', '?')} — SD: {data.get('sd', '?')}")
        else:
            print(f"❌ OFFLINE — {data.get('error', 'inconnue')}")
    elif cmd == "status":
        print(json.dumps(status_report(), indent=2))
    else:
        print(f"Usage: {sys.argv[0]} [capture|info|motion|health|status]")
