/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 the OpenRAC contributors
 *
 * Going Commando: the game's own particle effects for the mods (openrac_api.particle_spawn_ex,
 * interface 1.33; the port checks the options first, game/common/mod_api/visuals.cpp). Each kind is
 * one of the game's spawns of a particle record, which the game then moves, fades, ends and draws
 * itself:
 *   SMOKE  part_spawn_gravity_fade (func_002C65A0): a sprite that drifts along the velocity, grows to
 *          twice its size and fades from the colour's alpha to the end colour's over its life (its
 *          colour stays; the Lancer's bullets trail these).
 *   BURST  part_spawn_burst (func_002C16C0): a spray particle, the game's for blasts and the rockets'
 *          smoke; its colour is the colour's (alpha 0 to 127).
 *   SPARK  part_spawn_spinning_particle (func_L00_0033E888, every level's program): a spinning spark
 *          that fades from the colour to the end colour, the game's for hits.
 * The colours are the game's packed words (alpha in the top byte, as each spawn takes them). A
 * texture or blend of the mod's (0 to 255, -1 the game's) is written into the record the spawn made
 * (its texture byte +2, its blend byte +3). OPENRAC_PARTICLE_CHILDREN is ignored: these spawns have
 * no secondary ones.
 *
 * Since 1.106 the game's named effects too (SPARKS to MUZZLE, named_effect below): one of its
 * whole routines at the position, tinted (effects.c) by the spawn's colour and saturation. */
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "game_protos.h"
#include "openrac/game_host.h"
#include "openrac/guest.h"
#include "openrac/openrac_mod.h"
#include "rac2.h"

/* The record bytes a texture and a blend go in (every particle record's). */
#define RECORD_TEXTURE 2u
#define RECORD_BLEND 3u
/* A spark's spin (part_spawn_spinning_particle's rotation, the hits' 0.5), a burst's (500, its
 * growth 1 and phase 1, as the game's sprays pass them). */
#define SPARK_SPIN 0.5f
#define BURST_SPIN 500.0f

/* A moby-like record for part_spawn_line_at_moby, in the statics: its pvars pointer (+0x68) to
 * pvars whose +0x14 is the flash's place, and its matrix's second and third rows (+0xD0, +0xE0). */
#define FAKE_PVARS 0x68u
#define FAKE_ROW_BACK 0xD0u
#define FAKE_ROW_UP 0xE0u
#define FAKE_SIZE 0x100u
#define FAKE_PVARS_PLACE 0x14u

/* The named effects (interface 1.106; effects.c tints them): each whole routine of the game at
 * the position (in `room`), tinted by the spawn's colour and saturation. */
static int named_effect(const struct openrac_game_particle_spawn* s, gaddr room) {
    static gaddr fake;
    const uint32_t c = s->color;
    const int tint = c != 0 || s->saturation > 0.0f;
    const float rgb[3] = {
        c != 0 ? (float)(c & 0xFFu) / 128.0f : 1.0f,
        c != 0 ? (float)((c >> 8) & 0xFFu) / 128.0f : 1.0f,
        c != 0 ? (float)((c >> 16) & 0xFFu) / 128.0f : 1.0f,
    };
    if (tint) {
        rac2_tint_push(rgb, s->saturation > 0.0f ? s->saturation : 1.0f);
    }
    switch (s->kind) {
        case OPENRAC_PARTICLE_SPARKS:
            func_L00_003E3258(room, 0, (s->flags & OPENRAC_PARTICLE_CHILDREN) != 0 ? 1 : 0);
            break;
        case OPENRAC_PARTICLE_IMPACT:
            func_L00_0041FC20(room, 0, s->life_ticks <= 20u ? 1 : 0);
            break;
        case OPENRAC_PARTICLE_SPRAY:
            func_L00_003E3668(room);
            break;
        case OPENRAC_PARTICLE_ROCKET_SMOKE:
            func_L00_004200F0(room);
            break;
        default: { /* OPENRAC_PARTICLE_MUZZLE */
            if (fake == 0) {
                fake = openrac_guest_static(FAKE_SIZE + 0x40u);
            }
            float d[3] = {s->velocity[0], s->velocity[1], s->velocity[2]};
            const float n = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (n > 0.0f) {
                d[0] /= n;
                d[1] /= n;
                d[2] /= n;
            } else {
                d[0] = 1.0f;
                d[1] = d[2] = 0.0f;
            }
            /* The routine moves the place by -0.1 of +0xD0 (left at zero here: the flash is where
             * the mod says), then lays its nine flashes from +0.15 to -0.15 along +0xE0: the
             * direction given. */
            const float back[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            const float up[4] = {d[0], d[1], d[2], 0.0f};
            memset(G(fake), 0, FAKE_SIZE + 0x40u);
            GREF(gaddr, fake + FAKE_PVARS) = fake + FAKE_SIZE;
            memcpy(G(fake + FAKE_SIZE + FAKE_PVARS_PLACE), s->position, 12);
            memcpy(G(fake + FAKE_ROW_BACK), back, 16);
            memcpy(G(fake + FAKE_ROW_UP), up, 16);
            func_L00_003E4968(fake);
            break;
        }
    }
    if (tint) {
        rac2_tint_pop();
    }
    return 1;
}

int rac2_particle_spawn_ex(const struct openrac_game_particle_spawn* s) {
    static gaddr room;
    const int level = openrac_game_loaded_overlay();
    if (s == NULL || level < 0 || level >= RAC2_LEVELS) {
        return -1;
    }
    if (room == 0) {
        room = openrac_guest_static(0x20);
    }
    const float position[4] = {s->position[0], s->position[1], s->position[2], 1.0f};
    const float velocity[4] = {s->velocity[0], s->velocity[1], s->velocity[2], 0.0f};
    memcpy(G(room), position, 16);
    memcpy(G(room + 0x10u), velocity, 16);
    const int ticks = (int)s->life_ticks;
    const int alpha = (int)((s->color >> 24) & 0xFFu);
    gaddr made = 0;
    switch (s->kind) {
        case OPENRAC_PARTICLE_SMOKE: {
            /* Its turn each tick: the spin rate (-1 to 1) as the phase byte's step. */
            const int phase = (int)(s->rot * 127.0f);
            made = func_002C65A0(
                room,
                room + 0x10u,
                s->size,
                s->size * 2.0f,
                0.0f,
                ticks,
                s->color & 0xFFFFFFu,
                phase,
                alpha,
                (int)((s->color_end >> 24) & 0xFFu),
                1
            );
            break;
        }
        case OPENRAC_PARTICLE_BURST:
            made = func_002C16C0(
                room,
                room + 0x10u,
                ticks,
                alpha > 127 ? 127 : alpha,
                (int)(s->color & 0xFFFFFFu),
                1,
                s->size,
                BURST_SPIN,
                1.0f,
                0.0f,
                1.0f
            );
            break;
        case OPENRAC_PARTICLE_SPARK:
            made = func_L00_0033E888(
                s->size,
                SPARK_SPIN,
                room,
                room + 0x10u,
                (int)s->color,
                (int)s->color_end,
                ticks,
                1,
                -1,
                -1,
                1
            );
            break;
        case OPENRAC_PARTICLE_SPARKS:
        case OPENRAC_PARTICLE_IMPACT:
        case OPENRAC_PARTICLE_SPRAY:
        case OPENRAC_PARTICLE_ROCKET_SMOKE:
        case OPENRAC_PARTICLE_MUZZLE:
            return named_effect(s, room);
        default:
            return -1;
    }
    if (made == 0) {
        return 0; /* the pool is full */
    }
    if (s->texture >= 0) {
        GREF(uint8_t, made + RECORD_TEXTURE) = (uint8_t)s->texture;
    }
    if (s->blend >= 0) {
        GREF(uint8_t, made + RECORD_BLEND) = (uint8_t)s->blend;
    }
    return 1;
}
