/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 the OpenRAC contributors
 *
 * Going Commando: gadgets of the mods' as items of the game's own (openrac_api.gadget_register,
 * game_host.h's openrac_game_play.gadget_*).
 *
 * The game knows 56 items (0 to 55): a byte each of "owned" (0x1A7AF8) and "new" (0x1A7B30), an
 * ammo and an experience word each, all of it in the save, and loops bounded at 56. An id past 55
 * writes over the next array. So a mod's gadget is lent one of the ids the game leaves unused: their
 * records have no name and no icon and nothing gives them (no Item_Give with their number in the
 * decompilation, none in the vendor's list): 33, 34, 40, 11, 35 and 1, in that order.
 *
 * An item's record (0xE0 bytes, g_weapon_table 0x239BA0, reached through g_item_slot_to_record
 * 0x139568) is level data: each level's program has its own copy, read from the disc when it loads.
 * So the lent id's record is written each time a program is in (ParseBin, level.c), before the level
 * runs, and checked each frame: it is the record of the gadget the mod names as its host (the
 * Electrolyzer by default), whose model class, joint and hero animations it borrows, with the
 * port's own texts and icon key, no price and no ammo. It is put in the free cell of the pause
 * menu's Gadgets grid (cell 3, the game's 0: drawn when owned; the cursor never goes there), which
 * also makes Item_Give put it in the quick select.
 *
 * Owned is the port's (the mods' data kept with the save, mod_api/gadgets.cpp): each frame the
 * owned byte follows it. A save never holds a lent id (the serializer below takes them out of the
 * owned, new and quick select arrays, of the hand and the last weapon, and puts them back after), so
 * a save made with a gadget loads cleanly without the mod: the item is not there.
 *
 * In the hand the item is a moby of the host's class (hero+0x1220, the id at +0x1248). Its "use" is
 * the hand's fire button pressed (the hero's slot's mask, +0x1230, against the pad's pressed word):
 * the game's own input switch has no case for these ids (ProcessHeroEquippedItemInput), and the
 * host's moby update does nothing with it (the Electrolyzer's). Events: equipped, put away, used.
 *
 * With no gadget defined nothing here reads or writes the game.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game_protos.h"
#include "openrac/game_host.h"
#include "openrac/guest.h"
#include "openrac/host_env.h"
#include "rac2.h"

#define GADGETS 6
static const int32_t k_lent[GADGETS] = {33, 34, 40, 11, 35, 1};

#define ITEMS 56
#define OWNED RAC2_ADDR(0x001A7AF8u)        /* g_inventoryOwned, a byte an item */
#define NEW_FLAG RAC2_ADDR(0x001A7B30u)     /* g_inventoryNewFlag */
#define QUICK_SELECT RAC2_ADDR(0x001A73B8u) /* g_equippedItemSlots, s32[8] */
#define QUICK_SLOTS 8
#define HAND_CHOICES RAC2_ADDR(0x001A7398u) /* g_menu_choice_table, s32[7]: [0] the hand */
#define LAST_WEAPON RAC2_ADDR(0x001A7A04u)  /* progress +0x0C, the last weapon held */
#define AMMO RAC2_ADDR(0x00139688u)         /* s32 an item */
#define XP RAC2_ADDR(0x00139868u)           /* s32 an item */
#define RECORD_OF RAC2_ADDR(0x00139568u)    /* g_item_slot_to_record, u8 an item (80) */
#define RECORDS OPENRAC_DATA(RAC2_ADDR(0x00239BA0u))
#define RECORD_SIZE 0xE0u
#define RECORDS_COUNT 97
/* The pause menu's Gadgets grid (g_menu_item_table_second): cells of 10 bytes {u16 icon, s16
 * frame, s16 type, s16 item, s16 pad}; 3 x 2, cell 3 empty (item 0). */
#define GADGET_GRID OPENRAC_DATA(RAC2_ADDR(0x00259CE0u))
#define GRID_CELL 10u
#define GRID_FREE_CELL 3u

