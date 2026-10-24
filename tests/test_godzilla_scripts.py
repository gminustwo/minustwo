"""The Python side of the GODZILLA port: patch selection, the game's patch profile, the launcher."""
import contextlib
import io
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'scripts'))
sys.path.insert(0, str(ROOT / 'launcher'))

import graphics_options  # noqa: E402
import patches  # noqa: E402


def write_sfo(path, values):
    """A minimal param.sfo with string values."""
    keys = b''.join(k.encode() + b'\0' for k in values)
    data = b''.join(v.encode() + b'\0' for v in values.values())
    entries, key_offset, data_offset = b'', 0, 0
    for key, value in values.items():
        size = len(value.encode()) + 1
        entries += struct.pack('<HHIII', key_offset, 0x0204, size, size, data_offset)
        key_offset += len(key) + 1
        data_offset += size
    header_size = 20 + len(entries)
    path.write_bytes(struct.pack('<4sIIII', b'\0PSF', 0x101, header_size, header_size + len(keys), len(values))
                     + entries + keys + data)


class PatchSelectionTest(unittest.TestCase):
    def test_native_60_fps_asks_for_no_patch(self):
        options = graphics_options.startup_options({})
        self.assertEqual(options['engine_fps'], '60')
        self.assertEqual(graphics_options.patch_names(['Other'], options), ['Other'])

    def test_a_resolution_or_rate_asks_for_its_patch(self):
        options = graphics_options.startup_options({'render_resolution': '1440p', 'engine_fps': '120'})
        self.assertEqual(graphics_options.patch_names([], options),
                         ['Resolution Patch - 1440p', 'Frame Rate Patch - 120 FPS'])

    def test_resolution_patches_drop_silently(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(graphics_options.restrict_to_available(['Resolution Patch - 4K'], set()), [])
        self.assertEqual(out.getvalue(), '')

    def test_patches_the_game_has_none_of_are_dropped(self):
        self.assertEqual(graphics_options.restrict_to_available(['Resolution Patch - 4K', 'Other'], {'Other'}),
                         ['Other'])


class ProfileTest(unittest.TestCase):
    def test_godzilla_is_recognized_by_title_id_and_title(self):
        self.assertEqual(patches.game_profile('CUSA02046')[1].name, 'CUSA02046.xml')
        self.assertIsNotNone(patches.game_profile('CUSA99999', 'GODZILLA'))
        self.assertIsNone(patches.game_profile('CUSA01623', 'God of War III Remastered'))

    def test_built_in_patch_file_parses(self):
        self.assertEqual(patches.available_patches(patches.PATCHES_DIR / 'CUSA02046.xml', '01.01'), set())


class LauncherTest(unittest.TestCase):
    def setUp(self):
        self.data = tempfile.TemporaryDirectory()
        os.environ['MZ_DATA_DIR'] = self.data.name
        import importlib
        import minuszero_launcher
        self.launcher = importlib.reload(minuszero_launcher)

    def tearDown(self):
        os.environ.pop('MZ_DATA_DIR', None)
        self.data.cleanup()

    def dump(self, title_id, title):
        folder = Path(self.data.name) / title_id
        (folder / 'sce_sys').mkdir(parents=True)
        (folder / 'sce_module').mkdir()
        (folder / 'eboot.bin').write_bytes(b'x')
        (folder / 'sce_module' / 'libc.prx').write_bytes(b'x')
        write_sfo(folder / 'sce_sys' / 'param.sfo', {'TITLE': title, 'TITLE_ID': title_id, 'APP_VER': '01.01'})
        return folder

    def test_game_folder_check(self):
        self.assertIn('No game folder', self.launcher.game_problem(''))
        self.assertIn('not GODZILLA', self.launcher.game_problem(str(self.dump('CUSA01623', 'God of War III'))))
        folder = self.dump('CUSA02046', 'GODZILLA')
        self.assertIsNone(self.launcher.game_problem(str(folder)))
        self.assertEqual(self.launcher.game_version(str(folder)), 'CUSA02046 · version 01.01')
        (folder / 'eboot.bin').unlink()
        self.assertIn('no eboot.bin', self.launcher.game_problem(str(folder)))

    def test_environment_carries_settings_and_changed_keys(self):
        values = {**self.launcher.DEFAULTS, 'game_dir': 'G', 'language': '17', 'unlock_addons': False,
                  'keys': {'cross': 'Return', 'circle': ',', 'square': 'Mouse Left'}}
        env = self.launcher.game_environment(values)
        self.assertEqual((env['MZ_GAME_DIR'], env['MZ_LANGUAGE'], env['MZ_UNLOCK_ADDONS']), ('G', '17', '0'))
        self.assertEqual(env['MZ_KEY_MAP'], 'cross=Return,circle=Comma')

    def test_settings_with_a_byte_order_mark_load(self):
        self.launcher.SETTINGS.write_text(json.dumps({'game_dir': 'D:/G'}), encoding='utf-8-sig')
        self.assertEqual(self.launcher.load_settings()['game_dir'], 'D:/G')


if __name__ == '__main__':
    unittest.main()
