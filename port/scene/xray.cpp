// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 the OpenRAC contributors
//
// The mods' marked mobys in silhouette (OpenRAC Commando's interface 1.128, openrac_api.moby_xray):
// drawn after the effects over the frame's colour, tested against its depth, with a flat colour.
// For each marked moby (its instances: its model and its layers), in one pass with the stencil:
//   1. where it is seen (depth GEQUAL, reversed depth), the stencil takes its number, no colour;
//   2. where it is behind something (depth LESS) and the stencil does not hold its number, its
//      colour, and the stencil takes its number (each pixel coloured once, so a translucent colour
//      does not add up over its own faces).
// So the parts of it that are seen stay as they are drawn, and only its covered parts show as the
// silhouette; an arm in front of its own body is not painted over. With OPENRAC_XRAY_ALWAYS only the
// second step, with depth ALWAYS. Each vertex is moved a little towards the eye along its own ray
// (the same pixel, a nearer depth), so that the moby is not covered by its own faces as the colour
// passes drew them. The stencil is cleared at the start and left cleared at the end for no one:
// the stencil shadows clear it before they use it.

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <vector>

#include "common/log.h"
#include "renderer/gl/gl.h"
#include "renderer/gl/precise_depth.h"
#include "scene/level_renderer.h"
#include "scene/level_renderer_internal.h"

namespace openrac::scene {

using namespace openrac::gl;
using namespace render_detail;

namespace {

// How far towards the eye each vertex moves (the game's units): more than the colour passes' and
// this pass's depths differ by, less than any cover a moby hides behind.
constexpr float kXrayEyeBias = 0.15f;

constexpr const char* kXrayVertex = R"glsl(#version 410 core
layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 4) in mat4 model;
layout(location = 9) in float palette;
layout(location = 10) in uvec4 joints;
layout(location = 11) in vec4 weights;
uniform mat4 view_projection;
uniform vec3 eye;
uniform float eye_bias;
uniform sampler2D palette_texture;
out vec3 v_normal;
out vec3 v_to_eye;
const int kPaletteWidth = 1024;

mat4 joint_matrix(int index) {
    int texel = 4 * index;
    ivec2 at = ivec2(texel % kPaletteWidth, texel / kPaletteWidth);
    return mat4(
        texelFetch(palette_texture, at, 0),
        texelFetch(palette_texture, at + ivec2(1, 0), 0),
        texelFetch(palette_texture, at + ivec2(2, 0), 0),
        texelFetch(palette_texture, at + ivec2(3, 0), 0)
    );
}

void main() {
    vec4 p = vec4(position, 1.0);
    vec3 n = normal;
    float total = weights.x + weights.y + weights.z + weights.w;
    if (palette >= 0.0 && total > 0.0) {
        int base = int(palette + 0.5);
        mat4 skin = weights.x * joint_matrix(base + int(joints.x))
                    + weights.y * joint_matrix(base + int(joints.y))
                    + weights.z * joint_matrix(base + int(joints.z))
                    + weights.w * joint_matrix(base + int(joints.w));
        p = vec4((skin * p).xyz, 1.0);
        n = mat3(skin) * n;
    }
    vec4 world = model * p;
    vec3 to_eye = eye - world.xyz;
    float distance = length(to_eye);
    vec3 towards = distance > 1e-4 ? to_eye / distance : vec3(0.0);
    world.xyz += towards * min(eye_bias, distance * 0.5);
    v_normal = transpose(inverse(mat3(model))) * n;
    v_to_eye = towards;
    gl_Position = view_projection * world;
}
)glsl";

constexpr const char* kXrayFragment = R"glsl(#version 410 core
in vec3 v_normal;
in vec3 v_to_eye;
uniform vec4 xray_colour;
uniform int xray_outline;
uniform int xray_mark_only;
layout(location = 0) out vec4 colour;

