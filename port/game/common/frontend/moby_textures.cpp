// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the OpenRAC contributors
//
// OpenRAC Commando's 1.120 in the front end (moby_textures.h).

#include "game/common/frontend/moby_textures.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <format>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/log.h"
#include "frontend.h"
#include "game/common/frontend/state.h"
#include "game/common/mods.h"
#include "scene/level.h"
#include "scene/material_overrides.h"

namespace openrac::frontend {
namespace {

std::mutex s_lock;
// The models made this level, by request key, and the keys that could not be made.
std::unordered_map<std::string, std::uint32_t> s_made;
std::set<std::string> s_failed;
// The bind-pose differences made this level, by class model and mesh (nullopt: all identity).
std::map<std::pair<std::uint32_t, std::uint32_t>, std::optional<std::vector<renderer::Mat4>>>
    s_rebinds;

// The class model's textures in the order its parts first use them (the requests' indices).
std::vector<int> model_images(const scene::LevelData& level, const scene::Model& model) {
    std::vector<int> images;
    for (const scene::Primitive& p : model.primitives) {
        if (p.material < 0 || static_cast<std::size_t>(p.material) >= level.materials.size()) {
            continue;
        }
        const int image = level.materials[static_cast<std::size_t>(p.material)].image;
        if (image >= 0 && std::find(images.begin(), images.end(), image) == images.end()) {
            images.push_back(image);
        }
    }
    return images;
}

// Appends to `level` a copy of model `from` whose materials take the pictures `swaps` asks for.
// Returns its index, or none with a reason.
std::optional<std::uint32_t> make_textured(
    scene::LevelData& level,
    std::uint32_t from,
    const std::vector<mods::MobyTextureSwap>& swaps,
    std::string& why
) {
    if (from >= level.models.size()) {
        why = "its class has no model in this level";
        return std::nullopt;
    }
    const scene::Model source = level.models[from];
    const std::vector<int> images = model_images(level, source);
    // The class's texture each picture is drawn in place of.
    std::map<int, int> swapped;
    for (const mods::MobyTextureSwap& s : swaps) {
        int target = -1;
        if (s.index >= 0) {
            if (static_cast<std::size_t>(s.index) < images.size()) {
                target = images[static_cast<std::size_t>(s.index)];
            }
        } else {
            for (const int image : images) {
                if (static_cast<std::size_t>(image) < level.image_paths.size()
                    && std::filesystem::path(level.image_paths[static_cast<std::size_t>(image)])
                               .stem()
                               .string()
                           == s.name) {
                    target = image;
                    break;
                }
            }
        }
        if (target < 0) {
            log::warn(
                "mods: moby textures: class model {} has no texture {} ({} textures): {} not drawn",
                source.name,
                s.index >= 0 ? std::to_string(s.index) : s.name,
                images.size(),
                s.png.filename().string()
            );
            continue;
        }
        const auto loaded = scene::load_material_image(level, s.png.generic_string());
        if (!loaded) {
            why = std::format("cannot read {}", s.png.string());
            return std::nullopt;
        }
        swapped[target] = *loaded;
    }
    if (swapped.empty()) {
        why = "none of the textures it names is its class model's";
        return std::nullopt;
    }
    // A material of the class model with a swapped texture: a copy of it with the picture.
    std::map<int, int> material_of;
    const auto remap = [&](int material) {
        if (material < 0 || static_cast<std::size_t>(material) >= level.materials.size()) {
            return material;
        }
        if (const auto done = material_of.find(material); done != material_of.end()) {
            return done->second;
        }
        scene::Material copy = level.materials[static_cast<std::size_t>(material)];
        const auto swap = swapped.find(copy.image);
        int out = material;
        if (swap != swapped.end()) {
            copy.image = swap->second;
            // Part-clear texels are drawn as the loader draws them (material_index): with the
            // alpha test, unless the material is metal or blended.
            if (!copy.metal && !copy.blend
                && static_cast<std::size_t>(copy.image) < level.translucent_images.size()
                && level.translucent_images[static_cast<std::size_t>(copy.image)]) {
                copy.cutout = true;
            }
            level.materials.push_back(copy);
            out = static_cast<int>(level.materials.size() - 1);
        }
        material_of[material] = out;
        return out;
    };
    // Its vertices copied once each (add_loaded_models takes indices of new vertices only).
    scene::Model made = source;
    made.name = source.name + " (mod textures)";
    made.primitives.clear();
    std::unordered_map<std::uint32_t, std::uint32_t> vertex_of;
    for (const scene::Primitive& p : source.primitives) {
        scene::Primitive q = p;
        q.first_index = static_cast<std::uint32_t>(level.indices.size());
        const std::size_t end = static_cast<std::size_t>(p.first_index) + p.index_count;
        if (end > level.indices.size()) {
            why = "its class model's indices are out of range";
            return std::nullopt;
        }
        for (std::size_t i = p.first_index; i < end; ++i) {
            const std::uint32_t v = level.indices[i];
            if (v >= level.vertices.size()) {
                why = "its class model's indices are out of range";
                return std::nullopt;
            }
            const auto [at, fresh] =
                vertex_of.emplace(v, static_cast<std::uint32_t>(level.vertices.size()));
            if (fresh) {
                const scene::Vertex copy = level.vertices[v];
                level.vertices.push_back(copy);
            }
            level.indices.push_back(at->second);
        }
        q.material = remap(p.material);
        if (p.appearance_material >= 0) {
            q.appearance_material = remap(p.appearance_material);
        }
        made.primitives.push_back(q);
    }
    level.models.push_back(std::move(made));
    return static_cast<std::uint32_t>(level.models.size() - 1);
}

}  // namespace

std::optional<std::uint32_t> TexturedModels::find(
    std::uint32_t address, int oclass, std::uint32_t scope
) const {
    if (const auto own = by_address.find(address);
        own != by_address.end() && own->second.first == oclass) {
        return own->second.second;
    }
    if (const auto all = by_class.find({oclass, scope}); all != by_class.end()) {
        return all->second;
    }
    return std::nullopt;
}

TexturedModels textured_models_now() {
    TexturedModels out;
    std::vector<mods::MobyTexturesDraw> draws = mods::moby_textures_drawn();
    if (draws.empty()) {
        return out;
    }
    const std::lock_guard<std::mutex> lock(s_lock);
    for (const mods::MobyTexturesDraw& d : draws) {
        const auto made = s_made.find(d.key);
        if (made == s_made.end()) {
            continue;
        }
        if (d.scope == 0) {
            out.by_address[d.address] = {d.oclass, made->second};
        } else {
            out.by_class[{d.oclass, d.scope}] = made->second;
        }
    }
    return out;
}

void load_moby_textures() {
    if (!g || !g->prop_scene_ready) {
        return;
    }
    const std::vector<mods::MobyTexturesDraw> draws = mods::moby_textures_drawn();
    if (draws.empty()) {
        return;
    }
    // The requests whose models are not made yet, each key once.
    std::vector<const mods::MobyTexturesDraw*> wanted;
    {
        const std::lock_guard<std::mutex> lock(s_lock);
        std::set<std::string> seen;
        for (const mods::MobyTexturesDraw& d : draws) {
            if (!s_made.contains(d.key) && !s_failed.contains(d.key) && seen.insert(d.key).second) {
                wanted.push_back(&d);
            }
        }
    }
    if (wanted.empty()) {
        return;
    }
    const auto started = std::chrono::steady_clock::now();
    scene::LevelData& level = g->render.level;
    g->render.scene.align_models(level);
    std::vector<std::pair<std::string, std::uint32_t>> made;
    std::vector<std::string> failed;
    for (const mods::MobyTexturesDraw* d : wanted) {
        std::string why;
        const auto cls = level.moby_classes.find(d->oclass);
        std::optional<std::uint32_t> model;
        if (cls == level.moby_classes.end() || cls->second.second) {
            why = "the level draws no model of its class";
        } else {
            try {
                model = make_textured(level, cls->second.first, d->swaps, why);
            } catch (const std::exception& e) {
                why = e.what();
            }
        }
        if (model) {
            made.emplace_back(d->key, *model);
        } else {
            failed.push_back(d->key);
            log::warn("mods: moby textures for class {}: {}", d->oclass, why);
        }
    }
    bool added = true;
    if (!made.empty()) {
        std::string error;
        run_graphics([&] {
            added = g->render.scene.add_loaded_models(level, g->render.renderer->textures(), error);
        });
        if (!added) {
            log::error("mods: moby textures could not be added to the level: {}", error);
            g->prop_scene_ready = false;
        }
    }
    const std::lock_guard<std::mutex> lock(s_lock);
    for (const auto& [key, model] : made) {
        if (added) {
            s_made[key] = model;
        } else {
            s_failed.insert(key);
        }
    }
    s_failed.insert(failed.begin(), failed.end());
    if (added && !made.empty()) {
        log::info(
            "mods: {} moby texture model(s) made for level {} in {:.1f} ms",
            made.size(),
            g->loaded,
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
                .count()
        );
    }
}

void forget_moby_textures() {
    const std::lock_guard<std::mutex> lock(s_lock);
    s_made.clear();
    s_failed.clear();
    s_rebinds.clear();
}

const std::vector<renderer::Mat4>* game_pose_rebind(std::uint32_t class_model, std::uint32_t mesh) {
    const std::lock_guard<std::mutex> lock(s_lock);
    const auto key = std::make_pair(class_model, mesh);
    auto held = s_rebinds.find(key);
    if (held == s_rebinds.end()) {
        std::optional<std::vector<renderer::Mat4>> rebind;
        const auto& models = g->render.level.models;
        if (class_model < models.size() && mesh < models.size()) {
            const auto& own = models[class_model].bones;
            const auto& mine = models[mesh].bones;
            std::vector<renderer::Mat4> joints(
                std::max(own.size(), mine.size()), renderer::identity()
            );
            bool moved = false;
            for (std::size_t j = 0; j < joints.size(); ++j) {
                if (j >= own.size() || j >= mine.size() || own[j].name.empty()
                    || mine[j].name.empty()) {
                    continue;
                }
                const auto inverse = renderer::inverse_affine(mine[j].bind);
                if (!inverse) {
                    continue;
                }
                joints[j] = renderer::multiply(own[j].bind, *inverse);
                const renderer::Mat4 one = renderer::identity();
                for (std::size_t k = 0; k < 16; ++k) {
                    moved = moved || std::fabs(joints[j][k] - one[k]) > 1.0e-4f;
                }
            }
            if (moved) {
                rebind = std::move(joints);
            }
        }
        held = s_rebinds.emplace(key, std::move(rebind)).first;
    }
    return held->second ? &*held->second : nullptr;
}

}  // namespace openrac::frontend
