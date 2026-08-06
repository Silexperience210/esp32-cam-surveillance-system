#!/bin/bash
# AI Eye cron wrapper - silencieux si RAS, notifie uniquement sur alerte
RESULT=$(python3 ~/.hermes/scripts/ai_eye.py --loop 2>&1)
if echo "$RESULT" | grep -qiE "personne|vehicule|animal|alerte|mouvement|detecte"; then
    echo "🚨 $RESULT"
elif echo "$RESULT" | grep -qi "erreur"; then
    echo "⚠️ $RESULT"
fi
# Si RAS → pas de sortie → pas de message Telegram
