import pytest
from pixel_analyzer import PixelAnalyzer

def test_analyzer_default_initialization():
    #verify that default values are the calibrated ones
    analyzer = PixelAnalyzer()
    assert analyzer.green_threshold == 20
    assert analyzer.brown_threshold_r == 90
    assert analyzer.total_pixels == 19200

def test_analyzer_custom_initialization():
    #verify that custom parameters are correctly assigned
    analyzer = PixelAnalyzer(green_threshold=20, brown_threshold_r=90)
    assert analyzer.green_threshold == 20
    assert analyzer.brown_threshold_r == 90

def test_missing_image_exception():
    analyzer = PixelAnalyzer()
    with pytest.raises(FileNotFoundError):
        analyzer.process_image("invalid_path.jpg")