void main() {
    if (xray_mark_only != 0) {
        colour = vec4(0.0);
        return;
    }
    float alpha = xray_colour.a;
    if (xray_outline != 0) {
        float squared = dot(v_normal, v_normal);
        float facing = squared > 1e-12 ? abs(dot(v_normal * inversesqrt(squared), v_to_eye)) : 0.0;
        // The rim: strong where the surface turns away from the eye, clear where it faces it.
        alpha *= smoothstep(0.45, 0.9, 1.0 - facing);
        if (alpha <= 0.01) {
            discard;
        }
    }
    colour = vec4(xray_colour.rgb, alpha);
}
)glsl";

}  // namespace

void LevelScene::release_xray() {
    for (Group& g : m_xray_groups) {
        if (g.vao != 0) {
            glDeleteVertexArrays(1, &g.vao);
        }
        if (g.instances != 0) {
            glDeleteBuffers(1, &g.instances);
        }
    }
    m_xray_groups.clear();
    m_xray_shader.release();
    m_xray_failed = false;
    m_xray.clear();
}

void LevelScene::draw_xray(const renderer::FrameInput& input, renderer::RenderState& state) {
    if (m_xray.empty() || m_xray_failed || state.depth_prepass) {
        return;
    }
    if (m_xray_shader.id() == 0) {
        std::string error;
        if (!m_xray_shader.build("mod xray", kXrayVertex, kXrayFragment, error)) {
            log::warn("mod xray: {}", error);
            m_xray_failed = true;
            return;
        }
    }
    // Each instance's vertex array and instance (made for its model; the arrays read the level's
    // buffers or a mod's mesh's chunk), uploaded before the pass.
    std::vector<std::size_t> drawn;
    drawn.reserve(m_xray.size());
    for (std::size_t i = 0; i < m_xray.size(); ++i) {
        const std::uint32_t model = m_xray[i].instance.model;
        if (model >= m_models.size() || m_models[model].primitives.empty()) {
            continue;
        }
        if (m_xray_groups.size() <= i) {
            m_xray_groups.resize(i + 1);
        }
        Group& g = m_xray_groups[i];
        if (g.vao == 0 || g.model != model) {
            if (g.vao != 0) {
                glDeleteVertexArrays(1, &g.vao);
                glDeleteBuffers(1, &g.instances);
            }
            g = make_group(model);
        }
        fill_group(g, {instance_data(m_xray[i].instance)}, GL_STREAM_DRAW);
        drawn.push_back(i);
    }
    glBindVertexArray(0);
    if (drawn.empty()) {
        return;
    }
    std::vector<renderer::RasterBuffer> geometry;
    geometry.push_back({m_vertices, m_vertex_count * sizeof(Vertex)});
    if (m_position_stream) {
        geometry.push_back({m_positions, m_vertex_count * 3 * sizeof(float)});
    }
    geometry.push_back({m_indices, m_cpu_indices.size() * sizeof(std::uint32_t)});
    for (const std::size_t i : drawn) {
        geometry.push_back({m_xray_groups[i].instances, sizeof(InstanceData), true});
    }
    std::ranges::sort(geometry, {}, &renderer::RasterBuffer::name);
    const auto distinct = std::ranges::unique(geometry, {}, &renderer::RasterBuffer::name);
    geometry.erase(distinct.begin(), distinct.end());
    std::erase_if(geometry, [](const renderer::RasterBuffer& b) { return b.name == 0 || b.size == 0; });
    const std::array<renderer::RasterTexture, 1> textures{renderer::RasterTexture{m_palette}};

    const bool precise = renderer::precise_depth();
    const auto clip = renderer::multiply(
        precise ? renderer::zero_to_one(input.camera.projection) : input.camera.projection,
        input.camera.view
    );
    const auto record = [&] {
        glViewport(0, 0, state.target.width, state.target.height);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_CULL_FACE);
        if (precise) {
            glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
        }
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glEnable(GL_STENCIL_TEST);
        glStencilMask(0xff);
        glClearStencil(0);
        glClear(GL_STENCIL_BUFFER_BIT);
        glEnable(GL_BLEND);
        glBlendEquation(GL_FUNC_ADD);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
        m_xray_shader.use();
        glUniformMatrix4fv(m_xray_shader.uniform("view_projection"), 1, GL_FALSE, clip.data());
        glUniform3f(
            m_xray_shader.uniform("eye"),
            input.camera.position[0],
            input.camera.position[1],
            input.camera.position[2]
        );
        glUniform1f(m_xray_shader.uniform("eye_bias"), kXrayEyeBias);
        glUniform1i(m_xray_shader.uniform("palette_texture"), static_cast<GLint>(kPaletteUnit));
        glActiveTexture(GL_TEXTURE0 + kPaletteUnit);
        glBindTexture(GL_TEXTURE_2D, m_palette);
        glActiveTexture(GL_TEXTURE0);
        const auto draw_instances = [&](std::uint32_t number) {
            for (const std::size_t i : drawn) {
                if (m_xray[i].number != number) {
                    continue;
                }
                const Group& g = m_xray_groups[i];
                glBindVertexArray(g.vao);
                for (const Primitive& p : m_models[g.model].primitives) {
                    glDrawElementsInstanced(
                        GL_TRIANGLES,
                        static_cast<GLsizei>(p.index_count),
                        GL_UNSIGNED_INT,
                        offset(std::size_t{p.first_index} * sizeof(std::uint32_t)),
                        1
                    );
                    ++state.stats.draw_calls;
                    state.stats.triangles += p.index_count / 3;
                }
            }
        };
        // The marked mobys one after the other, each with its own stencil number.
        std::vector<std::uint32_t> numbers;
        for (const std::size_t i : drawn) {
            if (std::find(numbers.begin(), numbers.end(), m_xray[i].number) == numbers.end()) {
                numbers.push_back(m_xray[i].number);
            }
        }
        for (const std::uint32_t number : numbers) {
            const XrayDraw* first = nullptr;
            for (const std::size_t i : drawn) {
                if (m_xray[i].number == number) {
                    first = &m_xray[i];
                    break;
                }
            }
            const auto ref = static_cast<GLint>(number & 0xFFu);
            const bool always = (first->flags & kXrayAlways) != 0;
            glStencilOpSeparate(GL_FRONT_AND_BACK, GL_KEEP, GL_KEEP, GL_REPLACE);
            if (!always) {
                // 1. Where it is seen: its number, no colour.
                glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
                glDepthFunc(GL_GEQUAL);
                glStencilFunc(GL_ALWAYS, ref, 0xff);
                glUniform1i(m_xray_shader.uniform("xray_mark_only"), 1);
                draw_instances(number);
            }
            // 2. Where it is covered (or everywhere), once a pixel: its colour.
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glDepthFunc(always ? GL_ALWAYS : GL_LESS);
            glStencilFunc(GL_NOTEQUAL, ref, 0xff);
            glUniform1i(m_xray_shader.uniform("xray_mark_only"), 0);
            glUniform1i(
                m_xray_shader.uniform("xray_outline"),
                (first->flags & kXrayOutline) != 0 ? 1 : 0
            );
            glUniform4f(
                m_xray_shader.uniform("xray_colour"),
                first->rgba[0],
                first->rgba[1],
                first->rgba[2],
                first->rgba[3]
            );
            draw_instances(number);
        }
        glStencilFunc(GL_ALWAYS, 0, 0xff);
        glStencilOpSeparate(GL_FRONT_AND_BACK, GL_KEEP, GL_KEEP, GL_KEEP);
        glDisable(GL_STENCIL_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDisable(GL_BLEND);
        glDepthFunc(GL_GEQUAL);
        glDepthMask(GL_TRUE);
        glBindVertexArray(0);
        glActiveTexture(GL_TEXTURE0 + kPaletteUnit);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0);
        if (precise) {
            glClipControl(GL_LOWER_LEFT, GL_NEGATIVE_ONE_TO_ONE);
        }
    };
    renderer::raster_pass(
        "mod xray",
        state.target,
        {geometry, textures, renderer::ColourAttachments::ColourDepthStencil},
        record
    );
}

Result<void> XrayRenderer::record_colour(
    const renderer::FrameInput& input, renderer::RenderState& state
) {
    if (!m_scene.has_xray()) {
        return {};
    }
    try {
        render(input, state);
        return {};
    } catch (const std::exception& failure) {
        return unexpected(Error{failure.what()});
    }
}

}  // namespace openrac::scene
