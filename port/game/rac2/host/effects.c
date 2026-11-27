/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 the OpenRAC contributors
 *
 * Going Commando: the game's own effects recoloured for the mods (openrac_api.effect_tint and the
 * named kinds of particle_spawn_ex, interface 1.106).
 *
 * The game's effects are routines that make several particles each, every one through one of four
 * spawns that take its colours as arguments: part_spawn_spinning_particle (func_L00_0033E888, a
 * colour and an end colour), the flashes' spawn (func_L00_0033EC48, one colour), part_spawn_burst
 * (func_002C16C0) and part_spawn_gravity_fade (func_002C65A0), whose colour is a word without its
 * alpha. While a tint is active, these four have their colours changed on the way in: the
 * saturation first (each channel moved from the colour's luminance by the factor), then the
 * multipliers, alpha untouched, each channel clamped to a byte.
 *
 * A tint is active around one of the effects a mod recolours: the flash at a weapon's muzzle
 * (part_spawn_line_at_moby, func_L00_003E4968; its maker the moby it is put on), the sparks where a
 * shot hits (part_l00_spawn_impact_spark_burst_2, func_L00_0041FC20; its maker the moby whose update
 * runs, the shot), and the trail a shot draws (shot_update_and_draw_trail, func_L00_0041F570, whose
 * ribbon DrawFadingDualRibbon, func_L00_0041F820, takes the core and edge colours, two textures and
 * the width: its maker the shot); and around a named effect a mod makes (particles.c). For the game's
 * own, the last tint given that names the effect, the item in the hero's hand (or any) and the
 * maker's class (or any) applies.
 *
 * The Lancer's hit is the spark burst (part_l00_spawn_spark_burst_3, func_L00_003E3258, from its
 * update): IMPACT too, its maker the moby whose update runs (the Lancer, class 1490). A tint
 * already active (a mod's named effect) is left as it is.
 *
 * OPENRAC_RAC2_EFFECT_TRACE=1 prints each of these effects with its maker's class and the item in
 * hand on stderr, the first 200 times, to find which a weapon uses.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "game_protos.h"
#include "openrac/game_host.h"
#include "openrac/guest.h"
#include "openrac/host_env.h"
#include "openrac/openrac_mod.h"
#include "rac2.h"
#include "weapon_shots.h"

#define MOBY_OCLASS 0xAAu

static openrac_game_effect_tint tints[OPENRAC_EFFECT_TINT_MAX];
static int tint_count;

/* The tint the four spawns apply now (none: active 0). */
static struct {
    int active;
    float rgb[3];
    float saturation;
} now;

void rac2_effect_tints(const openrac_game_effect_tint* list, int count) {
    if (list == NULL || count < 0) {
        count = 0;
    }
    if (count > OPENRAC_EFFECT_TINT_MAX) {
        count = OPENRAC_EFFECT_TINT_MAX;
    }
    if (count > 0) {
        memcpy(tints, list, sizeof(tints[0]) * (size_t)count);
    }
    tint_count = count;
}

static int trace_left = -1;

static void trace(const char* what, int32_t oclass, int32_t item) {
    if (trace_left < 0) {
        const char* v = openrac_env_string("OPENRAC_RAC2_EFFECT_TRACE");
        trace_left = v != NULL && v[0] != '\0' && strcmp(v, "0") != 0 ? 200 : 0;
    }
    if (trace_left > 0) {
        --trace_left;
        fprintf(
            stderr, "[effects] %s: maker class %d, item in hand %d\n", what, (int)oclass, (int)item
        );
    }
}

/* The last tint for `effect` made by a moby of `oclass` with the hand's item, or NULL. */
static const openrac_game_effect_tint* tint_for(uint32_t effect, int32_t oclass) {
    if (tint_count == 0) {
        return NULL;
    }
    const int32_t item = rac2_hand_item();
    for (int i = tint_count - 1; i >= 0; --i) {
        const openrac_game_effect_tint* t = &tints[i];
        if ((t->effects & effect) != 0 && (t->item < 0 || t->item == item)
            && (t->oclass < 0 || t->oclass == oclass)) {
            return t;
        }
    }
    return NULL;
}

static int32_t class_of(gaddr moby) {
    return moby != 0 ? (int32_t)GREF(uint16_t, moby + MOBY_OCLASS) : -1;
}

/* The class of the moby making an effect: the one whose update runs (a shot), else the weapon in
 * the hero's hand (his weapons fire from his own update, outside the moby loop). */
static int32_t maker_class(void) {
    const gaddr m = rac2_moby_updating();
    return class_of(m != 0 ? m : rac2_hand_moby());
}

/* Makes the tint active (pass NULL for none); returns what was active, for rac2_tint_pop. */
typedef struct {
    int active;
    float rgb[3];
    float saturation;
} Saved;

static Saved push(int active, const float rgb[3], float saturation) {
    Saved s = {now.active, {now.rgb[0], now.rgb[1], now.rgb[2]}, now.saturation};
    now.active = active;
    if (active) {
        memcpy(now.rgb, rgb, sizeof now.rgb);
        now.saturation = saturation;
    }
    return s;
}

static void pop(Saved s) {
    now.active = s.active;
    memcpy(now.rgb, s.rgb, sizeof now.rgb);
    now.saturation = s.saturation;
}

static int saved_depth;
static Saved saved_stack[4];

void rac2_tint_push(const float rgb[3], float saturation) {
    const Saved s = push(1, rgb, saturation);
    if (saved_depth < 4) {
        saved_stack[saved_depth] = s;
    }
    ++saved_depth;
}

void rac2_tint_pop(void) {
    if (saved_depth > 0) {
        --saved_depth;
        if (saved_depth < 4) {
            pop(saved_stack[saved_depth]);
        }
    }
}

static uint32_t clamp_byte(float v) {
    return v <= 0.0f ? 0u : v >= 255.0f ? 255u : (uint32_t)(v + 0.5f);
}

/* A packed colour (red in the low byte) tinted; its top byte kept. */
static uint32_t tint_by(uint32_t color, const float rgb[3], float s) {
    const float r = (float)(color & 0xFFu), g = (float)((color >> 8) & 0xFFu),
                b = (float)((color >> 16) & 0xFFu);
    const float lum = 0.299f * r + 0.587f * g + 0.114f * b;
    const uint32_t nr = clamp_byte((lum + (r - lum) * s) * rgb[0]);
    const uint32_t ng = clamp_byte((lum + (g - lum) * s) * rgb[1]);
    const uint32_t nb = clamp_byte((lum + (b - lum) * s) * rgb[2]);
    return (color & 0xFF000000u) | nr | (ng << 8) | (nb << 16);
}

/* A colour under the tint active now; with none, a particle the weapon in the hero's hand makes
 * in its own update (the Lancer's muzzle smoke and its bullets' trail of sprites) takes the
 * weapon's MUZZLE tint. */
static uint32_t tinted(uint32_t color) {
    if (now.active) {
        return tint_by(color, now.rgb, now.saturation);
    }
    const gaddr m = tint_count > 0 ? rac2_moby_updating() : 0;
    if (m != 0 && m == rac2_hand_moby()) {
        const openrac_game_effect_tint* t = tint_for(OPENRAC_EFFECT_MUZZLE, class_of(m));
        if (t != NULL) {
            return tint_by(color, t->rgb, t->saturation);
        }
    }
    return color;
}

/* The trace of the spawns made outside the effects a mod tints (the first 300), with the moby
 * whose update runs: to find the routine a weapon's other effects come from. */
static void spawn_trace(const char* what, uint32_t color) {
    static int left = -1;
    if (left < 0) {
        const char* v = openrac_env_string("OPENRAC_RAC2_EFFECT_TRACE");
        left = v != NULL && v[0] != '\0' && strcmp(v, "0") != 0 ? 300 : 0;
    }
    if (left > 0 && !now.active) {
        --left;
        fprintf(
            stderr,
            "[effects] spawn %s colour %08X -> %08X, maker class %d (updating %d), item in "
            "hand %d\n",
            what,
            (unsigned)color,
            (unsigned)tinted(color),
            (int)maker_class(),
            (int)class_of(rac2_moby_updating()),
            (int)rac2_hand_item()
        );
    }
}

/* ---- The four spawns ---- */

gaddr func_L00_0033E888(
    float size,
    float spin,
    gaddr position,
    gaddr velocity,
    int color,
    int color_end,
    int a6,
    int a7,
    int a8,
    int a9,
    int a10
) {
    spawn_trace("spinning spark", (uint32_t)color);
    return func_L00_0033E888__game(
        size,
        spin,
        position,
        velocity,
        (int)tinted((uint32_t)color),
        (int)tinted((uint32_t)color_end),
        a6,
        a7,
        a8,
        a9,
        a10
    );
}

gaddr func_L00_0033EC48(
    float a0,
    float a1,
    gaddr position,
    int life,
    int color,
    int a5,
    int a6,
    gaddr a7,
    int a8,
    int a9,
    short a10,
    int a11
) {
    spawn_trace("flash", (uint32_t)color);
    return func_L00_0033EC48__game(
        a0, a1, position, life, (int)tinted((uint32_t)color), a5, a6, a7, a8, a9, a10, a11
    );
}

gaddr func_002C16C0(
    gaddr position,
    gaddr velocity,
    int ticks,
    int alpha,
    int rgb,
    int a5,
    float a6,
    float a7,
    float a8,
    float a9,
    float a10
) {
    spawn_trace("burst", (uint32_t)rgb);
    return func_002C16C0__game(
        position,
        velocity,
        ticks,
        alpha,
        (int)(tinted((uint32_t)rgb) & 0xFFFFFFu),
        a5,
        a6,
        a7,
        a8,
        a9,
        a10
    );
}

gaddr func_002C65A0(
    gaddr position,
    gaddr velocity,
    float size,
    float size_end,
    float a4,
    int ticks,
    unsigned int rgb,
    int a7,
    int a8,
    int a9,
    int a10
) {
    spawn_trace("gravity fade", rgb);
    return func_002C65A0__game(
        position, velocity, size, size_end, a4, ticks, tinted(rgb) & 0xFFFFFFu, a7, a8, a9, a10
    );
}

/* ---- The effects a mod recolours ---- */

/* Pushes the tint for `effect` made by `maker` (none when a tint is already active): 1 pushed. */
static int begin(const char* what, uint32_t effect, int32_t oclass) {
    trace(what, oclass, rac2_hand_item());
    if (now.active) {
        return 0;
    }
    const openrac_game_effect_tint* t = tint_for(effect, oclass);
    if (t == NULL) {
        return 0;
    }
    rac2_tint_push(t->rgb, t->saturation);
    return 1;
}

/* part_spawn_line_at_moby: the flash at a weapon's muzzle, put on `moby`. */
void func_L00_003E4968(gaddr moby) {
    const int pushed = begin("muzzle", OPENRAC_EFFECT_MUZZLE, class_of(moby));
    func_L00_003E4968__game(moby);
    if (pushed) {
        rac2_tint_pop();
    }
}

/* The Lancer's own muzzle flash (lancer_update calls it for class 1490, part_spawn_line_at_moby
 * for the others): two drifting sprites at its muzzle. */
void func_L00_003A78B0(gaddr moby) {
    const int pushed = begin("muzzle (Lancer)", OPENRAC_EFFECT_MUZZLE, class_of(moby));
    func_L00_003A78B0__game(moby);
    if (pushed) {
        rac2_tint_pop();
    }
}

/* part_l00_spawn_impact_spark_burst_2: a shot's sparks where it hits, in the shot's update. */
void func_L00_0041FC20(gaddr position, gaddr unused, int short_life) {
    const int pushed = begin("impact (spark burst 2)", OPENRAC_EFFECT_IMPACT, maker_class());
    func_L00_0041FC20__game(position, unused, short_life);
    if (pushed) {
        rac2_tint_pop();
    }
}

/* part_l00_spawn_spark_burst_3: the Lancer's hit, in its update. */
void func_L00_003E3258(gaddr position, int unused, int extra) {
    const int pushed = begin("impact (spark burst 3)", OPENRAC_EFFECT_IMPACT, maker_class());
    func_L00_003E3258__game(position, unused, extra);
    if (pushed) {
        rac2_tint_pop();
    }
}

/* The trail being drawn now (shot_update_and_draw_trail), for its ribbon. */
static const openrac_game_effect_tint* trail_tint;

void func_L00_0041F570(gaddr self) {
    const int32_t oclass = class_of(self);
    static int traced;
    if (!traced) {
        traced = 1;
        trace("trail", oclass, rac2_hand_item());
    }
    trail_tint = tint_for(OPENRAC_EFFECT_TRAIL, oclass);
    func_L00_0041F570__game(self);
    trail_tint = NULL;
}

/* A ribbon's colours, textures and width under the trail's tint: the shot whose trail is drawn
 * (shot_update_and_draw_trail), else the moby whose update draws it (the Lancer's bullet, 3070,
 * draws its own with DrawFadingTwoPassRibbon). */
static void ribbon_tint(
    int* first_texture,
    int* second_texture,
    unsigned int* first_color,
    unsigned int* second_color,
    float* width
) {
    const openrac_game_effect_tint* t = trail_tint;
    if (t == NULL) {
        const int32_t oclass = maker_class();
        static int32_t traced = -2;
        if (traced != oclass) {
            traced = oclass;
            trace("trail (ribbon)", oclass, rac2_hand_item());
        }
        t = tint_for(OPENRAC_EFFECT_TRAIL, oclass);
    }
    if (t == NULL) {
        return;
    }
    const Saved s = push(1, t->rgb, t->saturation);
    *first_color = tinted(*first_color);
    *second_color = tinted(*second_color);
    pop(s);
    if (t->texture[0] >= 0) {
        *first_texture = t->texture[0];
    }
    if (t->texture[1] >= 0) {
        *second_texture = t->texture[1];
    }
    if (t->width > 0.0f) {
        *width = t->width;
    }
}

/* DrawFadingTwoPassRibbon: the Lancer's bullets' trail. */
void func_L00_003E2E80(
    gaddr points,
    int first_texture,
    int second_texture,
    int count,
    unsigned int first_color,
    unsigned int second_color,
    float unused,
    float width,
    float uv
) {
    ribbon_tint(&first_texture, &second_texture, &first_color, &second_color, &width);
    func_L00_003E2E80__game(
        points, first_texture, second_texture, count, first_color, second_color, unused, width, uv
    );
}

/* DrawFadingDualRibbon: a trail's tint on its colours, textures and width. */
void func_L00_0041F820(
    gaddr points,
    int first_texture,
    int second_texture,
    int count,
    unsigned int first_color,
    unsigned int second_color,
    float unused,
    float width,
    float uv
) {
    ribbon_tint(&first_texture, &second_texture, &first_color, &second_color, &width);
    func_L00_0041F820__game(
        points, first_texture, second_texture, count, first_color, second_color, unused, width, uv
    );
}