/* The hero (D_00189E20): his hand slot (0x50 bytes at +0x1220): its moby, the fire mask, its mode
 * (2 held) and its item; the request word the pause menu and quick select write (+0x22C8). */
#define HERO (RAC2_g_heroPos - 0x80u)
#define HAND_MOBY (HERO + 0x1220u)
#define HAND_FIRE_MASK (HERO + 0x1230u)
#define HAND_MODE (HERO + 0x1244u)
#define HAND_ITEM (HERO + 0x1248u)
#define HAND_REQUEST (HERO + 0x22C8u)
/* Port 0's pad record (g_pad_state): the buttons pressed this frame, +0x1A4. */
#define PAD_PRESSED (RAC2_ADDR(0x00138180u) + 0x1A4u)

/* A record's fields (launcher/docs/RAC2_SAVE_FORMAT.md and the decompilation's record structs). */
enum {
    REC_ACTIVE = 0x00,
    REC_VERSION = 0x04,
    REC_NAME = 0x08,
    REC_SLOT = 0x0C,
    REC_ICON = 0x3C,
    REC_DESCRIPTION = 0x42,
    REC_QUICK_NAME = 0x48,
    REC_NEXT = 0x4A,
    REC_PREVIOUS = 0x4C,
    REC_XP_STEP = 0x6C,
    REC_PRICE = 0x80,
    REC_SELLS_AMMO = 0x88,
    REC_FULL_AMMO = 0x8E,
    REC_START_AMMO = 0x92,
};

/* The port's text ids for the gadgets' names and descriptions (Text_ById, options.c), two a gadget,
 * and the icon keys their records carry (Gui_DrawIcon draws the mod's picture there; GetIconFrame
 * answers the host's for any other reader). Neither is a text or icon of the game's. */
#define TEXT_FIRST 0x7F80
#define TEXT_BYTES 96
#define ICON_FIRST 0x7F80

/* A moby (play.c's). */
enum { MOBY_POS = 0x10, MOBY_STATE = 0x20, MOBY_FLAGS = 0x34, MOBY_ROT = 0xF0 };

typedef struct gadget {
    int defined;
    int owned;
    int32_t host;
    char name[TEXT_BYTES];
    char description[TEXT_BYTES];
} gadget;

static gadget s_gadgets[GADGETS];
static int s_defined;
static gaddr s_texts;
/* The slot in the hand last frame (-1 none), and the events since the mods' layer last took them. */
static int s_held = -1;
#define EVENTS 16
static openrac_game_gadget_event s_events[EVENTS];
static int s_event_count;

static int slot_of_item(int32_t item) {
    for (int i = 0; i < GADGETS; ++i) {
        if (s_gadgets[i].defined && k_lent[i] == item) {
            return i;
        }
    }
    return -1;
}

static int in_level(void) {
    return openrac_game_loaded_overlay() != OPENRAC_OVERLAY_EXE;
}

static gaddr record_of(int32_t item) {
    const uint8_t index = GREF(uint8_t, RECORD_OF + (gaddr)item);
    if (index >= RECORDS_COUNT) {
        return 0;
    }
    return RECORDS + (gaddr)index * RECORD_SIZE;
}

static gaddr text_of(int id) {
    if (s_texts == 0) {
        s_texts = openrac_guest_static(GADGETS * 2 * TEXT_BYTES);
        memset(G(s_texts), 0, GADGETS * 2 * TEXT_BYTES);
    }
    return s_texts + (gaddr)(id - TEXT_FIRST) * TEXT_BYTES;
}

