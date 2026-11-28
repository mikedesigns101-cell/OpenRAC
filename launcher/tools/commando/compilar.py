#!/usr/bin/env python3
"""Builds OpenRAC: Commando's signed NSIS installer and the files of its release.

    python -X utf8 launcher/tools/commando/compilar.py --clave <private key file> --salida <folder>

Run launcher/tools/commando/preparar_copia.py first (the copy of OpenRAC the installer carries).
- The paths of this computer are mapped out of the programs (CARGO_ENCODED_RUSTFLAGS with
  --remap-path-prefix for the fork, Cargo's registry and rustup), so no user name goes in.
- Tauri signs the installer with the updater's private key (TAURI_SIGNING_PRIVATE_KEY is given the
  key's PATH; its password, empty unless TAURI_SIGNING_PRIVATE_KEY_PASSWORD is set).
- In --salida: the installer and its .sig under a name without spaces (GitHub turns spaces in an
  asset's name into dots, which would break the address in latest.json), and latest.json, Tauri's
  update manifest, pointing at the release --tag (default v<version>) of --repo.
Nothing is published: see launcher/tools/commando/README.md for that.
"""
import argparse
import datetime
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

FORK = Path(__file__).resolve().parents[3]
LAUNCHER = FORK / "launcher"
CONF = LAUNCHER / "src-tauri" / "tauri.conf.json"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--clave", type=Path, required=True, help="the updater's private key (tauri signer generate)")
    ap.add_argument("--salida", type=Path, required=True, help="where the release's files go")
    ap.add_argument("--repo", default="mikedesigns101-cell/OpenRAC-Commando")
    ap.add_argument("--tag", help="the release's tag; default v<version of tauri.conf.json>")
    ap.add_argument("--notas", default="", help="the release notes the update window shows")
    ap.add_argument("--sin-compilar", action="store_true", help="only the release's files, from the last build")
    a = ap.parse_args()

    if not a.clave.is_file():
        sys.exit(f"no key at {a.clave}")
    conf = json.loads(CONF.read_text(encoding="utf-8"))
    version, product = conf["version"], conf["productName"]
    tag = a.tag or f"v{version}"
    home = Path(os.environ["USERPROFILE"])

    if not a.sin_compilar:
        env = dict(os.environ)
        env["PATH"] = str(home / ".cargo" / "bin") + os.pathsep + env.get("PATH", "")
        remaps = [(FORK, "/openrac"), (home / ".cargo", "/cargo"), (home / ".rustup", "/rustup")]
        env["CARGO_ENCODED_RUSTFLAGS"] = "\x1f".join(f"--remap-path-prefix={src}={dst}" for src, dst in remaps)
        env["TAURI_SIGNING_PRIVATE_KEY"] = str(a.clave.resolve())
        env.setdefault("TAURI_SIGNING_PRIVATE_KEY_PASSWORD", "")
        corepack = shutil.which("corepack") or sys.exit("corepack is not on the PATH")
        r = subprocess.run([corepack, "yarn", "tauri", "build", "--bundles", "nsis"], cwd=LAUNCHER, env=env)
        if r.returncode:
            return r.returncode

    nsis = LAUNCHER / "target" / "release" / "bundle" / "nsis"
    setup = nsis / f"{product}_{version}_x64-setup.exe"
    sig = setup.with_name(setup.name + ".sig")
    for f in (setup, sig):
        if not f.is_file():
            sys.exit(f"missing: {f}")

    out = a.salida.resolve()
    out.mkdir(parents=True, exist_ok=True)
    name = f"{product.replace(' ', '-')}_{version}_x64-setup.exe"
    shutil.copy2(setup, out / name)
    shutil.copy2(sig, out / (name + ".sig"))
    signature = sig.read_text(encoding="utf-8").strip()
    url = f"https://github.com/{a.repo}/releases/download/{tag}/{name}"
    entry = {"signature": signature, "url": url}
    manifest = {
        "version": version,
        "notes": a.notas,
        "pub_date": datetime.datetime.now(datetime.timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z"),
        "platforms": {"windows-x86_64": entry, "windows-x86_64-nsis": entry},
    }
    (out / "latest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"{out}:\n  {name}\n  {name}.sig\n  latest.json -> {url}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
