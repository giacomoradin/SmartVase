import pytest
import os
from pixel_analyzer import PixelAnalyzer

def test_analyzer_initialization():
    analyzer = PixelAnalyzer(green_threshold=15, brown_threshold_r=90)
    assert analyzer.green_threshold == 15
    assert analyzer.brown_threshold_r == 90
    assert analyzer.total_pixels == 19200

def test_missing_image_exception():
    analyzer = PixelAnalyzer()
    with pytest.raises(FileNotFoundError):
        analyzer.process_image("invalid_path.jpg")