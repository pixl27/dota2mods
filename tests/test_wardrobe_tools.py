"""wardrobe_tools.exe repair: the same cases as test_inventory_cache_repair.py, on
the C++ port the application uses, plus backups crossing between the two."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "build" / "wardrobe_tools.exe"


def varint(value):
    out = bytearray()
    while value > 127:
        out.append((value & 127) | 128)
        value >>= 7
    out.append(value)
    return bytes(out)


def integer(number, value):
    return varint(number << 3) + varint(value)


def blob(number, value):
    return varint(number << 3 | 2) + varint(len(value)) + value


def cache(local, definition=42):
    item = integer(1, (1 << 62) if local else 99) + integer(2, 123) + integer(4, definition)
    group = integer(1, 1) + blob(2, item) + integer(3, 1)
    return integer(1, 4) + blob(2, integer(1, 1) + integer(2, 123) + blob(4, group))


class WardrobeToolsRepairTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=ROOT / "build")
        self.addCleanup(self.temp.cleanup)
        base = Path(self.temp.name)
        self.game = base / "game/dota"
        self.game.mkdir(parents=True)
        self.affected = self.game / "cache_123_1.soc"
        self.affected.write_bytes(cache(True))
        self.clean = self.game / "cache_124_1.soc"
        self.clean.write_bytes(cache(False))
        self.catalog = base / "catalog.json"
        self.catalog.write_text(json.dumps({"skins": [{"def": 42}]}, indent=1))
        self.backup = base / "backups"

    def run_tool(self, *options, running=False):
        env = dict(os.environ, WARDROBE_TEST_DOTA_RUNNING="1" if running else "0")
        args = [str(TOOL), "repair", str(self.game), "--catalog", str(self.catalog), "--backup-directory", str(self.backup), *options]
        return subprocess.run(args, env=env, capture_output=True)

    def test_inspection_is_read_only(self):
        result = self.run_tool()
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("cache_123_1.soc : 1 objet(s) de Wardrobe sur 1", result.stdout.decode("utf-8"))
        self.assertIn("cache_124_1.soc : 0 objet(s) de Wardrobe sur 1", result.stdout.decode("utf-8"))
        self.assertTrue(self.affected.exists())
        self.assertFalse(self.backup.exists())

    def test_repair_preserves_backup_and_clean_cache(self):
        original, clean = self.affected.read_bytes(), self.clean.read_bytes()
        result = self.run_tool("--repair")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertFalse(self.affected.exists())
        self.assertEqual(self.clean.read_bytes(), clean)
        self.assertEqual(next(self.backup.rglob("cache_123_1.soc")).read_bytes(), original)
        manifest = json.loads(next(self.backup.rglob("manifest.json")).read_text(encoding="utf-8"))
        self.assertEqual([entry["file"] for entry in manifest], ["cache_123_1.soc"])

    def test_running_game_blocks_mutation(self):
        result = self.run_tool("--repair", running=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue(self.affected.exists())
        self.assertFalse(self.backup.exists())

    def test_unknown_cache_is_preserved(self):
        self.affected.write_bytes(b"\x08\x04\x12\x7f")
        result = self.run_tool("--repair")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("illisible", result.stdout.decode("utf-8"))
        self.assertTrue(self.affected.exists())
        self.assertFalse(self.backup.exists())

    def test_restore_returns_the_backup_and_keeps_a_recreated_cache(self):
        original = self.affected.read_bytes()
        self.assertEqual(self.run_tool("--repair").returncode, 0)
        recreated = cache(False)
        self.affected.write_bytes(recreated)
        result = self.run_tool("--restore", "latest")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(self.affected.read_bytes(), original)
        self.assertEqual(next(self.backup.rglob("cache_123_1.soc.replaced-*")).read_bytes(), recreated)
        self.assertNotEqual(self.run_tool("--restore", "latest", running=True).returncode, 0)

    def test_unknown_definition_is_not_attributed_to_wardrobe(self):
        self.affected.write_bytes(cache(True, definition=43))
        result = self.run_tool("--repair")
        self.assertIn("cache_123_1.soc : 0 objet(s) de Wardrobe sur 1", result.stdout.decode("utf-8"))
        self.assertTrue(self.affected.exists())

    def test_python_backup_restores_with_the_tool(self):
        # A backup made by repair_inventory_cache.py before this tool existed.
        original = self.affected.read_bytes()
        script = ROOT / "repair_inventory_cache.py"
        env = dict(os.environ, PYTHONWARNINGS="ignore")
        made = subprocess.run(["python", "-c",
                               "import sys, importlib.util; spec = importlib.util.spec_from_file_location('r', sys.argv[1]);"
                               "r = importlib.util.module_from_spec(spec); spec.loader.exec_module(r); r.dota_running = lambda: False;"
                               "sys.argv = ['r', sys.argv[2], '--catalog', sys.argv[3], '--backup-directory', sys.argv[4], '--repair'];"
                               "r.main()", str(script), str(self.game), str(self.catalog), str(self.backup)],
                              env=env, capture_output=True)
        self.assertEqual(made.returncode, 0, made.stderr)
        self.assertFalse(self.affected.exists())
        result = self.run_tool("--restore", "latest")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(self.affected.read_bytes(), original)


class WardrobeToolsCompatTests(unittest.TestCase):
    def test_missing_client_is_not_a_verdict(self):
        result = subprocess.run([str(TOOL), "compat", str(ROOT / "build" / "no-such-client.dll")], capture_output=True)
        self.assertEqual(result.returncode, 2)
        self.assertTrue(result.stdout.decode("utf-8").startswith("compat resolved=?"))

    def test_foreign_binary_is_incompatible(self):
        # A real PE image that is not client.dll: every entry must fail to resolve.
        result = subprocess.run([str(TOOL), "compat", str(TOOL)], capture_output=True)
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("compat resolved=0", result.stdout.decode("utf-8"))


if __name__ == "__main__":
    unittest.main()
