#!/usr/bin/env python3
"""One command for a release of OpenRAC: Commando: from a commit of the mod project's main and a validated
game build, the beta download, the launcher's copy and the signed installer, checked. Nothing is published.

    python -X utf8 launcher/tools/commando/release_commando.py --main <commit> --clave <updater key> [--notas "..."]

Steps (each stops the run if it fails):
1. The mod project at --main, in a worktree of its own (<proyecto>/.worktrees/release-<commit>, detached; made
   again if it is there), and the port's SDK of the game build's commit (git archive of --port-repo).
2. The mod compiled there against that SDK (tools/compilar_mod.bat with SDK_MOD and PY_MOD): build/mod.
3. Its beta download (tools/beta/armar_descarga.py --sin-zip, with the game build and the port's tools of the
   same commit): <worktree>/build/beta-release/OpenRAC Commando. Its own check says nothing from the disc.
4. The launcher's copy of OpenRAC (preparar_copia.py) with that game build, that mod and the beta's notices.
   The mod's version has to be modVersion in src/lib/commando.ts (bundle_standalone.py refuses it otherwise).
5. The signed installer, its .sig and latest.json (compilar.py) in --salida
   (default <proyecto>/build/launcher-commando/<version of tauri.conf.json>).
6. No user name and no C:\\Users\\<user> path (ASCII or UTF-16) in the launcher's program, its copy and --salida.

--exe-dir: the game build (openrac-rac2-ntsc.exe, openrac-shaders.dll and COMMIT.txt, whose first word is its
commit in --port-repo). Default: the one validated for 0.2.1, <proyecto>/build/commando-port/arreglos-78195faa3.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import io
from pathlib import Path

FORK = Path(__file__).resolve().parents[3]
LAUNCHER = FORK / "launcher"
PROYECTO = FORK.parent


def run(cmd, **kw):
    print("$", " ".join(str(c) for c in cmd), flush=True)
    r = subprocess.run([str(c) for c in cmd], **kw)
    if r.returncode:
        sys.exit(f"failed ({r.returncode}): {cmd[0]}")
    return r


def git(repo, *args):
    return subprocess.run(["git", "-C", str(repo), *args], check=True, capture_output=True, text=True).stdout.strip()


def clean(paths):
    user = os.environ.get("USERNAME", "")
    needles = []
    for p in {user, "C:\\Users\\" + user, "C:/Users/" + user}:
        if len(p) > 2:
            for v in {p, p.lower()}:
                needles += [v.encode("ascii"), v.encode("utf-16-le")]
    found, n = [], 0
    for root in paths:
        files = [root] if root.is_file() else [f for f in root.rglob("*") if f.is_file()]
        for f in files:
            n += 1
            data = f.read_bytes()
            if any(b in data for b in needles):
                found.append(f)
    print(f"6. {n} files checked for the user's name and paths: {'clean' if not found else found[:5]}")
    return not found


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--main", required=True, help="the commit of the mod project's main to release")
    ap.add_argument("--clave", type=Path, required=True, help="the updater's private key (never copied anywhere)")
    ap.add_argument("--proyecto", type=Path, default=PROYECTO)
    ap.add_argument("--exe-dir", type=Path)
    ap.add_argument("--port-repo", type=Path, help="the port's checkout with the game build's commit "
                    "(default <proyecto>/openrac-cmd-gadgets)")
    ap.add_argument("--python", type=Path,
                    default=Path(os.environ.get("LOCALAPPDATA", "")) / "Python" / "pythoncore-3.14-64" / "python.exe",
                    help="a Windows Python with Pillow (the mod's effects) and numpy 2.5.4 / Pillow 12.3.0")
    ap.add_argument("--salida", type=Path)
    ap.add_argument("--notas", default="")
    a = ap.parse_args()

    proyecto = a.proyecto.resolve()
    exe_dir = (a.exe_dir or proyecto / "build" / "commando-port" / "arreglos-78195faa3").resolve()
    port = (a.port_repo or proyecto / "openrac-cmd-gadgets").resolve()
    exe_commit = (exe_dir / "COMMIT.txt").read_text(encoding="utf-8", errors="replace").split()[0]
    exe_commit = git(port, "rev-parse", "--short=9", exe_commit)
    main_commit = git(proyecto, "rev-parse", "--short=7", a.main)
    version = json.loads((LAUNCHER / "src-tauri" / "tauri.conf.json").read_text(encoding="utf-8"))["version"]
    mod_version = re.search(r'modVersion:\s*"([^"]+)"', (LAUNCHER / "src" / "lib" / "commando.ts").read_text(encoding="utf-8")).group(1)
    salida = (a.salida or proyecto / "build" / "launcher-commando" / version).resolve()
    print(f"launcher {version} (mod {mod_version}), main {main_commit}, game {exe_commit} from {exe_dir}", flush=True)

    # 1. The project at main, and the SDK of the game's commit.
    wt = proyecto / ".worktrees" / f"release-{main_commit}"
    if wt.exists():
        subprocess.run(["git", "-C", str(proyecto), "worktree", "remove", "--force", str(wt)], check=False)
    run(["git", "-C", proyecto, "worktree", "add", "--detach", wt, main_commit])
    sdk = wt / "build" / f"sdk-{exe_commit}"
    sdk.mkdir(parents=True, exist_ok=True)
    tar = subprocess.run(["git", "-C", str(port), "archive", exe_commit, "sdk"], check=True, capture_output=True).stdout
    tarfile.open(fileobj=io.BytesIO(tar)).extractall(sdk)
    manifest = json.loads((wt / "mod" / "commando-protocol" / "openrac-mod.json").read_text(encoding="utf-8"))
    if manifest["version"] != mod_version:
        sys.exit(f"main's mod is {manifest['version']}, the launcher's modVersion {mod_version}: raise one of them")
    print("1. project", wt, "SDK", sdk / "sdk", flush=True)

    # 2. The mod against that SDK.
    env = dict(os.environ, SDK_MOD=str(sdk / "sdk").replace("\\", "/"), PY_MOD=str(a.python).replace("\\", "/"),
               PYTHON_MOD=str(a.python).replace("\\", "/"))
    run(["cmd", "/c", str(wt / "tools" / "compilar_mod.bat")], cwd=wt, env=env)
    mod = wt / "build" / "mod" / "package" / "commando_protocol"
    print("2. mod", mod, flush=True)

    # 3. The beta download.
    beta = wt / "build" / "beta-release" / "OpenRAC Commando"
    run([a.python, "-X", "utf8", wt / "tools" / "beta" / "armar_descarga.py", "--sin-zip", "--salida", beta,
         "--commit", exe_commit, "--exe-dir", exe_dir, "--fork", port, "--mod", mod])
    print("3. beta", beta, flush=True)

    # 4. The launcher's copy.
    run([a.python, "-X", "utf8", LAUNCHER / "tools" / "commando" / "preparar_copia.py", "--proyecto", wt,
         "--trabajo", wt / "build" / "launcher-trabajo", "--exe-dir", exe_dir,
         "--mod", beta / "mod" / "commando-protocol", "--licencias-port", beta / "licenses" / "openrac" / "licenses"])

    # 5. The installer.
    run([a.python, "-X", "utf8", LAUNCHER / "tools" / "commando" / "compilar.py", "--clave", a.clave,
         "--salida", salida, "--notas", a.notas])

    # 6. Clean of this computer's user.
    ok = clean([LAUNCHER / "target" / "release" / "openrac-commando.exe",
                LAUNCHER / "src-tauri" / "resources" / "openrac", salida])
    (salida / "RELEASE.txt").write_text(
        f"OpenRAC: Commando {version}\nmod project main {main_commit} (Commando Protocol {mod_version})\n"
        f"game {exe_commit} ({exe_dir.name})\n", encoding="utf-8")
    print("done:", salida if ok else "NOT CLEAN, do not publish", flush=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
