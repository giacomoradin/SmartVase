import cv2
import numpy as np

class PixelAnalyzer:
    def __init__(self, green_threshold=20, brown_threshold_r=90, disease_tolerance_pct=15.0):
        self.green_threshold = green_threshold
        self.brown_threshold_r = brown_threshold_r
        self.disease_tolerance_pct = disease_tolerance_pct

    def process_image(self, image_path):
        img = cv2.imread(image_path)
        if img is None:
            raise FileNotFoundError(f"Image not found: {image_path}")

        # calculate total pixels dynamically based on the original image
        height, width = img.shape[:2]
        self.total_pixels = width * height
        
        # convert to RGB and cast to int32 to avoid overflow during bitwise ops
        img_rgb = cv2.cvtColor(img, cv2.COLOR_BGR2RGB).astype(np.int32)
        
        # split channels for easier manipulation
        r = img_rgb[:, :, 0]
        g = img_rgb[:, :, 1]
        b = img_rgb[:, :, 2]

        # simulate ESP32-CAM RGB565 downsampling
        r_5bit = (r >> 3) & 0x1F
        g_6bit = (g >> 2) & 0x3F
        b_5bit = (b >> 3) & 0x1F

        # scale back to 8-bit using integer division to match C++ behavior
        r8 = (r_5bit * 255) // 31
        g8 = (g_6bit * 255) // 63
        b8 = (b_5bit * 255) // 31

        # vectorize the segmentation logic with boolean masks instead of loops
        healthy_mask = (g8 > (r8 + self.green_threshold)) & (g8 > (b8 + self.green_threshold))
        diseased_mask = (r8 > self.brown_threshold_r) & (r8 > (b8 + 15)) & (r8 >= (g8 - 10))

        # count matching pixels directly from the masks
        green_count = int(np.sum(healthy_mask))
        brown_count = int(np.sum(diseased_mask))

        # calculate final percentages
        biomass_percentage = (green_count / self.total_pixels) * 100
        plant_pixels = green_count + brown_count
        disease_percentage = (brown_count / plant_pixels) * 100 if plant_pixels > 0 else 0.0

        # set a simple binary status for the MQTT payload
        status = "WARNING" if disease_percentage > self.disease_tolerance_pct else "HEALTHY"

        # extract the filename to create clean debug images
        import os
        filename = os.path.basename(image_path)

        # save the masks as black and white images to see what is being counted
        cv2.imwrite(f"debug_green_{filename}", (healthy_mask.astype(np.uint8) * 255))
        cv2.imwrite(f"debug_brown_{filename}", (diseased_mask.astype(np.uint8) * 255))  

        return {
            "status": status,
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

    analyzer = PixelAnalyzer(brown_threshold_r=90)
        
    try:
        results = analyzer.process_image(sys.argv[1])
        print(f"Analysis Results for {sys.argv[1]}:")
        print(f"  Status: {results['status']}")
        print(f"  Biomass: {results['biomass_percentage']}%")
        print(f"  Disease: {results['disease_percentage']}%")
    except Exception as e:
        print(f"Error: {e}")