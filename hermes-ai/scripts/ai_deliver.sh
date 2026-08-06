#!/bin/bash
# Vérifie si le daemon a écrit une alerte, et la délivre
SIGNAL=~/.hermes/camera/last_alert.txt
SENT=~/.hermes/camera/last_alert_sent.txt
if [ -f "$SIGNAL" ]; then
    CURRENT=$(cat "$SIGNAL")
    LAST=$(cat "$SENT" 2>/dev/null)
    if [ "$CURRENT" != "$LAST" ]; then
        echo "$CURRENT"
        cp "$SIGNAL" "$SENT"
    fi
fi
