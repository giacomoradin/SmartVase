from __future__ import annotations

from dataclasses import dataclass
from typing import Dict, Tuple

import cv2
import numpy as np


FrameQuality = str  # "ok" | "too_dark" | "too_bright" | "blurry"


@dataclass(frozen=True)
class QualityGateConfig:
    # These are initial defaults. You will calibrate them on real images later.
    too_dark_mean_gray: float = 40.0
    too_bright_mean_gray: float = 220.0
    blurry_laplacian_var: float = 60.0


def quality_gate(
    img_bgr: np.ndarray,
    cfg: QualityGateConfig = QualityGateConfig(),
) -> Tuple[FrameQuality, Dict[str, float]]:
    """
    Input:
      img_bgr: OpenCV image in BGR format (uint8)

    Output:
      frame_quality: "ok" | "too_dark" | "too_bright" | "blurry"
      metrics: numeric diagnostics for observability and later calibration
    """
    if img_bgr is None or img_bgr.size == 0:
        return "too_dark", {"brightness_mean": 0.0, "sharpness_lap_var": 0.0}

    gray = cv2.cvtColor(img_bgr, cv2.COLOR_BGR2GRAY)
    brightness_mean = float(np.mean(gray))

    lap = cv2.Laplacian(gray, cv2.CV_64F)
    sharpness_lap_var = float(np.var(lap))

    if brightness_mean < cfg.too_dark_mean_gray:
        frame_quality = "too_dark"
    elif brightness_mean > cfg.too_bright_mean_gray:
        frame_quality = "too_bright"
    elif sharpness_lap_var < cfg.blurry_laplacian_var:
        frame_quality = "blurry"
    else:
        frame_quality = "ok"

    metrics = {
        "brightness_mean": brightness_mean,
        "sharpness_lap_var": sharpness_lap_var,
    }
    return frame_quality, metrics

