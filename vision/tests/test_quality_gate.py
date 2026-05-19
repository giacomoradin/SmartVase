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
    # Soglia blur abbassata per renderlo deterministico; luminosita' base
    # alta a sufficienza da non finire mai in too_dark.
    cfg = QualityGateConfig(
        too_dark_mean_gray=30.0,
        too_bright_mean_gray=240.0,
        blurry_laplacian_var=200.0,
    )

    # Immagine a luminosita' media controllata (sfondo grigio 128).
    sharp = np.full((144, 144, 3), 128, dtype=np.uint8)
    # Edge ad alto contrasto -> sharpness alta
    cv2.rectangle(sharp, (20, 20), (120, 120), (255, 255, 255), 2)
    cv2.rectangle(sharp, (40, 40), (100, 100), (0, 0, 0), 2)

    blurry = cv2.GaussianBlur(sharp, (15, 15), 0)

    q_sharp, m_sharp = quality_gate(sharp, cfg=cfg)
    q_blur,  m_blur  = quality_gate(blurry, cfg=cfg)

    # Lo sharp deve passare il gate; il blurry deve essere flaggato blurry.
    assert q_blur == "blurry", (q_blur, m_blur)
    assert q_sharp == "ok", (q_sharp, m_sharp)
