/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 the OpenRAC contributors
 *
 * Going Commando: the game's Glider in any level (OpenRAC Commando, interface 1.127:
 * openrac_api.glider_pad_add).
 *
 * In the game only Tabora, Dobbo and Grelbin have the Glider: a launch pad (moby class 2440, its
 * update only draws its glow) and the glider itself (class 2309, func_L08_003CDF98, the shared
 * code of levels 8, 9 and 19). The hero stands on the pad (his target and the platform he stands on
 * are the pad), X puts him in state 0x31 (ridden: every level's hero code has it, and nothing in his
 * state machine moves him in it) and the glider moves him each tick with the stick, until it comes
 * into a landing cuboid of Tabora's, hits a wall (a tumble, then the crash) or hits what the level
 * kills with (material 1).
 *
 * Here the two classes are read from the player's disc and registered in the level (foreign_class.c,
 * rac2_foreign_want), the hero's flying seqs (128 to 136, which most levels lack) are imported from
 * Tabora's core (hero_seqs.c), and the glider's update is this file's: the states of
 * func_L08_003CDF98 written again with the engine's own calls, Tabora's tuning read from its program
 * on the disc (openrac_guest_level_word), the loaded level's own hero_enter_state, and in place of
 * Tabora's data (its cuboids, scripts, help text, sounds and camera object) a landing zone the mod
 * gives. No sounds (the classes' sounds are another level's). The glider class draws nothing in
 * flight in the port, as in Tabora: what is seen is the hero's flying seqs and the smoke trail.
 *
 * v1.01 (NTSC-U) alone: the addresses below are its (RAC2_PRIMARY). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game_protos.h"
#include "openrac/foreign.h"
#include "openrac/game_host.h"
#include "openrac/guest.h"
#include "openrac/host_env.h"
#include "rac2.h"

#if defined(RAC2_PRIMARY)

/* ---- The game's places ---- */
#define HERO RAC2_ADDR(0x00189E20u)
#define HERO_POS (HERO + 0x80u)          /* g_hero_position; +0x10 his rotation */
#define HERO_TARGET (HERO + 0x33Cu)      /* the moby under his feet */
#define HERO_CAN_MOUNT (HERO + 0x340u)   /* nonzero: he may take a vehicle (on the ground) */
#define HERO_BUSY (HERO + 0x34Eu)        /* s16, 0: free */
#define HERO_ANIM_SPEED (HERO + 0xC20u)
#define HERO_ANIM_FLAGS (HERO + 0xC28u)  /* 2: his animation reached its end */
#define HERO_ANIM_TIME (HERO + 0xC38u)
#define HERO_BANK (HERO + 0x1FB4u)
#define HERO_VEHICLE_OBJ (HERO + 0x1830u)
#define HERO_MOBY (HERO + 0x2290u)
#define HERO_STATE (HERO + 0x2294u)
#define HERO_TICK_SCALE (HERO + 0x24B8u)
#define HERO_CONTACT (HERO + 0x24CCu)
#define PLATFORM RAC2_ADDR(0x0018A15Cu)  /* g_platform_actor_ref[0] */
#define PAD_HELD RAC2_ADDR(0x00138324u)  /* g_pad_held_flags[0]: 0x40 X */
#define PAD_STATE RAC2_ADDR(0x00138180u) /* g_pad_state: +0x108 the stick, +0x1A0 flags */
#define OWNED RAC2_ADDR(0x001A7AF8u)     /* g_inventoryOwned */
#define MAP_INDEX RAC2_ADDR(0x001A79F0u)
#define GAME_STATE OPENRAC_DATA(RAC2_g_nGameState) /* 0 in play */
/* CollLine's hit (g_collision_l08_hit_point, Tabora's place): +0x10 the point, +0x20 the normal. */
#define HIT_L08 0x001BFF60u

#define MOBY_POS 0x10u
#define MOBY_STATE 0x20u
#define MOBY_FLAGS 0x34u
#define MOBY_UPDATE 0x64u
#define MOBY_PVARS 0x68u
#define MOBY_ROT 0xF0u
#define MOBY_OCLASS 0xAAu

#define CLASS_GLIDER 2309
#define CLASS_PAD 2440
#define FROM_LEVEL 8 /* Tabora: the classes, the seqs and the tuning */

/* The hero's seqs the glider starts: 0x80 the crash, 0x81 to 0x86 flying and banking, 0x87 the
 * take-off, 0x88 the landing. */
#define SEQ_FIRST 0x80
#define SEQ_LAST 0x88

/* Tabora's tuning: its program's words from 0x1AFFD0 (g_vehicle_df8_step) to 0x1B0140
 * (g_vehicle_active_flag), read once from the disc. */
