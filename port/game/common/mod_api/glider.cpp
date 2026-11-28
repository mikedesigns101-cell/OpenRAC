// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the OpenRAC contributors
//
// The mods' interface, OpenRAC Commando's 1.127: the hero's seqs of another level
// (openrac_api.hero_seq_import) and the game's Glider in any level (glider_pad_add,
// glider_pad_remove, on_glider). The game's part: rac2/host/hero_seqs.c and glider.c, through
// openrac_game_play.hero_seq_import and glider_*. A pad is the port's from the call on (each load
// of its level makes it); each frame, before the mods' frame callbacks, the game's glider events go
// to the callbacks of the mod whose pad it is.

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include "mod_api/internal.h"
#include "mods.h"

namespace openrac::mods {

namespace {

struct GliderPad {
    openrac_mod* mod = nullptr;
    int id = -1;  // the game's
};

struct GliderCallback {
    openrac_glider_fn fn = nullptr;
    void* user = nullptr;
    openrac_mod* mod = nullptr;
};

std::vector<GliderPad> g_glider_pads;
std::vector<GliderCallback> g_glider_callbacks;

}  // namespace

void glider_frame() {
    if (g_glider_pads.empty() || detail::g_play == nullptr || detail::g_play->glider_events == nullptr) {
        return;
    }
    std::array<openrac_game_glider_event, 16> events{};
    const int n = detail::g_play->glider_events(events.data(), static_cast<int>(events.size()));
    for (int i = 0; i < n; ++i) {
        const openrac_game_glider_event& e = events[static_cast<std::size_t>(i)];
        const auto pad = std::find_if(g_glider_pads.begin(), g_glider_pads.end(),
            [&e](const GliderPad& p) { return p.id == e.pad; });
        if (pad == g_glider_pads.end()) {
            continue;
        }
        openrac_glider_event out{};
        out.size = sizeof out;
        out.kind = e.kind;
        out.pad = e.pad;
        std::copy(e.position, e.position + 3, out.position);
        for (const GliderCallback& c : g_glider_callbacks) {
            if (c.mod == pad->mod) {
                c.fn(c.user, &out);
            }
        }
    }
}

void glider_reset() {
    if (detail::g_play != nullptr && detail::g_play->glider_pad_remove != nullptr) {
        for (const GliderPad& p : g_glider_pads) {
            detail::g_play->glider_pad_remove(p.id);
        }
    }
    g_glider_pads.clear();
    g_glider_callbacks.clear();
}

namespace detail {

int api_hero_seq_import(openrac_mod* mod, int level, int seq) {
    if (mod == nullptr || g_play == nullptr || g_play->hero_seq_import == nullptr || level < 0
        || level >= g_play->levels || seq < 0 || seq > 255) {
        return OPENRAC_HOOK_UNKNOWN;
    }
    if (g_play->hero_seq_import(level, seq) != 0) {
        return OPENRAC_HOOK_UNKNOWN;
    }
    log::info("mod {}: the hero's seq {} of level {} imported where his class lacks it", mod->id, seq, level);
    return OPENRAC_HOOK_OK;
}

int api_glider_pad_add(openrac_mod* mod, const openrac_glider_pad* pad) {
    if (mod == nullptr || pad == nullptr || pad->size < sizeof(openrac_glider_pad) || g_play == nullptr
        || g_play->glider_pad_add == nullptr) {
        return OPENRAC_HOOK_UNKNOWN;
    }
    const bool finite = std::all_of(pad->position, pad->position + 3, [](float f) { return std::isfinite(f); })
                        && std::all_of(pad->landing, pad->landing + 3, [](float f) { return std::isfinite(f); })
                        && std::isfinite(pad->yaw) && std::isfinite(pad->landing_radius)
                        && std::isfinite(pad->landing_height) && pad->landing_radius >= 0.0f
                        && pad->landing_height >= 0.0f;
    if (!finite || pad->flags != 0) {
        return OPENRAC_HOOK_UNKNOWN;
    }
    openrac_game_glider_pad def{};
    def.level = pad->level;
    std::copy(pad->position, pad->position + 3, def.position);
    def.yaw = pad->yaw;
    std::copy(pad->landing, pad->landing + 3, def.landing);
    def.landing_radius = pad->landing_radius;
    def.landing_height = pad->landing_height;
    def.item = pad->item;
    const int id = g_play->glider_pad_add(&def);
    if (id < 0) {
        return OPENRAC_HOOK_UNKNOWN;
    }
    g_glider_pads.push_back({mod, id});
    log::info("mod {}: glider pad {} in level {} at {:.1f} {:.1f} {:.1f}", mod->id, id, pad->level, pad->position[0],
        pad->position[1], pad->position[2]);
    return id;
}

int api_glider_pad_remove(openrac_mod* mod, int pad) {
    const auto at = std::find_if(g_glider_pads.begin(), g_glider_pads.end(),
        [&](const GliderPad& p) { return p.mod == mod && p.id == pad; });
    if (mod == nullptr || at == g_glider_pads.end() || g_play == nullptr || g_play->glider_pad_remove == nullptr) {
        return OPENRAC_HOOK_UNKNOWN;
    }
    g_play->glider_pad_remove(pad);
    g_glider_pads.erase(at);
    return OPENRAC_HOOK_OK;
}

int api_on_glider(openrac_mod* mod, openrac_glider_fn fn, void* user) {
    if (mod == nullptr || fn == nullptr || g_play == nullptr || g_play->glider_events == nullptr) {
        return OPENRAC_HOOK_UNKNOWN;
    }
    if (mod != g_loading) {
        return OPENRAC_HOOK_TOO_LATE;
    }
    g_glider_callbacks.push_back({fn, user, mod});
    return OPENRAC_HOOK_OK;
}

}  // namespace detail

}  // namespace openrac::mods
