#!/usr/bin/env python3
"""
Hermes AI Eye — Détection d'anomalies
Analyse les patterns et détecte les activités inhabituelles.
À exécuter périodiquement (toutes les heures) ou à la demande.
"""
import json, os
from pathlib import Path
from datetime import datetime, timedelta
from collections import defaultdict
import statistics

HOME = Path.home()
BASE_DIR = HOME / ".hermes" / "camera"
AI_LOG = BASE_DIR / "ai_events.jsonl"
ANOMALY_LOG = BASE_DIR / "anomalies.jsonl"
HISTORY_DAYS = 14  # jours d'historique pour la baseline

def load_events(days=HISTORY_DAYS):
    """Charge les événements des N derniers jours."""
    cutoff = datetime.now() - timedelta(days=days)
    events = []
    if AI_LOG.exists():
        with open(AI_LOG) as f:
            for line in f:
                try:
                    e = json.loads(line)
                    ts = datetime.fromisoformat(e['timestamp'])
                    if ts >= cutoff:
                        e['_ts'] = ts
                        events.append(e)
                except: pass
    return events

def hourly_baseline(events):
    """Construit la baseline horaire : moyenne + écart-type par heure."""
    hours = defaultdict(list)
    for e in events:
        has_person = "person" in str(e.get("detections","")).lower()
        if has_person:
            hours[e['_ts'].hour].append(e['_ts'])
    
    baseline = {}
    for h in range(24):
        days_with_activity = defaultdict(int)
        for ts in hours[h]:
            days_with_activity[ts.strftime('%Y-%m-%d')] += 1
        counts = list(days_with_activity.values())
        baseline[h] = {
            "avg": statistics.mean(counts) if counts else 0,
            "std": statistics.stdev(counts) if len(counts) > 1 else 0,
            "max": max(counts) if counts else 0,
            "active_days": len(counts)
        }
    return baseline

def detect_anomalies():
    """Détecte les anomalies et retourne la liste."""
    events = load_events()
    if len(events) < 50:
        return [], {"message": "Pas assez de données (<50 événements)"}
    
    now = datetime.now()
    today = now.strftime('%Y-%m-%d')
    baseline = hourly_baseline(events)
    anomalies = []
    
    # 1. Activité nocturne (00h-05h)
    night_events = [e for e in events 
                   if e['_ts'].hour < 6 
                   and "person" in str(e.get("detections","")).lower()
                   and e['_ts'].strftime('%Y-%m-%d') == today]
    if len(night_events) > 0:
        anomalies.append({
            "type": "night_activity",
            "severity": "⚠️",
            "message": f"Activité nocturne: {len(night_events)} passage(s) entre 00h-05h",
            "count": len(night_events),
            "timestamp": now.isoformat()
        })
    
    # 2. Pic inhabituel cette heure (vs baseline)
    current_hour = now.hour
    hour_events_today = [e for e in events 
                        if e['_ts'].hour == current_hour 
                        and "person" in str(e.get("detections","")).lower()
                        and e['_ts'].strftime('%Y-%m-%d') == today]
    hour_count = len(hour_events_today)
    hbase = baseline.get(current_hour, {"avg": 0, "std": 0})
    
    if hbase['std'] > 0 and hour_count > hbase['avg'] + 2 * hbase['std']:
        anomalies.append({
            "type": "unusual_spike",
            "severity": "🔥",
            "message": f"Pic inhabituel à {current_hour}h: {hour_count} passages (moyenne: {hbase['avg']:.0f}±{hbase['std']:.0f})",
            "count": hour_count,
            "avg": round(hbase['avg'], 1),
            "std": round(hbase['std'], 1),
            "timestamp": now.isoformat()
        })
    
    # 3. Activité en baisse (chute > 50% vs moyenne mobile)
    daily_persons = defaultdict(int)
    for e in events:
        if "person" in str(e.get("detections","")).lower():
            daily_persons[e['_ts'].strftime('%Y-%m-%d')] += 1
    
    today_count = daily_persons.get(today, 0)
    past_days = {d: c for d, c in daily_persons.items() if d != today}
    if past_days and today_count > 0:
        past_avg = statistics.mean(past_days.values())
        if past_avg > 5 and today_count < past_avg * 0.3:
            anomalies.append({
                "type": "unusual_drop",
                "severity": "📉",
                "message": f"Baisse d'activité: {today_count} passages aujourd'hui (moyenne: {past_avg:.0f})",
                "count": today_count,
                "avg": round(past_avg, 1),
                "timestamp": now.isoformat()
            })
    
    # 4. 1er événement après une longue pause (>6h)
    person_events_today = sorted(
        [e for e in events 
         if "person" in str(e.get("detections","")).lower() 
         and e['_ts'].strftime('%Y-%m-%d') == today],
        key=lambda e: e['_ts']
    )
    if len(person_events_today) >= 2:
        max_gap = max(
            (person_events_today[i+1]['_ts'] - person_events_today[i]['_ts']).total_seconds() / 3600
            for i in range(len(person_events_today)-1)
        )
        if max_gap > 6:
            anomalies.append({
                "type": "long_gap",
                "severity": "⏸️",
                "message": f"Pause prolongée: {max_gap:.1f}h sans passage aujourd'hui",
                "gap_hours": round(max_gap, 1),
                "timestamp": now.isoformat()
            })
    
    # Log anomalies
    if anomalies:
        with open(ANOMALY_LOG, "a") as f:
            for a in anomalies:
                f.write(json.dumps(a, ensure_ascii=False) + "\n")
    
    return anomalies, {"baseline": baseline, "daily_persons": daily_persons}

if __name__ == "__main__":
    anomalies, info = detect_anomalies()
    if anomalies:
        print(f"🚨 {len(anomalies)} anomalie(s) détectée(s):")
        for a in anomalies:
            print(f"  {a['severity']} {a['message']}")
    else:
        pass  # Silent: nothing to report