#define K_FIRST 0x001AFFD0u
#define K_WORDS ((0x001B0144u - K_FIRST) / 4u)
static uint32_t s_k[K_WORDS];
static int s_k_read;
static float KF(uint32_t a) {
    float f;
    memcpy(&f, &s_k[(a - K_FIRST) / 4u], 4);
    return f;
}
static int32_t KI(uint32_t a) {
    return (int32_t)s_k[(a - K_FIRST) / 4u];
}
#define K_DF8_STEP KF(0x001AFFD0u)
#define K_TIMER_START KI(0x001AFFDCu)
#define K_TIMER_LENGTH KI(0x001AFFE0u)
#define K_TIMER_MARGIN KI(0x001AFFE4u)
#define K_TARGET_ANGLE KF(0x001AFFE8u)
#define K_PROBE_Z KF(0x001AFFECu)
#define K_BOUNCE_A KF(0x001AFFF0u)
#define K_BOUNCE_B KF(0x001AFFF4u)
#define K_MAX_SPEED KF(0x001AFFF8u)
#define K_SPEED_ACCEL KF(0x001AFFFCu)
#define K_TURN_RATE KF(0x001B0000u)
#define K_BANK_LIMIT_A KF(0x001B0004u)
#define K_BANK_LIMIT_B KF(0x001B0008u)
#define K_SLOPE_THRESHOLD KF(0x001B000Cu)
#define K_SLOW_SPEED KF(0x001B0010u)
#define K_SLOPE_LIMIT KF(0x001B0014u)
#define K_CRUISE_SPEED KF(0x001B0018u)
#define K_STEERING_FLAG KI(0x001B001Cu)
#define K_GAIN_LOW KF(0x001B0020u)
#define K_GAIN_HIGH KF(0x001B0024u)
#define K_GAIN_A KF(0x001B0028u)
#define K_GAIN_B KF(0x001B002Cu)
#define K_LIMIT_LOW KF(0x001B0030u)
#define K_LIMIT_HIGH KF(0x001B0034u)
#define K_LIMIT_A KF(0x001B0038u)
#define K_LIMIT_B KF(0x001B003Cu)
#define K_LIMIT2_LOW KF(0x001B0040u)
#define K_LIMIT2_HIGH KF(0x001B0044u)
#define K_LIMIT_C KF(0x001B0048u)
#define K_TARGET_SCALE KF(0x001B004Cu)
#define K_APPROACH_TARGET KF(0x001B0050u)
#define K_APPROACH_ACCEL KF(0x001B0054u)
#define K_APPROACH_DECEL KF(0x001B0058u)
#define K_DIVISOR_Y KF(0x001B005Cu)
#define K_ROT_SCALE_Y KF(0x001B0060u)
#define K_DIVISOR_X KF(0x001B0064u)
#define K_MULT_Y KF(0x001B0068u)
#define K_MULT_X KF(0x001B006Cu)
#define K_BANK_LIMIT_DEG KF(0x001B0070u)
#define K_TURN_STEP_SCALE KF(0x001B0074u)
#define K_TICK_TURN_SCALE KF(0x001B0078u)
#define K_BANK_APPROACH KF(0x001B0080u)
#define K_DF50_FRAMES KI(0x001B0084u)
#define K_DECEL_A KF(0x001B0088u)
#define K_ANGLE_LIMIT KF(0x001B008Cu)
#define K_DF50_RESET KI(0x001B0090u)
#define K_ANGLE_THRESHOLD KF(0x001B0094u)
#define K_YAW_STEP KF(0x001B0098u)
#define K_DECEL_B KF(0x001B009Cu)
#define K_DF64_OFFSET KF(0x001B00A0u)
#define K_CAM_FAR KF(0x001B00E0u)
#define K_CAM_NEAR KF(0x001B00E4u)
#define K_CAM_HEIGHT KF(0x001B00E8u)
#define K_CAM_OFFSET_X KF(0x001B00F0u)
#define K_CAM_OFFSET_Y KI(0x001B00F4u)
#define K_CAM_OFFSET_Z KF(0x001B00F8u)
#define K_PUSH_SPEED KF(0x001B00FCu)
#define K_MAX_REVERSE KF(0x001B0100u)
#define K_REVERSE_LIMIT KF(0x001B0104u)
#define K_DF50_ALT_FRAMES KI(0x001B0108u)
#define K_DECEL_C KF(0x001B010Cu)
#define K_COLLISION_VELOCITY_SCALE KF(0x001B0118u)
#define K_FALL_SPEED KF(0x001B0120u)
#define K_VELOCITY_DAMPING KF(0x001B0124u)
#define K_VELOCITY_LENGTH_SCALE KF(0x001B0128u)
#define K_THRESHOLD_FRAMES KI(0x001B0130u)
#define K_TARGET_ANGLE_STEP KF(0x001B0134u)
#define K_ANGLE_STEP_ZERO KF(0x001B0138u)
#define K_UNK_RATIO_SCALE KF(0x001B013Cu)
#define K_ACTIVE_FLAG KI(0x001B0140u)

/* Each level's hero_enter_state (state, animate), v1.01: the ones whose code has state 0x31. */
static const struct {
    int level;
    gaddr fn;
} k_enter_state[] = {
    {0, 0x002C42E0u},  {1, 0x002BEB90u},  {3, 0x002C6F08u},  {4, 0x002E7C08u},  {7, 0x002BE770u},
    {8, 0x002D2ED8u},  {9, 0x002C3580u},  {11, 0x002D8EF8u}, {12, 0x002C8BD0u}, {13, 0x002CEF00u},
    {16, 0x002C0BC8u}, {18, 0x002E0070u}, {19, 0x002C44F8u}, {20, 0x002D7730u}, {22, 0x002C9898u},
};

static gaddr enter_state_of(int level) {
    for (size_t i = 0; i < sizeof k_enter_state / sizeof k_enter_state[0]; ++i) {
        if (k_enter_state[i].level == level) {
            return k_enter_state[i].fn;
        }
    }
    return 0;
}

static void hero_enter_state(int state, int animate) {
    const gaddr fn = enter_state_of(openrac_game_loaded_overlay());
    if (fn == 0) {
        return;
    }
    const uint64_t ints[8] = {(uint64_t)(int64_t)state, (uint64_t)(int64_t)animate, 0, 0, 0, 0, 0, 0};
    const float floats[8] = {0};
    float result;
    (void)openrac_guest_call_regs(fn, ints, floats, &result, 0);
}

/* ---- The mods' pads ---- */
#define PADS_MAX 32
#define EVENTS_MAX 32
#define PVARS_BYTES 0x100u
#define TAG_GLIDER 0x0FF00200u
#define TAG_PAD 0x0FF00210u

typedef struct {
    int used;
    openrac_game_glider_pad def;
    /* This load's: the mobys, the glider's pvars, whether the hero stood on it. */
    uint32_t loads;
    gaddr pad, glider, d;
    int on;
    int refused_held;
} Pad;

static Pad s_pads[PADS_MAX];
static int s_class_glider = -1, s_class_pad = -1;
static openrac_game_glider_event s_events[EVENTS_MAX];
static int s_event_count;
static gaddr s_scratch;

static void event(int kind, int pad, const float* at) {
    if (s_event_count >= EVENTS_MAX) {
        return;
    }
    openrac_game_glider_event* e = &s_events[s_event_count++];
    e->kind = kind;
    e->pad = pad;
    for (int i = 0; i < 3; ++i) {
        e->position[i] = at != NULL ? at[i] : 0.0f;
    }
}

/* ---- Small vector helpers on the game's memory ---- */
#define DF(o) GREF(float, d + (o))
#define DI(o) GREF(int32_t, d + (o))
#define OB(o) GREF(uint8_t, obj + (o))
#define OH(o) GREF(uint16_t, obj + (o))
#define V(a, i) GREF(float, (a) + 4u * (i))

static void qcopy(gaddr to, gaddr from) {
    memmove(G(to), G(from), 16);
}
static void vadd(gaddr out, gaddr a, gaddr b) {
    for (unsigned i = 0; i < 3; ++i) {
        V(out, i) = V(a, i) + V(b, i);
    }
}
static void vsub(gaddr out, gaddr a, gaddr b) {
    for (unsigned i = 0; i < 3; ++i) {
        V(out, i) = V(a, i) - V(b, i);
    }
}
static void vscale(gaddr out, gaddr a, float k) {
    for (unsigned i = 0; i < 3; ++i) {
        V(out, i) = V(a, i) * k;
    }
}
static float vlen(gaddr a) {
    return sqrtf(V(a, 0) * V(a, 0) + V(a, 1) * V(a, 1) + V(a, 2) * V(a, 2));
}
static float vdist(gaddr a, gaddr b) {
    const float x = V(a, 0) - V(b, 0), y = V(a, 1) - V(b, 1), z = V(a, 2) - V(b, 2);
    return sqrtf(x * x + y * y + z * z);
}
static void vscale_to(gaddr out, gaddr a, float len) {
    const float l = vlen(a);
    const float k = l > 0.0f ? len / l : 0.0f;
    vscale(out, a, k);
}
static void vzero(gaddr a) {
    memset(G(a), 0, 16);
}
static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

