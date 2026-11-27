// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the OpenRAC contributors
//
// The mods' interface, OpenRAC Commando's 1.121: a live moby moved to another place of its level
// with what goes with it (openrac_api.moby_relocate; the game's part: rac2/host/moby_relocate.c).

#include <cmath>
#include <cstdint>

#include "mod_api/internal.h"

namespace openrac::mods::detail {

int api_moby_relocate(openrac_mod* mod, std::int32_t slot, float position[3], std::uint32_t flags) {
    constexpr std::uint32_t kFlags = OPENRAC_RELOCATE_HOMES | OPENRAC_RELOCATE_FREE
                                     | OPENRAC_RELOCATE_WAKE | OPENRAC_RELOCATE_ANYWHERE
                                     | OPENRAC_RELOCATE_NEAREST;
    std::int32_t level = -1;
    if (mod == nullptr || position == nullptr || slot < 0 || (flags & ~kFlags) != 0
        || g_play == nullptr || g_play->moby_relocate == nullptr || !in_level(phase_now(&level))
        || !std::isfinite(position[0]) || !std::isfinite(position[1])
        || !std::isfinite(position[2])) {
        return OPENRAC_HOOK_UNKNOWN;
    }
    const int r = g_play->moby_relocate(slot, position, flags);
    return r == 0 ? OPENRAC_HOOK_OK : r == -2 ? OPENRAC_RELOCATE_NO_FLOOR : OPENRAC_HOOK_UNKNOWN;
}

}  // namespace openrac::mods::detail
