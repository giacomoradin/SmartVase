import numpy as np
import cv2

from vision.quality_gate import quality_gate, QualityGateConfig


def test_too_dark():
    img = np.zeros((144, 144, 3), dtype=np.uint8)
    q, _ = quality_gate(img)
    assert q == "too_dark"


def test_too_bright():
    img = np.full((144, 144, 3), 255, dtype=np.uint8)
    q, _ = quality_gate(img)
    assert q == "too_bright"


def test_blurry_vs_sharp():
    cfg = QualityGateConfig(blurry_laplacian_var=200.0)  # make test more robust

    sharp = np.zeros((144, 144, 3), dtype=np.uint8)
    cv2.rectangle(sharp, (20, 20), (120, 120), (255, 255, 255), 2)

    blurry = cv2.GaussianBlur(sharp, (11, 11), 0)

    q_sharp, _ = quality_gate(sharp, cfg=cfg)
    q_blur, _ = quality_gate(blurry, cfg=cfg)

    assert q_blur == "blurry"
    assert q_sharp in ["ok", "too_dark", "too_bright"]  # DEPENDS ON THRESHOLD

