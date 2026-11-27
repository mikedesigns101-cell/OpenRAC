// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the OpenRAC contributors
//
// The mods' interface, area "input": the api_<member> functions of the openrac_api members
// sdk/api/members.tsv puts in this area, and what only they use.

#include "button_labels.h"
#include "mod_api/internal.h"

using namespace openrac;

namespace openrac::mods::detail {

const openrac_input* api_input(void) {
    static openrac_input none = [] {
        openrac_input i{};
        i.size = sizeof(openrac_input);
        return i;
    }();
    const openrac_input* in =
        g_host != nullptr && g_host->input != nullptr ? g_host->input() : nullptr;
    return in != nullptr ? in : &none;
}

int api_take_input(openrac_mod* mod, uint32_t devices) {
    if (mod == nullptr || mod != g_loading) {
        return OPENRAC_HOOK_TOO_LATE;
    }
    devices &= OPENRAC_INPUT_KEYBOARD | OPENRAC_INPUT_MOUSE;
    if (devices != 0 && g_host != nullptr && g_host->take_input != nullptr) {
        g_host->take_input(devices);
    }
    if ((devices & OPENRAC_INPUT_KEYBOARD) != 0) {
        log::info(
            "mod {} takes the keyboard: the port's own keys no longer press pad 0's buttons",
            mod->id
        );
    }
    return OPENRAC_HOOK_OK;
}

void api_capture_mouse(openrac_mod* mod, int capture) {
    (void)mod;
    if (g_host != nullptr && g_host->capture_mouse != nullptr) {
        g_host->capture_mouse(capture != 0);
    }
}

// Interface 1.31: a button's label for the device in use (button_labels.h).
int api_button_label(openrac_mod*, std::uint32_t button, char* out, std::uint32_t size) {
    return button_labels::label_into(button, out, size);
}

// Interface 1.37: a mod's motor values for a pad (Host::motors), played with the game's own.
int api_set_motors(openrac_mod* mod, int port, int small_motor, int large_motor) {
    // The game's two pad ports (on_pad's), libvib's small motor off or on, its large 0 to 255.
    // The game's thread only: the window's input reads the values without a lock.
    constexpr int kPorts = 2;
    if (mod == nullptr || port < 0 || port >= kPorts || (small_motor != 0 && small_motor != 1)
        || large_motor < 0 || large_motor > 255 || g_host == nullptr || g_host->motors == nullptr) {
        return OPENRAC_HOOK_UNKNOWN;
    }
    {
        // From now on the mod drives motors: the menu shows the player's vibration settings.
        const std::lock_guard<std::mutex> lock(g_motor_mods_lock);
        if (std::find(g_motor_mods.begin(), g_motor_mods.end(), mod) == g_motor_mods.end()) {
            g_motor_mods.push_back(mod);
        }
    }
    g_host->motors(mod, port, small_motor != 0, static_cast<std::uint8_t>(large_motor));
    return OPENRAC_HOOK_OK;
}

// openrac_api.pad_rumble. Since 1.107.
int api_pad_rumble(openrac_mod* mod, int port, int small_motor, int large_motor, int ticks) {
    constexpr int kPorts = 2;
    if (mod == nullptr || g_play == nullptr || g_play->pad_rumble == nullptr || port < 0
        || port >= kPorts || (small_motor != 0 && small_motor != 1) || large_motor < 0
        || large_motor > 255 || ticks < 0 || ticks > OPENRAC_PAD_RUMBLE_TICKS_MAX) {
        return OPENRAC_HOOK_UNKNOWN;
    }
    return g_play->pad_rumble(port, small_motor, large_motor, ticks) != 0 ? OPENRAC_HOOK_OK
                                                                          : OPENRAC_HOOK_UNKNOWN;
}

// The mods' mutes of the game's vibration (game_motors_mute), by mod; the game gets the items.
namespace {
std::mutex g_mutes_lock;
std::vector<std::pair<const openrac_mod*, std::int32_t>> g_mutes;

void push_mutes() {
    if (g_play == nullptr || g_play->motors_mute == nullptr) {
        return;
    }
    std::vector<std::int32_t> items;
    for (const auto& m : g_mutes) {
        items.push_back(m.second);
    }
    g_play->motors_mute(items.data(), static_cast<int>(items.size()));
}
}  // namespace

void motors_mute_forget(const openrac_mod* mod) {
    const std::lock_guard<std::mutex> lock(g_mutes_lock);
    const auto before = g_mutes.size();
    std::erase_if(g_mutes, [&](const auto& m) { return mod == nullptr || m.first == mod; });
    if (g_mutes.size() != before) {
        push_mutes();
    }
}

// openrac_api.game_motors_mute. Since 1.107.
int api_game_motors_mute(openrac_mod* mod, std::int32_t item) {
    if (mod == nullptr || g_play == nullptr || g_play->motors_mute == nullptr
        || item < OPENRAC_MOTORS_MUTE_NONE) {
        return OPENRAC_HOOK_UNKNOWN;
    }
    const std::lock_guard<std::mutex> lock(g_mutes_lock);
    if (item == OPENRAC_MOTORS_MUTE_NONE) {
        std::erase_if(g_mutes, [&](const auto& m) { return m.first == mod; });
        push_mutes();
        return OPENRAC_HOOK_OK;
    }
    const bool held = std::any_of(g_mutes.begin(), g_mutes.end(), [&](const auto& m) {
        return m.first == mod && m.second == item;
    });
    if (!held) {
        if (g_mutes.size() >= OPENRAC_MOTORS_MUTE_MAX) {
            return OPENRAC_HOOK_FULL;
        }
        g_mutes.emplace_back(mod, item);
    }
    push_mutes();
    return OPENRAC_HOOK_OK;
}

}  // namespace openrac::mods::detail
