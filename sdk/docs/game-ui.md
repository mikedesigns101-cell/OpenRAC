# A mod's interface with the game's own 2D

Going Commando, OpenRAC Commando's interface 1.129 (`has_game_ui()`, `has_game_sound()`). Native
mods.

A mod that draws its panels with the overlay (its own fonts, its own boxes) looks like a mod. These
calls draw them with what the game itself already has loaded, by the game's own routines: its font
(colour bytes, accented letters, button icons), its help box ("For a map press SELECT"), the
sprites of its menus, its HUD icons and its sounds. Nothing of the game's is read from the mod or
goes in its package: the port uses, live, what the player's game has in memory.

Everything is placed on the game's 2D screen, **512 x 416**: the 4:3 column the game draws its HUD
in (centred in a wider window; what falls outside it is cut). The game centres on x = 256. Its help
boxes sit at the foot (y = screen height - 60, about 356).

## What a mod draws: `gui_draw`

```cpp
openrac_gui_element e[2] = {};
e[0].size = sizeof e[0];
e[0].kind = OPENRAC_GUI_BOX;            // the help box's fill and two-line frame with its brackets
e[0].x = 256; e[0].y = 120; e[0].w = 320; e[0].h = 40;
e[1].size = sizeof e[1];
e[1].kind = OPENRAC_GUI_TEXT;
e[1].flags = OPENRAC_GUI_CENTER;
e[1].layer = 1;
e[1].x = 256; e[1].y = 112; e[1].scale_x = 1;
e[1].rgba[0] = 0x88; e[1].rgba[1] = 0xA8; e[1].rgba[2] = 0xFF; e[1].rgba[3] = 255;  // the box's blue
std::snprintf(e[1].text, sizeof e[1].text,
    OPENRAC_GUI_COLOUR_ORANGE "Lancer" OPENRAC_GUI_COLOUR_TEXT " upgraded to "
    OPENRAC_GUI_COLOUR_ORANGE "Mega" OPENRAC_GUI_COLOUR_TEXT "!");
if (mod.has_game_ui()) mod.gui_draw(e, 2);
```

- `gui_draw(elements, count)` sets **all** the mod draws, in place of what it drew before; `count`
  0 takes it away. It stays (drawn every frame) until the mod sets it again or the level loads.
- It is drawn after the game's HUD and its help box, in the same 2D pass: under a menu that covers
  the 2D, in a scene, it is not drawn (as the HUD is not). Elements go in the order of `layer`
  (0 first), then of the array. The mods' own help boxes go on top of everything.
- At most `OPENRAC_GUI_ELEMENTS_MAX` (64) a mod, `OPENRAC_GUI_ELEMENTS_ALL` (256) for all the mods.
- Colours are `rgba` bytes (red, green, blue, alpha, 0 to 255). As in the game, 128 a channel is
  the font's or the sprite's own shade (255 about doubles it); 0 in all four means "the game's
  default" for that element.

### Kinds

| kind | what | place | the rest |
|---|---|---|---|
| `OPENRAC_GUI_TEXT` | a line in the game's font (`func_0027F640`, the HUD's) | `x, y`: the line's top; `x` its start, middle (`OPENRAC_GUI_CENTER`) or end (`OPENRAC_GUI_RIGHT`) | `scale_x` (1: 16 pixels a line; 0.25 to 4), `OPENRAC_GUI_SMALL` the small font, `OPENRAC_GUI_SHADOW` a black shadow one pixel down right |
| `OPENRAC_GUI_BOX` | the help box's look: rounded fill and the frame with its brackets | centre `x, y`, size `w x h` | `rgba` the fill (0: the game's #002D44 at 96 of 128), `rgba2` the frame (0: the game's #70C0F0); `OPENRAC_GUI_NO_FILL`, `OPENRAC_GUI_NO_FRAME` |
| `OPENRAC_GUI_SPRITE` | a sprite of the game's menus by name (`OPENRAC_GUI_SPRITE_*`, table below) | its anchor at `x, y` | `scale_x, scale_y` (negative mirrors), `frame` (0 to 1: the sprite's own animation; how far a bracket reaches), `rgba` |
| `OPENRAC_GUI_ICON` | a HUD texture by its number | top left `x, y`, size `w x h` | `rgba` tint (0: as it is) |
| `OPENRAC_GUI_ITEM_ICON` | an item's icon (the weapons', gadgets'): `id` the game's item number | as `ICON` | the item's icon key, from its record |
| `OPENRAC_GUI_ICON_KEY` | an icon by its key (`GetIconFrame`) | as `ICON` | `frame` the key's picture |

