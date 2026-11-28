// OpenRAC: Commando: the OpenRAC Launcher made into the launcher of one game, Ratchet & Clank:
// Going Commando (NTSC-U v1.01, rac2/ntsc), and one mod, RAC 2: Commando Protocol. Everything
// that makes this launcher differ from OpenRAC's is named here, and the pages ask `COMMANDO.on`
// before they show what this launcher hides (the other games, Develop, the level editor, the mod
// hub, Steam, multiplayer, Discord), so that the rest of the code stays OpenRAC's and merges.
//
// tools/bundle_standalone.py --mod reads `id` and `modVersion` below: the mod package it carries
// (mods/<id>/ in the launcher's copy of OpenRAC) must be that version, or the copy is refused.

import { api } from "./api";
import type { Config } from "./generated/Config";
import type { PluginView } from "./generated/PluginView";

export const COMMANDO = {
  /** A boolean, not `true`, so that the OpenRAC branches beside it still type-check and lint. */
  on: true as boolean,
  name: "OpenRAC: Commando",
  /** The game and the disc this launcher plays. */
  game: "rac2",
  version: "ntsc",
  versionKey: "rac2/ntsc",
  mod: {
    id: "commando-protocol",
    name: "RAC 2: Commando Protocol",
    modVersion: "0.2.1",
  },
  /** The fork this launcher and its game are built from (GPL-3.0-or-later: its source). */
  sourceUrl: "https://github.com/mikedesigns101-cell/OpenRAC",
  issuesUrl: "https://github.com/mikedesigns101-cell/OpenRAC/issues",
  /** The project it derives from. */
  upstreamUrl: "https://github.com/OpenRAC/OpenRAC",
  upstreamSite: "https://openrac.dev",
} as const;

/** The step after the levels that builds the mod's models (bundle_standalone.py mod_models_action). */
export const MOD_MODELS = "mod-models";

/**
 * The settings this launcher keeps whatever was saved: mods always go to the game (the mod's own
 * switch, on the Mods tab, turns Commando Protocol on or off), no Discord presence (it would show
 * OpenRAC's app), no developer tools (they work on a checkout, which players do not have). Null
 * when the config is already so.
 */
export function commandoConfig(config: Config): Config | null {
  if (!COMMANDO.on) return null;
  // The saves: a folder of this launcher's own, in its install folder, unless the player chose one
  // (Settings): the game's default, <data home>/memcard, is the OpenRAC Launcher's too, and a
  // modded game's saves are kept apart from the unmodded game's.
  const saveDir = config.saveDir ?? (config.installDir ? joinPath(config.installDir, "memcard") : null);

  if (config.playWithMods && !config.discordRpc && !config.developer && saveDir === config.saveDir) return null;

  return { ...config, playWithMods: true, discordRpc: false, developer: false, saveDir };
}

/** DIR/NAME with DIR's own separator. */
function joinPath(dir: string, name: string): string {
  const sep = dir.includes("\\") && !dir.includes("/") ? "\\" : "/";

  return `${dir.replace(/[\\/]+$/, "")}${sep}${name}`;
}

/** The mod as installed in the game's set-up folder, if it is. */
export function commandoMod(mods: PluginView[]): PluginView | undefined {
  return mods.find((m) => m.id === COMMANDO.mod.id);
}

/**
 * Whether the carried mod is to be installed (again): it is not installed, or installed at another
 * version than the one this launcher carries (a launcher update brought a new one).
 */
export function needsInstall(mods: PluginView[]): boolean {
  const mod = commandoMod(mods);

  return !mod || mod.version !== COMMANDO.mod.modVersion;
}

/**
 * Installs the carried mod into the game's set-up folder when it is not there or is another
 * version (needsInstall), from `root`, the launcher's copy of OpenRAC; the mods as they are after.
 * The game must be set up (its data folder is where mods go). A mod installed again loses the
 * models built into its folder, and with them modelos_construidos.json: Play builds them again
 * (MOD_MODELS). Rejects with the core's reason.
 */
export async function ensureCommandoMod(root: string): Promise<PluginView[]> {
  const { game, version } = COMMANDO;
  let mods = await api.pluginsList(game, version);

  if (!needsInstall(mods)) return mods;
  const wasOn = commandoMod(mods)?.enabled;

  await api.pluginsInstall(game, version, carriedModPath(root));
  mods = await api.pluginsList(game, version);
  const mod = commandoMod(mods);

  // A first install is switched on; an update keeps the player's choice.
  if (mod && mod.enabled !== (wasOn ?? true)) {
    mods = await api.pluginsSetEnabled(game, version, mod.id, wasOn ?? true);
  }

  return mods;
}

/** The carried mod's folder in the launcher's copy of OpenRAC (`root`): <root>/mods/<id>. */
export function carriedModPath(root: string): string {
  return joinPath(joinPath(root, "mods"), COMMANDO.mod.id);
}
