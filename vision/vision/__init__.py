"""SmartVase Vision package.

Pipeline di analisi immagini fogliari per il robot SmartVase.

Moduli principali:
- quality_gate: filtro di qualita' del frame (luminosita', sharpness)
- metrics: estrazione di metriche di colore/forma sulla regione pianta
- leaf_health: classificazione healthy/warning/critical + sintomi
- pipeline: orchestrazione end-to-end e serializzazione JSON conforme
            a SmartVase_data_structure.md (topic vision/result)
"""

from .quality_gate import quality_gate, QualityGateConfig
from .metrics import compute_metrics, MetricsConfig
from .leaf_health import classify_leaf_health, LeafHealthConfig
from .pipeline import analyze_image, AnalysisResult, MODEL_VERSION, SCHEMA_VERSION

__all__ = [
    "quality_gate",
    "QualityGateConfig",
    "compute_metrics",
    "MetricsConfig",
    "classify_leaf_health",
    "LeafHealthConfig",
    "analyze_image",
    "AnalysisResult",
    "MODEL_VERSION",
    "SCHEMA_VERSION",
]
