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
        os.environ['MT_DATA_DIR'] = self.data.name
        import importlib
        import minustwo_launcher
        self.launcher = importlib.reload(minustwo_launcher)

    def tearDown(self):
        os.environ.pop('MT_DATA_DIR', None)
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
        self.assertEqual((env['MT_GAME_DIR'], env['MT_LANGUAGE'], env['MT_UNLOCK_ADDONS']), ('G', '17', '0'))
        self.assertEqual(env['MT_KEY_MAP'], 'cross=Return,circle=Comma')

    def test_settings_with_a_byte_order_mark_load(self):
        self.launcher.SETTINGS.write_text(json.dumps({'game_dir': 'D:/G'}), encoding='utf-8-sig')
        self.assertEqual(self.launcher.load_settings()['game_dir'], 'D:/G')

    def test_mods_switch_reaches_run_py(self):
        # The Mods & patches page's "Load mods": run.py reads MT_MODS_ENABLED (mods are on by default).
        values = {**self.launcher.DEFAULTS, 'game_dir': 'G'}
        self.assertEqual(self.launcher.game_environment(values)['MT_MODS_ENABLED'], '1')
        values['mods_enabled'] = False
        self.assertEqual(self.launcher.game_environment(values)['MT_MODS_ENABLED'], '0')

    def test_mod_and_patch_selections_round_trip(self):
        path = self.launcher.DATA / 'mods.json'
        self.assertEqual(self.launcher.load_json(path, {}), {})  # none yet
        self.launcher.write_json(path, {'order': ['B', 'A'], 'disabled': ['A']})
        self.assertEqual(self.launcher.load_json(path, {}), {'order': ['B', 'A'], 'disabled': ['A']})
        path.write_text('[1, 2]', encoding='utf-8')  # not what the page writes: the default
        self.assertEqual(self.launcher.load_json(path, {}), {})


class UpdateTest(unittest.TestCase):
    """The launcher's updates from GitHub releases (no network: file:// downloads)."""

    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.root = Path(self.folder.name)
        os.environ['MT_DATA_DIR'] = str(self.root / 'data')
        import importlib
        import minustwo_launcher
        self.launcher = importlib.reload(minustwo_launcher)

    def tearDown(self):
        os.environ.pop('MT_DATA_DIR', None)
        self.folder.cleanup()

    def test_versions_compare_as_numbers(self):
        v = self.launcher.version_tuple
        self.assertGreater(v('0.10.0'), v('0.9.1'))
        major, minor, patch = v(self.launcher.VERSION)
        self.assertGreater(v(f'v{major}.{minor}.{patch + 1}'), v(self.launcher.VERSION))
        self.assertEqual(v('windows-v0.2.0'), (0, 2, 0))

    def test_release_gives_the_zip_and_its_checksum(self):
        release = {'tag_name': 'v0.2.0', 'html_url': 'https://github.com/o/r/releases/tag/v0.2.0', 'assets': [
            {'name': 'minustwo-windows.zip.sha256', 'browser_download_url': 'https://x/zip.sha256'},
            {'name': 'minustwo-windows.zip', 'browser_download_url': 'https://x/zip'}]}
        self.assertEqual(self.launcher.release_info(release, 'o/r'),
                         ('0.2.0', 'https://x/zip', 'https://x/zip.sha256', 'https://github.com/o/r/releases/tag/v0.2.0'))
        self.assertEqual(self.launcher.release_info({'tag_name': 'v0.3'}, 'o/r'),
                         ('0.3', None, None, 'https://github.com/o/r/releases/latest'))

    def package(self, checksum=None):
        import hashlib
        import zipfile
        archive = self.root / 'minustwo-windows.zip'
        with zipfile.ZipFile(archive, 'w') as z:
            z.writestr('minustwo-windows/MinusTwo.exe', b'new launcher')
            z.writestr('minustwo-windows/bin/minustwo.exe', b'new game')
        digest = checksum or hashlib.sha256(archive.read_bytes()).hexdigest()
        (self.root / 'minustwo-windows.zip.sha256').write_text(f'{digest}  minustwo-windows.zip\n')
        return archive.as_uri(), (self.root / 'minustwo-windows.zip.sha256').as_uri()

    def test_download_is_checked_and_unpacked(self):
        url, checksum = self.package()
        seen = []
        new = self.launcher.download_update(url, checksum, self.root / 'update', seen.append)
        self.assertEqual((new / 'MinusTwo.exe').read_bytes(), b'new launcher')
        self.assertEqual(seen[-1], 100)

    def test_a_download_unlike_its_checksum_is_refused(self):
        url, checksum = self.package('0' * 64)
        with self.assertRaises(OSError):
            self.launcher.download_update(url, checksum, self.root / 'update')
        self.assertFalse((self.root / 'update' / 'new').exists())
        with self.assertRaises(OSError):  # a release without its .sha256
            self.launcher.download_update(url, None, self.root / 'update')

    def test_linux_takes_the_appimage(self):
        release = {'tag_name': 'v0.2.0', 'assets': [
            {'name': 'minustwo-windows.zip', 'browser_download_url': 'https://x/zip'},
            {'name': 'minustwo-x86_64.AppImage', 'browser_download_url': 'https://x/appimage'},
            {'name': 'minustwo-x86_64.AppImage.sha256', 'browser_download_url': 'https://x/appimage.sha256'}]}
        self.assertEqual(self.launcher.release_info(release, 'o/r', '.appimage')[1:3],
                         ('https://x/appimage', 'https://x/appimage.sha256'))

    def test_the_appimage_is_swapped_only_when_it_matches(self):
        import hashlib
        new = self.root / 'release.AppImage'
        new.write_bytes(b'new appimage')
        checksum = self.root / 'release.AppImage.sha256'
        installed = self.root / 'minustwo-x86_64.AppImage'
        installed.write_bytes(b'old appimage')
        checksum.write_text('0' * 64 + '  release.AppImage\n')
        with self.assertRaises(OSError):
            self.launcher.replace_appimage(new.as_uri(), checksum.as_uri(), installed)
        self.assertEqual(installed.read_bytes(), b'old appimage')
        self.assertEqual([p.name for p in self.root.glob('*.update')], [])
        checksum.write_text(hashlib.sha256(b'new appimage').hexdigest() + '  release.AppImage\n')
        self.launcher.replace_appimage(new.as_uri(), checksum.as_uri(), installed)
        self.assertEqual(installed.read_bytes(), b'new appimage')

    def test_installing_keeps_the_players_files(self):
        new, old = self.root / 'new', self.root / 'installed'
        for folder in (new, old):
            (folder / 'bin').mkdir(parents=True)
            (folder / 'bin' / 'minustwo.exe').write_text(folder.name)
        (new / 'minustwo.ini').write_text('from the package')
        (old / 'minustwo.ini').write_text('mine')
        (old / 'user').mkdir()
        (old / 'user' / 'save.dat').write_text('progress')
        self.launcher.copy_over(new, old)
        self.assertEqual((old / 'bin' / 'minustwo.exe').read_text(), 'new')
        self.assertEqual((old / 'minustwo.ini').read_text(), 'mine')
        self.assertEqual((old / 'user' / 'save.dat').read_text(), 'progress')