#define FAST_SUB(a, b) func_00283BE0((a), (b))
#define FAST_ADD(a, b) func_00283B98((a), (b))
#define SIN_A(a) func_00283180(a)
#define SIN_B(a) func_00283198(a)
#define TIMER(a) func_00282948(a) /* math_fast_float_d: counts the word down, 1 at its end */

static void anim(gaddr obj, int seq, int blend) {
    func_002A7470(obj, seq, 0, blend);
}
static void set_state(gaddr obj, int state) {
    func_002A7750(obj, state, -1);
}
static void hero_anim(int seq, float blend) {
    func_L00_002D2168(seq, 0, blend);
}

static void place_at_stored(gaddr obj, gaddr d) {
    qcopy(obj + MOBY_POS, d);
    qcopy(obj + MOBY_ROT, d + 0x10u);
    func_002A0F48(obj);
}

/* hero_copy_mount_transform_to_object, hero_move_with_mount_to_transform,
 * hero_save_transform_snap_object (func_L08_003CD600-003CD6C8). */
static void copy_mount_to(gaddr obj) {
    const gaddr hero = GREF(gaddr, HERO_MOBY);
    if (hero == 0) {
        return;
    }
    qcopy(obj + MOBY_POS, hero + MOBY_POS);
    qcopy(obj + MOBY_ROT, hero + MOBY_ROT);
    func_002A0F48(obj);
}
static void move_with_mount(gaddr obj, gaddr d) {
    qcopy(HERO_POS, d);
    qcopy(HERO_POS + 0x10u, d + 0x10u);
    const gaddr hero = GREF(gaddr, HERO_MOBY);
    if (hero != 0) {
        qcopy(hero + MOBY_POS, d);
        qcopy(hero + MOBY_ROT, d + 0x10u);
        func_002A0F48(hero);
    }
    copy_mount_to(obj);
}
static void save_and_snap(gaddr obj, gaddr d) {
    qcopy(d, HERO_POS);
    qcopy(d + 0x10u, HERO_POS + 0x10u);
    copy_mount_to(obj);
}

/* cam_update_aim_values (func_L08_002F2D10) without Tabora's camera object and cuboid. */
static void cam_aim(gaddr counter) {
    func_L00_00424A88(K_CAM_OFFSET_Y, K_CAM_OFFSET_X, K_CAM_OFFSET_Z);
    func_L00_00424A08(0, K_CAM_NEAR, K_CAM_FAR);
    func_L00_00424A50(K_CAM_HEIGHT, 0.003f);
    func_L00_00424C28();
    float fade = (float)GREF(int32_t, counter) / 90.0f;
    if (fade > 1.0f) {
        fade = 1.0f;
    }
    func_L00_00424B70(fade * 0.985f + 0.015f, fade * 0.8f + 0.2f);
    func_L00_00424BB8(fade * 0.044999998f + 0.015f, 0.2f);
    func_L00_00424C00();
    if (GREF(int32_t, counter) < 0x5A) {
        GREF(int32_t, counter) += 1;
    }
}

/* vehicle_update_steering (func_L08_003CD708). */
static void steering(gaddr obj, gaddr d, float input) {
    float gain = K_GAIN_A;
    float limit = K_LIMIT_A;
    float pitch_bias = 0.0f;
    if (OB(MOBY_STATE) == 8) {
        limit = K_LIMIT_C;
        if (DI(0x50) == 0) {
            pitch_bias = -2.0f;
        }
    }
    float tx = (GREF(float, PAD_STATE + 0x108u) - GREF(float, PAD_STATE + 0x114u) + GREF(float, PAD_STATE + 0x110u)) * input;
    float ty = (GREF(float, PAD_STATE + 0x10Cu) - GREF(float, PAD_STATE + 0x118u) + GREF(float, PAD_STATE + 0x11Cu) + pitch_bias) * input;
    tx = clampf(tx, -input, input);
    ty = clampf(ty, -input, input);
    if (GREF(uint32_t, PAD_STATE + 0x1A0u) & 0x200u) {
        tx = tx / K_DIVISOR_X * K_MULT_X;
        ty = ty / K_DIVISOR_Y * K_MULT_Y;
        gain = K_GAIN_B;
        limit = K_LIMIT_B;
    }
    const unsigned mode = OB(MOBY_STATE);
    if (!(mode == 7 || mode < 6)) {
        if (K_STEERING_FLAG) {
            float bank_limit;
            if (mode != 8) {
                func_002AA3A8(d + 0x60u, d + 0x44u, ty, K_LIMIT_LOW, K_LIMIT_HIGH, limit);
                if (K_ACTIVE_FLAG) {
                    DF(0x14) = FAST_SUB(DF(0x14), DF(0x60) * 0.00029088825f * K_DIVISOR_Y);
                }
                bank_limit = K_BANK_LIMIT_A;
            } else {
                func_002AA3A8(d + 0x60u, d + 0x44u, ty, K_LIMIT2_LOW, K_LIMIT2_HIGH, limit);
                if (K_ACTIVE_FLAG) {
                    DF(0x14) = FAST_SUB(DF(0x14), DF(0x60) * 0.00029088825f * K_ROT_SCALE_Y);
                }
                bank_limit = K_BANK_LIMIT_B;
            }
            bank_limit *= 0.017453294f;
            DF(0x14) = clampf(DF(0x14), -bank_limit, bank_limit);
        } else {
            const float bank = GREF(float, HERO_BANK);
            if (fabsf(bank) < fabsf(DF(0x60))) {
                func_002AA2E8(d + 0x60u, -bank, K_BANK_APPROACH);
            } else {
                DF(0x60) = -bank;
            }
            const float turn_step = DF(0x60) * 0.017453294f * K_TURN_STEP_SCALE;
            if (fabsf(DF(0x60)) > 0.2f) {
                float turn = FAST_SUB(turn_step, DF(0x14));
                const float lim = K_BANK_LIMIT_DEG * 0.00029088825f;
                turn = clampf(turn, -lim, lim);
                func_002AA2E8(d + 0x44u, turn, K_TICK_TURN_SCALE * (GREF(float, HERO_TICK_SCALE) * 4.8481375e-06f));
                DF(0x14) = FAST_ADD(DF(0x14), DF(0x44));
            } else {
                func_002AA898(d + 0x14u, 0.0f, d + 0x44u, 0.011f, 0.3f, 0.014544412f, 0);
            }
        }
    }
    func_002AA3A8(d + 0x5Cu, d + 0x48u, tx, K_GAIN_LOW, K_GAIN_HIGH, gain);
    if (K_ACTIVE_FLAG) {
        func_002AA898(d + 0x10u, DF(0x5C) * K_TARGET_SCALE, d + 0x40u, K_APPROACH_TARGET, K_APPROACH_ACCEL, K_APPROACH_DECEL, 0);
        DF(0x18) = FAST_SUB(DF(0x18), DF(0x5C) * 0.00029088825f * K_DIVISOR_X);
    }
}