### Text

`text` is UTF-8 (at most `OPENRAC_GUI_TEXT_BYTES` - 1 bytes in the game's coding). The port turns it
into the game's bytes:

- Printable ASCII as it is. The accented letters the font has (Á À Â Ä á à â ä É È Ê Ë é è ê ë
  Í Ì Î Ï í ì î ï Ó Ò Ô Ö ó ò ô ö Ú Ù Û Ü ú ù û ü Ñ ñ Ç ç ß … ¡ ¿) as the font's own (bytes 0x80
  to 0xAF: the accent is drawn over the plain letter). Typographic quotes and dashes as plain ones;
  anything else as `?`.
- The font's control bytes, as they are: `OPENRAC_GUI_COLOUR_TEXT` (0x08) the element's colour,
  0x09 to 0x0F the font's own colours (`OPENRAC_GUI_COLOUR_ORANGE`, 0x0C, is the help box's for
  names), the button icons `OPENRAC_UI_CROSS` ... `OPENRAC_UI_RIGHT_STICK` (0x10 to 0x19; on the
  keyboard the port shows the bound key there, as for the game's own prompts).
- No limit of 64 letters, as `hud_text_set` has. One line: for more, more elements (or a help box,
  which wraps).

`gui_text_width(text, flags, scale)` answers the width in 2D pixels (the same text and font as an
element would draw), to size a box round it: the game's help box is the text's width + 20 wide.

## The game's help box: `help_box_show`

```cpp
int top = mod.help_box_show("Megacorp thanks you for your purchase.", 70, 240);  // y 70, 240 ticks
int foot = mod.help_box_show("Press " OPENRAC_UI_TRIANGLE " to close this box.");   // the game's place and time
```

- It opens as the game's own: the frame grows 8 ticks, the text comes in 4, stays `ticks` (-1: the
  game's 7 a letter), goes out in 4, the frame shrinks 8. Its sound is the game's global sound 0
  (`OPENRAC_HELP_SILENT`: none). Triangle closes it, as the game's (`OPENRAC_HELP_KEEP`: it does
  not).
- `y` is its centre on the 416 (-1: where the game puts its own, the foot). The text is the same
  UTF-8 as an element's; the font wraps it inside the box and `\x01` breaks a line.
- The game's entry 0 (and its texts) are not touched: the port has its own queue, beside the
  game's. Several are up at once at different heights; one whose place another box takes (the
  game's own included) waits its turn, in the order they were asked for. At most
  `OPENRAC_HELP_BOXES_MAX` (8) waiting or up.
- `help_box_state(box)`: `OPENRAC_HELP_WAITING`, `_SHOWN` or `_GONE`. `help_box_close(box)`
  closes it the game's way (or takes it out of the queue).
