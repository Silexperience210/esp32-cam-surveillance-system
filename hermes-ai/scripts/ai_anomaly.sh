#!/bin/bash
# Vérifie les anomalies toutes les heures et alerte si nécessaire
SCRIPT="$HOME/.hermes/scripts/ai_anomaly.py"
if [ -f "$SCRIPT" ]; then
    OUTPUT=$($HOME/ai-venv/bin/python3 "$SCRIPT" 2>/dev/null)
    if echo "$OUTPUT" | grep -q "🚨"; then
        echo "🔮 $OUTPUT"
    fi
fi
