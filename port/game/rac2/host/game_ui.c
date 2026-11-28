/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 the OpenRAC contributors
 *
 * Going Commando: a mod's interface drawn with the game's own 2D (interface 1.129,
 * openrac_api.gui_draw, help_box_*, gui_text_width, game_sound_play; game_host.h's
 * openrac_game_play members of the same names and openrac_game_gui_elements).
 *
 * Where: DrawSubtitleBox (func_00289AA8), which the frame's 2D pass (0027BD70) calls right after the
 * level's callbacks and the HUD, is wrapped. After the game's own box, what the mods draw goes out
 * with the game's own routines, in the same pass and the same 512 x 416 screen:
 * - text: the font's scaled print (func_0027F640, the HUD's and the GUI's), the large font or the
 *   small one, with the font's colour bytes on (gfx_enable_screen_flag's word, 0x001B1668: bytes
 *   0x08 to 0x0F take g_text_color_table's colours, 0x0C the help box's orange) for that print only;
 * - the help box's look: gfx_draw_rounded_box (func_0027EB88), the fill, and level_set_rect
 *   (func_0029B658: text_box_draw_frame's two-line frame with its brackets, sprites of the resident
 *   GUI data);
 * - the menus' sprites: gfx_draw_scaled_object_two_axes (func_00300A00), as GuiSpriteElementDraw
 *   draws them, of an object the catalogue below names;
 * - the HUD's icons: the HUD's gradient sprite (func_0028EA58) of a HUD texture, as Gui_DrawIcon
 *   draws them; an icon key goes through GetIconFrame (func_0028E588), and an item's key is +0x3C
 *   of its record.
 * Then the mods' help boxes, on top.
 *
 * The help boxes: a queue of the port's own beside the game's g_subtitle_state, stepped as
 * subtitle_update_state_machine steps it (00289178.c: the frame grows 8 ticks, the text comes in
 * 4, stays, goes out in 4, the frame shrinks 8; Triangle closes it) and drawn as DrawSubtitleBox
 * draws it (FontSetWindow func_002802E0 and text_draw_window_medium func_00280200 for the text, at
 * the box's own y), sized as subtitle_init_state sizes it (the text measured with the window's
 * flag 4). Several are up at once at different heights; one at a height another (or the game's own)
 * takes waits its turn. The game's entry 0 is not touched.
 *
 * The sounds: StartSoundEmitter (func_002E5AD0) with the record PlayGlobalSound
 * (func_002E5E58) or snd_play_from_class_bank (func_002E5DA0) would give it, and the volume the
 * mod asks for in place of their 1024.
 *
 * With nothing asked by a mod the wrap calls the game's and reads one empty list: the game runs as
 * before. OPENRAC_GAME_UI_LOG=1 logs `[ui]` lines (what is drawn every 120 ticks, the boxes, the
 * sounds); OPENRAC_GAME_UI_LOG=sprites also each GUI sprite object, HUD texture and icon key the game
 * draws for the first time (a probe for the catalogue). v1.01 (NTSC-U) alone: the addresses below
 * are its (RAC2_PRIMARY).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "game_protos.h"
#include "openrac/game_host.h"
#include "openrac/guest.h"
#include "openrac/host_env.h"
#include "openrac/openrac_mod.h"
#include "rac2.h"

#if defined(RAC2_PRIMARY)

/* ---- The game's places ---- */
#define SUB_STATE OPENRAC_DATA(RAC2_ADDR(0x00254E78u)) /* g_subtitle_state: +0 state, +0xC halfHeight, +0x14 y */
#define SCREEN_HEIGHT RAC2_ADDR(0x001A7344u)            /* g_screen_height */
#define PAD_HELD RAC2_ADDR(0x00138324u)                 /* g_pad_held_flags: 0x10 Triangle */
#define GAME_MODE OPENRAC_DATA(RAC2_g_nGameState)       /* g_current_game_mode: 0 play, 7 */
#define COLOUR_CODES OPENRAC_DATA(RAC2_ADDR(0x001B1668u)) /* g_screen_flag_enabled: the font's colour bytes */
#define MENU_SCALE OPENRAC_DATA(RAC2_ADDR(0x001B23A8u))   /* g_menu_alpha_scale: GuiSpriteElementDraw's y scale */
#define RESIDENT OPENRAC_DATA(RAC2_ADDR(0x001A8CFCu))     /* g_resident_data_base */
#define VIEW_SCALE (OPENRAC_DATA(RAC2_ADDR(0x001B9040u)) + 0xB0u) /* g_view_params +0xB0 */
#define FONT_LARGE OPENRAC_DATA(RAC2_ADDR(0x00263B50u))
#define FONT_SMALL OPENRAC_DATA(RAC2_ADDR(0x00263EF0u))
#define RECORD_OF RAC2_ADDR(0x00139568u)                  /* g_item_slot_to_record, u8 an item */
#define RECORDS OPENRAC_DATA(RAC2_ADDR(0x00239BA0u))      /* the items' records, 0xE0 each */
#define RECORD_SIZE 0xE0u
#define RECORD_ICON 0x3Cu
#define RECORDS_COUNT 97
#define ITEMS 56
#define GLOBAL_SOUNDS OPENRAC_DATA(RAC2_ADDR(0x001B16ACu))      /* g_globalSoundDefsPtr */
#define GLOBAL_SOUND_COUNT OPENRAC_DATA(RAC2_ADDR(0x001A8BBCu)) /* g_nGlobalSoundDefs */
#define CLASS_OBJECTS OPENRAC_DATA(RAC2_ADDR(0x001CDB80u))      /* g_class_cache_objects, by slot */
#define CLASS_SLOT OPENRAC_DATA(RAC2_ADDR(0x001CE4E0u))         /* g_class_slot_lookup, by class (0xFF none) */
#define CLASS_SLOTS 240u
#define CLASS_MAX 0x2000
#define VOICES RAC2_ADDR(0x00188660u)                           /* the sound voices, 112 bytes each */
#define VOICE_BYTES 112u
#define TRIANGLE 0x10u

/* The game's functions called here are game_protos.h's (text_draw_window_medium, func_00280200, takes
 * the font window, the colour, the text and its length). */

/* ---- Log ---- */
static int s_log = -1; /* -1 unread, 0 off, 1 on, 2 the sprites' probe too */

static int ui_log(void) {
    if (s_log < 0) {
        const char* env = openrac_env_string("OPENRAC_GAME_UI_LOG");
        s_log = env == NULL || env[0] == '\0' || strcmp(env, "0") == 0 ? 0
                : strcmp(env, "sprites") == 0                          ? 2
                                                                       : 1;
    }
    return s_log;
}

static int seen_once(gaddr key);

static int in_ram(gaddr a) {
    a &= 0x1FFFFFFFu;
    return a >= 0x00100000u && a < openrac_guest_ram_size;
}

/* ---- Guest scratch: a text, a font window's layout, four colours ---- */
static gaddr s_scratch;
#define SCRATCH_TEXT 0u
#define SCRATCH_LAYOUT 256u
#define SCRATCH_COLOURS 320u
#define SCRATCH_BYTES 336u

static gaddr scratch(void) {
    if (s_scratch == 0) {
        s_scratch = openrac_guest_static(SCRATCH_BYTES);
    }
    return s_scratch;
}

static gaddr guest_text(const char* text) {
    const gaddr at = scratch() + SCRATCH_TEXT;
    size_t n = strlen(text);
    if (n > 255) {
        n = 255;
    }
    memcpy(G(at), text, n);
    GREF(char, at + (gaddr)n) = '\0';
    return at;
}

/* A colour (0xAABBGGRR, 0 to 255) as the GS takes it: the channels as they are, alpha 0 to 128. */
static uint32_t alpha128(uint32_t rgba) {
    return (((rgba >> 24) & 0xFFu) * 128u + 127u) / 255u;
}

static uint32_t gs_colour(uint32_t rgba) {
    return (rgba & 0xFFFFFFu) | (alpha128(rgba) << 24);
}

/* ---- Text ---- */
static float text_width(gaddr text, int small) {
    return (float)(small ? func_0027F198(text, -1) : func_0027F178(text, -1));
}

static void print(float x, float y, uint32_t colour, gaddr text, float scale, int small, int codes) {
    const int was = GREF(int, COLOUR_CODES);
    GREF(int, COLOUR_CODES) = codes;
    func_0027F640(x, y, colour, text, -1, scale, func_0027C878(small ? 2 : 1), small ? FONT_SMALL : FONT_LARGE);
    GREF(int, COLOUR_CODES) = was;
}

static void draw_text(const openrac_game_gui_element* e) {
    const gaddr text = guest_text(e->text);
    const int small = (e->flags & OPENRAC_GUI_SMALL) != 0;
    const float scale = e->scale_x;
    float x = e->x;
    if ((e->flags & (OPENRAC_GUI_CENTER | OPENRAC_GUI_RIGHT)) != 0) {
        const float w = text_width(text, small) * scale;
        x -= (e->flags & OPENRAC_GUI_RIGHT) != 0 ? w : w / 2.0f;
    }
    const uint32_t colour = gs_colour(e->rgba == 0 ? 0xFF808080u : e->rgba);
    if ((e->flags & OPENRAC_GUI_SHADOW) != 0) {
        /* Black, the colour bytes off: a shadow takes none of them. */
        print(x + 1.0f, e->y + 1.0f, colour & 0xFF000000u, text, scale, small, 0);
    }
    print(x, e->y, colour, text, scale, small, 1);
}

float rac2_ui_text_width(const char* text, uint32_t flags, float scale) {
    if (text == NULL || GREF(gaddr, RESIDENT) == 0) {
        return -1.0f;
    }
    return text_width(guest_text(text), (flags & OPENRAC_GUI_SMALL) != 0) * scale;
}

/* ---- The help box's look ---- */
#define BOX_FILL_ALPHA 96
#define BOX_FILL 0x60442D00
#define BOX_FRAME_ALPHA 96
#define BOX_FRAME 0x55F0C070

static void box_look(int top, int bottom, int left, int right, int fill_alpha, int fill, int frame_alpha,
                     int frame, int what) {
    if ((what & 1) != 0) {
        func_0027EB88(top, bottom, left, right, fill_alpha, fill);
    }
    if ((what & 2) != 0) {
        func_0029B658(frame_alpha, frame, (float)top, (float)bottom, (float)left, (float)right);
    }
}

static void draw_box(const openrac_game_gui_element* e) {
    const int top = (int)floorf(e->y - e->h / 2.0f), bottom = (int)floorf(e->y + e->h / 2.0f);
    const int left = (int)floorf(e->x - e->w / 2.0f), right = (int)floorf(e->x + e->w / 2.0f);
    const int fill_alpha = e->rgba == 0 ? BOX_FILL_ALPHA : (int)alpha128(e->rgba);
    const int fill = e->rgba == 0 ? BOX_FILL : (int)((e->rgba & 0xFFFFFFu) | ((uint32_t)fill_alpha << 24));
    const int frame_alpha = e->rgba2 == 0 ? BOX_FRAME_ALPHA : (int)alpha128(e->rgba2);
    const int frame = e->rgba2 == 0 ? BOX_FRAME : (int)(e->rgba2 & 0xFFFFFFu);
    box_look(top, bottom, left, right, fill_alpha, fill, frame_alpha, frame,
             ((e->flags & OPENRAC_GUI_NO_FILL) ? 0 : 1) | ((e->flags & OPENRAC_GUI_NO_FRAME) ? 0 : 2));
}

/* ---- The menus' sprites: the catalogue ----
 * A sprite is a native quads object (DrawNativeQuads, func_00300AA8) of the game's GUI data, which
 * stays loaded from level to level (g_resident_data_base). Each entry: where the object's pointer
 * is, from that base, and the native quads' view scale the game draws it at (0: as it is then). */
typedef struct sprite_entry {
    const char* name;
    uint32_t at;     /* from the base: the object (direct) or where its pointer is */
    int pointer;     /* 1: `at` holds a pointer to the object */
    float view_scale;
    uint32_t colour; /* the game's own (a GS word: alpha 0 to 128), for an element's rgba 0 */
} sprite_entry;

/* The places were read with OPENRAC_GAME_UI_LOG=sprites (GuiSpriteElementDraw's objects, the probe's
 * `+offset`) in Oozla and Maktar, NTSC-U v1.01; the help box's frame pieces are text_box_draw_frame's.
 * Same order as OPENRAC_GUI_SPRITE_* (openrac_mod.h). */
static const sprite_entry k_sprites[] = {
    {NULL, 0, 0, 0.0f, 0},
    {"frame_end", 0x36F28u + 0x220Cu, 1, 0.62f, 0x75F0C070u},    /* FRAME_END */
    {"frame_corner", 0x36F28u + 0x2210u, 1, 0.62f, 0x75F0C070u}, /* FRAME_CORNER */
    {"pause_plate_fill", 0x1C040u, 0, 0.0f, 0x60442D00u},        /* PAUSE_PLATE_FILL */
    {"pause_plate_frame", 0x1C100u, 0, 0.0f, 0x55F0C070u},       /* PAUSE_PLATE_FRAME */
    {"hud_left_fill", 0x8F30u, 0, 0.0f, 0x60442D00u},            /* HUD_LEFT_FILL */
    {"hud_left_frame", 0x90F0u, 0, 0.0f, 0x55F0C070u},           /* HUD_LEFT_FRAME */
    {"hud_right_fill", 0x93C0u, 0, 0.0f, 0x60442D00u},           /* HUD_RIGHT_FILL */
    {"hud_right_frame", 0x9590u, 0, 0.0f, 0x55F0C070u},          /* HUD_RIGHT_FRAME */
    {"top_plate_fill", 0xC1E0u, 0, 0.0f, 0x60442D00u},           /* TOP_PLATE_FILL */
    {"top_plate_frame", 0xC670u, 0, 0.0f, 0x55F0C070u},          /* TOP_PLATE_FRAME */
    {"corner_fill", 0xDD50u, 0, 0.0f, 0x80EFDFC3u},              /* CORNER_FILL */
    {"corner_frame", 0xDC50u, 0, 0.0f, 0x55F0C070u},             /* CORNER_FRAME */
    {"vendor_list_fill", 0xDE10u, 0, 0.0f, 0x60442D00u},         /* VENDOR_LIST_FILL */
    {"vendor_list_frame", 0xE170u, 0, 0.0f, 0x55F0C070u},        /* VENDOR_LIST_FRAME */
    {"vendor_list_inner", 0xE760u, 0, 0.0f, 0x55F0C070u},        /* VENDOR_LIST_INNER */
    {"vendor_arrows", 0xF5C0u, 0, 0.0f, 0x80FFDE8Du},            /* VENDOR_ARROWS */
    {"vendor_cursor", 0xF930u, 0, 0.0f, 0x70FFFEEDu},            /* VENDOR_CURSOR */
    {"bracket_end", 0x191D0u, 0, 0.0f, 0x60F0C070u},             /* BRACKET_END */
    {"bracket_line", 0x19410u, 0, 0.0f, 0x60F0C070u},            /* BRACKET_LINE */
};
#define SPRITES ((int)(sizeof k_sprites / sizeof k_sprites[0]))

/* A native quads object's header (NativeDrawHeaderView): its quad and texture counts and its vertex
 * and quad tables, sane. */
static int object_looks_right(gaddr obj) {
    if (obj == 0 || !in_ram(obj) || (obj & 3u) != 0) {
        return 0;
    }
    const int quads = GREF(int16_t, obj + 2u), textures = GREF(int16_t, obj + 6u);
    return quads > 0 && quads < 4096 && textures >= 0 && textures <= 16 && in_ram(GREF(gaddr, obj + 16u))
           && in_ram(GREF(gaddr, obj + 20u));
}

static gaddr sprite_object(int id, float* view) {
    const gaddr base = GREF(gaddr, RESIDENT);
    if (base == 0 || !in_ram(base)) {
        return 0;
    }
    /* The probe's own (OPENRAC_GAME_UI_LOG=sprites, a test): 0x10000 + an object's place from the
     * base, as the probe logs it, drawn as it is. */
    if (id >= 0x10000 && id < 0x10000 + 0x40000 && ui_log() == 2) {
        *view = 0.0f;
        return object_looks_right(base + (gaddr)(id - 0x10000)) ? base + (gaddr)(id - 0x10000) : 0;
    }
    if (id <= 0 || id >= SPRITES) {
        return 0;
    }
    const gaddr obj = k_sprites[id].pointer ? GREF(gaddr, base + k_sprites[id].at) : base + k_sprites[id].at;
    *view = k_sprites[id].view_scale;
    return object_looks_right(obj) ? obj : 0;
}

static void draw_sprite(const openrac_game_gui_element* e) {
    float view = 0.0f;
    const gaddr obj = sprite_object(e->id, &view);
    if (obj == 0) {
        return;
    }
    const uint32_t own = e->id > 0 && e->id < SPRITES ? k_sprites[e->id].colour : 0x80808080u;
    const uint32_t colour = e->rgba == 0 ? own : gs_colour(e->rgba);
    float saved = 0.0f;
    if (view != 0.0f) {
        saved = GREF(float, VIEW_SCALE);
        GREF(float, VIEW_SCALE) = view;
        func_0027AB40();
    }
    func_00300A00(obj, (int)colour, e->x, e->y, e->scale_x, e->scale_y * GREF(float, MENU_SCALE), e->frame);
    if (view != 0.0f) {
        GREF(float, VIEW_SCALE) = saved;
        func_0027AB40();
    }
}

/* ---- The HUD's icons ---- */
static int item_icon_key(int item) {
    if (item < 0 || item >= ITEMS) {
        return -1;
    }
    const unsigned record = GREF(uint8_t, RECORD_OF + (gaddr)item);
    if (record >= RECORDS_COUNT) {
        return -1;
    }
    return GREF(uint16_t, RECORDS + record * RECORD_SIZE + RECORD_ICON);
}

static void draw_icon(const openrac_game_gui_element* e) {
    int tex = e->id;
    if (e->kind == OPENRAC_GUI_ITEM_ICON) {
        const int key = item_icon_key(e->id);
        tex = key > 0 ? func_0028E588(key, (int)e->frame) : 0;
    } else if (e->kind == OPENRAC_GUI_ICON_KEY) {
        tex = func_0028E588(e->id, (int)e->frame);
    }
    if (ui_log() && seen_once(0x20000000u | ((gaddr)e->kind << 24) | ((gaddr)e->id & 0xFFFFFFu))) {
        fprintf(stderr, "[ui] icon of kind %u id %d: key %d, HUD texture %d\n", (unsigned)e->kind, e->id,
                e->kind == OPENRAC_GUI_ITEM_ICON ? item_icon_key(e->id) : e->id, tex);
    }
    if (tex <= 0) {
        return;
    }
    const gaddr colours = scratch() + SCRATCH_COLOURS;
    const uint32_t colour = gs_colour(e->rgba == 0 ? 0xFF808080u : e->rgba);
    for (int i = 0; i < 4; ++i) {
        GREF(uint32_t, colours + 4u * (gaddr)i) = colour;
    }
    func_0028EA58(tex, (int)floorf(e->x), (int)floorf(e->y), (int)floorf(e->w), (int)floorf(e->h), colours);
}

/* ---- The help boxes ---- */
#define BOXES 8
typedef struct help_box {
    int used;
    int id;
    int state; /* 0 waiting, 3 to 7 as the game's */
    int timer;
    int y;
    int half_w;
    int half_h;
    int box_hw;
    int box_hh;
    int ticks;
    uint32_t flags;
    int closing;
    char text[256];
} help_box;

static help_box s_boxes[BOXES];
static int s_box_count;
static int s_next_id;
static uint32_t s_box_loads;

static help_box* box_of(int id) {
    for (int i = 0; i < BOXES; ++i) {
        if (s_boxes[i].used && s_boxes[i].id == id) {
            return &s_boxes[i];
        }
    }
    return NULL;
}

static void box_free(help_box* b) {
    if (ui_log()) {
        fprintf(stderr, "[ui] help box %d gone\n", b->id);
    }
    memset(b, 0, sizeof *b);
    --s_box_count;
}

int rac2_help_box_show(const openrac_game_help_box* box) {
    if (box == NULL) {
        return -1;
    }
    if (s_box_count == 0) {
        s_box_loads = rac2_program_loads();
    }
    for (int i = 0; i < BOXES; ++i) {
        help_box* b = &s_boxes[i];
        if (!b->used) {
            memset(b, 0, sizeof *b);
            b->used = 1;
            b->id = s_next_id++;
            b->y = box->y;
            b->ticks = box->ticks;
            b->flags = box->flags;
            memcpy(b->text, box->text, sizeof b->text);
            b->text[sizeof b->text - 1] = '\0';
            ++s_box_count;
            if (ui_log()) {
                fprintf(stderr, "[ui] help box %d queued (y %d, ticks %d, flags %u): %s\n", b->id, b->y, b->ticks,
                        (unsigned)b->flags, b->text);
            }
            return b->id;
        }
    }
    return -1;
}

int rac2_help_box_close(int id) {
    help_box* b = box_of(id);
    if (b == NULL) {
        return -1;
    }
    switch (b->state) {
        case 0:
            box_free(b);
            break;
        case 3:
            b->state = 7;
            b->timer = 0;
            break;
        case 4:
            b->state = 6;
            b->timer = 4 - b->timer;
            break;
        case 5:
            b->state = 6;
            b->timer = 0;
            break;
        default:
            break;
    }
    return 0;
}

int rac2_help_box_state(int id) {
    const help_box* b = box_of(id);
    return b == NULL ? OPENRAC_HELP_GONE : b->state == 0 ? OPENRAC_HELP_WAITING : OPENRAC_HELP_SHOWN;
}

/* The box's size as subtitle_init_state measures it, and its centre y. */
static void box_measure(help_box* b) {
    const gaddr layout = scratch() + SCRATCH_LAYOUT;
    const gaddr text = guest_text(b->text);
    func_002802E0(layout, 0xF0, 0x1E0, 0x2C, 0x1D4, 0x100, 0x168, 0x10, 7);
    func_00280200(layout, 0x80FFA888ull, text, -1);
    b->half_w = ((int)GREF(int16_t, layout + 12u) >> 1) + 10;
    b->half_h = ((int)GREF(int16_t, layout + 14u) >> 1) + 5;
    const int sh = GREF(int, SCREEN_HEIGHT);
    if (b->y < 0) {
        b->y = sh - 60;
        if (sh - 12 < b->y + b->half_h) {
            b->y = sh - (b->half_h + 12);
        }
    } else {
        if (b->y < b->half_h + 4) {
            b->y = b->half_h + 4;
        }
        if (b->y > sh - b->half_h - 4) {
            b->y = sh - b->half_h - 4;
        }
    }
    if (b->ticks < 0) {
        b->ticks = (int)strlen(b->text) * 7;
    }
}

static int overlaps(int y0, int h0, int y1, int h1) {
    return abs(y0 - y1) < h0 + h1 + 4;
}

/* Whether nothing up (the game's box or another of the mods') takes the place the box would. */
static int place_free(const help_box* b) {
    if (GREF(int32_t, SUB_STATE) != 0
        && overlaps(b->y, b->half_h, GREF(int32_t, SUB_STATE + 0x14u), GREF(int32_t, SUB_STATE + 0x0Cu))) {
        return 0;
    }
    for (int i = 0; i < BOXES; ++i) {
        const help_box* o = &s_boxes[i];
        if (o != b && o->used && o->state != 0 && overlaps(b->y, b->half_h, o->y, o->half_h)) {
            return 0;
        }
    }
    return 1;
}

static void box_open(help_box* b) {
    b->state = 3;
    b->timer = 0;
    b->box_hw = 8;
    b->box_hh = 8;
    if ((b->flags & OPENRAC_HELP_SILENT) == 0) {
        func_002E5E58(0, 1, 0);
    }
    if (ui_log()) {
        fprintf(stderr, "[ui] help box %d opens at y %d, %d x %d, text up %d ticks%s\n", b->id, b->y, b->half_w * 2,
                b->half_h * 2, b->ticks, (b->flags & OPENRAC_HELP_SILENT) ? "" : ", global sound 0");
    }
}

/* One tick of a box, as subtitle_update_state_machine steps the game's. */
static void box_step(help_box* b, int triangle) {
    b->timer++;
    switch (b->state) {
        case 3:
            if (triangle) {
                b->state = 7;
                b->timer = 0;
            } else if (b->timer >= 8) {
                b->state = 4;
                b->timer = 0;
            }
            break;
        case 4:
            if (triangle) {
                b->state = 6;
                b->timer = 4 - b->timer;
            } else if (b->timer >= 4) {
                b->state = 5;
                b->timer = 0;
            }
            break;
        case 5:
            if (triangle || b->timer >= b->ticks) {
                b->state = 6;
                b->timer = 0;
            }
            break;
        case 6:
            if (b->timer >= 4 || triangle) {
                b->state = 7;
                b->timer = 0;
            }
            break;
        case 7:
            if (b->timer >= 8) {
                box_free(b);
            }
            break;
        default:
            break;
    }
}

static void boxes_tick(void) {
    if (s_box_count == 0) {
        return;
    }
    if (rac2_program_loads() != s_box_loads) {
        for (int i = 0; i < BOXES; ++i) {
            if (s_boxes[i].used) {
                box_free(&s_boxes[i]);
            }
        }
        return;
    }
    const int mode = GREF(int32_t, GAME_MODE);
    if (mode != 0 && mode != 7) {
        return;
    }
    const int triangle = (GREF(uint32_t, PAD_HELD) & TRIANGLE) != 0;
    /* In the order they were asked for: the first waiting at a place opens first. */
    for (int k = 0; k < BOXES; ++k) {
        help_box* first = NULL;
        for (int i = 0; i < BOXES; ++i) {
            help_box* b = &s_boxes[i];
            if (b->used && (first == NULL || b->id < first->id) && b->id >= 0 && !b->closing) {
                first = b;
            }
        }
        if (first == NULL) {
            break;
        }
        first->closing = 1; /* visited this tick */
        if (first->state == 0) {
            box_measure(first);
            if (place_free(first)) {
                box_open(first);
            } else if (first->y >= 0 && first->ticks >= 0) {
                /* Measured again next time, at the place asked for. */
            }
        } else {
            box_step(first, triangle && (first->flags & OPENRAC_HELP_KEEP) == 0);
        }
    }
    for (int i = 0; i < BOXES; ++i) {
        s_boxes[i].closing = 0;
    }
}

/* A box as DrawSubtitleBox draws the game's, at the box's own y. */
static void box_draw(help_box* b) {
    int p, hw, hh, alpha = BOX_FILL_ALPHA;
    switch (b->state) {
        case 3:
            p = b->timer > 8 ? 8 : b->timer < 0 ? 0 : b->timer;
            hw = 32 + (b->half_w - 32) * p / 8;
            hh = 32 + (b->half_h - 32) * p / 8;
            b->box_hw = hw;
            b->box_hh = hh;
            break;
        case 4:
        case 5:
        case 6:
            hw = b->box_hw = b->half_w;
            hh = b->box_hh = b->half_h;
            break;
        case 7:
            p = b->timer > 8 ? 8 : b->timer < 0 ? 0 : b->timer;
            hw = b->box_hw - (b->box_hw - 8) * p / 8;
            hh = b->box_hh - (b->box_hh - 8) * p / 8;
            alpha = (8 - p) * 12;
            break;
        default:
            return;
    }
    box_look(b->y - hh, b->y + hh, 256 - hw, 256 + hw, alpha, BOX_FILL, alpha, BOX_FRAME, 3);
    if (b->state >= 4 && b->state <= 6) {
        p = b->timer > 4 ? 4 : b->timer < 0 ? 0 : b->timer;
        int colour = (int)0x80FFA888u;
        if (b->state == 4) {
            colour = (int)(((uint32_t)p << 29) | 0xFFA888u);
        } else if (b->state == 6) {
            colour = (int)(((uint32_t)(4 - p) << 29) | 0xFFA888u);
        }
        const gaddr layout = scratch() + SCRATCH_LAYOUT;
        const gaddr text = guest_text(b->text);
        func_002802E0(layout, b->y - 120, b->y + 120, 44, 468, 256, b->y, 16, 3);
        func_00280200(layout, (unsigned long long)(uint32_t)colour, text, -1);
    }
}

/* ---- The sounds ---- */
int rac2_game_sound_play(int bank, int index, float volume) {
    gaddr def = 0;
    if (index < 0 || !(volume >= 0.0f)) {
        return -1;
    }
    if (bank < 0) {
        const gaddr table = GREF(gaddr, GLOBAL_SOUNDS);
        if (table == 0 || index >= GREF(int32_t, GLOBAL_SOUND_COUNT)) {
            return -1;
        }
        def = table + (gaddr)index * 32u;
    } else {
        if (bank >= CLASS_MAX) {
            return -1;
        }
        const unsigned slot = GREF(uint8_t, CLASS_SLOT + (gaddr)bank);
        if (slot >= CLASS_SLOTS) {
            return -1;
        }
        const gaddr header = GREF(gaddr, CLASS_OBJECTS + slot * 4u);
        if (header == 0 || !in_ram(header)) {
            return -1;
        }
        const gaddr entries = GREF(gaddr, header + 40u);
        if (entries == 0 || index >= (int)GREF(uint8_t, header + 13u)) {
            return -1;
        }
        def = entries + (gaddr)index * 32u;
    }
    int loud = (int)lroundf(volume * 1024.0f);
    loud = loud > 2048 ? 2048 : loud;
    const int voice = func_002E5AD0(def, 0, 0, 0, loud);
    if (voice >= 0) {
        GREF(int32_t, VOICES + (gaddr)voice * VOICE_BYTES + 136u) = 0;
        GREF(int16_t, VOICES + (gaddr)voice * VOICE_BYTES + 126u) = (int16_t)index;
    }
    if (ui_log()) {
        fprintf(stderr, "[ui] sound bank %d entry %d volume %.2f: voice %d\n", bank, index, (double)volume, voice);
    }
    return voice;
}

/* ---- The draw ---- */
#define ELEMENTS 256
static openrac_game_gui_element s_elements[ELEMENTS];

/* The window draws a record (a text's glyph, a flat box) where the display list's tag at the cursor
 * it was recorded at is reached. With nothing written there after it, the frame's DoGifPaging puts
 * its jump to the texture uploads at that very place, which the paging's first tag reaches first:
 * the record would be drawn before the sprites ahead of it in the list. An empty DMA tag (cnt, no
 * quadwords; VIF NOPs) written at the cursor keeps the records at their place in the list. */
static void list_hold(void) {
    const gaddr at = GREF(gaddr, RAC2_LIST_CURSOR);
    if (openrac_frame_ram_cursor(at) == UINT32_MAX) {
        return;
    }
    const gaddr ram = openrac_guest_ram_address(at);
    GREF(uint64_t, ram) = 0x10000000ull;
    GREF(uint64_t, ram + 8u) = 0;
    GREF(gaddr, RAC2_LIST_CURSOR) = at + 16u;
}
static unsigned s_draws;

static void draw_elements(void) {
    int n = openrac_game_gui_elements(s_elements, ELEMENTS);
    n = n < ELEMENTS ? n : ELEMENTS;
    if (n <= 0 && s_box_count == 0) {
        return;
    }
    /* The game's box drew its records at the cursor: they stay where they are (list_hold). */
    list_hold();
    for (int i = 0; i < n; ++i) {
        const openrac_game_gui_element* e = &s_elements[i];
        rac2_anchor();
        switch (e->kind) {
            case OPENRAC_GUI_TEXT:
                draw_text(e);
                break;
            case OPENRAC_GUI_BOX:
                draw_box(e);
                break;
            case OPENRAC_GUI_SPRITE:
                draw_sprite(e);
                break;
            case OPENRAC_GUI_ICON:
            case OPENRAC_GUI_ITEM_ICON:
            case OPENRAC_GUI_ICON_KEY:
                draw_icon(e);
                break;
            default:
                break;
        }
        /* What this element recorded stays here in the list, after what came before it. */
        list_hold();
    }
    for (int i = 0; i < BOXES; ++i) {
        if (s_boxes[i].used && s_boxes[i].state != 0) {
            rac2_anchor();
            box_draw(&s_boxes[i]);
            list_hold();
        }
    }
    if (ui_log() && (s_draws++ % 120u) == 0u) {
        fprintf(stderr, "[ui] drawn: %d elements, %d help boxes (view scale %.3f, menu scale %.3f)\n", n, s_box_count,
                (double)GREF(float, VIEW_SCALE), (double)GREF(float, MENU_SCALE));
    }
}

void func_00289AA8(void) {
    func_00289AA8__game(); /* DrawSubtitleBox */
    draw_elements();
}

void rac2_game_ui_tick(void) {
    boxes_tick();
}

/* ---- The probe (OPENRAC_GAME_UI_LOG=sprites) ---- */
#define SEEN 512
static gaddr s_seen[SEEN];
static int s_seen_count;

static int seen_once(gaddr key) {
    for (int i = 0; i < s_seen_count; ++i) {
        if (s_seen[i] == key) {
            return 0;
        }
    }
    if (s_seen_count < SEEN) {
        s_seen[s_seen_count++] = key;
    }
    return 1;
}

/* Each object the first time, and every 2 seconds every element of one frame (a dump), with what
 * GuiSpriteElementDraw passes to func_00300A00. */
void rac2_game_ui_probe_sprite(gaddr element) {
    if (ui_log() < 2) {
        return;
    }
    const gaddr obj = (gaddr)GREF(int32_t, element + 64u);
    if (obj == 0 || GREF(float, (gaddr)GREF(int32_t, element + 16u)) == 0.0f) {
        return;
    }
    static clock_t dump_start;
    static unsigned dump_number;
    const clock_t now = clock();
    int dump = 0;
    if (now - dump_start > 2 * CLOCKS_PER_SEC) {
        dump_start = now;
        ++dump_number;
        dump = 1;
    } else if (now - dump_start < CLOCKS_PER_SEC / 200) {
        dump = 1;
    }
    const int first = seen_once(obj);
    if (!first && !dump) {
        return;
    }
    const gaddr place = (gaddr)GREF(int32_t, element);
    const gaddr scale = (gaddr)GREF(int32_t, element + 4u);
    const gaddr base = GREF(gaddr, RESIDENT);
    fprintf(stderr,
            "[ui] %s%u sprite +%x element %08x at %.2f %.2f scale %.3f %.3f frame %.4f colour %08x level %d view %.3f\n",
            first ? "first " : "dump ", dump_number, (unsigned)(obj - base), (unsigned)element,
            (double)GREF(float, place), (double)GREF(float, place + 4u), (double)GREF(float, scale),
            (double)(GREF(float, scale + 4u) * GREF(float, MENU_SCALE)),
            (double)GREF(float, (gaddr)GREF(int32_t, element + 0x38u)),
            (unsigned)GREF(uint32_t, (gaddr)GREF(int32_t, element + 12u)), openrac_game_loaded_overlay(),
            (double)GREF(float, VIEW_SCALE));
}

void rac2_game_ui_probe_icon(int key, int index, int tex) {
    if (ui_log() < 2 || !seen_once(0x80000000u | ((gaddr)key << 8) | (gaddr)(index & 0xFF))) {
        return;
    }
    fprintf(stderr, "[ui] icon key %d frame %d: HUD texture %d\n", key, index, tex);
}

void rac2_game_ui_probe_hud(int tex, int x, int y, int w, int h) {
    if (ui_log() < 2 || !seen_once(0x40000000u | (gaddr)tex)) {
        return;
    }
    fprintf(stderr, "[ui] HUD texture %d at %d %d size %d %d\n", tex, x, y, w, h);
}

#else /* another release: not written for it yet */

int rac2_help_box_show(const openrac_game_help_box* box) {
    (void)box;
    return -1;
}
int rac2_help_box_close(int id) {
    (void)id;
    return -1;
}
int rac2_help_box_state(int id) {
    (void)id;
    return 0;
}
float rac2_ui_text_width(const char* text, uint32_t flags, float scale) {
    (void)text;
    (void)flags;
    (void)scale;
    return -1.0f;
}
int rac2_game_sound_play(int bank, int index, float volume) {
    (void)bank;
    (void)index;
    (void)volume;
    return -1;
}
void rac2_game_ui_tick(void) {
}
void rac2_game_ui_probe_sprite(gaddr element) {
    (void)element;
}
void rac2_game_ui_probe_icon(int key, int index, int tex) {
    (void)key;
    (void)index;
    (void)tex;
}
void rac2_game_ui_probe_hud(int tex, int x, int y, int w, int h) {
    (void)tex;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
}

#endif
