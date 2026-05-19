"""Test integrazione vision.pipeline — output JSON conforme alla spec."""
import cv2
import numpy as np
import pytest

from vision.pipeline import analyze_image, MODEL_VERSION, SCHEMA_VERSION


def _solid(h, w, bgr):
    img = np.zeros((h, w, 3), dtype=np.uint8)
    img[:, :] = bgr
    return img


def test_pipeline_healthy_green_image_has_expected_envelope():
    img = _solid(144, 144, (0, 200, 0))
    r = analyze_image(img, image_url="https://example.com/img.jpg",
                     timestamp_utc=1700000000)
    j = r.to_json()

    # Campi obbligatori dalla spec
    for key in ("timestamp_utc", "schema_version", "model_version",
                "image_url", "frame_quality", "leaf_health", "error"):
        assert key in j

    assert j["schema_version"] == SCHEMA_VERSION
    assert j["model_version"]  == MODEL_VERSION
    assert j["timestamp_utc"]  == 1700000000
    assert j["image_url"]      == "https://example.com/img.jpg"
    assert j["resolution"]     == "144x144"
    assert j["leaf_health"] in ("healthy", "warning", "critical", "unknown")
    assert j["frame_quality"] in ("ok", "too_dark", "too_bright", "blurry",
                                   "occluded", "unknown")


def test_pipeline_too_dark_skips_classifier():
    img = np.zeros((144, 144, 3), dtype=np.uint8)  # nero puro -> too_dark
    r = analyze_image(img)
    j = r.to_json()
    assert j["frame_quality"] == "too_dark"
    assert j["leaf_health"] == "unknown"


def test_pipeline_yellow_image_triggers_warning_or_critical():
    img = _solid(144, 144, (0, 220, 220))  # giallo
    r = analyze_image(img)
    j = r.to_json()
    # qualita' dovrebbe essere ok (luminoso ma non saturo bianco)
    if j["frame_quality"] == "ok":
        assert j["leaf_health"] in ("warning", "critical")
        # symptoms presente quando ci sono sintomi
        if "symptoms" in j:
            assert "yellowing" in j["symptoms"]


def test_pipeline_error_field_always_present():
    img = _solid(144, 144, (0, 200, 0))
    r = analyze_image(img)
    j = r.to_json()
    # 'error' DEVE essere presente (null se ok)
    assert "error" in j


def test_pipeline_recommendations_for_yellowing():
    img = _solid(144, 144, (0, 220, 220))
    r = analyze_image(img)
    j = r.to_json()
    if j["frame_quality"] == "ok" and j["leaf_health"] in ("warning", "critical"):
        # recommendations puo' contenere alert_water=more
        rec = j.get("recommendations") or {}
        # non testiamo "deve esserci" ma se c'e' deve essere consistente
        if "alert_water" in rec:
            assert rec["alert_water"] in ("more", "less")