static void back_to_pad(Pad* p, gaddr obj, gaddr d);

/* vehicle_handle_collision (func_L08_003CDB78): 1 on a hit. */
static int handle_collision(Pad* p, int index, gaddr obj, gaddr d, gaddr prev) {
    const gaddr hero_moby = GREF(gaddr, HERO_MOBY);
    const gaddr t = s_scratch, hit = s_scratch + 0x10u;
    vscale_to(t, obj + 0xC0u, K_BOUNCE_B);
    vadd(hit, t, obj + MOBY_POS);
    vscale_to(t, obj + 0xE0u, K_BOUNCE_A);
    vadd(hit, hit, t);
    const gaddr g = OPENRAC_LDATA(FROM_LEVEL, HIT_L08);
    int found = func_00277258(hit, 0, hero_moby, K_PROBE_Z);
    if (!found) {
        func_0029FCC0(obj, 1, t);
        found = func_002761F8(hit, t, 0x10, hero_moby, 0);
        if (!found) {
            func_0029FCC0(obj, 4, t);
            found = func_002761F8(hit, t, 0x10, hero_moby, 0);
            if (!found) {
                qcopy(hit, prev);
                qcopy(t, d);
                found = func_002761F8(prev, d, 0x10, hero_moby, 0);
            }
        }
        if (found) {
            const float full = vdist(hit, t);
            const float part = vdist(hit, g);
            vsub(g + 0x10u, hit, t);
            vscale_to(g + 0x10u, g + 0x10u, full - part);
            vadd(g + 0x10u, g + 0x10u, hit);
        }
    }
    if (!found) {
        return 0;
    }
    vsub(hit, d, hit);
    vadd(g + 0x10u, g + 0x10u, hit);
    const int material = GREF(int32_t, MAP_INDEX) != 9 ? func_00278E78() : 0;
    if (material == 1) {
        /* What the level kills with: the hero let go there, the glider back on its pad. */
        float at[3] = {DF(0), DF(4), DF(8)};
        DI(0x50) = 0;
        hero_enter_state(0, 1);
        back_to_pad(p, obj, d);
        GREF(int32_t, HERO_CONTACT) = material;
        event(OPENRAC_GAME_GLIDER_KILLED, index, at);
        return 1;
    }
    qcopy(d, g + 0x10u);
    func_00283028(d + 0x20u, d + 0x20u, g + 0x20u);
    vscale(d + 0x20u, d + 0x20u, K_COLLISION_VELOCITY_SCALE);
    const float fall = K_FALL_SPEED * 0.00027777778f;
    DF(0x28) -= fall;
    if (OB(MOBY_STATE) != 9) {
        if (DF(0x28) > 0.0f) {
            DF(0x28) = -fall;
        }
        hero_anim(0x80, 15.0f);
        set_state(obj, 9);
    }
    vadd(obj + MOBY_POS, obj + MOBY_POS, d + 0x20u);
    return 1;
}

/* The glider idle on its pad again, hidden (state 0 of the game's, then 1). */
static void back_to_pad(Pad* p, gaddr obj, gaddr d) {
    if (GREF(gaddr, HERO_VEHICLE_OBJ) == obj) {
        GREF(gaddr, HERO_VEHICLE_OBJ) = 0;
    }
    if (OB(0x43) != 0) {
        anim(obj, 0, 0xA);
    }
    qcopy(d, p->pad + MOBY_POS);
    qcopy(d + 0x10u, p->pad + MOBY_ROT);
    DF(8) += K_DF8_STEP;
    place_at_stored(obj, d);
    OB(0x31) = 0;
    OH(MOBY_FLAGS) |= 1u;
    set_state(obj, 1);
}

static Pad* pad_of_glider(gaddr obj, int* index) {
    for (int i = 0; i < PADS_MAX; ++i) {
        if (s_pads[i].used && s_pads[i].glider == obj && s_pads[i].loads == rac2_program_loads()) {
            *index = i;
            return &s_pads[i];
        }
    }
    return NULL;
}

static int owns(const Pad* p) {
    return p->def.item < 0 || (p->def.item < 0x60 && GREF(uint8_t, OWNED + (gaddr)p->def.item) != 0);
}