/* The lent id's record as its host's, with the port's texts and icon key. 1 when it was written. */
static int write_record(int i) {
    const gadget* g = &s_gadgets[i];
    const int32_t id = k_lent[i];
    /* The lent id is its own record (an upgrade rewrites the map for weapons only). */
    GREF(uint8_t, RECORD_OF + (gaddr)id) = (uint8_t)id;
    const gaddr host = record_of(g->host);
    const gaddr rec = record_of(id);
    if (host == 0 || rec == 0) {
        return 0;
    }
    uint8_t want[RECORD_SIZE];
    memcpy(want, G(host), RECORD_SIZE);
    const int32_t zero = 0, slot = 0;
    const int32_t name = TEXT_FIRST + 2 * i;
    const int16_t description = (int16_t)(TEXT_FIRST + 2 * i + 1);
    const int16_t quick_name = (int16_t)name;
    const uint16_t icon = (uint16_t)(ICON_FIRST + i);
    /* No version chain: its links are 0, the chain's end (menu_count_chain_entries_after_head walks
     * them, and -1 would read past the table). */
    const int16_t none = 0;
    memcpy(want + REC_ACTIVE, &zero, 4);
    want[REC_VERSION] = 0;
    memcpy(want + REC_NAME, &name, 4);
    memcpy(want + REC_SLOT, &slot, 4);
    memcpy(want + REC_ICON, &icon, 2);
    memcpy(want + REC_DESCRIPTION, &description, 2);
    memcpy(want + REC_QUICK_NAME, &quick_name, 2);
    memcpy(want + REC_NEXT, &none, 2);
    memcpy(want + REC_PREVIOUS, &none, 2);
    memcpy(want + REC_XP_STEP, &zero, 4);
    memcpy(want + REC_PRICE, &zero, 4);
    memset(want + REC_SELLS_AMMO, 0, 2);
    memset(want + REC_FULL_AMMO, 0, 2);
    memset(want + REC_START_AMMO, 0, 2);
    /* +0x24 is the game's (ApplyHeroEquipmentRequest writes 0x20 there): left as it is. */
    memcpy(want + 0x24, G(rec + 0x24u), 4);
    if (memcmp(want, G(rec), RECORD_SIZE) == 0) {
        return 0;
    }
    memcpy(G(rec), want, RECORD_SIZE);
    return 1;
}

/* The Gadgets grid's free cell: the first owned gadget's lent id, else the first defined one's (the
 * game draws a cell only when its item is owned). */
static void write_grid(void) {
    int32_t item = 0;
    for (int i = 0; i < GADGETS && item == 0; ++i) {
        if (s_gadgets[i].defined && s_gadgets[i].owned) {
            item = k_lent[i];
        }
    }
    for (int i = 0; i < GADGETS && item == 0; ++i) {
        if (s_gadgets[i].defined) {
            item = k_lent[i];
        }
    }
    const gaddr cell = GADGET_GRID + GRID_FREE_CELL * GRID_CELL;
    if (GREF(int16_t, cell + 4u) == 0 && GREF(int16_t, cell + 6u) != (int16_t)item) {
        GREF(int16_t, cell + 6u) = (int16_t)item;
    }
}

static void write_level(void) {
    if (s_defined == 0 || !in_level()) {
        return;
    }
    for (int i = 0; i < GADGETS; ++i) {
        if (s_gadgets[i].defined && write_record(i)) {
            fprintf(stderr, "[gadget] slot %d: item %d's record written as item %d's\n", i, k_lent[i],
                s_gadgets[i].host);
        }
    }
    write_grid();
}

void rac2_gadgets_program_loaded(void) {
    write_level();
}

static void push_event(int slot, int kind) {
    if (s_event_count >= EVENTS) {
        return;
    }
    openrac_game_gadget_event* e = &s_events[s_event_count++];
    memset(e, 0, sizeof *e);
    e->slot = slot;
    e->kind = kind;
    const gaddr moby = GREF(gaddr, HAND_MOBY);
    const float yaw = GREF(float, RAC2_g_heroOrientVec + 8u);
    for (unsigned k = 0; k < 3; ++k) {
        e->position[k] = moby != 0 ? GREF(float, moby + MOBY_POS + 4u * k)
                                   : GREF(float, RAC2_g_heroPos + 4u * k);
    }
    e->direction[0] = cosf(yaw);
    e->direction[1] = sinf(yaw);
    e->direction[2] = 0.0f;
}

