"""Pipeline end-to-end SmartVase Vision.

Input:  immagine BGR (numpy array) + URL/risoluzione (metadati).
Output: dict JSON-serializzabile conforme al topic `smartvase/{id}/vision/result`
        come specificato in `SmartVase_data_structure.md` (sezione 5).

L'idea operativa:
  1. Cloud Function riceve l'immagine via topic `vision/image`.
  2. Carica l'immagine da `image_url`.
  3. Chiama `analyze_image(...)` e ottiene un AnalysisResult.
  4. Pubblica il `.to_json()` su `vision/result` (via Firestore).

Versioning:
- SCHEMA_VERSION: bump quando cambiano fields/semantica del JSON di output.
- MODEL_VERSION:  bump quando cambiano soglie/algoritmo di classificazione.
"""
from __future__ import annotations

import time
from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional

import numpy as np

from .quality_gate import quality_gate, QualityGateConfig
from .metrics      import compute_metrics, MetricsConfig
from .leaf_health  import classify_leaf_health, LeafHealthConfig

SCHEMA_VERSION = 1
MODEL_VERSION  = "vision-0.2.0"  # rule-based v0.2 (post-refactor 2026-05-19)


@dataclass
class AnalysisResult:
    timestamp_utc: int
    schema_version: int
    model_version: str
    image_url: str
    resolution: str
    frame_quality: str                    # ok | too_dark | too_bright | blurry | occluded | unknown
    leaf_health: str                      # healthy | warning | critical | unknown
    symptoms: List[str]                   = field(default_factory=list)
    confidence: Dict[str, float]          = field(default_factory=dict)
    metrics: Dict[str, Any]               = field(default_factory=dict)
    plant_bbox: Optional[Dict[str, int]]  = None
    recommendations: Dict[str, Any]       = field(default_factory=dict)
    error: Optional[Dict[str, str]]       = None

    def to_json(self) -> Dict[str, Any]:
        # Output con chiavi opzionali omesse se vuote (per fedelta' alla spec).
        out: Dict[str, Any] = {
            "timestamp_utc":  self.timestamp_utc,
            "schema_version": self.schema_version,
            "model_version":  self.model_version,
            "image_url":      self.image_url,
            "resolution":     self.resolution,
            "frame_quality":  self.frame_quality,
            "leaf_health":    self.leaf_health,
        }
        if self.symptoms:       out["symptoms"]       = self.symptoms
        if self.confidence:     out["confidence"]     = self.confidence
        if self.metrics:        out["metrics"]        = self.metrics
        if self.plant_bbox:     out["plant_bbox"]     = self.plant_bbox
        if self.recommendations:out["recommendations"]= self.recommendations
        out["error"] = self.error  # sempre presente: null o object (spec)
        return out


def _build_recommendations(
    leaf_health: str,
    metrics: Dict[str, Any],
    frame_quality: str,
) -> Dict[str, Any]:
    """Best-effort: produce alert per l'irrigazione/luce a partire dal contesto.

    Regole semplici (estendere con sensor fusion lato Hub):
      - yellowing alto + leaf_health warning/critical → alert_water=more
      - brown_ratio alto (necrosi da troppa luce / siccita') → alert_light=less
      - frame too_dark frequente sarebbe alert_light=more, ma serve persistenza
        temporale: lo lasciamo a una logica lato Hub.
    """
    rec: Dict[str, Any] = {}
    yellow = float(metrics.get("yellow_ratio", 0.0))
    brown  = float(metrics.get("brown_ratio",  0.0))

    if leaf_health in ("warning", "critical") and yellow >= 0.20:
        rec["alert_water"] = "more"
    if brown >= 0.20:
        rec["alert_light"] = "less"

    if rec:
        # Confidenza compatta del consiglio (media delle conf disponibili).
        rec["confidence"] = round(min(1.0, max(yellow, brown)), 3)

    return rec


def analyze_image(
    img_bgr: np.ndarray,
    image_url: str = "",
    resolution: Optional[str] = None,
    quality_cfg: QualityGateConfig = QualityGateConfig(),
    metrics_cfg: MetricsConfig    = MetricsConfig(),
    health_cfg:  LeafHealthConfig = LeafHealthConfig(),
    timestamp_utc: Optional[int]  = None,
) -> AnalysisResult:
    """Esegue il pipeline su una singola immagine.

    Se la qualita' del frame non e' 'ok', leaf_health viene marcato 'unknown'
    ed evitiamo di consumare cicli inutili sulla classificazione: usiamo
    comunque le metriche di base (per debug/calibrazione).
    """
    ts = timestamp_utc if timestamp_utc is not None else int(time.time())

    # Risoluzione: deducibile dall'immagine se non passata esplicitamente.
    if resolution is None and img_bgr is not None and img_bgr.size > 0:
        h, w = img_bgr.shape[:2]
        resolution = f"{w}x{h}"
    if resolution is None:
        resolution = "0x0"

    # 1) Quality gate
    try:
        frame_quality, q_metrics = quality_gate(img_bgr, cfg=quality_cfg)
    except Exception as e:  # pragma: no cover - defensive
        return AnalysisResult(
            timestamp_utc=ts, schema_version=SCHEMA_VERSION, model_version=MODEL_VERSION,
            image_url=image_url, resolution=resolution,
            frame_quality="unknown", leaf_health="unknown",
            error={"type": "quality_gate_failure", "detail": str(e)},
        )

    # 2) Metriche
    try:
        metrics, bbox = compute_metrics(img_bgr, cfg=metrics_cfg)
    except Exception as e:  # pragma: no cover
        return AnalysisResult(
            timestamp_utc=ts, schema_version=SCHEMA_VERSION, model_version=MODEL_VERSION,
            image_url=image_url, resolution=resolution,
            frame_quality=frame_quality, leaf_health="unknown",
            error={"type": "metrics_failure", "detail": str(e)},
        )

    # Mescola le diagnostic metrics del quality gate (utili per calibrazione).
    metrics_full = {**metrics, **{f"qg_{k}": v for k, v in q_metrics.items()}}

    # 3) Classifica solo se il frame e' usabile.
    if frame_quality != "ok":
        return AnalysisResult(
            timestamp_utc=ts, schema_version=SCHEMA_VERSION, model_version=MODEL_VERSION,
            image_url=image_url, resolution=resolution,
            frame_quality=frame_quality, leaf_health="unknown",
            metrics=metrics_full, plant_bbox=bbox,
        )

    try:
        leaf_health, symptoms, confidences = classify_leaf_health(metrics_full, cfg=health_cfg)
    except Exception as e:  # pragma: no cover
        return AnalysisResult(
            timestamp_utc=ts, schema_version=SCHEMA_VERSION, model_version=MODEL_VERSION,
            image_url=image_url, resolution=resolution,
            frame_quality=frame_quality, leaf_health="unknown",
            metrics=metrics_full, plant_bbox=bbox,
            error={"type": "classifier_failure", "detail": str(e)},
        )

    return AnalysisResult(
        timestamp_utc=ts,
        schema_version=SCHEMA_VERSION,
        model_version=MODEL_VERSION,
        image_url=image_url,
        resolution=resolution,
        frame_quality=frame_quality,
        leaf_health=leaf_health,
        symptoms=symptoms,
        confidence=confidences,
        metrics=metrics_full,
        plant_bbox=bbox,
        recommendations=_build_recommendations(leaf_health, metrics_full, frame_quality),
    )
