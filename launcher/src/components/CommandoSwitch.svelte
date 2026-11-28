<script lang="ts">
  // OpenRAC: Commando's switch under Play, in ModsSwitch's place: the mod on or off for the next
  // Play (its own switch, which the Mod tab shows too), and beside it the way to its settings.
  // Before the game is set up there is no mod to switch (it is installed beside the game).
  import { api } from "$lib/api";
  import { app, toast } from "$lib/app.svelte";
  import { COMMANDO, commandoMod, ensureCommandoMod } from "$lib/commando";

  import Icon from "./Icon.svelte";

  let { setUp, onopen }: { setUp: boolean; onopen: () => void } = $props();

  /** The mod's state as last read: null until read, or while the game is not set up. */
  let on = $state<boolean | null>(null);
  let busy = $state(false);

  $effect(() => {
    const root = app.config?.root;

    on = null;
    if (!setUp || !root) return;
    let current = true;

    void ensureCommandoMod(root).then(
      (mods) => {
        if (current) on = commandoMod(mods)?.enabled ?? null;
      },
      () => undefined,
    );

    return () => {
      current = false;
    };
  });

  async function set(wanted: boolean) {
    busy = true;

    try {
      const mods = wanted
        ? (await api.pluginsTurnOn(COMMANDO.game, COMMANDO.version, COMMANDO.mod.id)).views
        : await api.pluginsSetEnabled(COMMANDO.game, COMMANDO.version, COMMANDO.mod.id, false);

      on = commandoMod(mods)?.enabled ?? null;
    } catch (e) {
      toast(typeof e === "string" ? e : String(e), "error");
    } finally {
      busy = false;
    }
  }
</script>

{#if setUp}
  <div class="with-mods">
    <label title="Play with RAC 2: Commando Protocol, or the game as it is"
      >{COMMANDO.mod.name}<input
        type="checkbox"
        role="switch"
        class="switch"
        checked={on ?? false}
        disabled={on === null || busy}
        onchange={(e) => void set(e.currentTarget.checked)}
      /></label
    >
    <button class="link" onclick={onopen}>Settings<Icon name="next" size={12} /></button>
  </div>
{/if}

<style>
  /* ModsSwitch's quiet pill: the switch, then the way to the mod's settings. */
  .with-mods {
    display: inline-flex;
    align-items: center;
    gap: 10px;
    margin-top: 12px;
    padding: 6px 14px;
    border-radius: var(--radius-sm);
    background: var(--control);
    font-size: 0.875rem;
    color: var(--soft);
  }

  label {
    display: inline-flex;
    align-items: center;
    gap: 10px;
    cursor: pointer;
  }

  .link {
    display: inline-flex;
    align-items: center;
    gap: 2px;
    min-height: 0;
    margin: -8px -10px;
    padding: 8px 10px;
    background: none;
    color: inherit;
    font-weight: 500;
    transition: opacity var(--fast);
  }

  .link:active {
    filter: none;
    opacity: 0.7;
  }
</style>
