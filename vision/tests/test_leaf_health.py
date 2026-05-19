"""Test per vision.leaf_health — classificatore rule-based."""
from vision.leaf_health import LeafHealthConfig, classify_leaf_health


def _m(green=0.9, yellow=0.05, brown=0.0, plant=0.4):
    return {
        "green_index": green,
        "yellow_ratio": yellow,
        "brown_ratio": brown,
        "plant_area_ratio": plant,
    }


def test_healthy_when_mostly_green():
    h, sym, conf = classify_leaf_health(_m(green=0.95, yellow=0.02, brown=0.0))
    assert h == "healthy"
    assert sym == []
    assert 0.0 <= conf["leaf_health"] <= 1.0


def test_warning_when_yellowing_above_warning_threshold():
    h, sym, _ = classify_leaf_health(_m(green=0.6, yellow=0.25, brown=0.0))
    assert h == "warning"
    assert "yellowing" in sym


def test_critical_when_yellowing_above_critical_threshold():
    h, sym, _ = classify_leaf_health(_m(green=0.3, yellow=0.5, brown=0.0))
    assert h == "critical"
    assert "yellowing" in sym


def test_critical_when_brown_above_critical_threshold():
    h, sym, _ = classify_leaf_health(_m(green=0.4, yellow=0.05, brown=0.3))
    assert h == "critical"
    assert "spots" in sym


def test_unknown_when_no_plant_detected():
    h, sym, conf = classify_leaf_health(_m(plant=0.01))
    assert h == "unknown"
    assert sym == []
    assert conf["leaf_health"] == 0.0


def test_threshold_override_with_config():
    cfg = LeafHealthConfig(yellowing_warning=0.50, yellowing_critical=0.80)
    h, _, _ = classify_leaf_health(_m(green=0.7, yellow=0.30, brown=0.0), cfg=cfg)
    # 0.30 sotto la nuova soglia warning -> healthy
    assert h == "healthy"
