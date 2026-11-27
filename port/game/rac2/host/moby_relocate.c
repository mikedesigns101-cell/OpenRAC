/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 the OpenRAC contributors
 *
 * Going Commando: a live moby moved to another place of its level, with what goes with it
 * (openrac_api.moby_relocate, OpenRAC Commando's 1.121; play.c hands it to the mods).
 *
 * A moby is more than its place (+0x10). Its bounding sphere (+0x00, when it has a radius) is what
 * the drawing and the hit tests read; the moby cells (MobyUpdateCell, func_002A0F48) are where the
 * moby loop finds the mobys near the camera to wake, run and draw; a moby left in its old cell
 * sleeps where it no longer is. That much the game's own spawners do, and moby_copy too.
 *
 * Then its class's own ties to where it was. Many enemies keep places of their own in their pvars:
 * where they wander around, back off to, turn back to, all set from where they were placed; carried
 * along with the moby (OPENRAC_RELOCATE_HOMES) they stay its own ground, else it walks back. Two
 * classes whose ties the decomp shows (OPENRAC_RELOCATE_FREE): the Thugs-4-Less Brute (2501,
 * ranged_l02_enemy_update) walks its level's route +0x320 (g_route_polygons) and calls the drop
 * squad +0x328 in states 17 to 22, and turns back to +0x340 and backs off to +0x350; the Megacorp
 * Chickenbot (2638, its state machine and enemy_init) is held to the area +0x218 and aims at the
 * target +0x210 of its level's table, and wanders around +0x2E0. FREE takes the route, the squad,
 * the area and the target off (-1: none, which both handle) and puts the homes at the new place.
 * WAKE starts a waiting one at once: a Chickenbot hidden in its drop ship (state 13) runs at the
 * hero (state 9), a Brute in its entry route or squad call (13 to 22 but its death, 16) walks
 * (state 1), through moby_set_state (func_002A7750). The Commando mod's agresivo.cpp found these.
 *
 * And the ground. A walker put where its level has no floor falls for ever and never dies (Brutes
 * over Maktar's void), so the point must have a floor of the level's under it, a slope a walker
 * stands on, from 2.5 units above to 8 below: the moby is set on it. OPENRAC_RELOCATE_ANYWHERE
 * puts it at the point as given (a flyer, a swimmer). */
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "enemy_copies.h"
#include "game_protos.h"
#include "moby_checks.h"
#include "rac2.h"

#define MOBY_SPHERE 0x00u
#define MOBY_POS 0x10u
#define MOBY_STATE 0x20u
#define MOBY_PVARS 0x68u
#define MOBY_OCLASS 0xAAu
#define MOBY_SIZE 0x100u
#define SLOT_PVARS 0x80u

/* OPENRAC_RELOCATE_* of openrac_mod.h. */
#define RELOCATE_HOMES 0x1u
#define RELOCATE_FREE 0x2u
#define RELOCATE_WAKE 0x4u
#define RELOCATE_ANYWHERE 0x8u
#define RELOCATE_NEAREST 0x10u

/* A pvars place this near the moby's old place is one of its own, carried along. */
#define HOME_NEAR 1.0f
/* The floor's search under the point, and a slope a walker stands on (its normal's up). */
#define FLOOR_ABOVE 2.5f
#define FLOOR_BELOW 8.0f
#define FLOOR_UP 0.5f
#define FLOOR_TRIANGLES 1024

#define BRUTE 2501
#define CHICKENBOT 2638

/* collision_triangles.c's triangle: openrac_collision_triangle's layout. */
typedef struct {
    float corners[3][3];
    float normal[3];
    uint32_t type;
    int32_t moby;
} Triangle;

static Triangle s_triangles[FLOOR_TRIANGLES];

/* The game's line test (CollLine, func_002761F8; coll_line.c bounds it) and where its hit is:
 * g_coll_hit_position (D_001BAFA0) and g_coll_hit_normal (D_001BAFC0). 0x12 is the mask its camera
 * probes the ground with (cam_update_grounded, func_00271E88). */
#define COLL_HIT_POSITION OPENRAC_DATA(RAC2_ADDR(0x001BAFA0u))
#define COLL_HIT_NORMAL OPENRAC_DATA(RAC2_ADDR(0x001BAFC0u))
#define COLL_GROUND_MASK 0x12

/* The highest floor under (x, y) from z + FLOOR_ABOVE down to z - FLOOR_BELOW by the game's own line
 * test, the whole level's collision (walls and steep faces passed through, up to six hits), in
 * *out; 0 for none. */
static int floor_by_line(const float p[3], float* out) {
    GFRAME(line, 32);
    float* top = (float*)G(line);
    float* bottom = (float*)G(line + 16u);
    top[0] = bottom[0] = p[0];
    top[1] = bottom[1] = p[1];
    top[2] = p[2] + FLOOR_ABOVE;
    bottom[2] = p[2] - FLOOR_BELOW;
    top[3] = bottom[3] = 1.0f;
    for (int i = 0; i < 6; ++i) {
        if (func_002761F8(line, line + 16u, COLL_GROUND_MASK, 0, 0) == 0) {
            return 0;
        }
        const float hit = GREF(float, COLL_HIT_POSITION + 8u);
        if (GREF(float, COLL_HIT_NORMAL + 8u) >= FLOOR_UP) {
            *out = hit;
            return 1;
        }
        top[2] = hit - 0.01f;
        if (top[2] <= bottom[2]) {
            return 0;
        }
    }
    return 0;
}

/* The highest floor of the level under (x, y) from z + FLOOR_ABOVE down to z - FLOOR_BELOW, in
 * *out; 0 for none. The game's line first; the triangles collision_triangles.c reads (the cells of
 * the collision mesh it has at hand, and the mobys' meshes: a moving platform is ground too). */
static int floor_under(const float p[3], float* out) {
    if (floor_by_line(p, out)) {
        return 1;
    }
    const float lo[3] = {p[0] - 0.25f, p[1] - 0.25f, p[2] - FLOOR_BELOW};
    const float hi[3] = {p[0] + 0.25f, p[1] + 0.25f, p[2] + FLOOR_ABOVE};
    int n = rac2_collision_triangles(lo, hi, s_triangles, FLOOR_TRIANGLES);
    if (n > FLOOR_TRIANGLES) {
        n = FLOOR_TRIANGLES;
    }
    int found = 0;
    float best = 0.0f;
    for (int i = 0; i < n; ++i) {
        const Triangle* t = &s_triangles[i];
        if (!(t->normal[2] >= FLOOR_UP)) {
            continue;
        }
        /* Whether (x, y) is inside the triangle seen from above, and its height there. */
        const float* a = t->corners[0];
        const float* b = t->corners[1];
        const float* c = t->corners[2];
        const float d = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1]);
        if (fabsf(d) < 1e-6f) {
            continue;
        }
        const float u = ((b[1] - c[1]) * (p[0] - c[0]) + (c[0] - b[0]) * (p[1] - c[1])) / d;
        const float v = ((c[1] - a[1]) * (p[0] - c[0]) + (a[0] - c[0]) * (p[1] - c[1])) / d;
        const float w = 1.0f - u - v;
        if (u < -1e-4f || v < -1e-4f || w < -1e-4f) {
            continue;
        }
        const float z = u * a[2] + v * b[2] + w * c[2];
        if (z > p[2] + FLOOR_ABOVE || z < p[2] - FLOOR_BELOW) {
            continue;
        }
        if (!found || z > best) {
            best = z;
            found = 1;
        }
    }
    if (found) {
        *out = best;
    }
    return found;
}

