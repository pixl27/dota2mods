"""Wardrobe-Setup.exe, end to end, in scratch folders: nothing global is touched
(--no-shell: no shortcuts, no registry; --no-launch; --silent: no window).

Run after build.bat and package.py."""
import ctypes
import hashlib
import shutil
import struct
import subprocess
import tempfile
import unittest
from ctypes import wintypes
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"


def latest(pattern):
    found = sorted(BUILD.glob(pattern), key=lambda p: p.stat().st_mtime)
    return found[-1] if found else None


SETUP = latest("Wardrobe-Setup-*.exe")
# The folder package.py built alongside it (named by date; the installer by version).
FOLDER = next(iter(sorted((p for p in BUILD.glob("Wardrobe-20*") if p.is_dir()), key=lambda p: p.stat().st_mtime, reverse=True)), None)
STUB = BUILD / "setup_stub.exe"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


class Locked:
    """Holds a file the way Windows holds a running executable's image: it can
    be renamed (FILE_SHARE_DELETE) but not overwritten (no FILE_SHARE_WRITE)."""

    def __init__(self, path):
        self.path = path

    def __enter__(self):
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.CreateFileW.restype = wintypes.HANDLE
        self.handle = kernel32.CreateFileW(str(self.path), 0x80000000, 0x1 | 0x4, None, 3, 0, None)
        if self.handle in (None, wintypes.HANDLE(-1).value):
            raise OSError(ctypes.get_last_error())
        self.kernel32 = kernel32
        return self

    def __exit__(self, *_):
        self.kernel32.CloseHandle(self.handle)


@unittest.skipUnless(SETUP and FOLDER and FOLDER.exists(), "run build.bat and package.py first")
class InstallerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=BUILD)
        self.addCleanup(self.temp.cleanup)
        self.target = Path(self.temp.name) / "Programs" / "Wardrobe"

    def run_setup(self, *args, exe=None):
        return subprocess.run([str(exe or SETUP), *args, "--no-shell", "--no-launch", "--silent"], capture_output=True, timeout=120)

    def install(self):
        result = self.run_setup("--target", str(self.target))
        self.assertEqual(result.returncode, 0, result)

    def test_install_writes_every_file_identically(self):
        self.install()
        expected = sorted(p.relative_to(FOLDER) for p in FOLDER.rglob("*") if p.is_file())
        self.assertGreater(len(expected), 5)
        for relative in expected:
            installed = self.target / relative
            self.assertTrue(installed.exists(), relative)
            self.assertEqual(digest(installed), digest(FOLDER / relative), relative)
            # dates survive: the catalog keeps its own, not the install's
            self.assertEqual(int(installed.stat().st_mtime), int((FOLDER / relative).stat().st_mtime), relative)
        # The uninstaller is this installer without its payload (not today's stub:
        # build.bat may have rebuilt that since the package was made).
        installer = SETUP.read_bytes()
        offset = struct.unpack_from("<Q", installer, len(installer) - 64 + 8)[0]
        self.assertEqual((self.target / "Uninstall.exe").read_bytes(), installer[:offset])
        self.assertFalse(list(self.target.rglob("*.new")) + list(self.target.rglob("*.old")))

    def test_update_while_the_app_is_running(self):
        self.install()
        app = self.target / "Wardrobe.exe"
        app.write_bytes(b"older build")
        with Locked(app):
            result = self.run_setup("--target", str(self.target))
            self.assertEqual(result.returncode, 0, result)
        self.assertEqual(digest(app), digest(FOLDER / "Wardrobe.exe"))

    def test_uninstall_removes_the_folder(self):
        self.install()
        result = self.run_setup("--uninstall-run", str(self.target))
        self.assertEqual(result.returncode, 0, result)
        self.assertFalse(self.target.exists())

    def test_uninstall_refuses_a_folder_that_is_not_wardrobe(self):
        stranger = Path(self.temp.name) / "Documents"
        stranger.mkdir()
        (stranger / "keep.txt").write_text("mine")
        result = self.run_setup("--uninstall-run", str(stranger))
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual((stranger / "keep.txt").read_text(), "mine")

    def test_damaged_installer_refuses_to_run(self):
        # LZMS can decode damaged data into garbage of the right length; only the
        # footer's SHA-256 stops it. Damage the payload at its start, middle and end.
        original = SETUP.read_bytes()
        offset, packed = struct.unpack_from("<QQ", original, len(original) - 64 + 8)
        for position in (offset + 16, offset + packed // 2, offset + packed - 16):
            damaged = Path(self.temp.name) / f"damaged-{position}.exe"
            data = bytearray(original)
            data[position] ^= 0xFF
            damaged.write_bytes(bytes(data))
            result = self.run_setup("--target", str(self.target), exe=damaged)
            self.assertEqual(result.returncode, 2, position)
            self.assertFalse((self.target / "Wardrobe.exe").exists(), position)

    def test_stub_alone_installs_nothing(self):
        result = self.run_setup("--target", str(self.target), exe=STUB)
        self.assertEqual(result.returncode, 2)
        self.assertFalse(self.target.exists())


if __name__ == "__main__":
    unittest.main()
