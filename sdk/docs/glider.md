# The Glider in any level, and the hero's seqs of other levels

Going Commando, OpenRAC Commando's interface 1.127 (`has_glider()`). Native mods.

## The hero's seqs of other levels

Each level's data holds only the hero's animations (seqs) that level uses: Oozla has 101 of his
class's 157, Maktar 103, Siberius all of them. Asking the game for a seq his class has no keys for
in the level loaded used to hang the game; since 1.127 the port never passes such a request on
(whoever makes it, a state of the game's or a mod's filter), it logs it once a load instead.

`hero_seq_import(level, seq)` brings a seq in from another level, from now on, in every level
whose own class lacks it:

```cpp
// In openrac_mod_load: the Glider's flying seqs (128..136) from Tabora, and a seq of Siberius's.
for (int seq = 128; seq <= 136; ++seq) mod.hero_seq_import(8, seq);
mod.hero_seq_import(7, 59);
// Later, in any level: hero_anim_frames(59) > 0, so hero_play_anim(59, ...) plays it.
```

- The seq is read from the player's disc image (never from the mod: nothing of the game's goes in a
  mod), copied into the game's RAM past the console's 32 MiB and relocated as the game relocates
  its own (a seq's block is self-relative), at each load of a level whose class has no such seq.
  A level's own seq is never replaced.
- The hero's skeleton is the same in every level, so any seq plays on him.
- The game's RAM past 32 MiB has to be asked for: the mod's manifest says `"expanded_ram": true`
  (read before the game starts). Without it the call answers `OPENRAC_HOOK_UNKNOWN`.
- The last level asked for a seq wins. A seq the source level has not answers `UNKNOWN`.

Which seqs a level has: the port logs each import; `hero_anim_frames(seq)` answers 0 for one the
level lacks. Missing in Oozla (level 1) and Maktar (level 2) on the NTSC-U disc:

- Oozla: 49 50 52 53 55 58-61 63-65 67 68 71-77 79 80 82-86 88-91 94 95 97 102-105 107 108 117-122
  128-136.
- Maktar: 47-50 52 53 55 58-61 71-77 79 80 82 83 88-91 93-95 97 102-108 118-122 124-126 128-136.

## The Glider from a launch pad

`glider_pad_add(pad)` puts the game's own launch pad and glider in a level. They are read from
Tabora on the player's disc (classes 2440 and 2309), with the hero's flying seqs (128 to 136,
imported as above); the flight is the game's (Tabora's state machine and tuning, run by the port).

```cpp
openrac_glider_pad pad{};
pad.size = sizeof pad;
pad.level = 1;                                  // Oozla
pad.position[0] = 296; pad.position[1] = 418; pad.position[2] = 145;   // high over the water
pad.yaw = 0.6f;                                 // radians about z: where it takes off to
pad.landing[0] = 350; pad.landing[1] = 455; pad.landing[2] = 120.74f;  // an islet's ground
pad.landing_radius = 7; pad.landing_height = 12;
pad.item = OPENRAC_GLIDER_ITEM;                 // he needs the Glider (item 21); -1 none
const int id = mod.glider_pad_add(pad);
mod.on_glider([](void*, const openrac_glider_event* e) { /* e->kind, e->pad */ }, nullptr);
```

- The pad has the game's pad's collision: put it where the hero can reach it (on a cliff, a roof);
  one in mid air stands there too (the test above teleports him onto it).
- Call it while the mod loads (or any time before the level's next load): the classes are
  registered as the level loads, and each load of the level makes the pad again.
- Standing on the pad and owning `item`, X takes off: the game's take-off, glide and banking with
  the stick. Without the item X does nothing (`OPENRAC_GLIDER_REFUSED`).
- The flight ends:
  - in the pad's landing zone: within `landing_radius` across of `landing`, at most
    `landing_height` above it (and below the hero): he is set down at `landing` (`LAND`);
  - against a wall: the game's tumble, then he is let go there and falls (`CRASH`);
  - in what the level kills with (the game's collision material 1: water, a pit): he is let go
    and the game's own death follows (`KILLED`);
  - another way (a cut scene, a death: his state is no longer the ridden one): `ABORT`.
  The glider goes back to its pad each time, hidden until the next take-off (as in Tabora).
- Not in Tabora, Dobbo or Grelbin (they have the game's own pads), nor in a level whose hero code
  has no ridden state (Maktar): `UNKNOWN`. Needs `"expanded_ram"` too.
- Events (`on_glider`, before the frame callbacks): `ON_PAD`, `OFF_PAD`, `REFUSED`, `LAUNCH`,
  `LEVELED`, `LAND`, `CRASH`, `KILLED`, `ABORT`, with where the hero (or the glider) was.
- Nothing is saved: the pads are the mod's calls, made again each run.
- What is seen is the game's: the glider's wings over the hero, his flying seqs and the smoke
  trail. (The port drew no glider before 1.127, in Tabora either: the glider keeps mode bit 0x80,
  which the port's moby drawing left out; the hero's vehicle is drawn now.)
- Not yet: the glider's sounds (the class's sounds are Tabora's), Tabora's camera object (the
  follow camera is fed the game's aim values for the glider).
- The flight's profile is the game's (measured in Oozla with the stick left alone):
  1. a hop: about 10 units ahead, up to 4 above the pad, in 100 ticks;
  2. a dive: it levels out about 35 units ahead of the pad and **17 below it**, in 120 ticks
     (event `LEVELED`, with the place);
  3. a level glide at that height, at 20 units a second (a third of a unit a tick), as far as it
     meets nothing: it never sinks on its own and never climbs past the pad's height (the stick
     pitches it; pulled up past the pad's height it stalls and dives again).
  So: **put a pad 20-25 units above what lies ahead**, nothing on the line higher than the
  glide, and the landing zone's ground between the glide's height minus `landing_height` and the
  glide's height. From the ground by Oozla's water (115, water at 108) it dives into the water and
  crashes; from a pad at 145 it glides at 128.
- NTSC-U v1.01 only for now.
