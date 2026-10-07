r"""
release.py - makes a new version of Wardrobe and, with --publish, puts it on
GitHub, where every installed copy finds it and offers to update itself.

usage: python release.py [--keep-version] [--publish] [--allow-unpushed] [--notes TEXT]

Steps, each stopping everything if it fails:
1. version: today's date, YYYY.MM.DD.N (N grows within a day), written into
   src/version.h; --keep-version reuses the current one;
2. build.bat, then tests\run_tests.bat, then package.py;
3. with --publish only: a GitHub release "v<version>" on the repository named
   in src/version.h, with Wardrobe-Setup-<version>.exe attached.

The GitHub token is the one Git already uses for this repository (Git
Credential Manager); it is read in memory and never printed. The release tag
points at the remote default branch, so --publish refuses to run while the
working tree has uncommitted or unpushed changes: the published installer must
match published code. --allow-unpushed overrides that, knowingly.
"""
import argparse
import datetime
import json
import re
import subprocess
import sys
import urllib.error
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
VERSION_H = HERE / "src" / "version.h"


def current():
    text = VERSION_H.read_text(encoding="utf-8")
    return re.search(r'#define WARDROBE_VERSION "([^"]+)"', text).group(1), re.search(r'#define WARDROBE_REPOSITORY "([^"]+)"', text).group(1)


def next_version(previous):
    today = datetime.date.today().strftime("%Y.%m.%d")
    if previous.startswith(today + "."):
        return f"{today}.{int(previous.rsplit('.', 1)[1]) + 1}"
    return f"{today}.1"


def write_version(version):
    text = VERSION_H.read_text(encoding="utf-8")
    text = re.sub(r'#define WARDROBE_VERSION "[^"]+"', f'#define WARDROBE_VERSION "{version}"', text)
    VERSION_H.write_text(text, encoding="utf-8", newline="\n")


def run(step, command):
    print(f"[*] {step}...")
    result = subprocess.run(command, cwd=HERE)
    if result.returncode:
        print(f"[!] {step} a échoué (code {result.returncode}) : rien n'est publié.")
        sys.exit(result.returncode)


def token(owner):
    """The credential Git Credential Manager holds for the owner's GitHub account, in memory only.

    Asked by account name: the manager can also hold restricted tokens (user
    "x-access-token") that push but cannot create releases, and without a name
    it may hand one of those out first."""
    answer = subprocess.run(["git", "credential", "fill"], input=f"protocol=https\nhost=github.com\nusername={owner}\n\n",
                            capture_output=True, text=True, cwd=HERE)
    for line in answer.stdout.splitlines():
        if line.startswith("password="):
            return line.split("=", 1)[1]
    return None


def api(method, url, secret, body=None, data=None, content_type="application/json"):
    request = urllib.request.Request(url, method=method, data=data if data is not None else
                                     (json.dumps(body).encode() if body is not None else None))
    request.add_header("Authorization", f"Bearer {secret}")
    request.add_header("Accept", "application/vnd.github+json")
    request.add_header("User-Agent", "wardrobe-release")
    if body is not None or data is not None:
        request.add_header("Content-Type", content_type)
    try:
        with urllib.request.urlopen(request, timeout=120) as response:
            return json.loads(response.read() or b"{}")
    except urllib.error.HTTPError as error:
        # GitHub explains its refusals in the body; keep that, drop the traceback.
        detail = error.read().decode("utf-8", "replace")[:500]
        error.detail = detail
        raise


