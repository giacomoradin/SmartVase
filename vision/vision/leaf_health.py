"""Classificatore leaf-health basato sulle metriche cromatiche.

Modello a regole, **non un classifier ML**: voluto cosi' per la v0.x perche'
abbiamo zero immagini etichettate. Quando avremo un dataset, sostituire
`classify_leaf_health` con un classificatore vero (es. piccolo CNN o
gradient boosting su feature) mantenendo invariata l'interfaccia output.

Enum di output coerenti con `SmartVase_data_structure.md`:
- leaf_health: healthy | warning | critical | unknown
- symptoms:    yellowing | spots | wilting   (sottoinsieme indicativo)
"""
from __future__ import annotations

from dataclasses import dataclass
from typing import Dict, List, Tuple


@dataclass(frozen=True)
class LeafHealthConfig:
    # Soglie sui ratio (sui pixel pianta)
    yellowing_warning:  float = 0.20   # >= warning
    yellowing_critical: float = 0.45   # >= critical
    brown_warning:      float = 0.08
    brown_critical:     float = 0.25
    # Confidenze (rule-based: euristica fissa)
    yellow_confidence:  float = 0.7
    brown_confidence:   float = 0.7
    healthy_confidence: float = 0.6
    # Area pianta minima per considerare la classificazione affidabile.
    min_plant_area_ratio: float = 0.05


LeafHealth = str        # "healthy" | "warning" | "critical" | "unknown"
Symptom    = str        # "yellowing" | "spots" | "wilting"


def classify_leaf_health(
    metrics: Dict[str, float],
    cfg: LeafHealthConfig = LeafHealthConfig(),
) -> Tuple[LeafHealth, List[Symptom], Dict[str, float]]:
    """Classifica leaf_health a partire dalle metriche.

    Args:
        metrics: dict prodotto da `metrics.compute_metrics`.
        cfg: soglie configurabili.

    Returns:
        (leaf_health, symptoms, confidences)
        - leaf_health: enum stringa
        - symptoms:    lista di sintomi rilevati (ordinata, deduplicata)
        - confidences: dict {leaf_health, yellowing, brown_spots, wilting}
          con valori in [0, 1].
    """
    yellow_ratio = float(metrics.get("yellow_ratio", 0.0))
    brown_ratio  = float(metrics.get("brown_ratio",  0.0))
    plant_area   = float(metrics.get("plant_area_ratio", 0.0))

    symptoms: List[Symptom] = []
    confidences: Dict[str, float] = {}

    # Pianta non rilevata in modo affidabile: unknown.
    if plant_area < cfg.min_plant_area_ratio:
        return "unknown", [], {"leaf_health": 0.0}

    # --- sintomi ---
    if yellow_ratio >= cfg.yellowing_warning:
        symptoms.append("yellowing")
        confidences["yellowing"] = round(
            min(1.0, yellow_ratio / max(cfg.yellowing_critical, 1e-6)) * cfg.yellow_confidence,
            3,
        )
    if brown_ratio >= cfg.brown_warning:
        # "spots" copre necrosi/macchie marroni; in v0.x non distinguiamo
        # bordo vs spot.
        symptoms.append("spots")
        confidences["spots"] = round(
            min(1.0, brown_ratio / max(cfg.brown_critical, 1e-6)) * cfg.brown_confidence,
            3,
        )
    # "wilting" non e' rilevabile da color metrics. Lasciato per estensione
    # futura (es. analisi forma/area pianta nel tempo).

    # --- health complessivo ---
    is_critical = (yellow_ratio >= cfg.yellowing_critical) or (brown_ratio >= cfg.brown_critical)
    is_warning  = (yellow_ratio >= cfg.yellowing_warning)  or (brown_ratio >= cfg.brown_warning)

    if is_critical:
        leaf_health = "critical"
        confidences["leaf_health"] = round(
            max(confidences.get("yellowing", 0.0), confidences.get("spots", 0.0)),
            3,
        ) or cfg.yellow_confidence
    elif is_warning:
        leaf_health = "warning"
        confidences["leaf_health"] = round(
            max(confidences.get("yellowing", 0.0), confidences.get("spots", 0.0)),
            3,
        ) or cfg.yellow_confidence
    else:
        leaf_health = "healthy"
        confidences["leaf_health"] = cfg.healthy_confidence

    # Dedup mantenendo ordine
    seen = set()
    symptoms = [s for s in symptoms if not (s in seen or seen.add(s))]

    return leaf_health, symptoms, confidences
