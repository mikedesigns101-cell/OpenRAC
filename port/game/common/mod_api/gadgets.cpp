// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the OpenRAC contributors
//
// The mods' interface, OpenRAC Commando's 1.124: a mod's own gadgets as items of the game's
// (openrac_api.gadget_register and the rest; the game's part: rac2/host/mod_gadgets.c, through
// openrac_game_play.gadget_*).
//
// A gadget is a slot of the game's (gadget_slots of them), its number here. What the hero has is
// kept with the mods' data of the save (g_save_data), under "openrac.gadgets/<mod id>": the keys of
// the gadgets he owns, one a line; mods.cpp writes it beside the save while that mod is loaded, and
// each time the game in progress takes other data (a save loaded, a new game) the owned states
// follow it. Each frame, before the mods' frame callbacks: the game's events go to the owners'
// callbacks, the model in the hand is the gadget's mesh (moby_model_at on the hand's moby), and the
// icons the game's 2D asked for during the tick are what the overlay draws on the next frames.

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "mod_api/internal.h"
#include "mods.h"

namespace openrac::mods {

namespace {

struct Gadget {
    openrac_mod* mod = nullptr;
    std::string key;
    std::string name;
    std::string description;
    std::int32_t host = 0;
    std::int32_t icon = 0;
    std::string mesh;
    std::array<float, 16> matrix{};
};

struct GadgetCallback {
    openrac_gadget_fn fn = nullptr;
    void* user = nullptr;
    openrac_mod* mod = nullptr;
};

std::vector<Gadget> g_gadgets;
std::vector<GadgetCallback> g_gadget_callbacks;
// The model put on the hand's moby: whose, where, which gadget.
openrac_mod* g_hand_mod = nullptr;
std::uint32_t g_hand_address = 0;
int g_hand_gadget = -1;

std::mutex g_icons_lock;
std::vector<GadgetIcon> g_icons_building;
std::vector<GadgetIcon> g_icons_shown;

constexpr std::string_view kSavePrefix = "openrac.gadgets/";

std::string save_key(const openrac_mod* mod) {
    return std::string(kSavePrefix) + mod->id;
}

// The keys a mod's gadgets the hero owns have, from the game in progress's data.
std::vector<std::string> owned_keys(const openrac_mod* mod) {
    std::vector<std::string> keys;
    const auto data = detail::g_save_data.find(save_key(mod));
    if (data == detail::g_save_data.end()) {
        return keys;
    }
    std::string line;
    for (const std::uint8_t c : data->second) {
        if (c == '\n') {
            if (!line.empty()) {
                keys.push_back(line);
            }
            line.clear();
        } else {
            line += static_cast<char>(c);
        }
    }
    if (!line.empty()) {
        keys.push_back(line);
    }
    return keys;
}

void write_owned_keys(const openrac_mod* mod, const std::vector<std::string>& keys) {
    std::vector<std::uint8_t> bytes;
    for (const std::string& k : keys) {
        bytes.insert(bytes.end(), k.begin(), k.end());
        bytes.push_back('\n');
    }
    if (bytes.empty()) {
        detail::g_save_data.erase(save_key(mod));
    } else {
        detail::g_save_data[save_key(mod)] = std::move(bytes);
    }
}

bool word(const char* key) {
    if (key == nullptr || *key == '\0' || std::strlen(key) >= OPENRAC_GADGET_KEY_MAX) {
        return false;
    }
    for (const char* c = key; *c != '\0'; ++c) {
        if (std::isalnum(static_cast<unsigned char>(*c)) == 0 && *c != '_') {
            return false;
        }
    }
    return true;
}

// ASCII only, at most the game's text slot.
std::string game_text(const char* text) {
    std::string out;
    if (text == nullptr) {
        return out;
    }
    for (const char* c = text; *c != '\0' && out.size() + 1 < OPENRAC_GADGET_TEXT_MAX; ++c) {
        const auto u = static_cast<unsigned char>(*c);
        out += u >= 0x20 && u < 0x7F ? static_cast<char>(u) : ' ';
    }
    return out;
}

bool own(const openrac_mod* mod, int gadget) {
    return mod != nullptr && gadget >= 0 && gadget < static_cast<int>(g_gadgets.size())
           && g_gadgets[static_cast<std::size_t>(gadget)].mod == mod;
}

// The game's 2D asks for a gadget's icon (rac2/host/mod_gadgets.c): 1 when it is the mod's picture
// (drawn by the overlay), 0 for the host's (the game draws it).
int gadget_icon(int slot, float x, float y, float w, float h, std::uint32_t rgba) {
    if (slot < 0 || slot >= static_cast<int>(g_gadgets.size())) {
        return 0;
    }
    const Gadget& g = g_gadgets[static_cast<std::size_t>(slot)];
    static int told = 0;
    if (told < 6) {
        ++told;
        log::info("gadget {}: icon asked at {} {} size {} {} colour {:08x}", slot, x, y, w, h, rgba);
    }
    if (g.icon <= 0) {
        return 0;
    }
    const std::lock_guard<std::mutex> lock(g_icons_lock);
    g_icons_building.push_back(GadgetIcon{g.mod, g.icon, x, y, w, h, rgba});
    return 1;
}

// The mesh in the hand: the held gadget's on the hand's moby, the last one's taken off.
void hand_model() {
    const int held = detail::g_play->gadget_held != nullptr ? detail::g_play->gadget_held() : -1;
    const std::uint32_t address = held >= 0 && detail::g_play->gadget_hand_moby != nullptr
                                      ? static_cast<std::uint32_t>(detail::g_play->gadget_hand_moby())
                                      : 0u;
    const Gadget* g = held >= 0 && held < static_cast<int>(g_gadgets.size())
                          ? &g_gadgets[static_cast<std::size_t>(held)]
                          : nullptr;
    const bool wanted = g != nullptr && !g->mesh.empty() && address != 0;
    if (g_hand_address != 0 && (!wanted || address != g_hand_address || held != g_hand_gadget)) {
        openrac_moby_model none{};
        none.size = sizeof none;
        none.model = -1;
        detail::api_moby_model_at(g_hand_mod, g_hand_address, &none);
        g_hand_address = 0;
        g_hand_mod = nullptr;
        g_hand_gadget = -1;
    }
    if (!wanted) {
        return;
    }
    const int model = detail::api_model_create(g->mod, g->mesh.c_str());
    if (model < 0) {
        return;
    }
    openrac_moby_model m{};
    m.size = sizeof m;
    m.model = model;
    std::copy(g->matrix.begin(), g->matrix.end(), m.matrix);
    const int r = detail::api_moby_model_at(g->mod, address, &m);
    if (r == OPENRAC_HOOK_OK) {
        if (g_hand_address != address || g_hand_gadget != held) {
            log::info("gadget {}: its mesh on the hand's moby {:08x}", held, address);
        }
        g_hand_address = address;
        g_hand_mod = g->mod;
        g_hand_gadget = held;
    } else {
        static int said = 0;
        if (said++ < 3) {
            log::warn("gadget {}: its mesh not put on the hand's moby {:08x}: {}", held, address, r);
        }
    }
}

}  // namespace

bool gadgets_have_icons() {
    return std::any_of(g_gadgets.begin(), g_gadgets.end(), [](const Gadget& g) { return g.icon > 0; });
}

std::vector<GadgetIcon> gadget_icons() {
    const std::lock_guard<std::mutex> lock(g_icons_lock);
    return g_icons_shown;
}

void gadgets_frame() {
    {
        const std::lock_guard<std::mutex> lock(g_icons_lock);
        g_icons_shown.swap(g_icons_building);
        g_icons_building.clear();
    }
    if (g_gadgets.empty() || detail::g_play == nullptr || detail::g_play->gadget_events == nullptr) {
        return;
    }
    std::array<openrac_game_gadget_event, 16> events{};
    const int n = detail::g_play->gadget_events(events.data(), static_cast<int>(events.size()));
    for (int i = 0; i < n; ++i) {
        const openrac_game_gadget_event& e = events[static_cast<std::size_t>(i)];
        if (e.slot < 0 || e.slot >= static_cast<int>(g_gadgets.size())) {
            continue;
        }
        openrac_gadget_event out{};
        out.size = sizeof out;
        out.gadget = e.slot;
        out.kind = e.kind;
        std::copy(e.position, e.position + 3, out.position);
        std::copy(e.direction, e.direction + 3, out.direction);
        const openrac_mod* owner = g_gadgets[static_cast<std::size_t>(e.slot)].mod;
        for (const GadgetCallback& c : g_gadget_callbacks) {
            if (c.mod == owner) {
                c.fn(c.user, &out);
            }
        }
    }
    hand_model();
}

void gadgets_save_changed() {
    if (detail::g_play == nullptr || detail::g_play->gadget_owned == nullptr) {
        return;
    }
    for (std::size_t i = 0; i < g_gadgets.size(); ++i) {
        const auto keys = owned_keys(g_gadgets[i].mod);
        const bool owned = std::find(keys.begin(), keys.end(), g_gadgets[i].key) != keys.end();
        detail::g_play->gadget_owned(static_cast<int>(i), owned ? 1 : 0);
    }
}

std::string gadgets_save_key(const openrac_mod* mod) {
    return save_key(mod);
}

void gadgets_reset() {
    if (detail::g_play != nullptr && detail::g_play->gadget_define != nullptr) {
        for (std::size_t i = 0; i < g_gadgets.size(); ++i) {
            detail::g_play->gadget_define(static_cast<int>(i), nullptr);
        }
    }
    g_gadgets.clear();
    g_gadget_callbacks.clear();
    g_hand_mod = nullptr;
    g_hand_address = 0;
    g_hand_gadget = -1;
    openrac_game_gadget_icon = nullptr;
    const std::lock_guard<std::mutex> lock(g_icons_lock);
    g_icons_building.clear();
    g_icons_shown.clear();
}

namespace detail {

int api_gadget_register(openrac_mod* mod, const openrac_gadget* gadget) {
    if (mod == nullptr || gadget == nullptr || gadget->size < sizeof(openrac_gadget)
        || !word(gadget->key) || gadget->flags != 0 || g_play == nullptr
        || g_play->gadget_define == nullptr || g_play->gadget_slots <= 0) {
        return OPENRAC_HOOK_UNKNOWN;
    }
    if (mod != g_loading) {
        return OPENRAC_HOOK_TOO_LATE;
    }
    for (const Gadget& g : g_gadgets) {
        if (g.mod == mod && g.key == gadget->key) {
            return OPENRAC_HOOK_UNKNOWN;
        }
    }
    if (static_cast<int>(g_gadgets.size()) >= g_play->gadget_slots) {
        return OPENRAC_HOOK_FULL;
    }
    if (gadget->mesh != nullptr && !std::all_of(gadget->mesh_matrix, gadget->mesh_matrix + 16, [](float f) {
            return std::isfinite(f);
        })) {
        return OPENRAC_HOOK_UNKNOWN;
    }
    Gadget g;
    g.mod = mod;
    g.key = gadget->key;
    g.name = game_text(gadget->name);
    g.description = game_text(gadget->description);
    g.host = gadget->host_item;
    g.icon = gadget->icon;
    g.mesh = gadget->mesh != nullptr ? gadget->mesh : "";
    std::copy(gadget->mesh_matrix, gadget->mesh_matrix + 16, g.matrix.begin());
    if (std::all_of(g.matrix.begin(), g.matrix.end(), [](float f) { return f == 0.0f; })) {
        g.matrix = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    }
    const int slot = static_cast<int>(g_gadgets.size());
    openrac_game_gadget define{};
    define.host_item = g.host;
    define.name = g.name.c_str();
    define.description = g.description.c_str();
    if (g_play->gadget_define(slot, &define) != 0) {
        return OPENRAC_HOOK_UNKNOWN;
    }
    g_gadgets.push_back(std::move(g));
    openrac_game_gadget_icon = gadget_icon;
    log::info("mod {}: gadget {} \"{}\" is gadget {} (item {})", mod->id, gadget->key,
        g_gadgets.back().name, slot, g_play->gadget_item != nullptr ? g_play->gadget_item(slot) : -1);
    return slot;
}

int api_gadget_give(openrac_mod* mod, int gadget, int owned) {
    if (!own(mod, gadget) || g_play == nullptr || g_play->gadget_owned == nullptr) {
        return OPENRAC_HOOK_UNKNOWN;
    }
    const Gadget& g = g_gadgets[static_cast<std::size_t>(gadget)];
    auto keys = owned_keys(mod);
    const auto at = std::find(keys.begin(), keys.end(), g.key);
    if (owned != 0 && at == keys.end()) {
        keys.push_back(g.key);
    } else if (owned == 0 && at != keys.end()) {
        keys.erase(at);
    }
    write_owned_keys(mod, keys);
    g_play->gadget_owned(gadget, owned != 0 ? 1 : 0);
    return OPENRAC_HOOK_OK;
}

int api_gadget_equip(openrac_mod* mod, int gadget) {
    std::int32_t level = -1;
    if (!own(mod, gadget) || g_play == nullptr || g_play->gadget_equip == nullptr
        || !in_level(phase_now(&level))) {
        return OPENRAC_HOOK_UNKNOWN;
    }
    return g_play->gadget_equip(gadget) == 0 ? OPENRAC_HOOK_OK : OPENRAC_HOOK_UNKNOWN;
}

int api_gadget_count() {
    return static_cast<int>(g_gadgets.size());
}

int api_gadget_info(int gadget, openrac_gadget_info* out) {
    if (out == nullptr || out->size < sizeof(openrac_gadget_info) || gadget < 0
        || gadget >= static_cast<int>(g_gadgets.size())) {
        return OPENRAC_HOOK_UNKNOWN;
    }
    const Gadget& g = g_gadgets[static_cast<std::size_t>(gadget)];
    const std::uint32_t size = out->size;
    std::memset(out, 0, sizeof *out);
    out->size = size;
    out->gadget = gadget;
    out->item = g_play != nullptr && g_play->gadget_item != nullptr ? g_play->gadget_item(gadget) : -1;
    const int owned = g_play != nullptr && g_play->gadget_owned != nullptr ? g_play->gadget_owned(gadget, -1) : 0;
    const int held = g_play != nullptr && g_play->gadget_held != nullptr ? g_play->gadget_held() : -1;
    out->flags = (owned > 0 ? OPENRAC_GADGET_OWNED : 0u) | (held == gadget ? OPENRAC_GADGET_HELD : 0u);
    std::snprintf(out->mod, sizeof out->mod, "%s", g.mod->id.c_str());
    std::snprintf(out->key, sizeof out->key, "%s", g.key.c_str());
    std::snprintf(out->name, sizeof out->name, "%s", g.name.c_str());
    std::snprintf(out->description, sizeof out->description, "%s", g.description.c_str());
    out->icon = g.icon;
    out->host_item = g.host;
    return OPENRAC_HOOK_OK;
}

int api_gadget_held() {
    if (g_gadgets.empty() || g_play == nullptr || g_play->gadget_held == nullptr) {
        return -1;
    }
    return g_play->gadget_held();
}

int api_on_gadget(openrac_mod* mod, openrac_gadget_fn fn, void* user) {
    if (mod == nullptr || fn == nullptr) {
        return OPENRAC_HOOK_UNKNOWN;
    }
    if (mod != g_loading) {
        return OPENRAC_HOOK_TOO_LATE;
    }
    g_gadget_callbacks.push_back({fn, user, mod});
    return OPENRAC_HOOK_OK;
}

}  // namespace detail

}  // namespace openrac::mods
