#!/bin/bash
# Health check - silencieux si ONLINE, alerte si OFFLINE
RESULT=$(python3 ~/.hermes/scripts/camera_client.py health 2>&1)
if echo "$RESULT" | grep -qi "OFFLINE"; then
    echo "🔴 ALERTE: Caméra ESP32-CAM OFFLINE!"
    echo "$RESULT"
fi
# Si ONLINE -> pas de sortie -> pas de message
