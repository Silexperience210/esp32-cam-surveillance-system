#!/bin/bash
# AI Eye cron — YOLO direct, sans filtre motion
python3 ~/.hermes/scripts/ai_eye.py --yolo-only 2>&1
# Silencieux si RAS (--yolo-only ne print que si alerte ou personne)
