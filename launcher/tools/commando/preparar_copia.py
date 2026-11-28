#!/usr/bin/env python3
"""Puts OpenRAC: Commando's copy of OpenRAC in launcher/src-tauri/resources/openrac (what the
installer carries), from the mod project's folder. Nothing from the game's disc goes in.

    python -X utf8 launcher/tools/commando/preparar_copia.py --proyecto <mod project> --trabajo <empty folder>

Steps (the --trabajo folder holds what they stage; it is emptied first):
1. Python: the standard library plus numpy and Pillow, trimmed by the project's
   tools/beta/armar_descarga.py (copiar_python), from --python-origen.
2. The game: openrac-rac2-ntsc.exe and openrac-shaders.dll from --exe-dir (the project's
   build/openrac-commando), with the port's third-party notices and numpy's and Pillow's.
3. The model builder: tools/beta/constructor of the project's --constructor branch (git archive:
   only what is committed), without tests.
4. tools/bundle_standalone.py with --only rac2/ntsc, the three above and --mod, the mod of the beta
   download (the project's build/beta/OpenRAC Commando/mod/commando-protocol, made by
   tools/beta/armar_descarga.py from the mod built on the project's main: no models from the disc).

Nothing here runs Python inside the staged or carried copy (it would leave .pyc files there).
"""
import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

FORK = Path(__file__).resolve().parents[3]
COPY = FORK / "launcher" / "src-tauri" / "resources" / "openrac"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--proyecto", type=Path, required=True, help="the mod project (tools/beta, build/)")
    ap.add_argument("--trabajo", type=Path, required=True, help="a folder of this script's own (emptied)")
    ap.add_argument("--python-origen", type=Path,
                    default=Path(os.environ.get("LOCALAPPDATA", "")) / "Python" / "pythoncore-3.14-64",
                    help="a Windows Python with numpy 2.5.4 and Pillow 12.3.0 (only read)")
    ap.add_argument("--exe-dir", type=Path, help="default: <proyecto>/build/openrac-commando")
    ap.add_argument("--mod", type=Path, help="default: <proyecto>/build/beta/OpenRAC Commando/mod/commando-protocol")
    ap.add_argument("--licencias-port", type=Path,
                    help="the port's third-party notices; default: those of the beta download "
                         "(<proyecto>/build/beta/OpenRAC Commando/licenses/openrac/licenses)")
    ap.add_argument("--constructor", default="sub/beta-constructor", help="branch with tools/beta/constructor")
    a = ap.parse_args()

    proyecto = a.proyecto.resolve()
    beta = proyecto / "build" / "beta" / "OpenRAC Commando"
    exe_dir = a.exe_dir or proyecto / "build" / "openrac-commando"
    mod = a.mod or beta / "mod" / "commando-protocol"
    lic_port = a.licencias_port or beta / "licenses" / "openrac" / "licenses"
    for need in (exe_dir / "openrac-rac2-ntsc.exe", exe_dir / "openrac-shaders.dll", mod / "openrac-mod.json",
                 lic_port, proyecto / "tools" / "beta" / "armar_descarga.py"):
        if not need.exists():
            sys.exit(f"missing: {need}")
    for banned in ("armas", "escuadrones", "amalgamas", "oozla"):
        if (mod / banned).exists():
            sys.exit(f"{mod} has {banned}/ (models or levels from the disc): use the beta download's mod")

    work = a.trabajo.resolve()
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)

    # 1. Python.
    sys.path.insert(0, str(proyecto / "tools" / "beta"))
    import armar_descarga  # noqa: E402  (the project's; it only copies)
    python, py_lic = work / "python", work / "python-licenses"
    py_lic.mkdir()
    armar_descarga.copiar_python(a.python_origen, python, py_lic)
    print("1. python:", python)

    # 2. The game.
    game = work / "game"
    game.mkdir()
    for name in ("openrac-rac2-ntsc.exe", "openrac-shaders.dll"):
        shutil.copy2(exe_dir / name, game / name)
    shutil.copytree(lic_port, game / "licenses")
    shutil.copytree(py_lic, game / "licenses" / "python-packages")
    print("2. game:", game, "from", exe_dir, "commit", (exe_dir / "COMMIT.txt").read_text(errors="replace").strip())

    # 3. The builder.
    builder = work / "builder"
    builder.mkdir()
    commit = armar_descarga.constructor_de_git(a.constructor, builder)
    print("3. builder:", builder, f"({a.constructor} {commit})")

    # 4. The copy.
    cmd = [sys.executable, "-X", "utf8", str(FORK / "tools" / "bundle_standalone.py"), str(COPY),
           "--only", "rac2/ntsc", "--game-build", str(game), "--python", str(python),
           "--mod", str(mod), "--builder", str(builder)]
    r = subprocess.run(cmd, cwd=FORK)
    if r.returncode:
        return r.returncode
    pyc = [p for p in COPY.rglob("*.pyc")]
    if pyc:
        sys.exit(f"the copy has {len(pyc)} .pyc files (paths of this computer): {pyc[0]}")
    print("4. copy:", COPY)
    return 0


if __name__ == "__main__":
    sys.exit(main())