/* The slot in the hand now, -1 for none. */
static int held_now(void) {
    if (GREF(int32_t, HAND_MODE) != 2 || GREF(gaddr, HAND_MOBY) == 0) {
        return -1;
    }
    return slot_of_item(GREF(int32_t, HAND_ITEM));
}

/* ---- OPENRAC_GADGET_PROBE: a gadget defined and given without a mod, for a hidden run ----
 * Its value is HOST[:EQUIP_FRAME]: slot 0 defined with host item HOST, given once the level plays,
 * and asked into the hand at EQUIP_FRAME frames into play (docs/port/DEBUGGING.md). */
static int s_probe = -1;
static int s_probe_equip = -1;
static int s_probe_frames;

static void probe_start(void) {
    static int read;
    if (read) {
        return;
    }
    read = 1;
    const char* env = openrac_env_string("OPENRAC_GADGET_PROBE");
    if (env == NULL || *env == '\0') {
        return;
    }
    char* end = NULL;
    s_probe = (int)strtol(env, &end, 10);
    if (end != NULL && *end == ':') {
        s_probe_equip = (int)strtol(end + 1, NULL, 10);
    }
    openrac_game_gadget g;
    memset(&g, 0, sizeof g);
    g.host_item = s_probe;
    g.name = "Probe Gadget";
    g.description = "A gadget the port defined for a test.";
    rac2_gadget_define(0, &g);
    fprintf(stderr, "[gadget] probe: slot 0 lent item %d, host %d, equip at %d\n", k_lent[0], s_probe,
        s_probe_equip);
}

static void probe_frame(void) {
    if (s_probe < 0 || !in_level() || GREF(int32_t, RAC2_g_health) <= 0) {
        return;
    }
    ++s_probe_frames;
    if (s_probe_frames == 60) {
        rac2_gadget_owned(0, 1);
        const gaddr rec = record_of(k_lent[0]);
        fprintf(stderr, "[gadget] probe: given; owned %d, quick select %d %d %d %d %d %d %d %d, record class %d\n",
            GREF(uint8_t, OWNED + (gaddr)k_lent[0]), GREF(int32_t, QUICK_SELECT), GREF(int32_t, QUICK_SELECT + 4u),
            GREF(int32_t, QUICK_SELECT + 8u), GREF(int32_t, QUICK_SELECT + 12u), GREF(int32_t, QUICK_SELECT + 16u),
            GREF(int32_t, QUICK_SELECT + 20u), GREF(int32_t, QUICK_SELECT + 24u), GREF(int32_t, QUICK_SELECT + 28u),
            rec != 0 ? GREF(int32_t, rec + 0x14u) : -1);
    }
    if (s_probe_equip > 0 && s_probe_frames == s_probe_equip) {
        fprintf(stderr, "[gadget] probe: equip asked: %d\n", rac2_gadget_equip(0));
    }
    if (s_probe_frames == 300) {
        /* The quick select's runtime ring (g_weapon_menu_slots_2, 8 of 0x1C bytes: +0 icon, +0x18 item). */
        const gaddr ring = OPENRAC_DATA(RAC2_ADDR(0x00255170u));
        fprintf(stderr, "[gadget] probe: ring");
        for (unsigned k = 0; k < 8; ++k) {
            fprintf(stderr, " %d/%x", GREF(int32_t, ring + 0x1Cu * k + 0x18u), GREF(uint32_t, ring + 0x1Cu * k));
        }
        fprintf(stderr, "\n");
    }
    if (s_probe_frames % 60 == 0) {
        const gaddr moby = GREF(gaddr, HAND_MOBY);
        fprintf(stderr,
            "[gadget] probe frame %d: hand item %d mode %d moby %08x class %d mask %x flags %04x alpha %02x drawn %02x seq %d scale %.4f\n",
            s_probe_frames, GREF(int32_t, HAND_ITEM), GREF(int32_t, HAND_MODE), moby,
            moby != 0 ? (int)GREF(int16_t, moby + 0xAAu) : -1, GREF(uint32_t, HAND_FIRE_MASK),
            moby != 0 ? GREF(uint16_t, moby + MOBY_FLAGS) : 0, moby != 0 ? GREF(uint8_t, moby + 0x23u) : 0,
            moby != 0 ? GREF(uint8_t, moby + 0x31u) : 0, moby != 0 ? GREF(uint8_t, moby + 0x42u) : 0,
            moby != 0 ? GREF(float, moby + 0x2Cu) : 0.0f);
    }
}

