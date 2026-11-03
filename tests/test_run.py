import json
import os
import sys
import tempfile
import unittest
from pathlib import Path

from paths import ROOT

sys.path.insert(0, str(ROOT))
import run


class ImageSplashSkipTests(unittest.TestCase):
    def test_splash_skip_choice_invalidates_a_prepared_image(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            game = root / 'game'
            out = root / 'out'
            game.mkdir()
            out.mkdir()
            (out / 'boot-linked.bin').touch()
            (out / 'content.bin').touch()
            (out / 'button_icons').mkdir()
            (out / 'config_icon').mkdir()
            (out / 'link.json').write_text(json.dumps({'splash_skip_patched': True}), encoding='utf-8')
            future = 4_000_000_000
            os.utime(out / 'boot-linked.bin', (future, future))

            self.assertTrue(run.image_is_current(game, out, skip_splash_logo=True))
            self.assertFalse(run.image_is_current(game, out, skip_splash_logo=False))

            (out / 'link.json').write_text(json.dumps({'splash_skip_patched': False}), encoding='utf-8')
            self.assertTrue(run.image_is_current(game, out, skip_splash_logo=False))
            self.assertFalse(run.image_is_current(game, out, skip_splash_logo=True))


if __name__ == '__main__':
    unittest.main()
