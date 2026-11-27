/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 the OpenRAC contributors
 *
 * Going Commando: a moby class of another level's, read from the player's disc image and
 * registered in the level loaded as the level registers its own (OpenRAC Commando).
 *
 * The class. A level's core holds each class's block as the loader finds it, its pointers
 * offsets into the block (openrac_game_level_class, program/levels.cpp, reads it). The game's own
 * registration (load_register_class_cache_object, func_00293890) puts it in the class tables and
 * relocates it (tex_relocate_block, func_002934F8: its meshes, collision, sequences, sounds and
 * parts become addresses, its textures' numbers the loaded level's through the 16-byte map), so
 * nothing here knows the block's layout. The block goes in the game's RAM past the console's
 * 32 MiB (FOREIGN_BLOBS, in the expanded RAM main.cpp gives a run that asks for classes of other
 * levels), copied afresh at each load: the class's animation frames are addresses with flags in
 * their top four bits (the decompressor func_002A1130 masks them with 0x0FFFFFFF), so a class
 * anywhere at or past 0x10000000 is read wrong (at 0x50000000, the port's memory, a Chickenbot
 * hung the game as it bit); and the level's own heap has no room for one under its ceiling
 * (moby_pool.c: 170 KB on Damosel, a Chickenbot's class is 204 KB).
 *
 * When. In a wrap of PrintMobyClassSizes (func_L00_00300438), which LoadLevelArchiveResources
 * (func_L00_00303990) calls once it has registered the level's classes: the hero's class takes
 * the next slot after it, and the level's file, read next (LoadLevelData), makes its mobys through
 * the tables (CreateMoby: the class's slot, record and update). A load after a death registers
 * nothing again (the archive's part of the loader runs only for a new load).
 *
 * In its own level (a test that the copy is self-sufficient): the class's slot lookup is left on
 * the copy, so every moby of the class the level makes is made from the copy, and the loader's
 * pass over its classes' sound slots fills the copy's.
 *
 * OPENRAC_FOREIGN_CLASSES="level:class[,level:class...]" names them for now (a test); the mods'
 * call comes later. OPENRAC_FOREIGN_SCAN=frames logs, that many frames into each load, how many
 * words of the class's live mobys and their pvars point into the copy and into the level's own
 * block of the class; OPENRAC_FOREIGN_DUMP=dir writes both blocks there then. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "enemy_copies.h"
#include "game_protos.h"
#include "openrac/foreign.h"
#include "openrac/game_host.h"
#include "openrac/guest.h"
#include "openrac/host_env.h"
#include "rac2.h"

/* The class tables (src/game/00291758-g8.c; the executable's names, each level's copy): the count
 * of slots, each slot's class, the slot of each class (0xFF none), each slot's block, its first
 * word, its update. 0x3C0 bytes of blocks: 240 slots. */
#define CACHE_COUNT OPENRAC_DATA(RAC2_ADDR(0x001B1B40u))
#define CACHE_IDS OPENRAC_DATA(RAC2_ADDR(0x001CE300u))
#define CACHE_OBJECTS OPENRAC_DATA(RAC2_ADDR(0x001CDB80u))
#define SLOT_LOOKUP OPENRAC_DATA(RAC2_ADDR(0x001CE4E0u))
#define CLASS_UPDATES OPENRAC_DATA(RAC2_ADDR(0x001CDF40u))
/* The loader's count of the same slots (g_level_fixup_count), which gives the hero his. */
#define FIXUP_COUNT OPENRAC_LDATA(0, RAC2_LADDR(00, 0x001B1E80u))
/* g_level_load_context: +0x0C the level's core index (its header), +0x3C there the offset of its
 * moby textures' table. */
#define LOAD_CONTEXT OPENRAC_LDATA(0, RAC2_LADDR(00, 0x001BF140u))
#define SLOTS_MAX 240u
/* Slots left for the hero's class and the ones the level registers past PrintMobyClassSizes. */
#define SLOTS_SPARE 4u
/* A class's sounds: +0x0D their count, +0x28 their 32-byte records, each +0x1A the level's sound
 * (-1: none, the loader's own value for one out of range) and +0x1C its first. */
#define CLASS_SOUND_COUNT 0x0Du
#define CLASS_SOUNDS 0x28u
#define SOUND_BYTES 0x20u

#define MOBY_POS 0x10u
#define MOBY_STATE 0x20u
#define MOBY_CLASS 0x24u
#define MOBY_FLAGS 0x34u
#define MOBY_UPDATE 0x64u
#define MOBY_PVARS 0x68u
#define MOBY_OCLASS 0xAAu
#define MOBY_SIZE 0x100u
#define SLOT_PVARS 0x80u
#define FLAG_KEEP_MATRIX 0x4u

#define FOREIGN_MAX 16
#define LEVELS 31
/* Where the classes go: 128 MiB into the expanded RAM, 8 MiB of it (far above the level's heap and
 * the heap sbrk grows from the program's end; below 0x10000000, where the frames' flags begin). */
#define FOREIGN_BLOBS 0x08000000u
#define FOREIGN_BLOBS_SIZE 0x00800000u

/* Its own level's update, run in its own level's program (openrac_guest_foreign_enter): the class's
 * slot names a stand-in, a host function at an address past the game's 32 MiB that the moby loop
 * calls as any update (weapon_shots.c's func_L00_0032CF60). The stand-in only notes the moby; once
 * the loop is done, rac2_foreign_flush enters each level's program once for all its mobys, runs
 * their updates and leaves, then puts each in its cells (MobyUpdateCell, func_002A0F48) as the loop
 * does after an update. */
#define FOREIGN_TAG_BASE 0x0FF00000u
#define DEFER_MAX 512
/* The level's dispatch table (g_class_dispatch_table, each program's own): records of three words,
 * a class, its update, a shared pointer, ended by a class of -1 (moby_queue_class_update). */
#define DISPATCH OPENRAC_DATA(RAC2_ADDR(0x0026E880u))
#define DISPATCH_MAX 1024
/* The engine's tables a foreign update reads and writes past the first words of, carried in and
 * out whole: the class tables, from the blocks to the slots' first words (the slots' blocks,
 * updates, classes, the classes' slots, the second updates and the first words), and the moby
 * cells (the 64 x 64 grid of 4-byte cells, its 3072 blocks of 32 bytes and their bitmap:
 * func_002A0C68, func_002A0BE8). */
#define CLASS_TABLES RAC2_ADDR(0x001CDB80u)
#define CLASS_TABLES_SIZE 0x3640u
#define MOBY_CELLS RAC2_ADDR(0x001D24E0u)
#define MOBY_CELLS_SIZE 0x1C180u

/* A moby made here of a foreign class (OPENRAC_FOREIGN_SPAWN, a test, and later the mods' call):
 * its pvars in the port's room, as its level's first moby of the class has them on the disc. */
#define SPAWN_MAX 64
#define SPAWN_PVARS 0x800u

typedef struct {
    int level, oclass;
    /* As registered at the last load: the copy, its size, its slot, the level's own block of the
     * class (0 none). */
    gaddr at;
    uint32_t size;
    int slot;
    gaddr own;
    uint32_t loads;
} Foreign;

static Foreign s_foreign[FOREIGN_MAX];
static int s_count = -1;
static uint32_t s_scan_at, s_scan_frames, s_scan_loads;

/* The foreign updates: each class's in its own level (0 until found there), the mobys noted this
 * frame, the levels warned of. */
static gaddr s_update[FOREIGN_MAX];
static uint8_t s_update_tried[FOREIGN_MAX];
static gaddr s_deferred[DEFER_MAX];
static uint8_t s_deferred_k[DEFER_MAX];
static int s_deferred_n;
static uint8_t s_level_warned[LEVELS];
/* Spawned Chickenbots whose first update has not run yet: freed of their level's target after it. */
#define FREE_MAX 64
static gaddr s_free[FREE_MAX];
static int s_free_n;
/* What the flushes cost: mobys run and time, over the last frames (OPENRAC_FOREIGN_SCAN logs it). */
static uint64_t s_cost_ns, s_cost_frames, s_cost_mobys;

static void stand_in(int k, gaddr moby) {
    static uint32_t told_loads;
    if (told_loads != rac2_program_loads()) {
        told_loads = rac2_program_loads();
        char message[120];
        snprintf(message, sizeof message, "foreign class %d: its stand-in called for moby %08x", s_foreign[k].oclass, moby);
        openrac_game_warn(message);
    }
    if (s_deferred_n < DEFER_MAX) {
        s_deferred[s_deferred_n] = moby;
        s_deferred_k[s_deferred_n] = (uint8_t)k;
        ++s_deferred_n;
    }
}

#define FOREIGN_STAND_IN(k)                                                                        \
    static void stand_in_##k(gaddr moby) {                                                         \
        stand_in(k, moby);                                                                         \
    }
FOREIGN_STAND_IN(0)
FOREIGN_STAND_IN(1)
FOREIGN_STAND_IN(2)
FOREIGN_STAND_IN(3)
FOREIGN_STAND_IN(4)
FOREIGN_STAND_IN(5)
FOREIGN_STAND_IN(6)
FOREIGN_STAND_IN(7)
FOREIGN_STAND_IN(8)
FOREIGN_STAND_IN(9)
FOREIGN_STAND_IN(10)
FOREIGN_STAND_IN(11)
FOREIGN_STAND_IN(12)
FOREIGN_STAND_IN(13)
FOREIGN_STAND_IN(14)
FOREIGN_STAND_IN(15)

static gaddr tag_of(int k) {
    return FOREIGN_TAG_BASE + (gaddr)k * 16u;
}

static void register_stand_ins(void) {
    static int done;
    if (done) {
        return;
    }
    done = 1;
    static void (*const fns[FOREIGN_MAX])(gaddr) = {
        stand_in_0, stand_in_1, stand_in_2,  stand_in_3,  stand_in_4,  stand_in_5,
        stand_in_6, stand_in_7, stand_in_8,  stand_in_9,  stand_in_10, stand_in_11,
        stand_in_12, stand_in_13, stand_in_14, stand_in_15,
    };
    static openrac_fn_entry entries[FOREIGN_MAX];
    for (int k = 0; k < FOREIGN_MAX; ++k) {
        entries[k].address = tag_of(k);
        entries[k].fn = (openrac_host_fn)fns[k];
        entries[k].name = "foreign_class_update";
    }
    openrac_guest_register(OPENRAC_OVERLAY_EXE, entries, FOREIGN_MAX);
    openrac_guest_foreign_carry_whole(OPENRAC_OVERLAY_EXE, CLASS_TABLES, CLASS_TABLES_SIZE);
    openrac_guest_foreign_carry_whole(OPENRAC_OVERLAY_EXE, MOBY_CELLS, MOBY_CELLS_SIZE);
}

static void read_config(void) {
    s_count = 0;
    const char* env = openrac_env_string("OPENRAC_FOREIGN_CLASSES");
    while (env != NULL && *env != '\0' && s_count < FOREIGN_MAX) {
        int level = -1, oclass = -1, used = 0;
        if (sscanf(env, "%d:%d%n", &level, &oclass, &used) != 2 || used <= 0) {
            break;
        }
        if (level >= 0 && level < LEVELS && oclass >= 0 && oclass < 0x2000) {
            s_foreign[s_count++] = (Foreign){level, oclass, 0, 0, -1, 0, 0};
        }
        env += used;
        while (*env == ',' || *env == ' ') {
            ++env;
        }
    }
    const char* scan = openrac_env_string("OPENRAC_FOREIGN_SCAN");
    s_scan_at = scan != NULL ? (uint32_t)atoi(scan) : 0u;
}

static void warn(const char* message) {
    openrac_game_warn(message);
}

/* PrintMobyClassSizes (func_L00_00300438): a debug listing, after the level's classes. */
void func_L00_00300438(gaddr records, int count, int end) {
    func_L00_00300438__game(records, count, end);
    if (s_count < 0) {
        read_config();
    }
    const int level = openrac_game_loaded_overlay();
    if (s_count == 0 || level < 0) {
        return;
    }
    static gaddr map;
    if (map == 0) {
        map = openrac_guest_static(16u * FOREIGN_MAX);
    }
    const gaddr header = GREF(gaddr, LOAD_CONTEXT + 0x0Cu);
    const gaddr textures = header + GREF(uint32_t, header + 0x3Cu);
    gaddr at = FOREIGN_BLOBS;
    if (openrac_guest_ram_size < FOREIGN_BLOBS + FOREIGN_BLOBS_SIZE) {
        warn("foreign classes: the game's RAM is not expanded; none registered");
        return;
    }
    /* A new level: the foreign programs start again from the disc's (their data as it loads). */
    openrac_guest_forget_foreign();
    s_deferred_n = 0;
    s_free_n = 0;
    memset(s_level_warned, 0, sizeof s_level_warned);
    for (int k = 0; k < s_count; ++k) {
        Foreign* f = &s_foreign[k];
        f->at = 0;
        f->slot = -1;
        f->own = 0;
        char message[256];
        const uint32_t used = GREF(uint32_t, CACHE_COUNT);
        if (used + SLOTS_SPARE > SLOTS_MAX) {
            snprintf(message, sizeof message, "foreign class %d: no class slot left (%u used)", f->oclass, used);
            warn(message);
            continue;
        }
        const uint8_t was = GREF(uint8_t, SLOT_LOOKUP + (gaddr)f->oclass);
        if (was != 0xFFu && was < used) {
            f->own = GREF(gaddr, CACHE_OBJECTS + 4u * was);
        }
        if (f->own != 0 && f->level != level) {
            snprintf(message, sizeof message, "foreign class %d is level %d's own: not imported", f->oclass, level);
            warn(message);
            continue;
        }
        const uint8_t* bytes = NULL;
        uint32_t size = 0;
        uint8_t remap[16];
        if (openrac_game_level_class(f->level, f->oclass, &bytes, &size, remap) != 0) {
            snprintf(message, sizeof message, "foreign class %d: level %d's disc image has no such class", f->oclass, f->level);
            warn(message);
            continue;
        }
        if (size > FOREIGN_BLOBS + FOREIGN_BLOBS_SIZE - at) {
            snprintf(message, sizeof message, "foreign class %d: %u bytes, no room left", f->oclass, size);
            warn(message);
            continue;
        }
        if (f->level != level) {
            /* Its textures are its own level's: here each of them is the level's first (the port
             * draws the class from its own model). */
            for (unsigned i = 0; i < 16u; ++i) {
                remap[i] = remap[i] == 0xFFu ? 0xFFu : 0u;
            }
        }
        memcpy(G(at), bytes, size);
        memcpy(G(map + 16u * (gaddr)k), remap, 16);
        const uint32_t fixups_before = GREF(uint32_t, FIXUP_COUNT);
        func_00293890(at, (int)textures, (int)(map + 16u * (gaddr)k), f->oclass);
        const uint32_t now = GREF(uint32_t, CACHE_COUNT);
        f->at = at;
        f->size = size;
        f->slot = (int)now - 1;
        f->loads = rac2_program_loads();
        if (f->level != level) {
            /* Not this level's: its update is its own level's, through the stand-in (the one this
             * level's table gave is another class's), and no sounds (this level's numbers name
             * others). */
            register_stand_ins();
            GREF(uint32_t, CLASS_UPDATES + 4u * (gaddr)f->slot) = tag_of(k);
            s_update[k] = 0;
            s_update_tried[k] = 0;
            const uint32_t sounds = GREF(uint8_t, at + CLASS_SOUND_COUNT);
            const gaddr list = GREF(gaddr, at + CLASS_SOUNDS);
            for (uint32_t s = 0; list != 0 && s < sounds; ++s) {
                GREF(int16_t, list + s * SOUND_BYTES + 0x1Au) = -1;
                GREF(int32_t, list + s * SOUND_BYTES + 0x1Cu) = 0;
            }
        }
        snprintf(
            message,
            sizeof message,
            "foreign class %d of level %d in level %d: slot %d at %08x, %u bytes (level's own block "
            "%08x, slot %d; slots %u -> %u, loader's count %u -> %u)",
            f->oclass,
            f->level,
            level,
            f->slot,
            at,
            size,
            f->own,
            was == 0xFFu ? -1 : (int)was,
            used,
            now,
            fixups_before,
            GREF(uint32_t, FIXUP_COUNT)
        );
        warn(message);
        at = (at + size + 15u) & ~15u;
    }
}

/* Inside its level's program: the class's update in that level's dispatch table, 0 for none. */
static gaddr find_update(int oclass) {
    const gaddr table = DISPATCH;
    for (uint32_t i = 0; i < DISPATCH_MAX; ++i) {
        const int32_t id = GREF(int32_t, table + i * 12u);
        if (id == -1) {
            break;
        }
        if (id == oclass) {
            return GREF(gaddr, table + i * 12u + 4u);
        }
    }
    return 0;
}

static uint64_t now_ns(void) {
    struct timespec t;
    timespec_get(&t, TIME_UTC);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* After the moby loop (weapon_shots.c): the noted mobys' updates, each level's program entered
 * once for all of its. */
void rac2_foreign_flush(void) {
    if (s_deferred_n == 0) {
        return;
    }
    const uint64_t start = now_ns();
    const int loaded = openrac_game_loaded_overlay();
    int ran = 0;
    for (int i = 0; i < s_deferred_n; ++i) {
        const int level = s_foreign[s_deferred_k[i]].level;
        int first = 1;
        for (int j = 0; j < i; ++j) {
            first &= s_foreign[s_deferred_k[j]].level != level;
        }
        if (!first || loaded < 0) {
            continue;
        }
        if (openrac_guest_foreign_enter(level, loaded) != 0) {
            if (level >= 0 && level < LEVELS && !s_level_warned[level]) {
                s_level_warned[level] = 1;
                char message[160];
                snprintf(message, sizeof message, "foreign classes of level %d do not run in level %d: its program does not fit", level, loaded);
                warn(message);
            }
            continue;
        }
        openrac_game_restore_overlay(level);
        for (int j = i; j < s_deferred_n; ++j) {
            const int k = s_deferred_k[j];
            const Foreign* f = &s_foreign[k];
            const gaddr m = s_deferred[j];
            if (f->level != level || GREF(uint8_t, m + MOBY_STATE) >= 0x80u
                || GREF(gaddr, m + MOBY_UPDATE) != tag_of(k)) {
                continue;
            }
            if (!s_update_tried[k]) {
                s_update_tried[k] = 1;
                s_update[k] = find_update(f->oclass);
                char message[160];
                snprintf(message, sizeof message, "foreign class %d runs level %d's update %08x", f->oclass, level, s_update[k]);
                warn(message);
            }
            if (s_update[k] != 0) {
                openrac_guest_call_a0(s_update[k], m);
                ++ran;
            }
        }
        openrac_guest_foreign_leave();
        openrac_game_restore_overlay(loaded);
    }
    for (int i = 0; i < s_deferred_n; ++i) {
        const gaddr m = s_deferred[i];
        if ((GREF(uint16_t, m + MOBY_FLAGS) & FLAG_KEEP_MATRIX) == 0) {
            func_002A0F48(m);
        }
    }
    s_deferred_n = 0;
    /* Spawned Chickenbots past their first update: free of this level's target (-1, as relocate's
     * FREE leaves a live one). */
    for (int i = 0; i < s_free_n;) {
        const gaddr m = s_free[i];
        const uint8_t state = GREF(uint8_t, m + MOBY_STATE);
        if (state == 0) {
            ++i;
            continue;
        }
        if (state < 0x80u && GREF(int16_t, m + MOBY_OCLASS) == 2638) {
            const gaddr pv = GREF(gaddr, m + MOBY_PVARS);
            GREF(int32_t, pv + 0x210u) = -1;
            GREF(int32_t, pv + 0x218u) = -1;
        }
        s_free[i] = s_free[--s_free_n];
    }
    s_cost_ns += now_ns() - start;
    s_cost_mobys += (uint64_t)ran;
    ++s_cost_frames;
}

/* A moby of foreign class k made at `to` (on the floor there): CreateMoby (func_0029EFD0) of the
 * class, then what LoadLevelData does with a placed record (func_L00_00300618), from its level's
 * first moby of the class on the disc (openrac_game_level_record): its scale, draw words, turn,
 * pvars (in the port's room, its pointers made addresses), flags and colour; no save id and no
 * reward, and none of its level's routes, areas or targets (-1: none). Returns the moby, or 0. */
static gaddr spawn_one(int k, const float to[3]) {
    static gaddr room;
    static uint32_t room_loads;
    static int used;
    const Foreign* f = &s_foreign[k];
    uint8_t rec[0x88];
    const uint8_t* pv = NULL;
    const uint32_t* inner = NULL;
    uint32_t pv_size = 0, inner_count = 0;
    if (f->at == 0 || f->loads != rac2_program_loads()
        || openrac_game_level_record(f->level, f->oclass, rec, &pv, &pv_size, &inner, &inner_count) != 0
        || pv_size > SPAWN_PVARS) {
        return 0;
    }
    if (room == 0) {
        room = openrac_guest_static(SPAWN_MAX * SPAWN_PVARS);
    }
    if (room_loads != rac2_program_loads()) {
        room_loads = rac2_program_loads();
        used = 0;
    }
    if (used >= SPAWN_MAX) {
        return 0;
    }
    const gaddr m = func_0029EFD0(f->oclass);
    if (m == 0) {
        return 0;
    }
    const gaddr cls = GREF(gaddr, m + MOBY_CLASS);
    if (cls != f->at) {
        func_0029F338(m);
        return 0;
    }
    const gaddr pvars = room + (gaddr)used++ * SPAWN_PVARS;
    memcpy(G(pvars), pv, pv_size);
    for (uint32_t i = 0; i < inner_count; ++i) {
        GREF(uint32_t, pvars + inner[i]) += pvars;
    }
    uint32_t w;
    float scale;
    memcpy(&scale, rec + 0x2C, 4);
    GREF(float, m + 0x2Cu) = GREF(float, cls + 0x24u) * scale;
    GREF(uint8_t, m + 0x30u) = rec[0x34];
    GREF(uint16_t, m + 0x32u) = (uint16_t)(rec[0x30] | rec[0x31] << 8);
    memcpy(G(m + 0xF0u), rec + 0x4C, 12);
    /* No group (+0x21 -1): the visible list (func_002A08A0) runs a moby of a group only from that
     * group's list of this level's (its record's group, 1 for Maktar's Chickenbots, is another set
     * here), so one left in it never ran. */
    GREF(int8_t, m + 0x21u) = -1;
    memcpy(&w, rec + 0x6C, 4);
    GREF(uint16_t, m + 0x36u) = w != 0 ? 0x7F80u : 0u;
    GREF(gaddr, m + MOBY_PVARS) = pvars;
    const uint16_t flags16 = (uint16_t)(rec[0x70] | rec[0x71] << 8);
    GREF(uint16_t, cls + 0x44u) |= flags16;
    GREF(uint16_t, m + MOBY_FLAGS) |= flags16;
    uint32_t r, g, b, upd;
    memcpy(&r, rec + 0x74, 4);
    memcpy(&g, rec + 0x78, 4);
    memcpy(&b, rec + 0x7C, 4);
    memcpy(&upd, rec + 0x80, 4);
    func_002A0468(m, (unsigned long long)((b << 16) + (g << 8) + r), 0, 0, 0);
    GREF(uint32_t, m + 0x38u) = upd;
    GREF(uint8_t, m + 0x7Eu) &= 0xFEu;
    GREF(int16_t, m + 0xB2u) = -1;
    GREF(uint8_t, m + 0xB0u) = rec[0x04];
    GREF(uint8_t, m + 0xB1u) = (uint8_t)((rec[0x08] & 0xFu) | 0x80u);
    GREF(int16_t, m + 0xB4u) = 0;
    GREF(int16_t, m + 0xB6u) = 0;
    /* Free of its level's tables: the Chickenbot's area +0x218 and target +0x210 (moby_relocate.c),
     * its wander home +0x2E0 here. Its first update (state 0, func_L02_003CE4D0) deletes a moby whose
     * target is -1 and reads that entry of the level's table (D_L02_001B3200, 48 bytes each): it starts
     * with this level's first, and is freed once that update has run (rac2_foreign_flush). */
    if (f->oclass == 2638 && pv_size >= 0x2ECu) {
        GREF(int32_t, pvars + 0x210u) = 0;
        /* Not in a drop ship (+0x214: its first update then hides it, +0x34 |= 0x41, with no
         * collision, and waits in state 13 for a ship this level has not): out on the ground. */
        GREF(int32_t, pvars + 0x214u) = 0;
        GREF(int32_t, pvars + 0x218u) = -1;
        memcpy(G(pvars + 0x2E0u), to, 12);
        if (s_free_n < SPAWN_MAX) {
            s_free[s_free_n++] = m;
        }
    }
    memcpy(G(m + MOBY_POS), to, 12);
    const gaddr base = GREF(gaddr, OPENRAC_DATA(RAC2_g_mobyTableBase));
    float at[3] = {to[0], to[1], to[2]};
    (void)rac2_moby_relocate((int32_t)((m - base) / MOBY_SIZE), at, 0u);
    func_002A0F48(m);
    return m;
}

static int points_into(gaddr v, gaddr at, uint32_t size) {
    return at != 0 && v >= at && v - at < size;
}

static void dump(const char* dir, const char* what, int oclass, gaddr at, uint32_t size) {
    char path[1024];
    snprintf(path, sizeof path, "%s/foreign_%d_%s_%08x.bin", dir, oclass, what, at);
    FILE* file = fopen(path, "wb");
    if (file != NULL) {
        fwrite(G(at), 1, size, file);
        fclose(file);
    }
}

/* The first moby the test spawn made, watched for a few frames (its state, flags, update, next). */
static gaddr s_watch;
static uint32_t s_watch_from;

/* The hero's moby (g_hero +0x2290; play.c). */
#define HERO_MOBY OPENRAC_DATA(RAC2_ADDR(0x0018C0B0u))

/* OPENRAC_FOREIGN_SPAWN="frame:count[:distance]" (a test): `frame` frames into each load, `count`
 * mobys of the first foreign class made in a ring `distance` units (default 6) around the hero. */
static void spawn_test(uint32_t frame) {
    const char* env = openrac_env_string("OPENRAC_FOREIGN_SPAWN");
    unsigned at = 0, count = 0;
    float distance = 6.0f;
    if (env == NULL || sscanf(env, "%u:%u:%f", &at, &count, &distance) < 2 || frame != at) {
        return;
    }
    const gaddr hero = GREF(gaddr, HERO_MOBY);
    if (hero == 0 || s_count <= 0 || s_foreign[0].at == 0) {
        warn("foreign spawn: no hero or no foreign class");
        return;
    }
    int made = 0;
    for (unsigned i = 0; i < count && i < SPAWN_MAX; ++i) {
        const float turn = 6.2831853f * (float)i / (float)(count > 0 ? count : 1);
        const float to[3] = {
            GREF(float, hero + MOBY_POS) + distance * cosf(turn),
            GREF(float, hero + MOBY_POS + 4u) + distance * sinf(turn),
            GREF(float, hero + MOBY_POS + 8u),
        };
        const gaddr m = spawn_one(0, to);
        made += m != 0;
        if (m != 0 && made == 1) {
            s_watch = m;
            s_watch_from = frame;
        }
        if (m != 0 && made <= 3) {
            char message[240];
            snprintf(message, sizeof message, "foreign spawn: class %d moby %08x at %.1f %.1f %.1f, pvars %08x, update %08x, "
                     "state %02x, +0x34 %04x, collision %08x, class %08x",
                     s_foreign[0].oclass, m, GREF(float, m + MOBY_POS), GREF(float, m + MOBY_POS + 4u),
                     GREF(float, m + MOBY_POS + 8u), GREF(gaddr, m + MOBY_PVARS), GREF(gaddr, m + MOBY_UPDATE),
                     GREF(uint8_t, m + MOBY_STATE), GREF(uint16_t, m + MOBY_FLAGS), GREF(gaddr, m + 0x98u),
                     GREF(gaddr, m + MOBY_CLASS));
            warn(message);
        }
    }
    char message[120];
    snprintf(message, sizeof message, "foreign spawn: %d of %u made", made, count);
    warn(message);
}

/* Once a frame (play.c's rac2_play_frame): the test spawn and the scan OPENRAC_FOREIGN_SCAN asks
 * for, with what the foreign updates cost. */
void rac2_foreign_tick(void) {
    if (s_count <= 0) {
        return;
    }
    /* The mobys the game ran this tick: its moby loop each frame is moby_update_visible_objects
     * (func_002B6658), which calls the stand-ins; the loop weapon_shots.c wraps runs at a load. */
    rac2_foreign_flush();
    const uint32_t loads = rac2_program_loads();
    if (loads != s_scan_loads) {
        s_scan_loads = loads;
        s_scan_frames = 0;
        s_cost_ns = s_cost_frames = s_cost_mobys = 0;
    }
    ++s_scan_frames;
    spawn_test(s_scan_frames);
    if (s_watch != 0) {
        const uint32_t d = s_scan_frames - s_watch_from;
        if (d == 1 || d == 2 || d == 10 || d == 60 || d == 300) {
            char message[200];
            snprintf(message, sizeof message,
                     "foreign watch +%u: moby %08x state %02x +0x34 %04x update %08x next %08x cell %08x pos %.1f %.1f %.1f",
                     d, s_watch, GREF(uint8_t, s_watch + MOBY_STATE), GREF(uint16_t, s_watch + MOBY_FLAGS),
                     GREF(gaddr, s_watch + MOBY_UPDATE), GREF(gaddr, s_watch + 0x28u), GREF(uint32_t, s_watch + 0xA4u),
                     GREF(float, s_watch + MOBY_POS), GREF(float, s_watch + MOBY_POS + 4u), GREF(float, s_watch + MOBY_POS + 8u));
            warn(message);
            if (d == 2) {
                const gaddr base = GREF(gaddr, OPENRAC_DATA(RAC2_g_mobyTableBase));
                const gaddr loop = GREF(gaddr, OPENRAC_LDATA(0, RAC2_LADDR(00, 0x001B1E9Cu)));
                gaddr stop = loop;
                while (stop != 0 && GREF(uint8_t, stop + MOBY_STATE) != 0xFFu && stop - loop < 0x200000u) {
                    stop += MOBY_SIZE;
                }
                snprintf(message, sizeof message,
                         "foreign watch: table %08x dynamic %08x end %08x, loop from %08x stops at %08x (slot %d), the moby slot %d",
                         base, GREF(gaddr, OPENRAC_DATA(RAC2_g_mobyTableBase + 4u)), GREF(gaddr, OPENRAC_DATA(RAC2_g_mobyTableEnd)),
                         loop, stop, (int)((stop - base) / MOBY_SIZE), (int)((s_watch - base) / MOBY_SIZE));
                warn(message);
            }
        }
    }
    if (s_scan_at != 0 && s_scan_frames % s_scan_at == 0 && s_cost_frames > 0) {
        char message[200];
        snprintf(message, sizeof message, "foreign updates at frame %u: %.1f mobys a frame, %.3f ms a frame (%llu frames)",
                 s_scan_frames, (double)s_cost_mobys / (double)s_cost_frames,
                 (double)s_cost_ns / 1e6 / (double)s_cost_frames, (unsigned long long)s_cost_frames);
        warn(message);
        s_cost_ns = s_cost_frames = s_cost_mobys = 0;
    }
    if (s_scan_at == 0 || s_scan_frames != s_scan_at) {
        return;
    }
    const gaddr base = GREF(gaddr, OPENRAC_DATA(RAC2_g_mobyTableBase));
    const gaddr dynamic = GREF(gaddr, OPENRAC_DATA(RAC2_g_mobyTableBase + 4u));
    const gaddr end = GREF(gaddr, OPENRAC_DATA(RAC2_g_mobyTableEnd));
    const char* dir = openrac_env_string("OPENRAC_FOREIGN_DUMP");
    for (int k = 0; k < s_count; ++k) {
        const Foreign* f = &s_foreign[k];
        if (f->at == 0 || f->loads != loads) {
            continue;
        }
        int mobys = 0, alive = 0, on_copy = 0, on_own = 0;
        uint32_t words_copy = 0, words_own = 0;
        for (gaddr m = base; m != 0 && m < end; m += MOBY_SIZE) {
            if (GREF(int16_t, m + MOBY_OCLASS) != f->oclass) {
                continue;
            }
            ++mobys;
            if (GREF(uint8_t, m + MOBY_STATE) >= 0x80u) {
                continue;
            }
            ++alive;
            const gaddr cls = GREF(gaddr, m + 0x24u);
            on_copy += points_into(cls, f->at, f->size);
            on_own += points_into(cls, f->own, f->size);
            uint32_t pv_bytes = m < dynamic ? rac2_placed_pvars_bytes(m) : SLOT_PVARS;
            const gaddr pv = GREF(gaddr, m + MOBY_PVARS);
            for (int part = 0; part < 2; ++part) {
                const gaddr from = part == 0 ? m : pv;
                const uint32_t bytes = part == 0 ? MOBY_SIZE : (pv != 0 ? pv_bytes : 0u);
                for (uint32_t off = 0; off + 4u <= bytes; off += 4u) {
                    const gaddr v = GREF(gaddr, from + off);
                    words_copy += points_into(v, f->at, f->size);
                    words_own += points_into(v, f->own, f->size);
                }
            }
        }
        char message[256];
        snprintf(
            message,
            sizeof message,
            "foreign class %d scan at frame %u: %d mobys (%d alive; class on the copy %d, on the "
            "level's own %d); words into the copy %u, into the level's own block %u",
            f->oclass,
            s_scan_at,
            mobys,
            alive,
            on_copy,
            on_own,
            words_copy,
            words_own
        );
        warn(message);
        if (dir != NULL) {
            dump(dir, "copy", f->oclass, f->at, f->size);
            if (f->own != 0) {
                dump(dir, "own", f->oclass, f->own, f->size);
            }
        }
    }
}
