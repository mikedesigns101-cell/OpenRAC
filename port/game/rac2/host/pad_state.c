/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 the OpenRAC contributors
 *
 * Going Commando's pad update, answered by the port above libpad2 and libvib.
 *
 * On the console pad_init_ports (func_002B8548) started libdbc and libpad2 and opened a socket
 * for each of the two ports, and each frame the game's update ran poll_pad_device_state
 * (func_002B85F8; PAL's own copy) on both pad records: it asked libpad2 for the socket's state,
 * read the button and vibration profiles once (state 0 of the record's +0x198), then each frame
 * (state 1) read a report, sent the two motor values it was given (+0x188 small, +0x189 large)
 * with sceVibSetActParam, and handed the report to the game's reader, pad_decode_reading
 * (func_002B88F0; PAL's func_PAL_002B94A8), which turns it into the record the game plays from.
 *
 * Here both are the port's. Each pad is always there, as the library replacements had it (a port
 * with no controller reads as one with nothing pressed); its profiles are what they answered
 * (every button and both sticks, two motors), the report is the port's controller as
 * openrac_pad_report gives it (through the mods' pad callbacks), and the motors go to the window
 * as they are. The reader stays the game's: dead zones, pressures, edges, key repeat.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "game/common/lib/pad_report.h"
#include "game_protos.h"
#include "openrac/game_host.h"
#include "openrac/guest.h"
#include "openrac/host_env.h"
#include "openrac/openrac_mod.h"
#include "rac2.h"
#include "weapon_shots.h"

#define PAD0 RAC2_ADDR(0x00138180u)      /* g_pad_state: port 0's record */
#define PAD1 RAC2_ADDR(0x00138500u)      /* g_pad_device_state: port 1's */
#define PAD0_IDLE RAC2_ADDR(0x001A7428u) /* 1 while port 0's record says no pad */

/* A pad record's fields this update writes (rac2-decomp src/game/002B7E28-g8.c, PadPollState). */
enum {
    PAD_PROFILE = 0x180,       /* the button profile, 4 bytes */
    PAD_ACT = 0x184,           /* the vibration profile, 4 bytes: bit 0 small motor, bit 1 large */
    PAD_MOTOR_SMALL = 0x188,   /* the game's small motor (on/off), cleared each poll */
    PAD_MOTOR_LARGE = 0x189,   /* the game's large motor (speed), cleared each poll */
    PAD_SOCKET = 0x194,        /* libpad2's socket: the port */
    PAD_PROFILE_STATE = 0x198, /* 0 profiles to read, 1 reading */
    PAD_DEVICE_STATE = 0x19C,  /* scePad2GetState's answer: 1 connected */
    PAD_HELD = 0x1A0,
    PAD_PREV_HELD = 0x1AC,
    PAD_RAW_HELD = 0x1B0,
    PAD_PREV_RAW_HELD = 0x1BC,
    PAD_PRESSURES = 0x1DC, /* 0x79 when the button profile has every button */
    PAD_LATCHED_HELD = 0x348,
};

static void open_port(gaddr pad, int port) {
    GREF(int32_t, pad + PAD_SOCKET) = port;
    GREF(int32_t, pad + PAD_PROFILE_STATE) = 0;
    GREF(int32_t, pad + PAD_DEVICE_STATE) = 0;
}

/* pad_init_ports: each record's socket is its port, its profiles still to read. */
void func_002B8548(gaddr info) {
    (void)info; /* libdbc's work area */
    open_port(PAD0, 0);
    open_port(PAD1, 1);
}

/* The mods' motors (openrac_api.pad_rumble, game_motors_mute; interface 1.107): a rumble per
 * port counted down a poll at a time, and the items while in the hero's hand the game's own
 * values are dropped (-1 always). Each motor the window gets is the stronger of the game's and
 * the rumble's (set_motors' go through the window's own mixing). OPENRAC_RAC2_MOTOR_TRACE=1
 * prints what goes to the window each time it changes. */
static struct {
    int small, large, ticks;
} rumble[2];

static int32_t muted[OPENRAC_MOTORS_MUTE_MAX];
static int muted_count;

int rac2_pad_rumble(int port, int small_motor, int large_motor, int ticks) {
    if (port < 0 || port > 1) {
        return 0;
    }
    rumble[port].small = small_motor != 0;
    rumble[port].large = large_motor < 0 ? 0 : large_motor > 255 ? 255 : large_motor;
    rumble[port].ticks = ticks;
    return 1;
}

void rac2_motors_mute(const int32_t* items, int count) {
    muted_count = 0;
    for (int i = 0; items != NULL && i < count && muted_count < OPENRAC_MOTORS_MUTE_MAX; ++i) {
        muted[muted_count++] = items[i];
    }
}

static int game_motors_muted(void) {
    if (muted_count == 0) {
        return 0;
    }
    const int32_t item = rac2_hand_item();
    for (int i = 0; i < muted_count; ++i) {
        if (muted[i] == OPENRAC_MOTORS_MUTE_ALL || (item >= 0 && muted[i] == item)) {
            return 1;
        }
    }
    return 0;
}

static void send_motors(int port, int small, int large) {
    static int trace = -1, last[2][2] = {{-1, -1}, {-1, -1}};
    const int game_small = small, game_large = large;
    const int mute = game_motors_muted();
    if (mute) {
        small = large = 0;
    }
    if (port >= 0 && port <= 1 && rumble[port].ticks > 0) {
        small = small > rumble[port].small ? small : rumble[port].small;
        large = large > rumble[port].large ? large : rumble[port].large;
        --rumble[port].ticks;
    }
    if (trace < 0) {
        const char* v = openrac_env_string("OPENRAC_RAC2_MOTOR_TRACE");
        trace = v != NULL && v[0] != '\0' && strcmp(v, "0") != 0;
    }
    if (trace && port >= 0 && port <= 1 && (last[port][0] != small || last[port][1] != large)) {
        last[port][0] = small;
        last[port][1] = large;
        fprintf(
            stderr,
            "[motors] port %d: small %d large %d (the game's %d %d%s, rumble %d ticks left)\n",
            port,
            small,
            large,
            game_small,
            game_large,
            mute ? " muted" : "",
            rumble[port].ticks
        );
    }
    openrac_game_set_motors(port, small, large);
}

/* poll_pad_device_state(pad), as the game's steps it. */
void func_002B85F8(gaddr pad) {
    const int port = GREF(int32_t, pad + PAD_SOCKET);

    GREF(int32_t, pad + PAD_HELD) = 0;
    GREF(int32_t, pad + PAD_PREV_HELD) = GREF(int32_t, pad + PAD_LATCHED_HELD);
    GREF(int32_t, pad + PAD_PREV_RAW_HELD) = GREF(int32_t, pad + PAD_RAW_HELD);
    GREF(int32_t, pad + PAD_RAW_HELD) = 0;
    GREF(int32_t, pad + PAD_DEVICE_STATE) = 1;

    if (GREF(int32_t, pad + PAD_PROFILE_STATE) == 0) {
        /* Every button and both sticks; then libvib's one byte, small and large motor, over the
         * same buffer, so the three bytes after it are the button profile's. */
        for (unsigned n = 0; n < 4; n++) {
            GREF(uint8_t, pad + PAD_PROFILE + n) = 0xFF;
            GREF(uint8_t, pad + PAD_ACT + n) = n == 0 ? 3 : 0xFF;
        }
        GREF(int32_t, pad + PAD_PRESSURES) = 0x79;
        GREF(int32_t, pad + PAD_PROFILE_STATE) = 1;
        func_002B8888(pad); /* ClearPadInput */
    } else {
        GFRAME(report, 0x40);
        const uint8_t act = GREF(uint8_t, pad + PAD_ACT);
        openrac_pad_report(port, (uint8_t*)G(report));
        send_motors(
            port,
            (act & 1) != 0 ? GREF(uint8_t, pad + PAD_MOTOR_SMALL) & 1 : 0,
            (act & 2) != 0 ? GREF(uint8_t, pad + PAD_MOTOR_LARGE) : 0
        );
        func_002B88F0(pad, report, OPENRAC_PAD_REPORT); /* pad_decode_reading */
    }
    GREF(uint8_t, pad + PAD_MOTOR_SMALL) = 0;
    GREF(uint8_t, pad + PAD_MOTOR_LARGE) = 0;
    GREF(int32_t, PAD0_IDLE) = GREF(int32_t, PAD0 + PAD_DEVICE_STATE) == 0;
}
