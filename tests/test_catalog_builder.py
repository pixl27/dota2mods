"""wardrobe_tools.exe catalog-build must produce the catalog that
gen_full_db.py + gen_names.py produce from the same game files: that is what
lets a copy without Python update its catalog by itself.

Needs Dota 2 installed and the Python `vdf` package (only for the reference)."""
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "build" / "wardrobe_tools.exe"
sys.path.insert(0, str(ROOT))
import dota_vpk  # noqa: E402

GAME = dota_vpk.game_directory()


@unittest.skipUnless(GAME, "Dota 2 is not installed here")
class CatalogBuilderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(dir=ROOT / "build")
        folder = Path(cls.temp.name)
        archive = dota_vpk.Archive(GAME)
        inputs = {"items_game.txt": "scripts/items/items_game.txt",
                  "items_english.txt": "resource/localization/items_english.txt",
                  "dota_english.txt": "resource/localization/dota_english.txt"}
        for name, entry in inputs.items():
            (folder / name).write_bytes(archive.read(entry))
        cls.python = folder / "python.json"
        cls.native = folder / "native.json"
        for step in ([sys.executable, str(ROOT / "gen_full_db.py"), "--items-game", str(folder / "items_game.txt"), "--out", str(cls.python)],
                     [sys.executable, str(ROOT / "gen_names.py"), "--db", str(cls.python), "--localization",
                      str(folder / "items_english.txt"), str(folder / "dota_english.txt")]):
            subprocess.run(step, check=True, capture_output=True)
        cls.result = subprocess.run([str(TOOL), "catalog-build", "--game", str(GAME), str(cls.native)], capture_output=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_tool_succeeds(self):
        self.assertEqual(self.result.returncode, 0, self.result.stdout.decode("utf-8", "replace"))

    def test_same_catalog_as_the_python_scripts(self):
        python = json.loads(self.python.read_text(encoding="utf-8"))
        native = json.loads(self.native.read_text(encoding="utf-8"))
        self.assertEqual(python["heroes"], native["heroes"])
        self.assertEqual(len(python["skins"]), len(native["skins"]))
        for left, right in zip(python["skins"], native["skins"]):
            self.assertEqual(left, right)

    def test_same_text_apart_from_line_endings(self):
        # Python writes through a text-mode file (CRLF on Windows); the tool writes LF.
        self.assertEqual(self.python.read_bytes().replace(b"\r\n", b"\n"), self.native.read_bytes())

    def test_previous_catalog_is_kept(self):
        again = subprocess.run([str(TOOL), "catalog-build", "--game", str(GAME), str(self.native)], capture_output=True)
        self.assertEqual(again.returncode, 0)
        self.assertTrue(Path(str(self.native) + ".previous").exists())


if __name__ == "__main__":
    unittest.main()
