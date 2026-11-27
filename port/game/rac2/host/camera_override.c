/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (c) 2026 the OpenRAC contributors */

/*
 * Going Commando: a mod's camera (openrac_api.camera_set and camera_view, interface 1.12), as
 * Ratchet & Clank's (port/game/rac1/host/camera_override.c).
 *
 * Each frame's world is drawn by DrawWorld (func_0027BD70), whose first call is BuildViewMatrices
 * (func_00279D18): it reads the camera block's rotation rows forward, left, up (g_cameraMatrix) and
 * its eye (g_cameraPos, 0x230 before them) and makes the view, the clip and cull matrices from them,
 * and the native renderers read the same eye and rows for the world they draw (render.c). The game's
 * cameras (the follow camera, a vendor's menu, the scenes) write the block before DrawWorld runs. A
 * mod's camera is written over it just before DrawWorld, so the picture, the game's matrices and
 * what the game's logic reads of its camera after it all see that camera; what the game wrote
 * itself is kept first (camera_view). The previews the game renders to a texture inside the frame
 * (RenderObjectPreviewToTexture, func_0027BA68: the vendor's weapon) save the block, build their own
 * view and put it back, so they keep theirs.
 *
 * It is written while the level plays (the game's mode word 0) and under a vendor's menu (5), whose
 * own camera it then replaces; never under the pause menu (3) or a scene, which set themselves up
 * from the camera they find. It goes with the level, or when the mod lets it go.
 */
#include <stdint.h>
#include <string.h>

#include "game_protos.h"
#include "openrac/game_host.h"
#include "openrac/guest.h"
#include "rac2.h"

#define CAMERA_EYE OPENRAC_DATA(RAC2_g_cameraPos)
#define CAMERA_ROWS OPENRAC_DATA(RAC2_g_cameraMatrix)
/* The game's mode word (mode_apply_pending_change): 0 play, 3 the pause menu, 5 a vendor's menu. */
#define GAME_MODE OPENRAC_DATA(RAC2_g_nGameState)
#define MODE_PLAY 0
#define MODE_VENDOR 5
#define EXIT_REQUESTED OPENRAC_DATA(RAC2_g_nLevelExitRequested)

static struct {
    int on;
    uint32_t loads;
    float eye[3], rows[9];
} mod_camera;

static struct {
    int seen;
    uint32_t loads;
    float eye[3], rows[9];
} game_camera;

static int level_running(void) {
    const int level = openrac_game_loaded_overlay();
    return level >= 0 && level < RAC2_LEVELS && GREF(int32_t, EXIT_REQUESTED) == 0;
}

static int mode_takes_camera(void) {
    const int32_t mode = GREF(int32_t, GAME_MODE);
    return mode == MODE_PLAY || mode == MODE_VENDOR;
}

int rac2_camera_overridden(void) {
    return level_running() && mod_camera.on && mod_camera.loads == rac2_program_loads()
           && mode_takes_camera();
}

int rac2_camera_set(const float eye[3], const float rows[9]) {
    if (eye == NULL || rows == NULL) {
        mod_camera.on = 0;
        return 1;
    }
    if (!level_running()) {
        return 0;
    }
    memcpy(mod_camera.eye, eye, sizeof mod_camera.eye);
    memcpy(mod_camera.rows, rows, sizeof mod_camera.rows);
    mod_camera.loads = rac2_program_loads();
    mod_camera.on = 1;
    return 1;
}

int rac2_camera_view(float eye[3], float rows[9]) {
    if (!level_running() || !game_camera.seen || game_camera.loads != rac2_program_loads()) {
        return 0;
    }
    memcpy(eye, game_camera.eye, sizeof game_camera.eye);
    memcpy(rows, game_camera.rows, sizeof game_camera.rows);
    return 1;
}

/* The level camera's shakes (openrac_api.camera_shake, interface 1.33): the camera block's three
 * CameraShake records from +0x160 (amplitude, offset, frames, longest; 0x10 bytes each), which
 * cam_update_shake (func_00271958) plays each frame, channels 0 and 1 moving the eye along two of
 * the camera's rows (its modes 0 and 1). The game's own triggers write the amplitude and the frames, in ticks
 * (part_emit_blast_and_shake: 0.1 for 40), and clear the longest. */
#define CAMERA_SHAKES OPENRAC_DATA(RAC2_g_cameraPos + 0x20u)

int rac2_camera_shake(int channel, float amplitude, int frames) {
    if (!level_running() || (channel != 0 && channel != 1)
        || !(amplitude >= 0.0f && amplitude <= 1.0f) || frames < 0 || frames > 600) {
        return 0;
    }
    const gaddr base = CAMERA_SHAKES + 0x10u * (unsigned)channel;
    GREF(float, base) = amplitude;
    GREF(int32_t, base + 8u) = frames;
    GREF(int32_t, base + 12u) = 0;
    return 1;
}

/* DrawWorld: the game's camera kept, then the mod's written over it. */
void func_0027BD70(void) {
    if (level_running() && mode_takes_camera()) {
        const float* eye = (const float*)G(CAMERA_EYE);
        const float* r = (const float*)G(CAMERA_ROWS);
        memcpy(game_camera.eye, eye, sizeof game_camera.eye);
        for (int i = 0; i < 3; ++i) {
            memcpy(game_camera.rows + 3 * i, r + 4 * i, 3 * sizeof(float));
        }
        game_camera.loads = rac2_program_loads();
        game_camera.seen = 1;
        if (mod_camera.on && mod_camera.loads != game_camera.loads) {
            mod_camera.on = 0;
        }
        if (mod_camera.on) {
            float* e = (float*)G(CAMERA_EYE);
            float* w = (float*)G(CAMERA_ROWS);
            memcpy(e, mod_camera.eye, sizeof mod_camera.eye);
            for (int i = 0; i < 3; ++i) {
                memcpy(w + 4 * i, mod_camera.rows + 3 * i, 3 * sizeof(float));
            }
        }
    }
    func_0027BD70__game();
}
