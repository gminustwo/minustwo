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

    def test_online_play_wants_the_rewrite(self):
        self.assertTrue(run.isa_rewrite_wanted([], {'MT_SHADNET_SERVER': '203.0.113.5'}))
        self.assertTrue(run.isa_rewrite_wanted([], {'MT_ONLINE': '1'}))
        self.assertFalse(run.isa_rewrite_wanted([], {'MT_SHADNET_SERVER': ' ', 'MT_ONLINE': '0'}))
        self.assertFalse(run.isa_rewrite_wanted([], {'MT_SHADNET_SERVER': '203.0.113.5', 'MT_ISA_REWRITE': '0'}))

    def test_prepare_stops_when_the_folder_has_no_eboot(self):
        with tempfile.TemporaryDirectory() as folder:
            env = {'MT_GAME_DIR': folder, 'MT_DATA_DIR': folder}
            with self.assertRaises(SystemExit) as stopped:
                run.prepare_only(env)
        self.assertEqual(stopped.exception.code, 1)


class ExitLineTests(unittest.TestCase):
    def test_session_length_and_how_it_ended(self):
        def line(code, seconds):
            return run.exit_line(code, seconds, now=0).split(' ', 2)[2]
        self.assertEqual(line(0, 8.9), 'the game process ended after 8 s, exit code 0')
        self.assertEqual(line(1, 754), 'the game process ended after 12 min 34 s, exit code 1')
        self.assertEqual(line(0, 3725), 'the game process ended after 1 h 02 min 05 s, exit code 0')
        self.assertEqual(line(0xC0000005, 60), 'the game process ended after 1 min 00 s, Windows exception 0xc0000005')
        self.assertEqual(line(-9, 5), 'the game process ended after 5 s, killed by signal 9')
        self.assertTrue(run.exit_line(0, 1).startswith('[exit] '))


if __name__ == '__main__':
    unittest.main()
