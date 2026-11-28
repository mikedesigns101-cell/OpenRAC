# OpenRAC: Commando — building and publishing the launcher

The launcher updates itself (Settings → Updates → **Check for updates**, and at every start) from
the releases of `mikedesigns101-cell/OpenRAC-Commando`, with Tauri's updater:

    https://github.com/mikedesigns101-cell/OpenRAC-Commando/releases/latest/download/latest.json

`/releases/latest/` is the newest **full release** (not a pre-release, not a draft). So:

- Every launcher release is published as a **full release, marked Latest**, with three assets:
  the installer, its `.sig` and `latest.json`. A full release without `latest.json` breaks the
  updater for everyone until the next one.
- Anything else (test ZIPs, betas without the launcher) goes out as a **pre-release**: the updater
  does not see those.
- Do not point the updater at a fixed tag (`/releases/download/<tag>/latest.json`): the launcher
  takes such an address for a development channel (core/src/update.rs).

## The mod inside

The launcher carries Commando Protocol in its copy (`mods/commando-protocol`) and installs it again
at Play when the installed one has another version (`needsInstall` in `src/lib/commando.ts`; the
player's settings and switch are kept). The comparison is by version number alone, so **every
release that changes the mod raises its version** in two places at once:

1. `version` in the mod's `openrac-mod.json` (the mod project, on its main branch);
2. `modVersion` in `src/lib/commando.ts`.

`tools/bundle_standalone.py --mod` refuses a mod whose version is not `modVersion`.

## One command (steps 2 to 5)

With the versions raised and committed (step 1, and the mod's `version` on the project's main):

    python -X utf8 launcher/tools/commando/release_commando.py --main <commit of the mod project's main> --clave <updater private key> --notas "<what changed>"

It makes a worktree of the project at that commit, compiles the mod against the SDK of the game build's own
commit, builds the beta download, the launcher's copy and the signed installer, and checks them (nothing from the
disc, no user name or `C:\Users` path). The files land in `<project>/build/launcher-commando/<version>/` with a
`RELEASE.txt` that says which main and which game build went in. `--exe-dir` picks the game build
(default: the one validated for 0.2.1, `build/commando-port/arreglos-78195faa3`, commit 78195faa3 of
`openrac-cmd-gadgets`, branch `commando/arreglos`). Then step 6.

## Steps

1. Raise `version` in `launcher/src-tauri/tauri.conf.json` (the updater only offers a newer one)
   and, if the mod changed, both mod versions above. Commit.
2. In the mod project: `python -X utf8 tools/beta/armar_descarga.py --sin-zip` (the mod of main
   without models from the disc, in `build/beta/OpenRAC Commando/mod/commando-protocol`).
3. `python -X utf8 launcher/tools/commando/preparar_copia.py --proyecto <mod project> --trabajo <scratch folder>`
4. `python -X utf8 launcher/tools/commando/compilar.py --clave <updater private key> --salida <folder> --notas "<what changed>"`
   (Rust from `%USERPROFILE%\.cargo\bin`; the key never goes into a repository).
5. Check the build: nothing from the disc, no user name or `C:\Users` path in
   `target/release/openrac-commando.exe` and in `src-tauri/resources/openrac` (ASCII and UTF-16).
6. On GitHub, in `OpenRAC-Commando`: a new **full** release with tag `v<version>` (exactly the tag
   `compilar.py` wrote into `latest.json`; `--tag` changes it), marked Latest, with the three files
   of `--salida`:
   - `OpenRAC-Commando_<version>_x64-setup.exe`
   - `OpenRAC-Commando_<version>_x64-setup.exe.sig`
   - `latest.json`

   For example: `gh release create v<version> --repo mikedesigns101-cell/OpenRAC-Commando --title "OpenRAC: Commando <version>" --notes-file notes.md --latest <the three files>`.

7. Check: `https://github.com/mikedesigns101-cell/OpenRAC-Commando/releases/latest/download/latest.json`
   answers with the new version, and an installed older launcher offers it under Check for updates.

The key's public half is `plugins.updater.pubkey` in `tauri.conf.json`. Losing the private key
means installed launchers can never update again (they only accept files signed with it): keep a
copy of it somewhere safe outside this computer.