void rac2_gadgets_frame(void) {
    probe_start();
    if (s_defined == 0 || !in_level()) {
        return;
    }
    write_level();
    for (int i = 0; i < GADGETS; ++i) {
        if (s_gadgets[i].defined) {
            GREF(uint8_t, OWNED + (gaddr)k_lent[i]) = (uint8_t)(s_gadgets[i].owned ? 1 : 0);
        }
    }
    const int held = held_now();
    if (held != s_held) {
        if (s_held >= 0) {
            push_event(s_held, OPENRAC_GAME_GADGET_PUT_AWAY);
        }
        if (held >= 0) {
            push_event(held, OPENRAC_GAME_GADGET_EQUIPPED);
        }
        s_held = held;
    }
    if (held >= 0 && (GREF(uint32_t, PAD_PRESSED) & GREF(uint32_t, HAND_FIRE_MASK)) != 0) {
        push_event(held, OPENRAC_GAME_GADGET_USED);
    }
    probe_frame();
    for (int k = 0; k < s_event_count && s_probe >= 0; ++k) {
        fprintf(stderr, "[gadget] event slot %d kind %d at %.1f %.1f %.1f dir %.2f %.2f\n", s_events[k].slot,
            s_events[k].kind, s_events[k].position[0], s_events[k].position[1], s_events[k].position[2],
            s_events[k].direction[0], s_events[k].direction[1]);
    }
    if (s_probe >= 0) {
        s_event_count = 0;
    }
}

/* ---- openrac_game_play's calls ---- */

int rac2_gadget_define(int slot, const openrac_game_gadget* g) {
    if (slot < 0 || slot >= GADGETS) {
        return -1;
    }
    gadget* d = &s_gadgets[slot];
    if (g == NULL) {
        if (d->defined) {
            --s_defined;
        }
        memset(d, 0, sizeof *d);
        return 0;
    }
    const int32_t host = g->host_item > 0 ? g->host_item : 38;
    if (host >= ITEMS || slot_of_item(host) >= 0) {
        return -1;
    }
    for (int i = 0; i < GADGETS; ++i) {
        if (k_lent[i] == host) {
            return -1;
        }
    }
    if (!d->defined) {
        ++s_defined;
    }
    d->defined = 1;
    d->host = host;
    snprintf(d->name, sizeof d->name, "%s", g->name != NULL ? g->name : "");
    snprintf(d->description, sizeof d->description, "%s", g->description != NULL ? g->description : "");
    snprintf((char*)G(text_of(TEXT_FIRST + 2 * slot)), TEXT_BYTES, "%s", d->name);
    snprintf((char*)G(text_of(TEXT_FIRST + 2 * slot + 1)), TEXT_BYTES, "%s", d->description);
    write_level();
    return 0;
}

