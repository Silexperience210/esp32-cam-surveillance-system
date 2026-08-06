#!/usr/bin/env python3
"""
Hermes AI Eye — Pipeline HARDCORE
Niveau 4 : YOLOv8n → DeepFace → MediaPipe Pose → OWL-ViT
"""
import time, os, sys, json, io, urllib.request
from pathlib import Path
from datetime import datetime
import cv2, numpy as np
from PIL import Image
import warnings
warnings.filterwarnings("ignore")

os.environ["TOKENIZERS_PARALLELISM"] = "false"
os.environ["DEEPFACE_HOME"] = str(Path.home() / ".deepface")

HOME = Path.home()
BASE_DIR = HOME / ".hermes" / "camera"
SNAPSHOT_DIR = BASE_DIR / "snapshots"
AI_LOG = BASE_DIR / "ai_events.jsonl"
if not SNAPSHOT_DIR.exists(): SNAPSHOT_DIR.mkdir(parents=True)

CAMERA_URL = os.environ.get("CAMERA_URL", "http://192.168.1.178")

# ═══ MODÈLES (lazy load) ═══
_yolo = None
_deepface_model = None
_mp_pose = None
_owl_model = _owl_processor = None

FR = {
    'person':'personne', 'dog':'chien', 'cat':'chat', 'bird':'oiseau',
    'car':'voiture', 'truck':'camion', 'motorcycle':'moto', 'bicycle':'vélo',
    'horse':'cheval', 'sheep':'mouton', 'cow':'vache',
    'backpack':'sac', 'suitcase':'valise', 'umbrella':'parapluie',
    'cell phone':'téléphone', 'laptop':'ordinateur', 'book':'livre',
    'bottle':'bouteille', 'cup':'tasse', 'chair':'chaise',
    'couch':'canapé', 'potted plant':'plante', 'bed':'lit',
    'dining table':'table', 'toilet':'toilettes', 'tv':'télé',
}

def get_yolo():
    global _yolo
    if _yolo is None:
        from ultralytics import YOLO
        _yolo = YOLO(BASE_DIR / "yolov8n.pt")
    return _yolo

def get_deepface():
    global _deepface_model
    if _deepface_model is None:
        from deepface import DeepFace
        _deepface_model = True  # DeepFace loads on demand
    return True

def get_pose():
    global _mp_pose
    if _mp_pose is None:
        import mediapipe as mp
        _mp_pose = mp.solutions.pose
    return _mp_pose

def get_owl():
    global _owl_model, _owl_processor
    if _owl_model is None:
        from transformers import OwlViTForObjectDetection, OwlViTProcessor
        _owl_processor = OwlViTProcessor.from_pretrained("google/owlvit-base-patch32")
        _owl_model = OwlViTForObjectDetection.from_pretrained("google/owlvit-base-patch32")
        if torch.cuda.is_available():
            _owl_model = _owl_model.to("cuda")
    return _owl_processor, _owl_model

# ═══ CAPTURE ═══
def capture():
    try:
        try: urllib.request.urlopen(f"{CAMERA_URL}/led?state=on", timeout=3)
        except: pass
        time.sleep(0.3)
        req = urllib.request.Request(f"{CAMERA_URL}/capture")
        req.add_header("X-Auth-Token", "hermes2024")
        data = urllib.request.urlopen(req, timeout=8).read()
        try: urllib.request.urlopen(f"{CAMERA_URL}/led?state=off", timeout=3)
        except: pass
        ts = datetime.now()
        path = SNAPSHOT_DIR / f"snap_{ts.strftime('%Y%m%d_%H%M%S')}.jpg"
        with open(path, "wb") as f: f.write(data)
        return str(path)
    except Exception as e:
        print(f"ERREUR capture: {e}")
        return None

# ═══ NIVEAU 1 : YOLO ═══
def yolo_detect(image_path):
    model = get_yolo()
    results = model(image_path, verbose=False)
    detections = []
    has_person = False
    for r in results:
        for box in r.boxes:
            cls = int(box.cls[0])
            label = model.names[cls]
            conf = float(box.conf[0])
            bbox = box.xyxy[0].tolist()
            detections.append({"label": label, "confidence": conf, "bbox": bbox})
            if label == "person" and conf > 0.5:
                has_person = True
    return detections, has_person

# ═══ NIVEAU 2 : DeepFace (identité) ═══
def identify_face(image_path):
    """Retourne le nom si connu, sinon 'inconnu'."""
    try:
        from deepface import DeepFace
        # Vérifier d'abord si un visage est présent
        img = cv2.imread(image_path)
        face_cascade = cv2.CascadeClassifier(cv2.data.haarcascades + 'haarcascade_frontalface_default.xml')
        gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
        faces = face_cascade.detectMultiScale(gray, 1.1, 4)
        if len(faces) == 0:
            return None  # pas de visage visible

        # Chercher dans la base locale
        db = HOME / ".deepface_db"
        if db.exists():
            dfs = DeepFace.find(img_path=image_path, db_path=str(db),
                              model_name="Facenet", silent=True, enforce_detection=False)
            if len(dfs) > 0 and len(dfs[0]) > 0:
                best = dfs[0].iloc[0]
                if best.get('Facenet_cosine', 1.0) < 0.4:
                    return os.path.basename(best['identity']).split('.')[0]
        return "inconnu"
    except Exception as e:
        return None  # erreur ou pas de visage

