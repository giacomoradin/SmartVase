import cv2
import numpy as np

class PixelAnalyzer:
    def __init__(self, green_threshold=20, brown_threshold_r=100):
        self.width = 160
        self.height = 120
        self.total_pixels = self.width * self.height
        self.green_threshold = green_threshold
        self.brown_threshold_r = brown_threshold_r

    def process_image(self, image_path):
        # Load image
        img = cv2.imread(image_path)
        if img is None:
            raise FileNotFoundError(f"Could not open or find the image: {image_path}")

        # Resize to QQVGA (160x120) to match ESP32-CAM internal buffer
        img_resized = cv2.resize(img, (self.width, self.height))
        
        # Convert BGR (OpenCV default) to RGB
        img_rgb = cv2.cvtColor(img_resized, cv2.COLOR_BGR2RGB)
        
        green_count = 0
        brown_count = 0

        # Simulate ESP32-CAM pixel-by-pixel processing loop
        for y in range(self.height):
            for x in range(self.width):
                r, g, b = img_rgb[y, x]
                
                # Downsample to 16-bit RGB565 (5 bits Red, 6 bits Green, 5 bits Blue)
                r_5bit = (r >> 3) & 0x1F
                g_6bit = (g >> 2) & 0x3F
                b_5bit = (b >> 3) & 0x1F
                
                # Upscale back to 8-bit range (0-255) for threshold logic verification
                r8 = int((r_5bit * 255) / 31)
                g8 = int((g_6bit * 255) / 63)
                b8 = int((b_5bit * 255) / 31)

                # Color Segmentation Logic
                # Healthy Leaf Condition (Dominant Green)
                if g8 > (r8 + self.green_threshold) and g8 > (b8 + self.green_threshold):
                    green_count += 1
                # Diseased/Dry Leaf Condition (High Red/Green, Low Blue)
                elif r8 > self.brown_threshold_r and g8 > 80 and b8 < 60:
                    brown_count += 1

        # Calculate metrics
        biomass_percentage = (green_count / self.total_pixels) * 100
        plant_pixels = green_count + brown_count
        disease_percentage = (brown_count / plant_pixels) * 100 if plant_pixels > 0 else 0.0

        return {
            "biomass_percentage": round(biomass_percentage, 2),
            "disease_percentage": round(disease_percentage, 2),
            "green_pixels": green_count,
            "brown_pixels": brown_count
        }

if __name__ == "__main__":
    import sys
    if len(sys.argv) < 2:
        print("Usage: python pixel_analyzer.py <path_to_image>")
        sys.exit(1)
        
    analyzer = PixelAnalyzer()
    try:
        results = analyzer.process_image(sys.argv[1])
        print(f"Analysis Results for {sys.argv[1]}:")
        print(f"  Biomass Index: {results['biomass_percentage']}%")
        print(f"  Disease Index: {results['disease_percentage']}%")
    except Exception as e:
        print(f"Error: {e}")