/* The glider's update (func_L08_003CDF98's states), called by the moby loop. */
static void glider_update(gaddr obj) {
    int index = -1;
    Pad* p = pad_of_glider(obj, &index);
    if (p == NULL) {
        return;
    }
    const gaddr d = GREF(gaddr, obj + MOBY_PVARS);
    const gaddr prev = s_scratch + 0x40u;
    qcopy(prev, d);
    if (OH(0x96) < 0x7FFDu) {
        OH(0x96) = (uint16_t)(OH(0x96) + 1u);
    }
    switch (OB(MOBY_STATE)) {
    case 0:
        qcopy(d, p->pad + MOBY_POS);
        qcopy(d + 0x10u, p->pad + MOBY_ROT);
        place_at_stored(obj, d);
        DF(8) += K_DF8_STEP;
        DF(0x3C) = 1.0f;
        if (OB(0x43) != 0) {
            anim(obj, 0, 0xA);
        }
        OB(0x30) = 0xFF;
        OB(0x31) = 0;
        GREF(int16_t, obj + 0x32u) = 0x3FF;
        OH(MOBY_FLAGS) |= 1u;
        set_state(obj, 1);
        DI(0x98) = 0;
        DI(0xB0) = 0;
        break;
    case 1: {
        const int on = GREF(gaddr, PLATFORM) == p->pad;
        if (on && owns(p)) {
            if (OB(0x43) != 0) {
                anim(obj, 0, 0xA);
            }
            set_state(obj, 2);
        }
        break;
    }
    case 2: {
        save_and_snap(obj, d);
        if (GREF(gaddr, HERO_TARGET) != p->pad) {
            if (OB(0x43) != 0) {
                anim(obj, 0, 0xA);
            }
            qcopy(d, p->pad + MOBY_POS);
            DF(8) += K_DF8_STEP;
            set_state(obj, 1);
            break;
        }
        if ((GREF(uint32_t, PAD_HELD) & 0x40u) && GREF(int32_t, HERO_CAN_MOUNT) != 0
            && GREF(int16_t, HERO_BUSY) == 0) {
            hero_enter_state(0x31, 0);
            hero_anim(0x87, 3.0f);
            GREF(int32_t, GREF(gaddr, HERO_MOBY) + 0x9Cu) = -1;
            vscale_to(d + 0x20u, p->pad + 0xC0u, K_PUSH_SPEED * 0.016666668f);
            DI(0x50) = K_DF50_ALT_FRAMES;
            DF(0x28) = K_MAX_REVERSE * 0.016666668f;
            DF(0x18) = GREF(float, p->pad + 0xF8u);
            GREF(gaddr, HERO_VEHICLE_OBJ) = obj;
            DI(0x9C) = 0;
            set_state(obj, 3);
            float at[3] = {DF(0), DF(4), DF(8)};
            event(OPENRAC_GAME_GLIDER_LAUNCH, index, at);
        }
        break;
    }
    case 3:
        if (func_002B3868(obj) != 0) {
            DI(0xB0) = 0x1E;
        } else if (TIMER(d + 0xB0u) != 0) {
            DI(0xB0) = 0xD2;
        }
        if (DI(0x9C) != 0 || GREF(float, HERO_ANIM_TIME) > 10.0f) {
            DI(0x9C) |= 4;
            func_002AA800(d + 0x14u, 0, K_TARGET_ANGLE * 0.017453294f, K_TARGET_ANGLE_STEP * 0.00029088825f);
            if (K_ACTIVE_FLAG) {
                vadd(d, d, d + 0x20u);
            }
            DF(0x28) -= K_DECEL_C * 0.00027777778f;
            if (DF(0x28) < -(K_REVERSE_LIMIT * 0.016666668f)) {
                DI(0x9C) |= 1;
            }
            if (GREF(uint32_t, HERO_ANIM_FLAGS) & 2u) {
                DI(0x9C) |= 2;
                hero_anim(0x81, 15.0f);
            }
            if (TIMER(d + 0x50u) != 0 && DI(0x9C) == 7) {
                if (OH(MOBY_FLAGS) & 1u) {
                    OH(MOBY_FLAGS) &= 0xFFFEu;
                    OB(0x31) = 1;
                    if (OB(0x43) != 1) {
                        anim(obj, 1, 5);
                    }
                }
                DI(0x50) = K_TIMER_START;
                hero_enter_state(0x31, 0);
                hero_anim(0x81, 15.0f);
                GREF(int32_t, GREF(gaddr, HERO_MOBY) + 0x9Cu) = -1;
                if (K_ACTIVE_FLAG) {
                    DF(0x14) = K_TARGET_ANGLE * 0.017453294f;
                }
                DF(0x54) = 0.060000002f;
                vzero(d + 0x20u);
                DI(0xA8) = 0;
                DF(0x58) = DF(8);
                cam_aim(d + 0xA8u);
                DF(0x64) = 0.0f;
                GREF(gaddr, HERO_VEHICLE_OBJ) = obj;
                set_state(obj, 5);
            }
            move_with_mount(obj, d);
        }
        DI(0xB4) = 0;
        break;
    case 4:
        break;
    case 5: {
        cam_aim(d + 0xA8u);
        DF(0x28) = -SIN_B(DF(0x14)) * DF(0x54);
        {
            const float c = DF(0x54) * SIN_A(DF(0x14));
            DF(0x20) = c * SIN_A(DF(0x18));
            DF(0x24) = c * SIN_B(DF(0x18));
        }
        if (K_TIMER_LENGTH >= DI(0x50)) {
            if (OH(MOBY_FLAGS) & 1u) {
                OH(MOBY_FLAGS) &= 0xFFFEu;
                OB(0x31) = 1;
                if (OB(0x43) != 1) {
                    anim(obj, 1, 0xA);
                }
            }
            DF(0x64) = clampf((float)(K_TIMER_LENGTH - DI(0x50)) / (float)K_TIMER_LENGTH, 0.0f, 1.0f);
            steering(obj, d, DF(0x64));
            vscale(d + 0x20u, d + 0x20u, 1.0f - DF(0x64));
            const gaddr v = s_scratch + 0x20u;
            func_002AF000(v, DF(0x54), DF(0x18), -DF(0x14));
            vscale(v, v, DF(0x64));
            vadd(d + 0x20u, d + 0x20u, v);
        }
        if (K_ACTIVE_FLAG) {
            vadd(d, d, d + 0x20u);
        }
        if (handle_collision(p, index, obj, d, prev) == 0) {
            const float speed = DF(0x54) + K_SPEED_ACCEL * 0.00027777778f;
            const float max_speed = K_MAX_SPEED * 0.016666668f;
            DF(0x54) = speed > max_speed ? max_speed : speed;
            if (DI(0x50) < K_TIMER_START - K_TIMER_MARGIN && K_ACTIVE_FLAG) {
                DF(0x14) = FAST_SUB(DF(0x14), K_TURN_RATE * 0.00029088825f);
            }
            if (DF(0x14) < 0.0f) {
                DF(0x14) = 0.0f;
            }
            if (TIMER(d + 0x50u) != 0) {
                if (OB(0x43) != 2) {
                    anim(obj, 2, 0xA);
                }
                hero_anim(0x82, 15.0f);
                DF(0x40) = 0.0f;
                DF(0x48) = 0.0f;
                DF(0x44) = -(K_TURN_RATE * 0.00029088825f);
                set_state(obj, 6);
            }
        }
        if (OB(MOBY_STATE) == 1) {
            break; /* killed: back on its pad */
        }
        if ((OB(0x60) & 2u) && OB(0x43) != 2) {
            anim(obj, 2, 0xA);
        }
        move_with_mount(obj, d);
        break;
    }
    case 6: {
        cam_aim(d + 0xA8u);
        steering(obj, d, 1.0f);
        const gaddr hero_moby = GREF(gaddr, HERO_MOBY);
        const uint8_t s42 = hero_moby != 0 ? GREF(uint8_t, hero_moby + 0x42u) : 0;
        const uint8_t s43 = hero_moby != 0 ? GREF(uint8_t, hero_moby + 0x43u) : 0;
        const float amount = DF(0x5C);
        if (amount < -0.4f) {
            if (s42 != 0x85 && s43 != 0x85) {
                hero_anim(0x85, 15.0f);
            }
        } else if (amount > 0.4f) {
            if (s42 != 0x86 && s43 != 0x86) {
                hero_anim(0x86, 15.0f);
            }
        } else if (K_BANK_LIMIT_DEG * 0.000041555468f < DF(0x44)) {
            if (s42 != 0x83 && s43 != 0x83) {
                hero_anim(0x83, 25.0f);
            }
        } else if (DF(0x44) < -K_BANK_LIMIT_DEG * 0.000041555468f) {
            if (s42 != 0x81 && s43 != 0x81) {
                hero_anim(0x81, 25.0f);
                if (DI(0xB0) == 0) {
                    DI(0xB0) = 0x2D;
                }
            }
        } else if (s42 != 0x82 && s43 != 0x82) {
            hero_anim(0x82, 20.0f);
        }
        {
            const float a = DF(8), b = DF(0x58), c = K_SLOPE_LIMIT;
            if (a - b <= c) {
                if (DI(0x98) == 0) {
                    const float s = DF(0x54);
                    DF(0x54) = s + ((K_CRUISE_SPEED * 0.016666668f) - s) * 0.1f;
                }
            } else {
                const float e = K_SLOPE_THRESHOLD;
                float k = clampf((b - a + e) / (e - c), 0.0f, 1.0f);
                const float lo = K_CRUISE_SPEED * 0.016666668f;
                k = 1.0f - k;
                const float speed = lo + k * ((K_SLOW_SPEED * 0.016666668f) - lo);
                if (DI(0x98) == 0 || speed < DF(0x54)) {
                    DF(0x54) = DF(0x54) + (speed - DF(0x54)) * 0.1f;
                }
                if (lo < DF(0x54)) {
                    DF(0x54) = lo;
                }
            }
        }
        if (DF(0x64) < 1.0f) {
            DF(0x64) = DF(0x64) + K_DF64_OFFSET;
            if (DF(0x64) > 1.0f) {
                DF(0x64) = 1.0f;
            }
            const gaddr v = s_scratch + 0x20u;
            func_002AF000(v, DF(0x54), DF(0x18), -DF(0x14));
            vscale(d + 0x20u, d + 0x20u, 1.0f - DF(0x64));
            vscale(v, v, DF(0x64));
            vadd(d + 0x20u, d + 0x20u, v);
        } else {
            func_002AF000(d + 0x20u, DF(0x54), DF(0x18), -DF(0x14));
        }
        if (K_ACTIVE_FLAG) {
            vadd(d, d, d + 0x20u);
        }
        if (K_SLOPE_THRESHOLD < DF(8) - DF(0x58)) {
            if (OB(0x43) != 2) {
                anim(obj, 2, 3);
            }
            hero_anim(0x84, 15.0f);
            const float lim = K_ANGLE_LIMIT * 0.017453294f;
            DI(0x60) = 0;
            DI(0x50) = K_DF50_FRAMES;
            DF(0x68) = lim < DF(0x14) ? FAST_SUB(lim, DF(0x14)) / (float)K_DF50_FRAMES : 0.0f;
            set_state(obj, 7);
        }
        move_with_mount(obj, d);
        handle_collision(p, index, obj, d, prev);
        break;
    }
    case 7:
        cam_aim(d + 0xA8u);
        if (TIMER(d + 0x50u) != 0) {
            DI(0x50) = K_DF50_RESET;
            set_state(obj, 8);
        }
        DF(0x28) += -K_DECEL_A * 0.00027777778f;
        vadd(d, d, d + 0x20u);
        DF(0x54) = vlen(d + 0x20u);
        if (K_ACTIVE_FLAG) {
            DF(0x14) = FAST_ADD(DF(0x14), DF(0x68));
        }
        steering(obj, d, 1.0f);
        if (handle_collision(p, index, obj, d, prev) && OB(MOBY_STATE) == 1) {
            break;
        }
        move_with_mount(obj, d);
        break;
    case 8:
        cam_aim(d + 0xA8u);
        if (K_ANGLE_THRESHOLD * 0.017453294f < DF(0x14) && DF(8) - DF(0x58) < K_SLOPE_THRESHOLD) {
            hero_anim(0x82, 15.0f);
            if (OB(0x43) != 2) {
                anim(obj, 2, 0xA);
            }
            DF(0x64) = 0.0f;
            set_state(obj, 6);
        } else {
            TIMER(d + 0x50u);
        }
        DF(0x28) += -K_DECEL_A * 0.00027777778f;
        if (K_ACTIVE_FLAG) {
            vadd(d, d, d + 0x20u);
        }
        DF(0x54) = vlen(d + 0x20u);
        steering(obj, d, 1.0f);
        move_with_mount(obj, d);
        handle_collision(p, index, obj, d, prev);
        break;
    case 9: {
        cam_aim(d + 0xA8u);
        if (func_002B3868(obj) != 0) {
            DI(0xB0) = 0xF;
        }
        if (K_ACTIVE_FLAG) {
            DF(0x18) = FAST_ADD(DF(0x18), K_YAW_STEP * 0.00029088825f);
        }
        DF(0x28) += -K_DECEL_B * 0.00027777778f;
        if (K_ACTIVE_FLAG) {
            vadd(d, d, d + 0x20u);
        }
        move_with_mount(obj, d);
        if (handle_collision(p, index, obj, d, prev) && OB(MOBY_STATE) == 1) {
            break;
        }
        if (DF(0x28) > 0.0f || DF(8) < 12.0f || vlen(d + 0x20u) < 0.016666668f
            || K_THRESHOLD_FRAMES < (int32_t)OH(0x96)) {
            /* The crash: in the game an explosion and the hero's death counters; here he is let go
             * where he is (he falls), the glider back on its pad. */
            float at[3] = {DF(0), DF(4), DF(8)};
            hero_enter_state(0, 1);
            back_to_pad(p, obj, d);
            event(OPENRAC_GAME_GLIDER_CRASH, index, at);
        }
        break;
    }
    case 10: {
        const float dist = vdist(d, d + 0x80u);
        const float lim = DF(0x94) < dist ? DF(0x94) : dist;
        if (K_ACTIVE_FLAG) {
            func_002AA800(d + 0x14u, 0, 0.0f, K_ANGLE_STEP_ZERO * 0.00029088825f);
            func_002AA800(d + 0x10u, 0, 0.0f, K_ANGLE_STEP_ZERO * 0.00029088825f);
        }
        if ((OB(0x60) & 2u) && OB(0x42) == 3) {
            if (OB(0x43) != 0) {
                anim(obj, 0, 0xA);
            }
            OB(0x31) = 0;
            OH(MOBY_FLAGS) |= 1u;
        }
        if (lim < dist) {
            const gaddr v = s_scratch + 0x20u;
            vsub(v, d + 0x80u, d);
            vscale_to(v, v, lim * K_VELOCITY_LENGTH_SCALE);
            vscale(d + 0x20u, d + 0x20u, K_VELOCITY_DAMPING);
            vadd(d + 0x20u, d + 0x20u, v);
            if (K_ACTIVE_FLAG) {
                vadd(d, d, d + 0x20u);
            }
            move_with_mount(obj, d);
            break;
        }
        qcopy(d, d + 0x80u);
        DF(0x94) = 0.0f;
        if (GREF(uint32_t, HERO_ANIM_FLAGS) & 2u) {
            float at[3] = {DF(0), DF(4), DF(8)};
            DI(0x50) = 0;
            hero_enter_state(0, 1);
            func_L00_002B2BD0();
            back_to_pad(p, obj, d);
            event(OPENRAC_GAME_GLIDER_LAND, index, at);
        }
        break;
    }
    default:
        break;
    }
    /* Flying with the hero let go another way (a cut scene, a death): back on the pad. */
    const unsigned state = OB(MOBY_STATE);
    if (state >= 3 && state != 4 && GREF(int32_t, HERO_STATE) != 0x31) {
        float at[3] = {DF(0), DF(4), DF(8)};
        back_to_pad(p, obj, d);
        event(OPENRAC_GAME_GLIDER_ABORT, index, at);
        return;
    }
    DF(0) = clampf(DF(0), 10.0f, 1013.0f);
    DF(4) = clampf(DF(4), 10.0f, 1013.0f);
    DF(8) = clampf(DF(8), 10.0f, 1013.0f);
    if (state - 5u < 5u) {
        if (OB(0x31) != 0) {
            const gaddr hero = GREF(gaddr, HERO_MOBY);
            if (hero != 0 && vdist(obj + MOBY_POS, hero + MOBY_POS) < 80.0f) {
                func_002A93B0(obj);
                OB(0x6F) = 0x4B;
            }
        }
        /* The smoke trail: from each of the glider's first 7 joints, five sparks along its move
         * (SpawnSpinningSparkParticle, func_002BDCE8; its ints, then its floats, as the EE passes
         * them), each given a random id (rand, func_001163B0) as the game does. */
        {
            const gaddr vel = s_scratch + 0x50u, point = s_scratch + 0x60u, joint = s_scratch + 0x70u;
            vscale(vel, d + 0x20u, 0.25f);
            for (int j = 0; j < 7; ++j) {
                func_0029FCC0(obj, j, joint);
                for (float f = 0.0f; f < 1.0f; f += 0.2f) {
                    vscale(point, d + 0x20u, -f);
                    vadd(point, point, joint);
                    const float amount = OB(MOBY_STATE) >= 7 ? 0.07f : 0.0f;
                    const float life = 24000.0f + (float)(func_001163B0() % 11001);
                    const gaddr particle = func_002BDCE8(point, 2, vel, 0x0E7F7F7F, 0, amount, 0.5f, 1.01f, life);
                    if (particle != 0) {
                        GREF(uint8_t, particle + 8u) = (uint8_t)func_001163B0();
                    }
                }
            }
        }
        /* The pad's landing zone in place of Tabora's cuboids: within its radius, not above its
         * height, and below the hero. */
        const float* l = p->def.landing;
        const float dx = DF(0) - l[0], dy = DF(4) - l[1], dz = DF(8) - l[2];
        if (p->def.landing_radius > 0.0f && dx * dx + dy * dy <= p->def.landing_radius * p->def.landing_radius
            && dz >= 0.0f && dz <= p->def.landing_height && l[2] < GREF(float, HERO_POS + 8u)) {
            if (OB(0x43) != 3) {
                anim(obj, 3, 3);
            }
            hero_anim(0x88, 15.0f);
            DF(0x80) = l[0];
            DF(0x84) = l[1];
            DF(0x88) = l[2];
            DF(0x8C) = 1.0f;
            DF(0x90) = vdist(HERO_POS, d + 0x80u);
            DF(0x94) = vlen(d + 0x20u) > 0.06666667f ? vlen(d + 0x20u) : 0.06666667f;
            GREF(float, HERO_ANIM_SPEED) = K_UNK_RATIO_SCALE / (DF(0x90) / DF(0x94));
            set_state(obj, 10);
        }
    }
    TIMER(d + 0x98u);
    TIMER(d + 0xB0u);
    /* OPENRAC_GLIDER_LOG=N: every N ticks of a flight, its state, place, speed and angles. */
    static int log_every = -1;
    if (log_every < 0) {
        const char* env = openrac_env_string("OPENRAC_GLIDER_LOG");
        log_every = env != NULL ? atoi(env) : 0;
    }
    if (log_every > 0 && OB(MOBY_STATE) >= 3 && OH(0x96) % (unsigned)log_every == 0) {
        fprintf(stderr, "[glider] tick %u state %u at %.2f %.2f %.2f speed %.3f vel %.3f %.3f %.3f pitch %.3f yaw %.3f roll %.3f stick %.2f timer %d\n",
            (unsigned)OH(0x96), (unsigned)OB(MOBY_STATE), (double)DF(0), (double)DF(4), (double)DF(8), (double)DF(0x54),
            (double)DF(0x20), (double)DF(0x24), (double)DF(0x28), (double)DF(0x14), (double)DF(0x18), (double)DF(0x10),
            (double)DF(0x5C), (int)DI(0x50));
    }
}

