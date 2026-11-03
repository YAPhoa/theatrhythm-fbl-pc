"""link_modules.module on a synthetic SELF: reads the file once and reports its hash."""
import hashlib
import tempfile
import unittest
from pathlib import Path

from paths import ROOT  # noqa: F401  (sys.path for scripts/)
import link_modules
from test_link_libc import self_fixture


class ModuleTests(unittest.TestCase):
    def test_module_accepts_unpacked_elf_with_the_same_exports(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'libc.prx'
            source = self_fixture()
            path.write_bytes(source)
            expected = link_modules.module(path)
            elf = link_modules.parse_self(source)[0]
            path.write_bytes(elf)
            actual = link_modules.module(path)
        self.assertEqual(actual['symbols'], expected['symbols'])
        self.assertEqual(actual['ph'], expected['ph'])
        self.assertEqual(actual['relocs'], expected['relocs'])
        self.assertEqual(actual['sha256'], hashlib.sha256(elf).hexdigest())

    def test_module_reads_the_file_once_and_reports_its_hash(self):
        data = self_fixture()
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'libc.prx'
            path.write_bytes(data)
            info = link_modules.module(path)
        self.assertEqual(info['sha256'], hashlib.sha256(data).hexdigest())
        self.assertEqual(info['missing'], [])
        self.assertEqual(info['symbols'][0]['identity'], ('bzQExy189ZI', ('libc', 1), ('libc', 1)))


class IntroSkipPatchTests(unittest.TestCase):
    def test_patch_skips_movie_and_uses_the_normal_completion_transition(self):
        image = bytearray(link_modules.INTRO_SKIP_END_MOVIE_OFFSET + len(link_modules.INTRO_SKIP_END_MOVIE))
        call = link_modules.INTRO_SKIP_CALL_OFFSET
        end_movie = link_modules.INTRO_SKIP_END_MOVIE_OFFSET
        image[call:call + len(link_modules.INTRO_SKIP_CALL)] = link_modules.INTRO_SKIP_CALL
        image[end_movie:end_movie + len(link_modules.INTRO_SKIP_END_MOVIE)] = link_modules.INTRO_SKIP_END_MOVIE

        link_modules.patch_intro_skip(image, 0, {'sha256': link_modules.INTRO_SKIP_MODULE_SHA256})

        self.assertEqual(image[call:call + 5], bytes.fromhex('e855030000'))

    def test_patch_rejects_an_unknown_revision_or_changed_call_site(self):
        with self.assertRaisesRegex(ValueError, 'supported game module revision'):
            link_modules.patch_intro_skip(bytearray(), 0, {'sha256': 'unknown'})

        image = bytearray(link_modules.INTRO_SKIP_END_MOVIE_OFFSET + len(link_modules.INTRO_SKIP_END_MOVIE))
        end_movie = link_modules.INTRO_SKIP_END_MOVIE_OFFSET
        image[end_movie:end_movie + len(link_modules.INTRO_SKIP_END_MOVIE)] = link_modules.INTRO_SKIP_END_MOVIE
        with self.assertRaisesRegex(ValueError, 'call site did not match'):
            link_modules.patch_intro_skip(image, 0, {'sha256': link_modules.INTRO_SKIP_MODULE_SHA256})


class SplashSkipPatchTests(unittest.TestCase):
    def test_patch_resolves_startup_logo_to_main_menu(self):
        logo_id = link_modules.SPLASH_SKIP_STARTUP_LOGO_ID_OFFSET
        menu_id = link_modules.SPLASH_SKIP_MAIN_MENU_ID_OFFSET
        image = bytearray(max(logo_id, menu_id) + len(link_modules.SPLASH_SKIP_MAIN_MENU_ID))
        image[logo_id:logo_id + len(link_modules.SPLASH_SKIP_STARTUP_LOGO_ID)] = (
            link_modules.SPLASH_SKIP_STARTUP_LOGO_ID
        )
        image[menu_id:menu_id + len(link_modules.SPLASH_SKIP_MAIN_MENU_ID)] = (
            link_modules.SPLASH_SKIP_MAIN_MENU_ID
        )

        link_modules.patch_splash_skip(image, 0, {'sha256': link_modules.INTRO_SKIP_MODULE_SHA256})

        self.assertEqual(
            image[logo_id:logo_id + len(link_modules.SPLASH_SKIP_STARTUP_LOGO_ID)],
            link_modules.SPLASH_SKIP_MAIN_MENU_ID,
        )

    def test_patch_rejects_unknown_revisions_or_changed_scene_ids(self):
        with self.assertRaisesRegex(ValueError, 'supported game module revision'):
            link_modules.patch_splash_skip(bytearray(), 0, {'sha256': 'unknown'})

        logo_id = link_modules.SPLASH_SKIP_STARTUP_LOGO_ID_OFFSET
        menu_id = link_modules.SPLASH_SKIP_MAIN_MENU_ID_OFFSET
        image = bytearray(max(logo_id, menu_id) + len(link_modules.SPLASH_SKIP_MAIN_MENU_ID))
        image[logo_id:logo_id + len(link_modules.SPLASH_SKIP_STARTUP_LOGO_ID)] = (
            link_modules.SPLASH_SKIP_STARTUP_LOGO_ID
        )
        with self.assertRaisesRegex(ValueError, 'main-menu ID did not match'):
            link_modules.patch_splash_skip(image, 0, {'sha256': link_modules.INTRO_SKIP_MODULE_SHA256})


if __name__ == '__main__':
    unittest.main()