- A level load takes them all away; so does a scene (the game's mode not "play"), where they stop
  until it ends.

## The game's sounds: `game_sound_play`

```cpp
mod.game_sound_play(OPENRAC_SOUND_GLOBAL, 3);        // the menus' "confirm"
mod.game_sound_play(OPENRAC_SOUND_GLOBAL, 5, 0.6f);  // "error", softer
mod.game_sound_play(11, 1);                          // the vendor's (class 11) cursor, where it is loaded
```

- `OPENRAC_SOUND_GLOBAL`: the game's global bank, loaded in every level (20 sounds): 0 the help
  box's notice, 3 confirm, 4 move or assign, 5 error (the pause menu's; others are the game's other
  menus').
- Any other bank is a moby class: its own sounds, when the loaded level has that class
  (`UNKNOWN` otherwise).
- `volume`: 1 the game's own loudness (its 1024), 0 to 2. Not placed in the world.
- Answers the game's voice, or `OPENRAC_HOOK_UNKNOWN`.

## The game's sprites

The plates and frames of the game's menus are "native quads" objects of its resident GUI data
(loaded once, the same in every level): each has its own shape and its own animation, where
`frame` (0 to 1) stretches it (how wide a plate, how far a bracket reaches). `x, y` is the
object's anchor, where the game itself puts it (below); `scale_x`, `scale_y` scale it from there
(-1 mirrors it). Leave `rgba` at 0 for the game's own colour (the fills' #002D44 at 96 of 128, the
frames' #70C0F0); a plate is its fill, then its frame over it, at the same place and `frame`.

| id | `OPENRAC_GUI_SPRITE_` | what | the game draws it at |
|---|---|---|---|
| 1 | `FRAME_END` | the help box frame's end bracket | the box's corners (the frame of `OPENRAC_GUI_BOX`) |
| 2 | `FRAME_CORNER` | the help box frame's line | idem |
| 3 | `PAUSE_PLATE_FILL` | the pause menu's plate, its fill | x 124 (its left end), y 36, 86 ... 336 (its top), `frame` 0.37 (about 250 wide, 28 tall) |
| 4 | `PAUSE_PLATE_FRAME` | its double frame | idem; the chosen one in white (#ECFDFE) |
| 5, 6 | `HUD_LEFT_FILL`, `_FRAME` | the HUD's top left plate (health) | x 10, y 10, `frame` 4 |
| 7, 8 | `HUD_RIGHT_FILL`, `_FRAME` | the HUD's top right plate (bolts) | x 498, y 10 |
| 9, 10 | `TOP_PLATE_FILL`, `_FRAME` | the HUD's top middle plate | x 147, y 10 |
| 11, 12 | `CORNER_FILL`, `_FRAME` | a small corner piece | x 184 / 185, y 29 / 30, four times mirrored |
| 13 | `VENDOR_LIST_FILL` | the vendor's six plates, stacked: fill | x 250, y 190 |
| 14, 15 | `VENDOR_LIST_FRAME`, `_INNER` | their double frame, their thin inner frame | idem |
| 16 | `VENDOR_ARROWS` | the arrows beside each plate | idem |
| 17 | `VENDOR_CURSOR` | the chosen plate's frame | idem |
| 18 | `BRACKET_END` | a bracket's end | x 165, y 68, `scale_x` -1 |
| 19 | `BRACKET_LINE` | a bracket's line | x 330, y 68, `frame` 0.145 |

```cpp
// A pause-menu plate with a word in it, as the game draws its own.
openrac_gui_element p[3] = {};
for (auto& e : p) { e.size = sizeof e; e.scale_x = e.scale_y = 1; }
p[0].kind = OPENRAC_GUI_SPRITE; p[0].id = OPENRAC_GUI_SPRITE_PAUSE_PLATE_FILL;  p[0].x = 124; p[0].y = 210; p[0].frame = 0.37f;
p[1].kind = OPENRAC_GUI_SPRITE; p[1].id = OPENRAC_GUI_SPRITE_PAUSE_PLATE_FRAME; p[1].x = 124; p[1].y = 210; p[1].frame = 0.37f;
p[2].kind = OPENRAC_GUI_TEXT; p[2].layer = 1; p[2].flags = OPENRAC_GUI_CENTER; p[2].x = 249; p[2].y = 216;
std::snprintf(p[2].text, sizeof p[2].text, "Weapons");
mod.gui_draw(p, 3);
```

A text with a higher `layer` goes over a sprite (the port keeps each element's place in the
game's display list); one with a lower layer goes under it.

The catalogue was made with `OPENRAC_GAME_UI_LOG=sprites` (below), on the NTSC-U disc (v1.01; the
other releases have none of this yet). The icons: `OPENRAC_GUI_ITEM_ICON` draws any item's icon
(all 55 were drawn in a test); HUD textures by number change from level to level, keys do not.

## Tests

`OPENRAC_GAME_UI_LOG=1` logs `[ui]` lines: what is drawn (every 120 frames), each help box queued,
opened and gone, each sound played and its voice. `OPENRAC_GAME_UI_LOG=sprites` also logs, once
each, every GUI sprite object, HUD texture and icon key the game itself draws (how the catalogue
was made). With no mod using these calls the game runs as before.
