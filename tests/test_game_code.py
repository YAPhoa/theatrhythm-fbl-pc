from paths import ROOT
import os
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(ROOT / 'tools'))
from game_code import Game, parse, sites  # noqa: E402

GAME = Path(os.environ['THEATRHYTHM_GAME_DIR']) if os.environ.get('THEATRHYTHM_GAME_DIR') else None


class TableTests(unittest.TestCase):
    def test_every_site_parses(self):
        # the runtime's tables as tools/game_code.py reads them: hooks move fixed bytes only
        table = sites()
        self.assertGreaterEqual(len(table), 70)
        for file, method, inside, instances, instance, patterns in table:
            self.assertTrue(patterns, f'{file} {method}')
            self.assertTrue(method or instances > instance, f'{file}: a pattern-only site needs its match count')
            for pattern in patterns:
                data, mask, moved = parse(pattern)
                self.assertLessEqual(len(data), 512)
                if moved is not None:
                    self.assertTrue(5 <= moved <= 16 and all(mask[:moved]), f'{file} {method}: {pattern}')


@unittest.skipUnless(GAME and (GAME / 'Media/Metadata/global-metadata.dat').is_file(),
                     'needs the game dump (THEATRHYTHM_GAME_DIR)')
class GameTests(unittest.TestCase):
    def test_every_site_is_found(self):
        # what runtime_game_code.c does at boot, on this dump's version
        game = Game(GAME)
        for file, method, inside, instances, instance, patterns in sites():
            found = game.find(method, inside, instances, instance, patterns)
            self.assertNotIsInstance(found, str, f'{file} {method or patterns[0]}: {found}')


if __name__ == '__main__':
    unittest.main()
