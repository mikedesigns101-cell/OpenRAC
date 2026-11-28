<script lang="ts">
  // OpenRAC: Commando's Mod tab: its one mod, RAC 2: Commando Protocol, on its own page (ModPage,
  // as the Mods tab shows a mod): the switch that turns it on or off for the next Play, and its
  // settings. The mod comes with the launcher and is installed into the game's set-up folder
  // (lib/commando.ts), so there is nothing to add, update or remove here, and no other mods.
  import ModPage from "$components/ModPage.svelte";
  import Reason from "$components/Reason.svelte";
  import { api } from "$lib/api";
  import { app, guardShown } from "$lib/app.svelte";
  import { COMMANDO, commandoMod, ensureCommandoMod, MOD_MODELS } from "$lib/commando";
  import type { PluginView } from "$lib/generated/PluginView";

  const game = $derived(app.library?.games.find((g) => g.id === COMMANDO.game) ?? null);
  const version = $derived(game?.versions.find((v) => v.key === COMMANDO.versionKey) ?? null);
  /** The disc is set up: the mod has a folder to go in. */
  const setUp = $derived(!!game?.install?.extracted);
  const models = $derived(
    version?.actions.find((a) => a.id === MOD_MODELS && a.thisPlatform) ?? null,
  );

  let mods = $state<PluginView[]>([]);
  let failure = $state<string | null>(null);
  let loading = $state(true);
  const mod = $derived(commandoMod(mods));

  async function load() {
    const root = app.config?.root;

    if (!setUp || !root) {
      loading = false;

      return;
    }

    try {
      mods = await ensureCommandoMod(root);
      failure = null;
    } catch (e) {
      failure = typeof e === "string" ? e : String(e);
    } finally {
      loading = false;
    }
  }

  $effect(() => {
    if (setUp) void load();
  });

  async function setOn(on: boolean) {
    if (!mod) return;

    try {
      mods = on
        ? (await api.pluginsTurnOn(COMMANDO.game, COMMANDO.version, mod.id)).views
        : await api.pluginsSetEnabled(COMMANDO.game, COMMANDO.version, mod.id, false);
      failure = null;
    } catch (e) {
      failure = typeof e === "string" ? e : String(e);
      await load();
    }
  }

  async function saveSetting(key: string, value: unknown) {
    if (!mod) return;
    mods = await api.pluginsSetSetting(COMMANDO.game, COMMANDO.version, mod.id, key, value);
  }
</script>

<div class="commando-mod">
  {#if !setUp}
    <section class="panel note">
      <h2>{COMMANDO.mod.name}</h2>
      <p class="muted">
        Set the game up from your disc first (the Game tab): the mod is installed beside it, and its
        settings show here.
      </p>
    </section>
  {:else if loading}
    <section class="panel note"><p class="muted">Reading the mod…</p></section>
  {:else if mod}
    <ModPage
      {mod}
      standalone
      onsave={saveSetting}
      onenable={(on) => void setOn(on)}
      onupdate={() => undefined}
      onpin={() => undefined}
      onfolder={() => void guardShown(api.openOutput(mod.path))}
      onhomepage={() => void guardShown(api.openUrl(mod.homepage))}
      onremove={() => undefined}
    />
    <p class="models muted">
      {#if models?.done}
        The mod's models are built from your disc.
      {:else if models}
        The mod's models are built from your disc before the next Play (a minute or two, once).
      {:else}
        This launcher carries no model builder: the mod uses the game's own models.
      {/if}
    </p>
  {/if}
  {#if failure}<Reason text={failure} tone="err" role="status" />{/if}
</div>

<style>
  .commando-mod {
    display: flex;
    flex-direction: column;
    gap: 14px;
    max-width: 980px;
    margin: 0 auto;
    padding: 8px 4px 24px;
  }

  .note h2 {
    margin: 0 0 6px;
  }

  .models {
    margin: 0;
    font-size: 0.9em;
  }
</style>
