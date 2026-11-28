# X-ray: a moby seen through what covers it, and marks on the screen

Three calls of OpenRAC Commando's interface 1.128 for a weapon's sight that shows the enemies
hidden behind cover, and for HUD marks pinned on a point of the world. Native mods; guard them
with `has_moby_xray()` and `has_world_to_screen()` (mod.hpp). The drawing is the common
frontend's; Going Commando is where they were tested.

```cpp
// Every Brute near the hero, red where something covers it, for two seconds.
mod.moby_xray(brute, 0xFF3020C0u, 120);
// Its outline only, and drawn whole even where nothing covers it:
openrac_moby_xray x{};
x.size = sizeof x;
x.flags = OPENRAC_XRAY_OUTLINE | OPENRAC_XRAY_ALWAYS;
x.rgba[0] = 0.2f; x.rgba[1] = 0.9f; x.rgba[2] = 1.0f; x.rgba[3] = 1.0f;
mod.moby_xray(brute, x);
mod.moby_xray_off(brute);

// A mark over its head on the HUD.
openrac_screen_point p{};
const float head[3] = {pos[0], pos[1], pos[2] + 3.4f};
if (mod.world_to_screen(head, p) == OPENRAC_HOOK_OK && p.in_front) {
    openrac_hud_text t{};
    std::snprintf(t.text, sizeof t.text, "V");
    t.x = p.x / 512.0f;   // hud_text_set takes 0 to 1 of the game's 4:3 picture
    t.y = p.y / 416.0f;
    t.scale = 1.5f;
    t.rgba[0] = 255; t.rgba[1] = 220; t.rgba[2] = 40; t.rgba[3] = 255;
    api->hud_text_set(handle, &t, 1);
}
```

## moby_xray

`moby_xray(address, xray)` marks the live moby at `address`; `xray` null takes the mod's mark off.

- **What is drawn**: the moby's mesh with the pose it is drawn with this frame: its class's model,
  the mesh `moby_model_at` gives it and its layers (`moby_layer_at`) alike, in `rgba` (0 to 1, alpha
  its opacity), over the level's geometry, after the effects and before the HUD.
- **Only where covered** (the default): the parts of the moby that are seen are drawn as they are,
  and only its covered parts as the silhouette, each pixel once (a translucent colour does not add
  up over its own faces; an arm in front of its own body is not painted). `OPENRAC_XRAY_ALWAYS`
  draws the whole moby over everything.
- `OPENRAC_XRAY_OUTLINE`: only its rim, strongest where its surface turns away from the camera, the
  inside clear; it follows the mesh's normals, so it is not a pixel outline of the silhouette.
- **A moby the game does not draw**: the game leaves out of its draw list what it judges hidden
  (behind a wall of another zone, outside its sight). A marked moby it left out is read from its
  address and posed for its silhouette alone (never drawn as itself) while it is in the camera's
  sight and within `max_distance` of the camera (0: `OPENRAC_MOBY_XRAY_DISTANCE`, 30 units). A moby
  its game hides on purpose (its mode's bits 0x81, as the game's own draw leaves it out) is not
  drawn, nor one the game leaves undrawn by its own draw distance while it is in its list.
- **It goes** after its `ticks` (0 or less: until taken off), when the moby is freed or its slot
  made another class, and with the level load. Marking a moby again replaces the mod's own mark.
- A mark is one mod's (another mod's answers `OPENRAC_HOOK_TAKEN`); `OPENRAC_MOBY_XRAY_MAX` (16)
  mobys at once (`OPENRAC_HOOK_FULL`).
- Textures are not read: holes a texture cuts (a grating) are filled in the silhouette.
- Surfaces nearer than 0.15 units in front of the moby do not count as cover (the moby must not be
  covered by its own faces).

## world_to_screen and camera_lens

`world_to_screen(position, out)` gives where a point of the world is with the camera the last
frame was drawn with (a mod's `camera_set`, the player's field of view and a wider window
included): `x`, `y` in the game's 2D picture (0 to 512 across, 0 to 416 down: the 4:3 column of
the middle of the frame the HUD is drawn in; past it on a wider window), `px`, `py` in the frame's
pixels from its top left (`width` x `height`), and `in_front`. Divide `x` by 512 and `y` by 416 for
`hud_text_set`. The camera is the last frame's: called from a frame callback it is the frame
before the tick's.

`camera_lens(out)` gives that camera: eye, forward, left and up (the game's axes), the tangents of
half the field of view across (the whole frame's width) and down, the view and projection
matrices (column-major; the projection is reversed depth), the frame's size and the share of its
width the 2D column takes. `camera_view` is unchanged (the game's own camera, before a mod's).

## Cost

Each marked moby is drawn twice (once only with `OPENRAC_XRAY_ALWAYS`) with a program of its own,
one draw a primitive; a frame without marks draws nothing more (its picture is the same pixel for
pixel). Measured in Going Commando's Maktar (3414x1920, scene + submit + GPU, two runs each, other
games running on the machine): 0 marks 10.80 and 9.02 ms, 4 marks 10.36 and 11.04 ms, 16 marks
10.31 and 10.02 ms, within the spread between runs; 16 marks add about 50 draw calls and 40 000
triangles.