/* The pad's update: nothing (the game's draws its glow from Tabora's data). */
static void pad_update(gaddr obj) {
    (void)obj;
}

static void register_updates(void) {
    static int done;
    if (done) {
        return;
    }
    done = 1;
    static openrac_fn_entry entries[2];
    entries[0].address = TAG_GLIDER;
    entries[0].fn = (openrac_host_fn)glider_update;
    entries[0].name = "glider_update";
    entries[1].address = TAG_PAD;
    entries[1].fn = (openrac_host_fn)pad_update;
    entries[1].name = "glider_pad_update";
    openrac_guest_register(OPENRAC_OVERLAY_EXE, entries, 2);
}

static int read_tuning(void) {
    if (s_k_read) {
        return s_k_read > 0;
    }
    s_k_read = -1;
    for (uint32_t i = 0; i < K_WORDS; ++i) {
        s_k[i] = openrac_guest_level_word(FROM_LEVEL, K_FIRST + 4u * i);
    }
    /* Tabora's df50 frames and timer are small positive counts on the disc: a zero program reads 0. */
    if (KI(0x001AFFE0u) <= 0 || KI(0x001B0084u) <= 0) {
        openrac_game_warn("glider: Tabora's program cannot be read from the disc image");
        return 0;
    }
    s_k_read = 1;
    char message[200];
    snprintf(message, sizeof message, "glider: Tabora's tuning read (max speed %.2f, cruise %.2f, timer %d, push %.2f, active %d)",
             (double)K_MAX_SPEED, (double)K_CRUISE_SPEED, (int)K_TIMER_LENGTH, (double)K_PUSH_SPEED, (int)K_ACTIVE_FLAG);
    openrac_game_warn(message);
    return 1;
}

