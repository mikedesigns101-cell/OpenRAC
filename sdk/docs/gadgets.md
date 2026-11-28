# Gadgets: a mod's own items in the hero's inventory

A **gadget** of a mod's is an item of the game's own: the hero has it or not, it is listed in the
pause menu's Gadgets page and in the quick select, he holds it in his hand, and pressing the hand's
button with it there is an event the mod gets. What he has is kept with the save. Native mods,
OpenRAC Commando's interface 1.124, Going Commando only (`gadget_count()` stays 0 elsewhere).

## How the port does it (Going Commando)

The game knows 56 items, and every array of them (owned, new, ammo, experience, the save) is 56
long, so a gadget is not a new row: the port **lends it one of the item numbers the game leaves
unused** (33, 34, 40, 11, 35 and 1, in that order; six gadgets for all the mods together). Each time
a level's program loads, the port writes that item's record as the record of the gadget's **host**
(an item the game holds in the hand; the Electrolyzer, 38, by default): the hand's model class, its
joint and the hero's animations are the host's, with the gadget's own name, description and icon,
no price and no ammo. The item goes in the one free cell of the pause menu's Gadgets grid (the
first owned gadget's; the game's cursor does not go there, so the gadget is chosen from the quick
select, which giving it fills, or by the mod: `gadget_equip`).

In the hand the game makes a moby of the host's class; the port draws it with the gadget's mesh.
The game does nothing when the hand's button (the fire button) is pressed with the item there: the
port reports it as `OPENRAC_GADGET_USED`, with where the hand's model is and the way the hero faces,
and the mod decides what happens (near which panel, door or point).

**Saves.** A save never holds a lent item: while the game writes one (every save, and the port's
own captures), the port takes the lent numbers out of the owned and new bytes, the quick select,
the hand and the last weapon, and puts them back after. What the hero owns goes beside the save
with the mods' data (`openrac.gadgets/<mod id>`: the keys he owns), and comes back when that save
is loaded with the mod. A save loads without the mod as if the gadget never existed. A save made
with a gadget in the hand starts with the Lancer (or the weapon held before) in the hand.

## The calls

| Call | What |
|---|---|
| `gadget_register(mod, &gadget)` | Only while the mod loads. The gadget's number, `OPENRAC_HOOK_FULL` (no item number left) or an error |
| `gadget_give(mod, gadget, owned)` | Gives it (owned, new, in the quick select) or takes it away (and out of the hand) |
| `gadget_equip(mod, gadget)` | Asks the owned gadget into the hand, as the quick select does |
| `gadget_count()`, `gadget_info(gadget, &info)` | Every mod's gadgets: whose, key, name, description, icon, item, `OPENRAC_GADGET_OWNED` / `_HELD` |
| `gadget_held()` | The gadget in the hand, -1 for none |
| `on_gadget(mod, fn, user)` | Only while the mod loads: `fn(user, event)` for its gadgets' events, before the frame callbacks |

`openrac_gadget` (the mod fills it, the port copies it):

| Field | What |
|---|---|
| `key` | The mod's own name for it, kept with the save: letters, digits, `_`, under 32 |
| `name`, `description` | ASCII, under 96 each: the pause list's name and line, the quick select's name |
| `host_item` | The game item whose hand model and animations it borrows (0: the Electrolyzer, 38) |
| `icon` | A picture of the mod's (`overlay_image_create`), drawn where the game draws the item's icon; 0 the host's |
| `mesh`, `mesh_matrix` | A `.glb` in the mod's folder drawn as the hand's model, placed by the matrix (column-major, all 0 for none) |

Events (`openrac_gadget_event.kind`): `OPENRAC_GADGET_EQUIPPED`, `_PUT_AWAY`, `_USED`.

```cpp
int chip = -1;

void on_chip(void*, const openrac_gadget_event* e) {
    if (e->kind == OPENRAC_GADGET_USED && near_panel(e->position)) {
        open_seal();
    }
}

OPENRAC_MOD_LOAD {
    openrac_gadget g{};
    g.size = sizeof g;
    g.key = "override_chip";
    g.name = "Override Chip";
    g.description = "Megacorp's security override.";
    g.icon = chip_icon;                  // overlay_image_create
    g.mesh = "models/override_chip.glb";
    chip = mod.gadget_register(g);
    mod.on_gadget(on_chip);
}
```

## A menu of the mod's own

`gadget_count()` and `gadget_info()` give a menu of the mod's everything the game's pause list
shows: name, description, owned, held. The icon's picture can only be drawn by the mod that made it
(`openrac_overlay.image`), so a menu lists other mods' gadgets by name.

## Limits

- Six gadgets in all, for every mod together; a seventh registration answers `OPENRAC_HOOK_FULL`.
- One cell in the game's Gadgets page (the first owned gadget's); the rest are in the quick select.
- The hand's animations (taking it out, holding it) are the host's: the mesh is rigid on the hand.
- The game's vendors do not sell it.
- Going Commando NTSC-U (v1.01) only for now.
