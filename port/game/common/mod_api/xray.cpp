// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the OpenRAC contributors
//
// The mods' interface, OpenRAC Commando's 1.128: a moby's silhouette drawn through what covers it
// (openrac_api.moby_xray), a point of the world on the screen (world_to_screen) and the camera the
// last frame was drawn with (camera_lens). The marks are kept here; the front end reads them each
// tick (moby_xrays_drawn: frame.cpp poses a marked moby the game did not draw, the scene's x-ray
// bucket draws them) and gives back the camera of each frame that drew a level's world
// (set_drawn_camera).

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <mutex>
#include <optional>
#include <vector>

#include "mod_api/internal.h"
#include "mods.h"

namespace openrac::mods {

namespace {

struct XrayMark {
    const openrac_mod* mod = nullptr;
    int oclass = -1;
    std::uint32_t loads = 0;
    std::array<float, 4> rgba{};
    std::uint32_t flags = 0;
    std::int32_t ticks = 0;  // left; 0 or less: until taken off
    float max_distance = OPENRAC_MOBY_XRAY_DISTANCE;
};

std::mutex g_xray_lock;
std::map<std::uint32_t, XrayMark> g_xray_marks;  // by address
std::mutex g_camera_lock;
std::optional<DrawnCamera> g_drawn_camera;

bool finite(float f) {
    return std::isfinite(f);
}

}  // namespace

std::vector<MobyXrayDraw> moby_xrays_drawn() {
    std::vector<MobyXrayDraw> out;
    const std::uint32_t loads = detail::loads_now();
    const std::lock_guard<std::mutex> lock(g_xray_lock);
    for (const auto& [address, m] : g_xray_marks) {
        if (m.loads == loads) {
            out.push_back({address, m.oclass, m.rgba, m.flags, m.max_distance});
        }
    }
    return out;
}

// Once a tick, before the mods' frame callbacks: each mark's ticks counted down, and the marks of
// another level load, of a freed moby or of a slot made another class dropped.
void xray_frame() {
    std::vector<std::pair<std::uint32_t, int>> held;
    {
        const std::lock_guard<std::mutex> lock(g_xray_lock);
        if (g_xray_marks.empty()) {
            return;
        }
        for (const auto& [address, m] : g_xray_marks) {
            held.emplace_back(address, m.oclass);
        }
    }
    std::map<std::uint32_t, int> classes;
    for (const auto& [address, oclass] : held) {
        classes[address] = detail::g_play != nullptr && detail::g_play->moby_class_at != nullptr
                               ? detail::g_play->moby_class_at(address)
                               : -1;
    }
    const std::uint32_t loads = detail::loads_now();
    const std::lock_guard<std::mutex> lock(g_xray_lock);
    std::erase_if(g_xray_marks, [&](auto& e) {
        XrayMark& m = e.second;
        const auto c = classes.find(e.first);
        if (m.loads != loads || (c != classes.end() && c->second != m.oclass)) {
            return true;
        }
        if (m.ticks > 0) {
            --m.ticks;
            return m.ticks == 0;
        }
        return false;
    });
}

void xray_reset() {
    const std::lock_guard<std::mutex> lock(g_xray_lock);
    g_xray_marks.clear();
}

void set_drawn_camera(const DrawnCamera& camera) {
    const std::lock_guard<std::mutex> lock(g_camera_lock);
    g_drawn_camera = camera;
}

std::optional<DrawnCamera> drawn_camera() {
    const std::lock_guard<std::mutex> lock(g_camera_lock);
    return g_drawn_camera;
}

}  // namespace openrac::mods

