import contextlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("cache_repair", ROOT / "repair_inventory_cache.py")
repair = importlib.util.module_from_spec(spec)
spec.loader.exec_module(repair)


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


def cache(local):
    item = integer(1, (1 << 62) if local else 99) + integer(2, 123) + integer(4, 42)
    group = integer(1, 1) + blob(2, item) + integer(3, 1)
    return integer(1, 4) + blob(2, integer(1, 1) + integer(2, 123) + blob(4, group))


class CacheRepairTests(unittest.TestCase):
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
        self.catalog.write_text(json.dumps({"skins": [{"def": 42}]}))
        self.backup = base / "backups"

    def invoke(self, *options, running=False):
        args = ["repair", str(self.game), "--catalog", str(self.catalog), "--backup-directory", str(self.backup), *options]
        with patch("sys.argv", args), patch.object(repair, "dota_running", return_value=running), contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            repair.main()

    def test_inspection_is_read_only(self):
        self.invoke()
        self.assertTrue(self.affected.exists())
        self.assertFalse(self.backup.exists())
        self.assertEqual(repair.inspect(self.affected, {42})["wardrobe_items"], 1)
        self.assertEqual(repair.inspect(self.clean, {42})["wardrobe_items"], 0)

    def test_repair_preserves_backup_and_clean_cache(self):
        original = self.affected.read_bytes()
        clean = self.clean.read_bytes()
        self.invoke("--repair")
        self.assertFalse(self.affected.exists())
        self.assertEqual(self.clean.read_bytes(), clean)
        self.assertEqual(next(self.backup.rglob("cache_123_1.soc")).read_bytes(), original)
        self.assertTrue(list(self.backup.rglob("manifest.json")))

    def test_running_game_blocks_mutation(self):
        with self.assertRaises(SystemExit):
            self.invoke("--repair", running=True)
        self.assertTrue(self.affected.exists())
        self.assertFalse(self.backup.exists())

    def test_unknown_cache_is_preserved(self):
        self.affected.write_bytes(b"\x08\x04\x12\x7f")
        self.invoke("--repair")
        self.assertTrue(self.affected.exists())
        self.assertFalse(self.backup.exists())

    def test_restore_returns_the_backup_and_keeps_a_recreated_cache(self):
        original = self.affected.read_bytes()
        self.invoke("--repair")
        recreated = cache(False)
        self.affected.write_bytes(recreated)
        self.invoke("--restore", "latest")
        self.assertEqual(self.affected.read_bytes(), original)
        self.assertEqual(next(self.backup.rglob("cache_123_1.soc.replaced-*")).read_bytes(), recreated)
        with self.assertRaises(SystemExit):
            self.invoke("--restore", "latest", running=True)

    def test_default_backup_directory_is_outside_build(self):
        self.assertNotIn("build", repair.default_backup_directory().parts)

    def test_unknown_definition_is_not_attributed_to_wardrobe(self):
        self.assertEqual(repair.inspect(self.affected, {43})["wardrobe_items"], 0)


if __name__ == "__main__":
    unittest.main()