def published_state():
    """None when HEAD is committed and pushed, else a sentence saying what is not."""
    dirty = subprocess.run(["git", "status", "--porcelain"], capture_output=True, text=True, cwd=HERE).stdout.strip()
    if dirty:
        return "des modifications ne sont pas commitées"
    subprocess.run(["git", "fetch", "--quiet"], cwd=HERE)
    ahead = subprocess.run(["git", "rev-list", "--count", "@{u}..HEAD"], capture_output=True, text=True, cwd=HERE).stdout.strip()
    if ahead not in ("", "0"):
        return f"{ahead} commit(s) ne sont pas poussés"
    return None


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--keep-version", action="store_true", help="reuse the version already in src/version.h")
    parser.add_argument("--publish", action="store_true", help="create the GitHub release and upload the installer")
    parser.add_argument("--allow-unpushed", action="store_true", help="publish even if the code is not committed and pushed")
    parser.add_argument("--notes", default="", help="what changed, shown on the release page")
    parser.add_argument("--skip-build", action="store_true", help="publish the installer already built for this version")
    args = parser.parse_args(argv)

    previous, repository = current()
    version = previous if args.keep_version else next_version(previous)
    if args.publish and not args.allow_unpushed:
        problem = published_state()
        if problem:
            print(f"[!] {problem} : la release pointerait vers un code différent de l'installateur.")
            print("    Commite et pousse d'abord, ou relance avec --allow-unpushed en connaissance de cause.")
            return 1
    if version != previous:
        write_version(version)
    print(f"[*] Version {version} ({repository})")
    if not args.skip_build:
        run("Compilation", ["cmd", "/c", str(HERE / "build.bat"), "--no-pause"])
        run("Tests", ["cmd", "/c", str(HERE / "tests" / "run_tests.bat")])
        run("Paquet et installateur", [sys.executable, str(HERE / "package.py")])
    installer = HERE / "build" / f"Wardrobe-Setup-{version}.exe"
    if not installer.exists():
        print(f"[!] {installer} est introuvable.")
        return 1
    print(f"[OK] {installer.name} prêt ({installer.stat().st_size / 1e6:.1f} Mo)")
    if not args.publish:
        print(f"    Rien n'est publié. Pour le mettre en ligne : python release.py --keep-version --publish")
        return 0

    secret = token(repository.split("/")[0])
    if not secret:
        print("[!] Aucun identifiant GitHub dans Git : fais un git push une fois pour te connecter, puis réessaie.")
        return 1
    base = f"https://api.github.com/repos/{repository}"
    tag = f"v{version}"
    try:
        api("GET", f"{base}/releases/tags/{tag}", secret)
        print(f"[!] La release {tag} existe déjà : relance sans --keep-version pour une nouvelle version.")
        return 1
    except urllib.error.HTTPError as error:
        if error.code != 404:
            print(f"[!] GitHub a répondu {error.code} en vérifiant {tag}.")
            return 1
    notes = args.notes or "Nouvelle version de Wardrobe."
    body = (f"{notes}\n\n**Installer** : télécharge `{installer.name}` ci-dessous, lance-le, clique sur « Installer Wardrobe ».\n"
            "**Mettre à jour** : Wardrobe propose la nouvelle version tout seul sur son écran d'accueil.\n\n"
            "Windows peut afficher « Windows a protégé votre ordinateur » (programme non signé) : "
            "« Informations complémentaires » puis « Exécuter quand même ».")
    head = subprocess.run(["git", "rev-parse", "HEAD"], capture_output=True, text=True, cwd=HERE).stdout.strip()
    try:
        release = api("POST", f"{base}/releases", secret, {"tag_name": tag, "target_commitish": head, "name": f"Wardrobe {version}",
                                                            "body": body, "draft": False, "prerelease": False})
        upload = release["upload_url"].split("{")[0] + f"?name={installer.name}"
        asset = api("POST", upload, secret, data=installer.read_bytes(), content_type="application/octet-stream")
    except urllib.error.HTTPError as error:
        print(f"[!] GitHub a refusé ({error.code}) : {getattr(error, 'detail', '')}")
        print("    Vérifie sur la page des releases si quelque chose a été créé avant de relancer.")
        return 1
    print(f"[OK] Publié : {release['html_url']}")
    print(f"     {asset['browser_download_url']}")
    print("     Les copies installées le proposeront à la prochaine vérification (au lancement, puis toutes les six heures).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
