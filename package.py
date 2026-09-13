r"""
package.py - builds the folder you hand to someone else, and checks it.

usage: python package.py [--out DIR] [--no-zip]

Collects the application, the loader, the in-game library, the catalog and the
maintenance scripts into one folder, writes a plain-French LISEZMOI.txt, and
zips it. Refuses to package a binary that depends on anything the receiving
machine might not have: every import must be a Windows system DLL, so there is
no Visual C++ redistributable to install first.

Run build.bat before this.
"""
import argparse
import datetime
import shutil
import sys
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent

# What the friend needs to run it at all.
REQUIRED = [
    ("build/Wardrobe.exe", "Wardrobe.exe"),
    ("build/map.exe", "map.exe"),
    ("build/wardrobe_dll.dll", "wardrobe_dll.dll"),
    ("data/skins_full.json", "data/skins_full.json"),
    ("data/native_profile.json", "data/native_profile.json"),
]
# Only needed by the Entretien tab, and only if they have Python.
OPTIONAL = ["refresh_profile.py", "update_db.py", "dota_vpk.py", "gen_full_db.py", "gen_names.py",
            "repair_inventory_cache.py", "verify_profile.py"]

# Anything outside this set would have to be installed on the other machine.
SYSTEM_DLLS = {
    "kernel32.dll", "user32.dll", "gdi32.dll", "advapi32.dll", "shell32.dll", "ole32.dll", "oleaut32.dll",
    "shlwapi.dll", "ws2_32.dll", "dwmapi.dll", "d3d11.dll", "dxgi.dll", "bcrypt.dll", "psapi.dll",
    "version.dll", "winmm.dll", "imm32.dll", "rpcrt4.dll", "ntdll.dll", "crypt32.dll", "setupapi.dll",
}

LISEZMOI = """WARDROBE — cosmetiques Dota 2
=============================

INSTALLATION
Il n'y a rien a installer. Decompresse ce dossier ou tu veux, par exemple sur
le Bureau, et garde les fichiers ensemble.

UTILISATION
1. Lance Wardrobe.exe.
2. L'ecran d'accueil te dit ce qui va et ce qui ne va pas, et propose la seule
   action utile du moment. Suis-la.
3. Une fois Wardrobe actif, equipe tes objets dans le menu de Dota 2 comme
   d'habitude, puis lance une partie.
4. En jeu, la touche INSERT ouvre un panneau de diagnostic.

AU PREMIER LANCEMENT
Windows affichera « Windows a protege votre ordinateur » : le programme n'est
pas signe. Clique sur « Informations complementaires » puis « Executer quand
meme ». Ton antivirus peut aussi le signaler : le programme charge du code dans
Dota 2, ce qui ressemble de l'exterieur a ce que fait un logiciel malveillant.

CE QUE CA FAIT, ET CE QUE CA NE FAIT PAS
Les cosmetiques n'apparaissent que sur ton propre ecran. Rien n'est achete,
rien n'est ajoute a ton inventaire Steam, et les autres joueurs ne voient rien.
Ferme Dota 2 et il ne reste rien de charge.

Dota 2 peut toutefois garder en memoire des objets d'apercu et continuer a les
afficher apres coup. L'onglet Entretien contient « Restaurer l'inventaire
Steam » pour remettre les choses en place.

LE RISQUE, EN CLAIR
Dota 2 est protege par VAC. Ce programme charge du code dans le jeu : c'est
exactement le genre de chose qu'un anti-triche cherche. Personne ne peut te
garantir que ton compte ne sera pas sanctionne. Utilise-le en connaissance de
cause, et de preference pas sur un compte auquel tu tiens.

ENTRETIEN
L'onglet Entretien a besoin de Python pour certaines operations (mise a jour du
catalogue apres une mise a jour de Dota, regeneration du profil). Sans Python,
le reste fonctionne quand meme : l'application te le dira clairement.

Si le jeu affiche « client.dll non reconnu », c'est que Dota a change plus que
prevu. Redemande le paquet a la personne qui te l'a donne.
"""


def imports(path):
    try:
        import pefile
    except ImportError:
        return None
    binary = pefile.PE(str(path), fast_load=True)
    binary.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"]])
    return sorted({entry.dll.decode().lower() for entry in binary.DIRECTORY_ENTRY_IMPORT})


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", type=Path, default=None)
    parser.add_argument("--no-zip", action="store_true")
    args = parser.parse_args(argv)

    missing = [source for source, _ in REQUIRED if not (HERE / source).exists()]
    if missing:
        print("[!] manquant : " + ", ".join(missing))
        print("    Lance build.bat (et update_db.py si le catalogue manque) avant de packager.")
        return 1

    # The application locks its own binary while it runs, so a rebuild can fail
    # silently and leave build/ holding a stale exe. Packaging that would ship
    # yesterday's fixes without a word.
    newest = max((f.stat().st_mtime for f in (HERE / "src").rglob("*.cpp")), default=0)
    newest = max(newest, max((f.stat().st_mtime for f in (HERE / "src").rglob("*.h")), default=0))
    application = HERE / "build" / "Wardrobe.exe"
    if newest and application.stat().st_mtime < newest:
        print(r"[!] build\Wardrobe.exe est plus ancien que les sources.")
        print("    Ferme l'application si elle est ouverte, relance build.bat, puis reessaie.")
        return 1

    stamp = datetime.date.today().strftime("%Y-%m-%d")
    out = args.out or HERE / "build" / f"Wardrobe-{stamp}"
    if out.exists():
        shutil.rmtree(out)
    (out / "data").mkdir(parents=True)

    for source, destination in REQUIRED:
        shutil.copy2(HERE / source, out / destination)
    carried = []
    for name in OPTIONAL:
        if (HERE / name).exists():
            shutil.copy2(HERE / name, out / name)
            carried.append(name)
    (out / "LISEZMOI.txt").write_text(LISEZMOI, encoding="utf-8")

    # A dependency the other machine lacks turns into "il manque VCRUNTIME140.dll"
    # on their first double-click, so it is checked here rather than discovered there.
    unknown = {}
    for _, destination in REQUIRED:
        target = out / destination
        if target.suffix.lower() not in (".exe", ".dll"):
            continue
        needed = imports(target)
        if needed is None:
            print("[*] pefile absent : dependances non verifiees")
            break
        extra = [dll for dll in needed if dll not in SYSTEM_DLLS]
        if extra:
            unknown[destination] = extra
    if unknown:
        for binary, extra in unknown.items():
            print(f"[!] {binary} depend de {', '.join(extra)} — a installer sur l'autre machine")
        print("    Recompile avec /MT pour un binaire autonome, ou joins le redistribuable.")
        return 1

    size = sum(f.stat().st_size for f in out.rglob("*") if f.is_file())
    print(f"[OK] {out}")
    print(f"     {len(REQUIRED)} fichiers requis, {len(carried)} scripts d'entretien, {size / 1e6:.1f} Mo")
    print("     aucune dependance hors Windows : rien a installer en face")

    if not args.no_zip:
        archive = out.with_suffix(".zip")
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as bundle:
            for item in sorted(out.rglob("*")):
                if item.is_file():
                    bundle.write(item, item.relative_to(out.parent))
        print(f"[OK] {archive} ({archive.stat().st_size / 1e6:.1f} Mo) — c'est ce fichier que tu envoies")
    return 0


if __name__ == "__main__":
    sys.exit(main())
