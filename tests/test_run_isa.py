from paths import ROOT
import sys
import tempfile
import unittest

sys.path.insert(0, str(ROOT))
import run


class RunIsaTests(unittest.TestCase):
    def test_parse_cpu_features(self):
        self.assertEqual(run.parse_cpu_features('CPU: missing MOVBE, BMI1, LZCNT\n'), ['MOVBE', 'BMI1', 'LZCNT'])
        self.assertEqual(run.parse_cpu_features('CPU: complete\n'), [])

    def test_rewrite_wanted_follows_cpu_by_default(self):
        self.assertTrue(run.isa_rewrite_wanted(['MOVBE'], {}))
        self.assertFalse(run.isa_rewrite_wanted([], {}))

    def test_rewrite_forced(self):
        self.assertTrue(run.isa_rewrite_wanted([], {'MT_ISA_REWRITE': '1'}))

    def test_rewrite_disabled(self):
        self.assertFalse(run.isa_rewrite_wanted(['MOVBE', 'BMI1'], {'MT_ISA_REWRITE': '0'}))

    def test_prepare_stops_when_the_folder_has_no_eboot(self):
        with tempfile.TemporaryDirectory() as folder:
            env = {'MT_GAME_DIR': folder, 'MT_DATA_DIR': folder}
            with self.assertRaises(SystemExit) as stopped:
                run.prepare_only(env)
        self.assertEqual(stopped.exception.code, 1)


if __name__ == '__main__':
    unittest.main()
