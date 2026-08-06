#!/usr/bin/env python3
"""Hermes AI Eye Daemon — boucle YOLO toutes les 15s, silencieux si RAS"""
import time, os, sys, json
from pathlib import Path
from datetime import datetime

sys.path.insert(0, os.path.expanduser("~/.hermes/scripts"))
exec(open(os.path.expanduser("~/.hermes/scripts/ai_eye.py")).read().split("if __name__")[0])

FLAG = Path.home() / ".hermes" / "camera" / "ai_enabled"
LOG = Path.home() / ".hermes" / "camera" / "ai_events.jsonl"
INTERVAL = 15  # secondes entre chaque check

print(f"👁️ AI Eye Daemon — check toutes les {INTERVAL}s")
last_desc = ""

while True:
    if not FLAG.exists():
        time.sleep(5)
        continue

    try:
        # YOLO rapide d'abord (3s)
        r = analyze(require_motion=False, skip_ollama=True)
        desc = r.get('description_full', '')
        alert = r.get('alert', False)
        count = r.get('count', 0)

        # Ne notifier que si changement ou alerte
        has_person = 'personne' in desc.lower() or 'person' in str(r.get('detections','')).lower()
        if (alert or has_person) and desc != last_desc:
            last_desc = desc
            emoji = "🚨" if alert else "👤"
            # Sauvegarder dans un fichier signal pour le cron de livraison
            signal = Path.home() / ".hermes" / "camera" / "last_alert.txt"
            with open(signal, "w") as f:
                f.write(f"{emoji} {desc} ({count} objets)")
            print(f"{emoji} {desc[:100]}")

    except Exception as e:
        print(f"⚠️ Daemon erreur: {e}")

    time.sleep(INTERVAL)
