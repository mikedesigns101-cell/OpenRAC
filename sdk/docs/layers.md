# Layers: meshes a moby wears over its own model

A **layer** is a mesh drawn over a moby's own model with the moby's pose: a chest plate, gloves,
boots. A moby wears up to eight at once (`OPENRAC_MOBY_LAYERS_MAX`), each in its own numbered layer,
put on and taken off alone without touching the others. Native mods, OpenRAC Commando's interface
1.126 (`has_moby_layers()`); the drawing is the common frontend's, so it works in every game the
port draws, Going Commando first.

```cpp
const std::uint32_t hero = /* the hero's moby, from the game's memory */;
std::uint32_t gloves_hide[OPENRAC_MOBY_LAYER_JOINT_WORDS] = {};
for (int j : {56, 76}) gloves_hide[j / 32] |= 1u << (j % 32);   // and the fingers under them
const int gloves = mod.model_create("armour/gloves/model.glb");
mod.moby_layer_skinned(hero, 2, gloves, gloves_hide);            // put on: layer 2
// ...
mod.moby_layer_off(hero, 2);                                     // taken off: the hands are back
```

## The mesh

A layer is a `.glb` made with `model_create` (one level load's handle, asked again each load, as
any model). With `OPENRAC_MODEL_GAME_POSE` (`moby_layer_skinned` sets it) it is a skinned mesh on
the moby's class skeleton: joints named `joint_NNN`, as the port's exports name them; it is posed
by the moby's joints every frame (its animation, its joint modifiers, the mods' pose layers), each
joint moved from the class's bind pose to the mesh's where they differ, as `moby_model_at` does.
Without the flag it is drawn rigid with the moby. `matrix` takes the mesh's coordinates to the
class model's (identity for a mesh made on the exported class).

The easiest pieces are cut from a whole suit made on the class: keep the triangles whose vertices
are weighted mostly to the piece's joints (the gloves: the hands and the fingers). Each piece keeps
the suit's skin, so it follows the body exactly where the suit did.

## What the layer covers

`hide_joints` names the joints of the moby's **own model** a layer covers (256 bits, joint `j` in
word `j / 32`, bit `j % 32`). While the layer is worn each of those joints is drawn shrunk to a
point where the joint is, so what is weighted to it (a hand under a glove, a foot in a boot) is
not seen through the layer; the layers' joints add up, and taking a layer off gives them back the
next frame. All the bits set hides the moby's own model while its layers are still drawn (the old
"whole suit" look, from pieces).

A shrunk joint pulls the vertices shared with its neighbours towards it (a wrist weighted half to
the hand and half to the forearm comes half way in). Under a glove or a boot that is covered; for a
part whose joints carry much of the body (the chest: the spine, the neck and the shoulders), give
the moby a **base without that zone** instead: `moby_model_at` with `OPENRAC_MODEL_GAME_POSE` and
the class model with those triangles taken out, and the chest plate as a layer over it. The two
ways go together: the hidden joints are counted on whatever model the moby is drawn with (its
class's, or the one `moby_model_at` gives it), which has to be skinned for them to apply.

## Rules

- A layer belongs to the mod that set it: another mod's live layer of the same moby and number
  answers `OPENRAC_HOOK_TAKEN`. Different numbers may be different mods'.
- It goes with the level load, or when the class at the address changes (the moby freed and its
  slot made again).
- At most `OPENRAC_MOBY_LAYERED_MAX` (32) mobys wear layers at once: a 33rd answers
  `OPENRAC_HOOK_FULL`.
- A layer is drawn while its moby is, with its light, colour, blending and the mods' materials on
  it, and casts the hero's shadow when its moby does. A moby a menu draws into a picture of its own
  takes its layers into that picture (not yet seen in a game: a menu's 3D hero is a moby of the
  menu's, at its own address, which the mod has to put the layers on).
- A moby hidden with `moby_model_at`'s OPENRAC_MODEL_HIDE_NATIVE is not drawn at all, its layers
  with it: to keep only the layers, set all of a layer's `hide_joints` instead.
- A moby drawn as another model with its animations (`moby_skin_at`) or as a shop preview wears no
  layers.

## Cost

The moby's pose is worked out once: each layer adds its joints' matrices (one copy and multiply
each) and its own draw calls. Measured in Going Commando with Ratchet standing in Maktar (3414x1920,
six 300-frame samples each, scene + submit + GPU): nothing 9.40 ms, one whole suit with
`moby_model_at` 9.57 ms, a base without its chest and four layers 9.83 ms, within the spread between
runs; the scene's CPU part 4.47, 4.53 and 4.59 ms.

