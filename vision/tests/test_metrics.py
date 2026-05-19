"""Test per vision.metrics — ratio cromatici e bbox."""
import cv2
import numpy as np
import pytest

from vision.metrics import MetricsConfig, compute_metrics


def _solid(h, w, bgr):
    img = np.zeros((h, w, 3), dtype=np.uint8)
    img[:, :] = bgr
    return img


def test_metrics_full_green_image():
    # Verde puro (BGR=(0,200,0)) in HSV cade nel range green default.
    img = _solid(120, 120, (0, 200, 0))
    metrics, bbox = compute_metrics(img)
    assert metrics["green_index"] >= 0.95
    assert metrics["yellow_ratio"] <= 0.05
    assert metrics["brown_ratio"] <= 0.05
    assert metrics["plant_area_ratio"] >= 0.95
    assert bbox is not None
    assert bbox["w"] > 0 and bbox["h"] > 0


def test_metrics_yellow_dominant():
    img = _solid(120, 120, (0, 220, 220))  # giallo BGR ≈ (B=0,G=220,R=220)
    metrics, _ = compute_metrics(img)
    assert metrics["yellow_ratio"] >= 0.5


def test_metrics_empty_image():
    img = np.zeros((0, 0, 3), dtype=np.uint8)
    metrics, bbox = compute_metrics(img)
    assert metrics["plant_area_ratio"] == 0.0
    assert bbox is None


def test_dominant_hex_format():
    img = _solid(64, 64, (0, 200, 0))
    metrics, _ = compute_metrics(img)
    h = metrics["color_dominant_hex"]
    assert isinstance(h, str)
    assert h.startswith("#") and len(h) == 7