int rac2_gadget_owned(int slot, int owned) {
    if (slot < 0 || slot >= GADGETS || !s_gadgets[slot].defined) {
        return -1;
    }
    gadget* g = &s_gadgets[slot];
    if (owned < 0) {
        return g->owned;
    }
    const int32_t id = k_lent[slot];
    if (owned && !g->owned) {
        g->owned = 1;
        if (in_level()) {
            write_level();
            /* The game's giving: owned, new, and the quick select (the item is in the Gadgets grid
             * and held in the hand); no ammo (none in its record). */
            func_00288308(id);
        }
    } else if (!owned && g->owned) {
        g->owned = 0;
        if (in_level()) {
            GREF(uint8_t, OWNED + (gaddr)id) = 0;
            GREF(uint8_t, NEW_FLAG + (gaddr)id) = 0;
            for (unsigned k = 0; k < QUICK_SLOTS; ++k) {
                if (GREF(int32_t, QUICK_SELECT + 4u * k) == id) {
                    GREF(int32_t, QUICK_SELECT + 4u * k) = 0;
                }
            }
            /* Out of the hand: the game's own request for nothing (the Lancer, as for a hand of 0). */
            if (held_now() == slot) {
                GREF(int32_t, HAND_REQUEST) = 0;
            }
        }
    }
    return g->owned;
}

int rac2_gadget_held(void) {
    return s_defined != 0 && in_level() ? held_now() : -1;
}

int32_t rac2_gadget_item(int slot) {
    return slot >= 0 && slot < GADGETS ? k_lent[slot] : -1;
}

int32_t rac2_gadget_hand_moby(void) {
    if (rac2_gadget_held() < 0) {
        return 0;
    }
    return (int32_t)GREF(gaddr, HAND_MOBY);
}

int rac2_gadget_equip(int slot) {
    if (slot < 0 || slot >= GADGETS || !s_gadgets[slot].defined || !s_gadgets[slot].owned || !in_level()) {
        return -1;
    }
    write_level();
    /* Item_Equip: the hand's request, with the class loaded. */
    return func_002AD860(k_lent[slot]) != 0 ? 0 : -1;
}

int rac2_gadget_events(openrac_game_gadget_event* out, int max) {
    const int n = s_event_count < max ? s_event_count : max;
    if (out != NULL && n > 0) {
        memcpy(out, s_events, sizeof *out * (size_t)n);
    }
    s_event_count = 0;
    return n;
}

/* ---- the game's calls the port wraps ---- */

/* Text_ById (options.c): a gadget's name or description, 0 for an id that is not one. */
gaddr rac2_gadget_text(int id) {
    if (s_defined == 0 || id < TEXT_FIRST || id >= TEXT_FIRST + 2 * GADGETS
        || !s_gadgets[(id - TEXT_FIRST) / 2].defined) {
        return 0;
    }
    return text_of(id);
}

/* The slot whose icon key `key` is, -1 for none. */
int rac2_gadget_icon_slot(int key) {
    if (s_defined == 0 || key < ICON_FIRST || key >= ICON_FIRST + GADGETS
        || !s_gadgets[key - ICON_FIRST].defined) {
        return -1;
    }
    return key - ICON_FIRST;
}

/* The host's icon key for a gadget's (GetIconFrame's other readers draw the host's). */
int rac2_gadget_host_icon(int slot) {
    const gaddr host = record_of(s_gadgets[slot].host);
    return host != 0 ? GREF(uint16_t, host + REC_ICON) : 0;
}

/* Save_Serialize: the lent ids out of what the save holds while the game writes it, and back. */
typedef struct gadget_save_aside {
    uint8_t owned[GADGETS];
    uint8_t fresh[GADGETS];
    int32_t ammo[GADGETS];
    int32_t xp[GADGETS];
    int32_t quick[QUICK_SLOTS];
    int32_t hand;
    int32_t last;
} gadget_save_aside;

static gadget_save_aside s_aside;

static int lent_id(int32_t item) {
    for (int i = 0; i < GADGETS; ++i) {
        if (k_lent[i] == item) {
            return 1;
        }
    }
    return 0;
}