int rac2_glider_pad_add(const openrac_game_glider_pad* def) {
    /* Tabora's tuning is read once a level plays (rac2_glider_tick): asked before the port can
     * read level programs (a mod's load), the program would be taken as missing. */
    if (def == NULL || def->level < 0 || def->level >= 32 || enter_state_of(def->level) == 0
        || def->level == 8 || def->level == 9 || def->level == 19) {
        return -1;
    }
    for (int i = 0; i < 3; ++i) {
        if (!isfinite(def->position[i]) || !isfinite(def->landing[i])) {
            return -1;
        }
    }
    if (!isfinite(def->yaw) || !isfinite(def->landing_radius) || !isfinite(def->landing_height)) {
        return -1;
    }
    if (openrac_guest_ram_size < 0x08C00000u) {
        openrac_game_warn("glider: the game's RAM is not expanded (a mod's manifest asks with \"expanded_ram\"): no pad");
        return -1;
    }
    for (int i = 0; i < PADS_MAX; ++i) {
        if (!s_pads[i].used) {
            const int g = rac2_foreign_want(FROM_LEVEL, CLASS_GLIDER, def->level);
            const int k = rac2_foreign_want(FROM_LEVEL, CLASS_PAD, def->level);
            if (g < 0 || k < 0) {
                return -1;
            }
            s_class_glider = g;
            s_class_pad = k;
            for (int seq = SEQ_FIRST; seq <= SEQ_LAST; ++seq) {
                (void)rac2_hero_seq_import(FROM_LEVEL, seq);
            }
            memset(&s_pads[i], 0, sizeof s_pads[i]);
            s_pads[i].used = 1;
            s_pads[i].def = *def;
            s_pads[i].loads = UINT32_MAX;
            return i;
        }
    }
    return -1;
}

