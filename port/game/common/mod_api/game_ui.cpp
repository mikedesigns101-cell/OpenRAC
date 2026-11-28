// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the OpenRAC contributors
//
// The mods' interface, OpenRAC Commando's 1.129: a mod's interface drawn with the game's own 2D
// (openrac_api.gui_draw, gui_text_width, help_box_show, help_box_close, help_box_state) and the
// game's loaded sounds by number (game_sound_play). The game's part: rac2/host/game_ui.c, through
// openrac_game_play.help_box_*, gui_text_width, game_sound_play and openrac_game_gui_elements, which
// the game reads where it draws its 2D. Texts are turned here from UTF-8 into the game's bytes.
// With no mod calling these, nothing here is read by the game but an empty list.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <string>
#include <vector>

#include "mod_api/internal.h"
#include "mods.h"

namespace openrac::mods {

namespace {

// What each mod draws, of a level load (set on the game's thread, read where the game draws).
struct UiSet {
    const openrac_mod* mod = nullptr;
    std::uint32_t loads = 0;
    std::vector<openrac_game_gui_element> elements;
};

std::mutex g_ui_lock;
std::vector<UiSet> g_ui;
std::atomic<int> g_ui_count{0};

// The help boxes each mod opened (the game's ids).
struct HelpOwner {
    const openrac_mod* mod = nullptr;
    int id = -1;
};
std::vector<HelpOwner> g_help;

// The game font's letters past ASCII (Going Commando's: bytes 0x80 to 0xAF, an accent drawn over
// the plain letter), by code point.
struct Letter {
    std::uint32_t cp;
    unsigned char game;
};
constexpr Letter kLetters[] = {
    {0xC1, 0x80}, {0xC0, 0x81}, {0xC2, 0x82}, {0xC4, 0x83}, {0xE1, 0x84}, {0xE0, 0x85},
    {0xE2, 0x86}, {0xE4, 0x87}, {0xC9, 0x88}, {0xC8, 0x89}, {0xCA, 0x8A}, {0xCB, 0x8B},
    {0xE9, 0x8C}, {0xE8, 0x8D}, {0xEA, 0x8E}, {0xEB, 0x8F}, {0xCD, 0x90}, {0xCC, 0x91},
    {0xCE, 0x92}, {0xCF, 0x93}, {0xED, 0x94}, {0xEC, 0x95}, {0xEE, 0x96}, {0xEF, 0x97},
    {0xD3, 0x98}, {0xD2, 0x99}, {0xD4, 0x9A}, {0xD6, 0x9B}, {0xF3, 0x9C}, {0xF2, 0x9D},
    {0xF4, 0x9E}, {0xF6, 0x9F}, {0xDA, 0xA0}, {0xD9, 0xA1}, {0xDB, 0xA2}, {0xDC, 0xA3},
    {0xFA, 0xA4}, {0xF9, 0xA5}, {0xFB, 0xA6}, {0xFC, 0xA7}, {0xD1, 0xA8}, {0xF1, 0xA9},
    {0xC7, 0xAA}, {0xE7, 0xAB}, {0xDF, 0xAC}, {0x2026, 0xAD}, {0xA1, 0xAE}, {0xBF, 0xAF},
};

// The next code point of `s` from `i` (UTF-8), `i` moved past it; 0xFFFD for a broken one.
std::uint32_t next_code_point(const char* s, std::size_t n, std::size_t& i) {
    const auto b = [&](std::size_t k) { return static_cast<unsigned char>(s[k]); };
    const unsigned char c = b(i);
    const int more = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
    if (more < 0) {
        ++i;
        return 0xFFFD;
    }
    std::uint32_t cp = more == 0 ? c : more == 1 ? (c & 0x1Fu) : more == 2 ? (c & 0x0Fu) : (c & 0x07u);
    for (int k = 1; k <= more; ++k) {
        if (i + static_cast<std::size_t>(k) >= n || (b(i + static_cast<std::size_t>(k)) & 0xC0) != 0x80) {
            ++i;
            return 0xFFFD;
        }
        cp = (cp << 6) | (b(i + static_cast<std::size_t>(k)) & 0x3Fu);
    }
    i += static_cast<std::size_t>(more) + 1;
    return cp;
}

}  // namespace

// `text` (UTF-8) in the game's bytes, at most `max` - 1 of them and an end: printable ASCII as it
// is, the font's control bytes (0x01 a line break, 0x08 to 0x0F colours, 0x10 to 0x19 the pad's
// buttons) as they are, the accented letters the font has as its own, typographic quotes and dashes
// as plain ones, anything else as '?'.
std::string ui_game_text(const char* text, std::size_t max) {
    std::string out;
    if (text == nullptr) {
        return out;
    }
    const std::size_t n = std::strlen(text);
    for (std::size_t i = 0; i < n && out.size() + 1 < max;) {
        const std::uint32_t cp = next_code_point(text, n, i);
        if (cp == 0x01 || (cp >= 0x08 && cp <= 0x19) || (cp >= 0x20 && cp <= 0x7E)) {
            out.push_back(static_cast<char>(cp));
            continue;
        }
        if (cp < 0x20 || cp == 0x7F) {
            continue;
        }
        const auto letter = std::find_if(std::begin(kLetters), std::end(kLetters),
            [cp](const Letter& l) { return l.cp == cp; });
        if (letter != std::end(kLetters)) {
            out.push_back(static_cast<char>(letter->game));
        } else if (cp == 0xAB || cp == 0xBB || cp == 0x201C || cp == 0x201D) {
            out.push_back('"');
        } else if (cp == 0x2018 || cp == 0x2019) {
            out.push_back('\'');
        } else if (cp == 0x2013 || cp == 0x2014) {
            out.push_back('-');
        } else if (cp == 0xA0) {
            out.push_back(' ');
        } else {
            out.push_back('?');
        }
    }
    return out;
}

void ui_reset() {
    {
        const std::lock_guard<std::mutex> lock(g_ui_lock);
        g_ui.clear();
        g_ui_count = 0;
    }
    if (detail::g_play != nullptr && detail::g_play->help_box_close != nullptr) {
        for (const HelpOwner& h : g_help) {
            detail::g_play->help_box_close(h.id);
        }
    }
    g_help.clear();
}

// The elements of this level load in drawing order (by layer, then as each mod gave them).
int ui_elements(openrac_game_gui_element* out, int max) {
    if (g_ui_count.load(std::memory_order_relaxed) == 0) {
        return 0;
    }
    const std::lock_guard<std::mutex> lock(g_ui_lock);
    const std::uint32_t loads = detail::loads_now();
    std::vector<const openrac_game_gui_element*> all;
    for (const UiSet& s : g_ui) {
        if (s.loads == loads) {
            for (const openrac_game_gui_element& e : s.elements) {
                all.push_back(&e);
            }
        }
    }
    std::stable_sort(all.begin(), all.end(), [](const openrac_game_gui_element* a, const openrac_game_gui_element* b) {
        return a->layer < b->layer;
    });
    const int n = static_cast<int>(all.size());
    for (int i = 0; out != nullptr && i < n && i < max; ++i) {
        out[i] = *all[static_cast<std::size_t>(i)];
    }
    return n;
}

namespace detail {

namespace {

std::uint32_t rgba_word(const std::uint8_t c[4]) {
    return static_cast<std::uint32_t>(c[0]) | static_cast<std::uint32_t>(c[1]) << 8
           | static_cast<std::uint32_t>(c[2]) << 16 | static_cast<std::uint32_t>(c[3]) << 24;
}

bool finite_all(const openrac_gui_element& e) {
    const float v[] = {e.x, e.y, e.w, e.h, e.scale_x, e.scale_y, e.frame};
    return std::all_of(std::begin(v), std::end(v), [](float f) { return std::isfinite(f); });
}

}  // namespace

int api_gui_draw(openrac_mod* mod, const openrac_gui_element* elements, int count) {
    std::int32_t level = -1;
    if (mod == nullptr || count < 0 || count > OPENRAC_GUI_ELEMENTS_MAX || (count > 0 && elements == nullptr)
        || g_play == nullptr || g_play->gui_text_width == nullptr || (count > 0 && !in_level(phase_now(&level)))) {
        return OPENRAC_HOOK_UNKNOWN;
    }
    std::vector<openrac_game_gui_element> converted;
    converted.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        const openrac_gui_element& e = elements[i];
        const bool ended = std::find(std::begin(e.text), std::end(e.text), '\0') != std::end(e.text);
        if (e.size < sizeof(openrac_gui_element) || e.kind < OPENRAC_GUI_TEXT || e.kind > OPENRAC_GUI_ICON_KEY
            || !ended || !finite_all(e)
            || (e.kind == OPENRAC_GUI_TEXT && (e.scale_x < 0.25f || e.scale_x > 4.0f))) {
            return OPENRAC_HOOK_UNKNOWN;
        }
        openrac_game_gui_element g{};
        g.kind = e.kind;
        g.flags = e.flags;
        g.layer = e.layer;
        g.x = e.x;
        g.y = e.y;
        g.w = e.w;
        g.h = e.h;
        g.scale_x = e.scale_x;
        g.scale_y = e.scale_y;
        g.frame = e.frame;
        g.rgba = rgba_word(e.rgba);
        g.rgba2 = rgba_word(e.rgba2);
        g.id = e.id;
        const std::string text = ui_game_text(e.text, sizeof g.text);
        std::memcpy(g.text, text.c_str(), text.size() + 1);
        converted.push_back(g);
    }
    const std::lock_guard<std::mutex> lock(g_ui_lock);
    const std::uint32_t loads = loads_now();
    std::size_t others = 0;
    for (const UiSet& s : g_ui) {
        if (s.mod != mod && s.loads == loads) {
            others += s.elements.size();
        }
    }
    if (others + converted.size() > OPENRAC_GUI_ELEMENTS_ALL) {
        return OPENRAC_HOOK_UNKNOWN;
    }
    std::erase_if(g_ui, [&](const UiSet& s) { return s.mod == mod || s.loads != loads; });
    if (!converted.empty()) {
        g_ui.push_back({mod, loads, std::move(converted)});
    }
    int total = 0;
    for (const UiSet& s : g_ui) {
        total += static_cast<int>(s.elements.size());
    }
    g_ui_count = total;
    return OPENRAC_HOOK_OK;
}

float api_gui_text_width(openrac_mod* mod, const char* text, std::uint32_t flags, float scale) {
    std::int32_t level = -1;
    if (mod == nullptr || text == nullptr || !std::isfinite(scale) || scale <= 0.0f || g_play == nullptr
        || g_play->gui_text_width == nullptr || !in_level(phase_now(&level))) {
        return -1.0f;
    }
    const std::string game = ui_game_text(text, OPENRAC_GUI_TEXT_BYTES);
    return g_play->gui_text_width(game.c_str(), flags, scale);
}

int api_help_box_show(openrac_mod* mod, const openrac_help_box* box) {
    std::int32_t level = -1;
    if (mod == nullptr || box == nullptr || box->size < sizeof(openrac_help_box) || g_play == nullptr
        || g_play->help_box_show == nullptr || !in_level(phase_now(&level))
        || std::find(std::begin(box->text), std::end(box->text), '\0') == std::end(box->text)
        || (box->flags & ~(OPENRAC_HELP_SILENT | OPENRAC_HELP_KEEP)) != 0) {
        return OPENRAC_HOOK_UNKNOWN;
    }
    openrac_game_help_box g{};
    g.flags = box->flags;
    g.y = box->y;
    g.ticks = box->ticks;
    const std::string text = ui_game_text(box->text, sizeof g.text);
    std::memcpy(g.text, text.c_str(), text.size() + 1);
    const int id = g_play->help_box_show(&g);
    if (id < 0) {
        return OPENRAC_HOOK_UNKNOWN;
    }
    std::erase_if(g_help, [&](const HelpOwner& h) { return g_play->help_box_state(h.id) == OPENRAC_HELP_GONE; });
    g_help.push_back({mod, id});
    return id;
}

int api_help_box_close(openrac_mod* mod, int box) {
    const auto at = std::find_if(g_help.begin(), g_help.end(),
        [&](const HelpOwner& h) { return h.mod == mod && h.id == box; });
    if (mod == nullptr || at == g_help.end() || g_play == nullptr || g_play->help_box_close == nullptr) {
        return OPENRAC_HOOK_UNKNOWN;
    }
    return g_play->help_box_close(box) == 0 ? OPENRAC_HOOK_OK : OPENRAC_HOOK_UNKNOWN;
}

int api_help_box_state(openrac_mod* mod, int box) {
    const auto at = std::find_if(g_help.begin(), g_help.end(),
        [&](const HelpOwner& h) { return h.mod == mod && h.id == box; });
    if (mod == nullptr || at == g_help.end() || g_play == nullptr || g_play->help_box_state == nullptr) {
        return OPENRAC_HELP_GONE;
    }
    return g_play->help_box_state(box);
}

int api_game_sound_play(openrac_mod* mod, int bank, int index, float volume) {
    std::int32_t level = -1;
    if (mod == nullptr || index < 0 || !std::isfinite(volume) || volume < 0.0f || volume > 2.0f
        || g_play == nullptr || g_play->game_sound_play == nullptr || !in_level(phase_now(&level))) {
        return OPENRAC_HOOK_UNKNOWN;
    }
    const int voice = g_play->game_sound_play(bank, index, volume);
    return voice >= 0 ? voice : OPENRAC_HOOK_UNKNOWN;
}

}  // namespace detail

}  // namespace openrac::mods

extern "C" int openrac_game_gui_elements(openrac_game_gui_element* out, int max) {
    return openrac::c_boundary::call("openrac_game_gui_elements", 0, [&]() -> int {
        return openrac::mods::ui_elements(out, max);
    });
}
