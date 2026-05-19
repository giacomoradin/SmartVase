"""Estrazione di metriche di colore e forma per le foglie SmartVase.

Le metriche sono calcolate **solo** sui pixel "pianta" (verde + giallo +
marrone). Lo sfondo viene segmentato in HSV per evitare di contaminare gli
indici cromatici.

Output coerente con il payload `vision/result` definito in
`SmartVase_data_structure.md` (sezione 5):
- color_dominant_hex
- green_index
- yellow_ratio
- brown_ratio
- sharpness
- plant_area_ratio
- plant_bbox

Le soglie HSV iniziali sono calibrate su foglie generiche; vanno
ritarate quando avremo immagini reali del prototipo.
"""
from __future__ import annotations

from dataclasses import dataclass
from typing import Dict, Optional, Tuple

import cv2
import numpy as np


# Range HSV (OpenCV: H 0..179, S 0..255, V 0..255)
@dataclass(frozen=True)
class MetricsConfig:
    # Verde (foglia sana)
    green_hsv_low:  Tuple[int, int, int] = (35,  40,  40)
    green_hsv_high: Tuple[int, int, int] = (85, 255, 255)
    # Giallo (clorosi)
    yellow_hsv_low:  Tuple[int, int, int] = (20,  60,  80)
    yellow_hsv_high: Tuple[int, int, int] = (34, 255, 255)
    # Marrone (necrosi/secco)
    brown_hsv_low:  Tuple[int, int, int] = (5,  40,  20)
    brown_hsv_high: Tuple[int, int, int] = (19, 255, 200)
    # Pulizia mask: kernel morfologico
    morph_kernel:    int = 3


def _color_mask(hsv: np.ndarray, lo, hi) -> np.ndarray:
    return cv2.inRange(hsv, np.array(lo, dtype=np.uint8),
                            np.array(hi, dtype=np.uint8))


def _bbox_of_mask(mask: np.ndarray) -> Optional[Dict[str, int]]:
    """Bounding box (x,y,w,h) del piu' grande componente connesso."""
    nlabels, _, stats, _ = cv2.connectedComponentsWithStats(mask, connectivity=8)
    if nlabels <= 1:
        return None
    # Salta l'etichetta 0 (sfondo)
    areas = stats[1:, cv2.CC_STAT_AREA]
    idx = int(np.argmax(areas)) + 1
    x = int(stats[idx, cv2.CC_STAT_LEFT])
    y = int(stats[idx, cv2.CC_STAT_TOP])
    w = int(stats[idx, cv2.CC_STAT_WIDTH])
    h = int(stats[idx, cv2.CC_STAT_HEIGHT])
    if w * h == 0:
        return None
    return {"x": x, "y": y, "w": w, "h": h}


def _dominant_hex(img_bgr: np.ndarray, plant_mask: np.ndarray) -> str:
    """Colore medio dei pixel pianta convertito in #RRGGBB."""
    if cv2.countNonZero(plant_mask) == 0:
        return "#000000"
    mean = cv2.mean(img_bgr, mask=plant_mask)
    b, g, r = int(mean[0]), int(mean[1]), int(mean[2])
    return f"#{r:02x}{g:02x}{b:02x}"


def compute_metrics(
    img_bgr: np.ndarray,
    cfg: MetricsConfig = MetricsConfig(),
) -> Tuple[Dict[str, float], Optional[Dict[str, int]]]:
    """Calcola le metriche cromatiche/spaziali su un'immagine BGR.

    Ritorna:
      metrics: dizionario serializzabile (color_dominant_hex, green_index,
               yellow_ratio, brown_ratio, sharpness, plant_area_ratio).
      plant_bbox: bounding box del componente pianta piu' grande, oppure None.
    """
    if img_bgr is None or img_bgr.size == 0:
        return ({
            "color_dominant_hex": "#000000",
            "green_index": 0.0,
            "yellow_ratio": 0.0,
            "brown_ratio": 0.0,
            "sharpness": 0.0,
            "plant_area_ratio": 0.0,
        }, None)

    h, w = img_bgr.shape[:2]
    total = float(h * w) if h * w > 0 else 1.0

    hsv = cv2.cvtColor(img_bgr, cv2.COLOR_BGR2HSV)

    green_mask  = _color_mask(hsv, cfg.green_hsv_low,  cfg.green_hsv_high)
    yellow_mask = _color_mask(hsv, cfg.yellow_hsv_low, cfg.yellow_hsv_high)
    brown_mask  = _color_mask(hsv, cfg.brown_hsv_low,  cfg.brown_hsv_high)
    plant_mask  = cv2.bitwise_or(cv2.bitwise_or(green_mask, yellow_mask), brown_mask)

    # Pulizia morfologica
    k = cfg.morph_kernel
    if k > 0:
        kernel = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (k, k))
        plant_mask = cv2.morphologyEx(plant_mask, cv2.MORPH_OPEN,  kernel)
        plant_mask = cv2.morphologyEx(plant_mask, cv2.MORPH_CLOSE, kernel)

    green_px  = float(cv2.countNonZero(green_mask))
    yellow_px = float(cv2.countNonZero(yellow_mask))
    brown_px  = float(cv2.countNonZero(brown_mask))
    plant_px  = float(cv2.countNonZero(plant_mask))

    # Sharpness = varianza del laplaciano sull'immagine grigia complessiva.
    gray = cv2.cvtColor(img_bgr, cv2.COLOR_BGR2GRAY)
    sharpness = float(np.var(cv2.Laplacian(gray, cv2.CV_64F)))

    metrics: Dict[str, float] = {
        "color_dominant_hex": _dominant_hex(img_bgr, plant_mask),
        "green_index":     round(green_px / plant_px, 4) if plant_px > 0 else 0.0,
        "yellow_ratio":    round(yellow_px / plant_px, 4) if plant_px > 0 else 0.0,
        "brown_ratio":     round(brown_px / plant_px, 4) if plant_px > 0 else 0.0,
        "sharpness":       round(sharpness, 2),
        "plant_area_ratio": round(plant_px / total, 4),
    }

    bbox = _bbox_of_mask(plant_mask)
    return metrics, bbox