int rac2_glider_pad_remove(int id) {
    if (id < 0 || id >= PADS_MAX || !s_pads[id].used) {
        return -1;
    }
    s_pads[id].used = 0;
    return 0;
}

int rac2_glider_events(openrac_game_glider_event* out, int max) {
    int n = s_event_count < max ? s_event_count : max;
    if (n < 0) {
        n = 0;
    }
    if (out != NULL && n > 0) {
        memcpy(out, s_events, (size_t)n * sizeof *out);
    }
    if (n < s_event_count) {
        memmove(s_events, s_events + n, (size_t)(s_event_count - n) * sizeof *s_events);
    }
    s_event_count -= n;
    return n;
}

void rac2_glider_reset(void) {
    memset(s_pads, 0, sizeof s_pads);
    s_event_count = 0;
}

/* Once a frame: this load's pads made once the level plays, the hero on and off them. */
void rac2_glider_tick(void) {
    const int level = openrac_game_loaded_overlay();
    const gaddr hero = GREF(gaddr, HERO_MOBY);
    if (level < 0 || hero == 0 || GREF(int32_t, GAME_STATE) != 0) {
        return;
    }
    for (int i = 0; i < PADS_MAX; ++i) {
        Pad* p = &s_pads[i];
        if (!p->used || p->def.level != level) {
            continue;
        }
        if (p->loads != rac2_program_loads()) {
            if (rac2_foreign_class_at(s_class_glider) == 0 || rac2_foreign_class_at(s_class_pad) == 0
                || !read_tuning()) {
                p->loads = rac2_program_loads(); /* told once a load */
                p->pad = p->glider = 0;
                openrac_game_warn("glider: no pad this load (the classes or Tabora's tuning are missing)");
                continue;
            }
            if (s_scratch == 0) {
                s_scratch = openrac_guest_static(0x100u);
            }
            register_updates();
            p->loads = rac2_program_loads();
            p->on = 0;
            p->pad = rac2_foreign_spawn(s_class_pad, p->def.position, 0);
            if (p->d == 0) {
                p->d = openrac_guest_static(PVARS_BYTES);
            }
            memset(G(p->d), 0, PVARS_BYTES);
            p->glider = p->pad != 0 ? rac2_foreign_spawn(s_class_glider, p->def.position, p->d) : 0;
            char message[240];
            if (p->pad == 0 || p->glider == 0) {
                snprintf(message, sizeof message, "glider pad %d: not made (pad %08x, glider %08x)", i, p->pad, p->glider);
                openrac_game_warn(message);
                p->pad = p->glider = 0;
                continue;
            }
            GREF(float, p->pad + MOBY_ROT + 8u) = p->def.yaw;
            func_002A0F48(p->pad);
            GREF(gaddr, p->pad + MOBY_UPDATE) = TAG_PAD;
            GREF(gaddr, p->glider + MOBY_UPDATE) = TAG_GLIDER;
            GREF(gaddr, p->glider + MOBY_PVARS) = p->d;
            GREF(uint8_t, p->glider + MOBY_STATE) = 0;
            snprintf(message, sizeof message,
                     "glider pad %d in level %d: pad %08x at %.1f %.1f %.1f (collision %08x), glider %08x",
                     i, level, p->pad, (double)p->def.position[0], (double)p->def.position[1],
                     (double)p->def.position[2], GREF(gaddr, p->pad + 0x98u), p->glider);
            openrac_game_warn(message);
        }
        if (p->pad == 0) {
            continue;
        }
        const int on = GREF(gaddr, PLATFORM) == p->pad || GREF(gaddr, HERO_TARGET) == p->pad;
        if (on != p->on) {
            p->on = on;
            event(on ? OPENRAC_GAME_GLIDER_ON_PAD : OPENRAC_GAME_GLIDER_OFF_PAD, i, &GREF(float, HERO_POS));
        }
        const int x = (GREF(uint32_t, PAD_HELD) & 0x40u) != 0;
        if (on && x && !owns(p) && !p->refused_held) {
            event(OPENRAC_GAME_GLIDER_REFUSED, i, &GREF(float, HERO_POS));
        }
        p->refused_held = x;
    }
}

#else /* another release: not written for it yet */

int rac2_glider_pad_add(const struct openrac_game_glider_pad* pad) {
    (void)pad;
    return -1;
}
int rac2_glider_pad_remove(int id) {
    (void)id;
    return -1;
}
int rac2_glider_events(struct openrac_game_glider_event* out, int max) {
    (void)out;
    (void)max;
    return 0;
}
void rac2_glider_tick(void) {
}
void rac2_glider_reset(void) {
}

#endif
