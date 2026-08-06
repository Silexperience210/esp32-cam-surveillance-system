#!/bin/bash
# Détection de mouvement - silencieux si pas de mouvement
RESULT=$(python3 ~/.hermes/scripts/camera_client.py motion 2>/dev/null)
MOTION=$(echo "$RESULT" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('motion',False))" 2>/dev/null)
if [ "$MOTION" = "True" ]; then
    echo "$RESULT" | python3 -c "import sys,json; d=json.load(sys.stdin); print(f'MOUVEMENT détecté: {d[\"diff_pct\"]}%')" 2>/dev/null
fi
