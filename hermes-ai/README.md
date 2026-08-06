# Hermes AI — Intégration ESP32-CAM

Pipeline IA complet pour caméra de surveillance ESP32-CAM, piloté par l'agent Hermes.

## 🧠 Fonctionnalités

- **Motion detection** → ne déclenche l'analyse que si mouvement (CPU, ~1.4s)
- **YOLOv8** → détection 80 classes (personne, voiture, animal...) en ~3s GPU
- **Ollama + LLaVA** → descriptions naturelles en français (~5s)
- **Alertes intelligentes** → ne notifie que personne/véhicule/animal
- **Compteur de passages** → cooldown 5 min entre chaque comptage
- **Anomalies** → personne la nuit, pic d'activité
- **Dashboard web** → stats, événements, heatmap, toggle ON/OFF
- **Mini App Telegram** → snapshot live, contrôle LED
- **Silencieux si RAS** → 0 message si rien ne se passe

## 📁 Structure

```
hermes-ai/
├── scripts/
│   ├── ai_eye.py          → Pipeline IA principal (motion → YOLO → Ollama → alerte)
│   ├── camera_client.py   → Client HTTP pour l'ESP32-CAM
│   ├── eye_proxy.py       → Proxy Flask (Mini App + API dashboard)
│   ├── ai_smart_cron.sh   → Cron silencieux (alerte uniquement)
│   ├── camera_health.sh   → Health check caméra
│   └── ai_cleanup.sh      → Nettoyage snapshots >7j
└── dashboards/
    ├── eye.html            → Dashboard full (stream MJPEG)
    ├── eye_telegram.html   → Mini App Telegram (mobile)
    └── ai_dashboard.html   → Dashboard IA (stats, events, heatmap)
```

## 🚀 Quick Start

```bash
# 1. Installer les dépendances
uv pip install ultralytics torch torchvision Pillow numpy flask

# 2. Ollama (pour LLaVA)
ollama pull llava-llama3:8b

# 3. Lancer le proxy
python3 eye_proxy.py

# 4. Activer l'IA
python3 ai_eye.py --on

# 5. Analyse unique
python3 ai_eye.py

# 6. Status
python3 ai_eye.py --status
```

## 🔧 Commandes

| Commande | Action |
|----------|--------|
| `ai_eye.py --on` | Activer l'IA |
| `ai_eye.py --off` | Suspendre l'IA |
| `ai_eye.py --status` | État GPU, événements, snapshots |
| `ai_eye.py --cleanup 7` | Nettoyer les fichiers >7 jours |
| `camera_client.py capture` | Photo instantanée |
| `camera_client.py motion` | Détection de mouvement |
| `camera_client.py health` | Vérifier si la caméra est en ligne |

## 🌐 Endpoints (proxy Flask :8084)

| URL | Description |
|-----|-------------|
| `/` | Mini App Telegram |
| `/dashboard` | Dashboard IA |
| `/api/capture` | Snapshot JPEG |
| `/api/info` | Infos caméra |
| `/api/led?state=toggle` | Contrôle LED |
| `/api/ai/status` | Statut IA (GPU, events) |
| `/api/ai/events` | Derniers événements |
| `/api/ai/counter` | Compteur passages |
| `/api/ai/report` | Rapport 24h |
| `/api/ai/toggle` | ON/OFF IA |
| `/api/ai/analyze` | Forcer analyse |

## 📊 Crons recommandés

| Cron | Fréquence | Script |
|------|-----------|--------|
| AI Eye | 5 min | `ai_smart_cron.sh` |
| Health check | 5 min | `camera_health.sh` |
| Nettoyage | 3h du matin | `ai_cleanup.sh` |
| Rapport quotidien | 20h | prompt LLM → `/api/ai/report` |