# ═══ NIVEAU 3 : MediaPipe Pose ═══
def detect_pose(image_path):
    """Retourne la posture: debout, assis, bras_levés, etc."""
    try:
        pose = get_pose()
        img = cv2.imread(image_path)
        img_rgb = cv2.cvtColor(img, cv2.COLOR_BGR2RGB)
        with pose.Pose(static_image_mode=True, min_detection_confidence=0.5) as detector:
            results = detector.process(img_rgb)
        if not results.pose_landmarks:
            return None

        lm = results.pose_landmarks.landmark
        # Analyse simple
        left_shoulder = lm[11].y
        left_hip = lm[23].y
        left_wrist = lm[15].y
        head = lm[0].y
        knee = lm[25].y if len(lm) > 25 else left_hip

        # Debout si les épaules sont au-dessus des hanches
        if left_shoulder < left_hip * 0.9:
            if left_wrist < head:  return "bras levés"
            if knee > left_hip * 1.2: return "debout"
            return "debout"
        elif left_hip - left_shoulder > 0.05:
            return "assis"
        return None
    except:
        return None

# ═══ NIVEAU 4 : OWL-ViT (attributs) ═══
def owl_describe(image_path):
    """Détection par description texte: couleurs, vêtements, actions."""
    try:
        import torch
        processor, model = get_owl()
        img = Image.open(image_path)

        queries = [
            "a person wearing a red shirt", "a person wearing a blue shirt",
            "a person wearing a white shirt", "a person wearing a black shirt",
            "a person wearing a yellow shirt", "a person wearing a jacket",
            "a person wearing a hat", "a person with glasses",
            "a person holding a phone", "a bald man",
            "a woman", "a man", "a child"
        ]
        inputs = processor(text=queries, images=img, return_tensors="pt")
        if torch.cuda.is_available():
            inputs = {k: v.to("cuda") for k, v in inputs.items()}

        with torch.no_grad():
            outputs = model(**inputs)

        target_sizes = torch.tensor([img.size[::-1]])
        results = processor.post_process_object_detection(
            outputs, threshold=0.3, target_sizes=target_sizes)

        attrs = []
        for box, label, score in zip(results[0]["boxes"], results[0]["labels"], results[0]["scores"]):
            attrs.append((queries[label], float(score)))

        attrs.sort(key=lambda x: -x[1])
        return [a[0] for a in attrs[:3]] if attrs else []
    except:
        return []

# ═══ ANALYSE COMPLÈTE ═══
def analyze(save=True):
    t0 = time.time()
    img = capture()
    if not img: return {"error": "capture failed"}

    # Niveau 1: YOLO
    detections, has_person = yolo_detect(img)
    t1 = time.time()

    identity = pose = "?"
    attrs = []

    if has_person:
        # Niveau 2: DeepFace
        identity = identify_face(img)
        # Niveau 3: Pose
        pose = detect_pose(img) or "debout"
        # Niveau 4: OWL-ViT
        attrs = owl_describe(img)

    t2 = time.time()

    # Build description
    counts = {}
    for d in detections: counts[d['label']] = counts.get(d['label'], 0) + 1

    parts = []
    # Personnes
    if 'person' in counts:
        npers = counts['person']
        part = f"{npers} personne{'s' if npers > 1 else ''}"
        if identity and identity != "inconnu":
            part = identity
        if pose and pose != "?":
            part += f" {pose}"
        if attrs:
            part += f" ({'; '.join(attrs[:2])})"
        parts.append(part)
    # Animaux
    for l in ['dog','cat','bird','horse','sheep','cow']:
        if l in counts:
            parts.append(f"{counts[l]} {FR.get(l,l)}")
    # Véhicules
    for l in ['car','truck','motorcycle','bicycle']:
        if l in counts:
            parts.append(f"{counts[l]} {FR.get(l,l)}")

    desc = ", ".join(parts) if parts else "RAS"
    alert = has_person

    # Log
    event = {
        "timestamp": datetime.now().isoformat(),
        "description": desc,
        "identity": identity,
        "pose": pose,
        "attrs": attrs,
        "detections": detections[:10],
        "alert": alert,
        "timing": {"yolo": round(t1-t0,2), "advanced": round(t2-t1,2), "total": round(t2-t0,2)},
        "image": img if save else None
    }
    if save:
        with open(AI_LOG, "a") as f:
            f.write(json.dumps(event, ensure_ascii=False) + "\n")

    return event