/* OPENRAC_RELOCATE_NEAREST, for a point with no floor under it (a spot past a platform's edge): the
 * nearest point with one within NEAREST_RADIUS across, rings NEAREST_STEP apart, each with points
 * NEAREST_STEP apart around it; `p` is moved there and its floor put in *out. 0 for none. */
#define NEAREST_RADIUS 6.0f
#define NEAREST_STEP 0.5f
static int nearest_floor(float p[3], float* out) {
    for (float r = NEAREST_STEP; r <= NEAREST_RADIUS + 1e-3f; r += NEAREST_STEP) {
        const int around = (int)ceilf(6.2831853f * r / NEAREST_STEP);
        for (int i = 0; i < around; ++i) {
            const float turn = 6.2831853f * (float)i / (float)around;
            float q[3] = {p[0] + r * cosf(turn), p[1] + r * sinf(turn), p[2]};
            if (floor_under(q, out)) {
                p[0] = q[0];
                p[1] = q[1];
                return 1;
            }
        }
    }
    return 0;
}

/* The bytes of pvars of the moby at `m`: a copy's as the port made them, a placed one's from the
 * level's file, a spawned one's its slot's. */
static uint32_t pvars_bytes(gaddr m, gaddr dynamic) {
    uint32_t bytes = 0;
    if (rac2_copy_pvars(m, &bytes) != 0) {
        return bytes;
    }
    return m < dynamic ? rac2_placed_pvars_bytes(m) : SLOT_PVARS;
}

static void put_floats(gaddr at, const float v[3]) {
    for (unsigned k = 0; k < 3; ++k) {
        GREF(float, at + 4u * k) = v[k];
    }
}