void rac2_gadgets_save_strip(void) {
    if (s_defined == 0) {
        return;
    }
    for (int i = 0; i < GADGETS; ++i) {
        const gaddr id = (gaddr)k_lent[i];
        if (!s_gadgets[i].defined) {
            continue;
        }
        s_aside.owned[i] = GREF(uint8_t, OWNED + id);
        s_aside.fresh[i] = GREF(uint8_t, NEW_FLAG + id);
        s_aside.ammo[i] = GREF(int32_t, AMMO + 4u * id);
        s_aside.xp[i] = GREF(int32_t, XP + 4u * id);
        GREF(uint8_t, OWNED + id) = 0;
        GREF(uint8_t, NEW_FLAG + id) = 0;
        GREF(int32_t, AMMO + 4u * id) = 0;
        GREF(int32_t, XP + 4u * id) = 0;
    }
    for (unsigned k = 0; k < QUICK_SLOTS; ++k) {
        s_aside.quick[k] = GREF(int32_t, QUICK_SELECT + 4u * k);
        if (lent_id(s_aside.quick[k])) {
            GREF(int32_t, QUICK_SELECT + 4u * k) = 0;
        }
    }
    s_aside.hand = GREF(int32_t, HAND_CHOICES);
    s_aside.last = GREF(int32_t, LAST_WEAPON);
    if (lent_id(s_aside.hand)) {
        GREF(int32_t, HAND_CHOICES) = lent_id(s_aside.last) ? 0 : s_aside.last;
    }
    if (lent_id(s_aside.last)) {
        GREF(int32_t, LAST_WEAPON) = 0;
    }
}

void rac2_gadgets_save_restore(void);

/* The lent ids are out of the game's arrays while any of the two packers runs (one may call the
 * other): taken out at the outer call's start, put back at its end. */
static int s_strip_depth;

static void strip_enter(void) {
    if (s_strip_depth++ == 0) {
        rac2_gadgets_save_strip();
    }
}

static void strip_leave(void) {
    if (--s_strip_depth == 0) {
        rac2_gadgets_save_restore();
    }
}

/* Save_Serialize (save_build_packet, func_0029ADA0; a hook point too): the packet of the save the
 * port's captures read (play.c's save_capture). */
void func_0029ADA0__hooked(gaddr out);
void func_0029ADA0(gaddr out) {
    strip_enter();
    func_0029ADA0__hooked(out);
    strip_leave();
}

/* save_pack_records (func_0029AF98): what MemcardSave writes to the card, the save's records copied
 * from the game's arrays (g_save_record_layout_main and _slot). */
int func_0029AF98__game(gaddr buffer, int slot, gaddr records);
int func_0029AF98(gaddr buffer, int slot, gaddr records) {
    strip_enter();
    const int size = func_0029AF98__game(buffer, slot, records);
    strip_leave();
    return size;
}

/* Gui_DrawIcon (hud_draw_colored_quad_widget, func_00336828; a hook point too): an icon widget of
 * the game's 2D, its key and frame the floats at its +0x34, its corner +0x00 and size +0x04, its
 * colour +0x0C and visibility +0x10. A gadget's key is drawn as the mod's picture instead
 * (openrac_game_gadget_icon, the mods' layer's), and the game draws nothing there. */

void func_00336828__hooked(gaddr widget);
void func_00336828(gaddr widget) {
    if (s_defined != 0) {
        const gaddr value = GREF(gaddr, widget + 0x34u);
        const int slot = value != 0 ? rac2_gadget_icon_slot((int)GREF(float, value)) : -1;
        if (slot >= 0) {
            const gaddr corner = GREF(gaddr, widget);
            const gaddr size = GREF(gaddr, widget + 4u);
            const float shown = GREF(float, GREF(gaddr, widget + 0x10u));
            int drawn = 0;
            if (shown != 0.0f && openrac_game_gadget_icon != NULL) {
                drawn = openrac_game_gadget_icon(slot, GREF(float, corner), GREF(float, corner + 4u),
                    GREF(float, size), GREF(float, size + 4u), GREF(uint32_t, GREF(gaddr, widget + 0x0Cu)));
            }
            if (!drawn) {
                /* No picture of the mod's: the host gadget's icon, the widget's key put back after. */
                const float key = GREF(float, value);
                GREF(float, value) = (float)rac2_gadget_host_icon(slot);
                func_00336828__hooked(widget);
                GREF(float, value) = key;
            }
            static int told;
            if (s_probe >= 0 && told < 8) {
                ++told;
                fprintf(stderr, "[gadget] icon of slot %d at %.1f %.1f size %.1f %.1f colour %08x shown %.2f\n", slot,
                    GREF(float, corner), GREF(float, corner + 4u), GREF(float, size), GREF(float, size + 4u),
                    GREF(uint32_t, GREF(gaddr, widget + 0x0Cu)), shown);
            }
            return;
        }
    }
    func_00336828__hooked(widget);
}

