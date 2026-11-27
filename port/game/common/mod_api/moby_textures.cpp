// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the OpenRAC contributors
//
// The mods' interface, OpenRAC Commando's 1.120: a moby drawn with its class's own model, skin and
// animation, but with pictures of the mod's in place of some of its textures (openrac_api
// moby_textures_at, moby_textures_class). Writes no game memory: the front end reads the requests
// (mods::moby_textures_drawn), makes one model per class and set of pictures (a copy of the class
// model whose materials use the pictures: frontend/moby_textures.cpp) and draws the mobys with it.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "common/log.h"
#include "mod_api/internal.h"
#include "mods.h"

using namespace openrac;

namespace openrac::mods::detail {
namespace {

// The most mobys one mod draws with textures of its own at once (moby_textures_at).
constexpr std::size_t kMobysPerMod = 4096;

struct TexturesEntry {
    const openrac_mod* mod = nullptr;
    std::uint32_t loads = 0;
    int oclass = -1;
    std::string key;
    std::vector<MobyTextureSwap> swaps;
};

std::mutex g_lock;
std::map<std::uint32_t, TexturesEntry> g_by_address;
std::map<std::pair<int, std::uint32_t>, TexturesEntry> g_by_class;

// The swaps `t` asks for, read and checked: false for a request the interface does not describe.
bool read_swaps(openrac_mod* mod, const openrac_moby_textures* t, std::vector<MobyTextureSwap>& out) {
    if (t == nullptr || t->size < sizeof(openrac_moby_textures) || t->count > OPENRAC_MOBY_TEXTURES_MAX) {
        return false;
    }
    out.clear();
    for (std::uint32_t i = 0; i < t->count; ++i) {
        const openrac_moby_texture& e = t->textures[i];
        const bool named = e.name != nullptr && e.name[0] != 0;
        if (e.png == nullptr || (e.index < 0 && !named)) {
            return false;
        }
        const std::filesystem::path png = mod_file(mod, e.png);
        if (png.empty() || png.extension() != ".png") {
            log::warn(
                "mods: {}: moby texture {} is not a .png in the mod's folder", mod_id(mod), e.png
            );
            return false;
        }
        MobyTextureSwap s;
        s.index = e.index >= 0 ? e.index : -1;
        s.name = e.index < 0 ? std::string(e.name) : std::string();
        s.png = png;
        out.push_back(std::move(s));
    }
    return true;
}

// The model key: the class and every swap, so equal requests share one model.
std::string key_of(int oclass, const std::vector<MobyTextureSwap>& swaps) {
    std::string key = std::to_string(oclass);
    for (const MobyTextureSwap& s : swaps) {
        key += '|';
        key += s.index >= 0 ? std::to_string(s.index) : "n:" + s.name;
        key += '=';
        key += s.png.generic_string();
    }
    return key;
}

}  // namespace

int api_moby_textures_at(openrac_mod* mod, std::uint32_t address, const openrac_moby_textures* t) {
    std::int32_t level = -1;
    if (mod == nullptr || address == 0 || g_play == nullptr || g_play->moby_class_at == nullptr
        || !in_level(phase_now(&level))) {
        return OPENRAC_HOOK_UNKNOWN;
    }
    std::vector<MobyTextureSwap> swaps;
    if (!read_swaps(mod, t, swaps)) {
        return OPENRAC_HOOK_UNKNOWN;
    }
    const std::int32_t oclass = g_play->moby_class_at(address);
    if (oclass < 0) {
        return OPENRAC_HOOK_UNKNOWN;
    }
    const std::uint32_t loads = loads_now();
    const std::lock_guard<std::mutex> lock(g_lock);
    std::erase_if(g_by_address, [&](const auto& e) { return e.second.loads != loads; });
    if (swaps.empty()) {
        g_by_address.erase(address);
        return OPENRAC_HOOK_OK;
    }
    const auto held = g_by_address.find(address);
    if (held == g_by_address.end() || held->second.mod != mod) {
        const auto own = std::count_if(g_by_address.begin(), g_by_address.end(), [&](const auto& e) {
            return e.second.mod == mod;
        });
        if (static_cast<std::size_t>(own) >= kMobysPerMod) {
            return OPENRAC_HOOK_FULL;
        }
    }
    TexturesEntry& e = g_by_address[address];
    e.mod = mod;
    e.loads = loads;
    e.oclass = oclass;
    e.key = key_of(oclass, swaps);
    e.swaps = std::move(swaps);
    return OPENRAC_HOOK_OK;
}

int api_moby_textures_class(
    openrac_mod* mod, int oclass, std::uint32_t scope, const openrac_moby_textures* t
) {
    constexpr std::uint32_t kScopes = OPENRAC_MODEL_WORLD | OPENRAC_MODEL_MENU;
    std::int32_t level = -1;
    if (mod == nullptr || oclass < 0 || scope == 0 || (scope & ~kScopes) != 0
        || !in_level(phase_now(&level))) {
        return OPENRAC_HOOK_UNKNOWN;
    }
    std::vector<MobyTextureSwap> swaps;
    if (!read_swaps(mod, t, swaps)) {
        return OPENRAC_HOOK_UNKNOWN;
    }
    const std::uint32_t loads = loads_now();
    const std::lock_guard<std::mutex> lock(g_lock);
    std::erase_if(g_by_class, [&](const auto& e) { return e.second.loads != loads; });
    for (const std::uint32_t bit : {OPENRAC_MODEL_WORLD, OPENRAC_MODEL_MENU}) {
        if ((scope & bit) == 0) {
            continue;
        }
        if (swaps.empty()) {
            g_by_class.erase({oclass, bit});
            continue;
        }
        TexturesEntry& e = g_by_class[{oclass, bit}];
        e.mod = mod;
        e.loads = loads;
        e.oclass = oclass;
        e.key = key_of(oclass, swaps);
        e.swaps = swaps;
    }
    return OPENRAC_HOOK_OK;
}

void moby_textures_forget(const openrac_mod* mod) {
    const std::lock_guard<std::mutex> lock(g_lock);
    if (mod == nullptr) {
        g_by_address.clear();
        g_by_class.clear();
        return;
    }
    std::erase_if(g_by_address, [&](const auto& e) { return e.second.mod == mod; });
    std::erase_if(g_by_class, [&](const auto& e) { return e.second.mod == mod; });
}

}  // namespace openrac::mods::detail

namespace openrac::mods {

std::vector<MobyTexturesDraw> moby_textures_drawn() {
    using namespace detail;
    std::vector<MobyTexturesDraw> out;
    const std::uint32_t loads = loads_now();
    const std::lock_guard<std::mutex> lock(g_lock);
    out.reserve(g_by_address.size() + g_by_class.size());
    for (const auto& [address, e] : g_by_address) {
        if (e.loads == loads) {
            out.push_back({address, e.oclass, 0, e.key, e.swaps});
        }
    }
    for (const auto& [key, e] : g_by_class) {
        if (e.loads == loads) {
            out.push_back({0, key.first, key.second, e.key, e.swaps});
        }
    }
    return out;
}

}  // namespace openrac::mods