namespace openrac::mods::detail {

// Interface 1.128: marks (or unmarks) the moby at `address` for its silhouette.
int api_moby_xray(openrac_mod* mod, std::uint32_t address, const openrac_moby_xray* xray) {
    if (mod == nullptr || g_play == nullptr || g_play->moby_class_at == nullptr || address == 0) {
        return OPENRAC_HOOK_UNKNOWN;
    }
    if (xray != nullptr
        && (xray->size < sizeof(openrac_moby_xray)
            || (xray->flags & ~(OPENRAC_XRAY_OUTLINE | OPENRAC_XRAY_ALWAYS)) != 0
            || !std::all_of(xray->rgba, xray->rgba + 4, finite) || !finite(xray->max_distance)
            || xray->max_distance < 0.0f)) {
        return OPENRAC_HOOK_UNKNOWN;
    }
    const std::int32_t oclass = g_play->moby_class_at(address);
    const std::uint32_t loads = loads_now();
    const std::lock_guard<std::mutex> lock(g_xray_lock);
    std::erase_if(g_xray_marks, [&](const auto& e) {
        return e.second.loads != loads || (e.first == address && e.second.oclass != oclass);
    });
    const auto now = g_xray_marks.find(address);
    if (now != g_xray_marks.end() && now->second.mod != mod) {
        return OPENRAC_HOOK_TAKEN;
    }
    if (xray == nullptr) {
        if (now != g_xray_marks.end()) {
            g_xray_marks.erase(now);
        }
        return OPENRAC_HOOK_OK;
    }
    if (oclass < 0) {
        return OPENRAC_HOOK_UNKNOWN;
    }
    if (now == g_xray_marks.end() && g_xray_marks.size() >= OPENRAC_MOBY_XRAY_MAX) {
        return OPENRAC_HOOK_FULL;
    }
    XrayMark& m = g_xray_marks[address];
    m.mod = mod;
    m.oclass = oclass;
    m.loads = loads;
    for (int i = 0; i < 4; ++i) {
        m.rgba[static_cast<std::size_t>(i)] = std::clamp(xray->rgba[i], 0.0f, 1.0f);
    }
    m.flags = xray->flags;
    m.ticks = std::max<std::int32_t>(xray->ticks, 0);
    m.max_distance = xray->max_distance > 0.0f ? xray->max_distance : OPENRAC_MOBY_XRAY_DISTANCE;
    return OPENRAC_HOOK_OK;
}

// Interface 1.128: `position` through the last drawn frame's camera.
int api_world_to_screen(const float position[3], openrac_screen_point* out) {
    if (position == nullptr || out == nullptr || out->size < sizeof(openrac_screen_point)
        || !std::all_of(position, position + 3, finite)) {
        return OPENRAC_HOOK_UNKNOWN;
    }
    const auto camera = drawn_camera();
    if (!camera || camera->width <= 0 || camera->height <= 0) {
        return OPENRAC_HOOK_UNKNOWN;
    }
    // Column-major: clip = projection * view * (position, 1).
    const auto& v = camera->view;
    const auto& p = camera->projection;
    float eye[4];
    for (int r = 0; r < 4; ++r) {
        eye[r] = v[r] * position[0] + v[4 + r] * position[1] + v[8 + r] * position[2] + v[12 + r];
    }
    float clip[4];
    for (int r = 0; r < 4; ++r) {
        clip[r] = p[r] * eye[0] + p[4 + r] * eye[1] + p[8 + r] * eye[2] + p[12 + r] * eye[3];
    }
    const bool in_front = clip[3] > 1e-6f;
    const float w = std::abs(clip[3]) > 1e-6f ? std::abs(clip[3]) : 1e-6f;
    const float ndc_x = clip[0] / w;
    const float ndc_y = clip[1] / w;
    const float width = static_cast<float>(camera->width);
    const float height = static_cast<float>(camera->height);
    out->px = (ndc_x * 0.5f + 0.5f) * width;
    out->py = (0.5f - ndc_y * 0.5f) * height;
    // The game's 2D: a 4:3 column of picture_scale of the width in the middle, 512 x 416.
    const float column = std::max(camera->picture_scale, 1e-3f) * width;
    out->x = (out->px - (width - column) * 0.5f) / column * 512.0f;
    out->y = out->py / height * 416.0f;
    out->width = camera->width;
    out->height = camera->height;
    out->in_front = in_front ? 1 : 0;
    return OPENRAC_HOOK_OK;
}

// Interface 1.128: the last drawn frame's camera with its lens.
int api_camera_lens(openrac_camera_lens* out) {
    if (out == nullptr || out->size < sizeof(openrac_camera_lens)) {
        return OPENRAC_HOOK_UNKNOWN;
    }
    const auto camera = drawn_camera();
    if (!camera || camera->width <= 0 || camera->height <= 0) {
        return OPENRAC_HOOK_UNKNOWN;
    }
    const auto& v = camera->view;
    std::copy(camera->eye.begin(), camera->eye.end(), out->eye);
    // The view's rows: x right, y up, z back (OpenGL's); the game's rows are forward, left, up.
    for (int a = 0; a < 3; ++a) {
        out->left[a] = -v[static_cast<std::size_t>(a * 4)];
        out->up[a] = v[static_cast<std::size_t>(a * 4 + 1)];
        out->forward[a] = -v[static_cast<std::size_t>(a * 4 + 2)];
    }
    const auto& p = camera->projection;
    out->tan_half_fov_x = p[0] != 0.0f ? 1.0f / p[0] : 0.0f;
    out->tan_half_fov_y = p[5] != 0.0f ? 1.0f / p[5] : 0.0f;
    std::copy(v.begin(), v.end(), out->view);
    std::copy(p.begin(), p.end(), out->projection);
    out->width = camera->width;
    out->height = camera->height;
    out->picture_scale = camera->picture_scale;
    return OPENRAC_HOOK_OK;
}

}  // namespace openrac::mods::detail