/* DrawItemGrid (func_00337210): a grid of the pause menu's (Grid: +0 its cells, +0x50 the cells it
 * leaves out, -1 ended, at most 10). The Gadgets grid's free cell (3) is left out by the game; with
 * an owned gadget in it, it is drawn (its icon; the game's cursor still never goes there). */
void func_00337210__game(gaddr grid);
void func_00337210(gaddr grid) {
    static gaddr kept;
    const gaddr hidden = GREF(gaddr, grid + 0x50u);
    const gaddr cell = GADGET_GRID + GRID_FREE_CELL * GRID_CELL;
    if (s_defined == 0 || hidden == 0 || GREF(gaddr, grid) != GADGET_GRID
        || slot_of_item(GREF(int16_t, cell + 6u)) < 0
        || !GREF(uint8_t, OWNED + (gaddr)GREF(int16_t, cell + 6u))) {
        func_00337210__game(grid);
        return;
    }
    if (kept == 0) {
        kept = openrac_guest_static(11 * 4);
    }
    int n = 0;
    for (int i = 0; i < 10 && GREF(int32_t, hidden + 4u * (gaddr)i) >= 0; ++i) {
        if (GREF(int32_t, hidden + 4u * (gaddr)i) != (int32_t)GRID_FREE_CELL) {
            GREF(int32_t, kept + 4u * (gaddr)n++) = GREF(int32_t, hidden + 4u * (gaddr)i);
        }
    }
    GREF(int32_t, kept + 4u * (gaddr)n) = -1;
    GREF(gaddr, grid + 0x50u) = kept;
    func_00337210__game(grid);
    GREF(gaddr, grid + 0x50u) = hidden;
}

/* GetIconFrame (func_0028E588): a gadget's key is not in the game's icon table (a key it lacks
 * reads past the table's end): any other reader of it gets the host gadget's picture. */
int func_0028E588__game(int key, int index);
int func_0028E588(int key, int index) {
    if (s_defined != 0) {
        const int slot = rac2_gadget_icon_slot(key);
        if (slot >= 0) {
            static int told;
            if (told < 6) {
                ++told;
                fprintf(stderr, "[gadget] GetIconFrame asked for slot %d's key (frame %d)\n", slot, index);
            }
            key = rac2_gadget_host_icon(slot);
        }
    }
    const int tex = func_0028E588__game(key, index);
    rac2_game_ui_probe_icon(key, index, tex); /* game_ui.c: OPENRAC_GAME_UI_LOG=sprites */
    return tex;
}

void rac2_gadgets_save_restore(void) {
    if (s_defined == 0) {
        return;
    }
    for (int i = 0; i < GADGETS; ++i) {
        const gaddr id = (gaddr)k_lent[i];
        if (!s_gadgets[i].defined) {
            continue;
        }
        GREF(uint8_t, OWNED + id) = s_aside.owned[i];
        GREF(uint8_t, NEW_FLAG + id) = s_aside.fresh[i];
        GREF(int32_t, AMMO + 4u * id) = s_aside.ammo[i];
        GREF(int32_t, XP + 4u * id) = s_aside.xp[i];
    }
    for (unsigned k = 0; k < QUICK_SLOTS; ++k) {
        GREF(int32_t, QUICK_SELECT + 4u * k) = s_aside.quick[k];
    }
    GREF(int32_t, HAND_CHOICES) = s_aside.hand;
    GREF(int32_t, LAST_WEAPON) = s_aside.last;
}
