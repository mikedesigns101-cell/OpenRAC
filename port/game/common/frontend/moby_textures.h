// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the OpenRAC contributors
//
// OpenRAC Commando's 1.120 in the front end: the models that draw mobys with a mod's textures
// (openrac_api.moby_textures_at, moby_textures_class), and the bind poses of a mod's skinned mesh
// posed by a moby's own joints (OPENRAC_MODEL_GAME_POSE).
//
// A textured model is a copy of the class's model (its vertices, with their skin, and its indices)
// whose materials take the mod's pictures where the request names a texture of the class: added to
// the level and the scene as the props' meshes are (LevelScene::add_loaded_models), once for each
// class and set of pictures (mods::MobyTexturesDraw::key), and kept until the level is left. It has
// the class's joints and bounds, so the moby is posed and culled as with its own model.

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "renderer/math.h"

namespace openrac::frontend {

// The textured models of this level as a frame looks them up: by moby address (with the class it
// was set for) and by class and scope, each its model index in the level.
struct TexturedModels {
    std::unordered_map<std::uint32_t, std::pair<int, std::uint32_t>> by_address;
    std::map<std::pair<int, std::uint32_t>, std::uint32_t> by_class;

    // The model for the moby at `address` of class `oclass` drawn in `scope`: its own, else its
    // class's; none while it is not made (or could not be).
    std::optional<std::uint32_t> find(std::uint32_t address, int oclass, std::uint32_t scope) const;
};

// The mods' textured mobys whose models are made, for this frame. From the game's thread.
TexturedModels textured_models_now();

// Makes the models the mods' requests need that the level does not hold yet (after
// load_prop_meshes, with the frame's render job finished). From the game's thread.
void load_moby_textures();

// Forgets the made models: a new level starts again. From the game's thread.
void forget_moby_textures();

// The bind-pose difference of mesh `mesh` against the class model `class_model` (both models of the
// level), one matrix a joint (the class's bind times the inverse of the mesh's; the identity where
// either has no bind for it): multiplied after the game's palette, it poses the mesh's vertices by
// the class's joints. Null when every joint's is the identity. Made once a pair for the level; the
// pointer stays valid until forget_moby_textures. From the game's thread.
const std::vector<renderer::Mat4>* game_pose_rebind(std::uint32_t class_model, std::uint32_t mesh);

}  // namespace openrac::frontend