class TrophyTest(unittest.TestCase):
    """The Achievements page's trophies (scripts/godzilla_trophies.py)."""

    def test_the_list_is_the_runtimes(self):
        # The runtime's notice and the launcher tell the same names; the grades are the C table's comments.
        import re
        import godzilla_trophies
        source = (ROOT / 'src' / 'runtime_services.c').read_text(encoding='utf-8')
        table = re.findall(r'\{"([^"]*)","([^"]*)"\}, /\* (\d+): (\w+) \*/', source)
        self.assertEqual([(name, text, grade) for name, text, _id, grade in table], list(godzilla_trophies.TROPHIES))
        self.assertEqual([int(i) for _n, _t, i, _g in table], list(range(45)))
        grades = [grade for _n, _t, grade in godzilla_trophies.TROPHIES]
        self.assertEqual((grades.count('Platinum'), grades.count('Gold'), grades.count('Silver'), grades.count('Bronze')),
                         (1, 3, 11, 30))

    def test_icons_and_unlocked_set_are_read(self):
        import godzilla_trophies
        with tempfile.TemporaryDirectory() as folder:
            names = (b'TROPCONF.ESFM', b'TROP000.PNG', b'TROP007.PNG')
            payloads = (b'secret', b'\x89PNG zero', b'\x89PNG seven')
            data_at = 0x60 + 0x40 * len(names)
            header = bytes((0xDC, 0xA2, 0x4D, 0x00)) + bytes(12) + struct.pack('>II', len(names), 0x40)
            header += bytes(0x60 - len(header))
            entries, blob = b'', b''
            for name, payload in zip(names, payloads):
                entries += name.ljust(32, b'\0') + struct.pack('>QQ', data_at + len(blob), len(payload)) + bytes(16)
                blob += payload
            trp = Path(folder) / 'trophy00.trp'
            trp.write_bytes(header + entries + blob)
            self.assertEqual(godzilla_trophies.read_trp_icons(trp), {0: b'\x89PNG zero', 7: b'\x89PNG seven'})
            self.assertEqual(godzilla_trophies.read_trp_icons(Path(folder) / 'none.trp'), {})
            unlocked = Path(folder) / 'trophies.dat'
            unlocked.write_bytes(struct.pack('<4I', (1 << 1) | (1 << 2) | (1 << 7), 0, 1 << 3, 0))
            self.assertEqual(godzilla_trophies.read_unlocked(unlocked), {1, 2, 7, 67})
            self.assertEqual(godzilla_trophies.read_unlocked(Path(folder) / 'none.dat'), set())


if __name__ == '__main__':
    unittest.main()
