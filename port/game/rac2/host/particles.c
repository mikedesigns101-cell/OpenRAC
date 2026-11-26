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
 * no secondary ones. */
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
