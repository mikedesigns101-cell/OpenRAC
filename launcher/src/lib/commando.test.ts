import { describe, expect, it } from "vitest";

import { commandoConfig } from "./commando";
import type { Config } from "./generated/Config";

/** A config with only what commandoConfig reads. */
function config(fields: Partial<Config>): Config {
  return {
    playWithMods: true,
    discordRpc: false,
    developer: false,
    installDir: "C:\\Games\\OpenRAC Commando",
    saveDir: null,
    ...fields,
  } as Config;
}

describe("commandoConfig", () => {
  it("keeps the saves in <install folder>\\saves", () => {
    expect(commandoConfig(config({}))?.saveDir).toBe("C:\\Games\\OpenRAC Commando\\saves");
  });

  it("moves 0.1.0's <install folder>\\memcard, which the launcher moves away at start", () => {
    const old = config({ saveDir: "C:\\Games\\OpenRAC Commando\\memcard" });

    expect(commandoConfig(old)?.saveDir).toBe("C:\\Games\\OpenRAC Commando\\saves");
  });

  it("keeps a folder the player chose", () => {
    expect(commandoConfig(config({ saveDir: "D:\\Saves" }))).toBeNull();
  });
});