int rac2_moby_relocate(int32_t slot, float to[3], uint32_t flags) {
    const gaddr base = GREF(gaddr, OPENRAC_DATA(RAC2_g_mobyTableBase));
    const gaddr dynamic = GREF(gaddr, OPENRAC_DATA(RAC2_g_mobyTableBase + 4u));
    const gaddr end = GREF(gaddr, OPENRAC_DATA(RAC2_g_mobyTableEnd));
    if (slot < 0 || base == 0 || to == NULL || !isfinite(to[0]) || !isfinite(to[1])
        || !isfinite(to[2])) {
        return -1;
    }
    const gaddr m = base + (gaddr)slot * MOBY_SIZE;
    if (m >= end || GREF(uint8_t, m + MOBY_STATE) >= 0x80u) {
        return -1;
    }
    if ((flags & RELOCATE_ANYWHERE) == 0) {
        float floor = 0.0f;
        if (!floor_under(to, &floor)) {
            if ((flags & RELOCATE_NEAREST) == 0 || !nearest_floor(to, &floor)) {
                return -2;
            }
        }
        to[2] = floor;
    }
    float was[3], delta[3];
    for (unsigned k = 0; k < 3; ++k) {
        was[k] = GREF(float, m + MOBY_POS + 4u * k);
        delta[k] = to[k] - was[k];
    }
    put_floats(m + MOBY_POS, to);
    /* The sphere is kept in 1/1024 units (func_002A4E68; profile.cpp's moby_sphere): moved in world
     * units it stayed at the old place, and MobyProc's distance test culled the moby there (a
     * Chickenbot copied out of its drop ship ran at Ratchet unseen). */
    if (GREF(float, m + MOBY_SPHERE + 0x0Cu) > 0.0f) {
        for (unsigned k = 0; k < 3; ++k) {
            GREF(float, m + MOBY_SPHERE + 4u * k) += delta[k] * 1024.0f;
        }
    }
    const gaddr pv = GREF(gaddr, m + MOBY_PVARS);
    const uint32_t bytes = pv != 0 ? pvars_bytes(m, dynamic) : 0;
    /* A copy's pvars may be in the port's room past the game's RAM (enemy_copies.c). */
    const int pv_ok = bytes >= 12u && (rac2_copy_pvars(m, NULL) == pv || rac2_ram_at(pv, bytes));
    if ((flags & RELOCATE_HOMES) != 0 && pv_ok) {
        for (uint32_t off = 0; off + 12u <= bytes; off += 4u) {
            float v[3];
            int near = 1;
            for (unsigned k = 0; k < 3 && near; ++k) {
                v[k] = GREF(float, pv + off + 4u * k);
                near = isfinite(v[k]) && fabsf(v[k] - was[k]) < HOME_NEAR;
            }
            if (near) {
                for (unsigned k = 0; k < 3; ++k) {
                    v[k] += delta[k];
                }
                put_floats(pv + off, v);
                off += 8u; /* the next word after this place */
            }
        }
    }
    const int oclass = GREF(int16_t, m + MOBY_OCLASS);
    const uint8_t state = GREF(uint8_t, m + MOBY_STATE);
    if ((flags & RELOCATE_FREE) != 0 && oclass == BRUTE && pv_ok && bytes >= 0x35Cu) {
        GREF(int32_t, pv + 0x320u) = -1;
        GREF(int32_t, pv + 0x328u) = -1;
        put_floats(pv + 0x340u, to);
        put_floats(pv + 0x350u, to);
    } else if ((flags & RELOCATE_FREE) != 0 && oclass == CHICKENBOT && pv_ok && bytes >= 0x2ECu) {
        GREF(int32_t, pv + 0x210u) = -1;
        GREF(int32_t, pv + 0x218u) = -1;
        put_floats(pv + 0x2E0u, to);
    }
    if ((flags & RELOCATE_WAKE) != 0 && pv != 0) {
        if (oclass == CHICKENBOT && state == 13) {
            /* In its drop ship it is hidden (+0x34 bits 0x81, 0x71 there) and has no collision
             * (+0x98 0); the drop that shows it is skipped, so both are given back here: its
             * class's collision (class header +0x10). Without it a woken one ran at Ratchet unseen
             * and unhittable. */
            GREF(uint8_t, m + 0x34u) = (uint8_t)(GREF(uint8_t, m + 0x34u) & ~0x81u);
            const gaddr cls = GREF(gaddr, m + 0x24u);
            if (GREF(gaddr, m + 0x98u) == 0 && cls != 0) {
                GREF(gaddr, m + 0x98u) = GREF(gaddr, cls + 0x10u);
            }
            func_002A7750(m, 9, 2);
        } else if (oclass == BRUTE && state >= 13 && state <= 22 && state != 16) {
            func_002A7750(m, 1, 0);
        }
    }
    func_002A0F48(m);
    return 0;
}
