/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 the OpenRAC contributors
 *
 * Going Commando: seqs of the hero's class another level has, in a level whose own class lacks them
 * (OpenRAC Commando, interface 1.127: openrac_api.hero_seq_import).
 *
 * Each level's core holds the hero's seqs it uses (the index's +0x78, an offset a seq or 0 for
 * none; Oozla has 101 of the class's 157, Siberius all), and LoadLevelArchiveResources
 * (func_L00_00303990) puts each one's address in the class's +0x48 table and relocates it with
 * relocate_asset_entry_pointers (func_00293260), which adds the block's own address to its
 * pointers: a seq's block works anywhere. The game does the same at play with a talking
 * character's streamed seqs (update_streamed_moby_animation). So a seq the class lacks here is read
 * from the other level's core on the player's disc (openrac_game_level_hero_seq), copied into the
 * RAM past the console's 32 MiB (below 0x10000000: the frames' pointers carry flags in their top
 * four bits, which the decompressor func_002A1130 masks off), given its table entry and relocated.
 * The class's header (its skeleton) is the same in every level.
 *
 * When. Each frame (rac2_play_frame) once the hero's class is there, once a load: each seq asked
 * for that the level's own table on the disc lacks is copied again from the room's start and its
 * entry set (whatever it holds: the loader leaves a missing seq's entry as the last level left
 * it); a seq the level has is never touched. */
#include <stdio.h>
#include <string.h>

#include "game_protos.h"
#include "openrac/game_host.h"
#include "openrac/guest.h"
#include "rac2.h"

#if defined(RAC2_PRIMARY) /* v1.01's addresses; another release has no rows for it yet */

#define HERO_MOBY OPENRAC_DATA(RAC2_ADDR(0x0018C0B0u))
#define MOBY_CLASS 0x24u
#define CLASS_SEQ_COUNT 0x0Cu
#define CLASS_SEQS 0x48u
#define SEQ_FRAMES 0x10u

/* The room: 136 MiB into the expanded RAM (past foreign_class.c's 8 MiB of classes), 4 MiB. */
#define SEQ_ROOM 0x08800000u
#define SEQ_ROOM_SIZE 0x00400000u
#define WANT_MAX 64

typedef struct {
    int level, seq;
} Want;

static Want s_want[WANT_MAX];
static int s_want_count;
/* This load's copies: the class they went into, where the next goes, and each seq's address. */
static gaddr s_class;
static gaddr s_next;
static gaddr s_copied[256];
static uint8_t s_failed[256];

int rac2_hero_seq_import(int level, int seq) {
    if (level < 0 || level > 30 || seq < 0 || seq > 255) {
        return -1;
    }
    if (openrac_guest_ram_size < SEQ_ROOM + SEQ_ROOM_SIZE) {
        static int told;
        if (!told) {
            told = 1;
            openrac_game_warn("hero seqs of other levels: the game's RAM is not expanded (a mod's manifest asks with \"expanded_ram\"); none imported");
        }
        return -1;
    }
    const uint8_t* bytes = NULL;
    uint32_t size = 0;
    if (openrac_game_level_hero_seq(level, seq, &bytes, &size) != 0) {
        return -1;
    }
    for (int i = 0; i < s_want_count; ++i) {
        if (s_want[i].seq == seq) {
            s_want[i].level = level; /* the last asked wins */
            return 0;
        }
    }
    if (s_want_count >= WANT_MAX) {
        return -1;
    }
    s_want[s_want_count++] = (Want){level, seq};
    return 0;
}

/* Whether seq `seq` of the hero's class here is one of this port's copies. */
int rac2_hero_seq_imported(int seq) {
    return seq >= 0 && seq < 256 && s_class != 0 && s_copied[seq] != 0;
}

void rac2_hero_seqs_reset(void) {
    s_want_count = 0;
}

void rac2_hero_seqs_tick(void) {
    if (s_want_count == 0) {
        return;
    }
    const gaddr hero = GREF(gaddr, HERO_MOBY);
    if (hero == 0) {
        return;
    }
    const gaddr cls = GREF(gaddr, hero + MOBY_CLASS);
    if (cls == 0) {
        return;
    }
    static uint32_t loads = UINT32_MAX;
    static int level = -1;
    if (cls != s_class || loads != rac2_program_loads()) {
        /* Another class or load: every copy is made again. The loader writes only the entries of
         * the seqs the level has (LoadLevelArchiveResources skips a 0 offset), so an entry the level
         * lacks may still hold the last level's address (its core, gone now: Tabora's 128-136 after
         * Tabora), never to be trusted; whether the level has a seq is read from its own table on
         * the disc. */
        loads = rac2_program_loads();
        level = openrac_game_loaded_overlay();
        s_class = cls;
        s_next = SEQ_ROOM;
        memset(s_copied, 0, sizeof s_copied);
        memset(s_failed, 0, sizeof s_failed);
    }
    const int count = GREF(uint8_t, cls + CLASS_SEQ_COUNT);
    for (int i = 0; i < s_want_count; ++i) {
        const int seq = s_want[i].seq;
        if (seq >= count || s_failed[seq] || s_copied[seq] != 0) {
            continue;
        }
        const gaddr slot = cls + CLASS_SEQS + 4u * (gaddr)seq;
        const uint8_t* own = NULL;
        uint32_t own_size = 0;
        if (level < 0 || openrac_game_level_hero_seq(level, seq, &own, &own_size) == 0) {
            s_failed[seq] = 1; /* the level's own seq (or no level): left to the game */
            continue;
        }
        const uint8_t* bytes = NULL;
        uint32_t size = 0;
        char message[200];
        if (openrac_game_level_hero_seq(s_want[i].level, seq, &bytes, &size) != 0
            || s_next + ((size + 15u) & ~15u) > SEQ_ROOM + SEQ_ROOM_SIZE) {
            s_failed[seq] = 1;
            snprintf(message, sizeof message, "hero seq %d of level %d: not imported (%s)", seq, s_want[i].level,
                     bytes == NULL ? "no such seq on the disc" : "no room left");
            openrac_game_warn(message);
            continue;
        }
        const gaddr at = s_next;
        memcpy(G(at), bytes, size);
        GREF(gaddr, slot) = at;
        func_00293260(cls, seq);
        s_copied[seq] = at;
        s_next = (at + size + 15u) & ~15u;
        snprintf(message, sizeof message, "hero seq %d of level %d imported at %08x: %u bytes, %d keys", seq,
                 s_want[i].level, at, size, GREF(uint8_t, at + SEQ_FRAMES));
        openrac_game_warn(message);
    }
}

#else

int rac2_hero_seq_import(int level, int seq) {
    (void)level;
    (void)seq;
    return -1;
}
int rac2_hero_seq_imported(int seq) {
    (void)seq;
    return 0;
}
void rac2_hero_seqs_reset(void) {
}
void rac2_hero_seqs_tick(void) {
}

#endif
