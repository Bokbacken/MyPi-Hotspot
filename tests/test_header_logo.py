"""Run with python -m unittest discover -s tests -p 'test_header_logo.py'."""

import base64
from io import BytesIO
import importlib.util
from pathlib import Path
import re
import unittest

from PIL import Image


ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("render_header_logo", ROOT / "tools/render_header_logo.py")
generator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(generator)


class HeaderLogoTests(unittest.TestCase):
    def setUp(self):
        self.header = (ROOT / "main/header_logo.h").read_text()

    def test_original_variants(self):
        with Image.open(ROOT / "MyPi.png") as opaque:
            self.assertEqual(opaque.mode, "RGB")
        with Image.open(ROOT / "MyPi-transparent.png") as transparent:
            self.assertEqual(transparent.mode, "RGBA")
            self.assertEqual(transparent.getchannel("A").getbbox(), (6, 4, 497, 393))

    def test_reproducible_export(self):
        self.assertEqual(self.header, generator.render(ROOT / "MyPi-transparent.png"))

    def test_white_transparent_web_mark(self):
        uri = re.search(r'#define HEADER_LOGO_URI "([^"]+)"', self.header)[1]
        with Image.open(BytesIO(base64.b64decode(uri.split(",")[1]))) as web:
            self.assertEqual(web.size, (81, 64))
            self.assertEqual(web.getchannel("A").getextrema(), (0, 255))
            for red, green, blue, alpha in web.get_flattened_data():
                self.assertEqual((red, green, blue), (255, 255, 255))
            self.assertLess(abs(web.width / web.height - 491 / 389), 1 / web.height)

    def test_lcd_geometry(self):
        width = int(re.search(r"#define HEADER_LOGO_W (\d+)", self.header)[1])
        height = int(re.search(r"#define HEADER_LOGO_H (\d+)", self.header)[1])
        self.assertEqual((width, height), (20, 16))
        self.assertLess(abs(width / height - 491 / 389), 1 / height)
        pixels = re.search(r"s_header_logo\[\] = \{(.*?)\};", self.header, re.S)[1]
        values = [int(v) for v in re.findall(r"\d+", pixels)]
        self.assertEqual(len(values), width * height)
        self.assertTrue(all(0 <= value <= 255 for value in values))
        self.assertEqual((min(values), max(values)), (0, 255))
        self.assertEqual((18 - height) // 2, 1)
        self.assertLess(4 + width + 6 + 72, 212)  # Title ends before Wi-Fi icon.


if __name__ == "__main__":
    unittest.main()
