// The starting-light rig, as the renderer sees it. Wave Race's own: every course has a rig
// and nothing else is drawn the way they are, which is what the detector below relies on.
#include "start_rig.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace gx {

// Whether `b` draws the starting-light rig. See render_gl.h.
//
// Every course has a rig of its own -- vines on Lost Temple Lagoon, bamboo on Southern
// Island, a wooden frame on Aspen Lake, a scoreboard on Ocean City Harbor -- so nothing
// here may depend on the model. What they share is how the game draws them: in
// perspective through an identity position matrix, hung in view space a fixed distance in
// front of the camera (PixelState::view_space). That is not the only thing drawn that way
// -- Championship's opening screen, the one that cycles views of the course until A is
// pressed, and every course flyover draw the whole course through the identity too,
// transformed on the CPU -- but nothing else so drawn stays near the camera. GCN_RIGLOG on
// all five reachable courses:
//
//   the rigs                    10-33 draws, 1,686-2,898 vertices, no more than 100
//                               units across or 210 from the camera
//   opening screen, flyovers    150-600 draws, 29,000-85,000 vertices, 25,000 to 85,000
//                               units across
//
// and in a race, the rider close-ups, the results and the menus nothing is drawn that
// way at all. The thresholds sit far from both: within 2,000 units of the camera, ten
// times the farthest rig and a tenth of the nearest course, and at least 100 vertices,
// enough to ignore a stray quad and well under any rig.
bool batch_shows_start_rig(const Batch& b) {
    constexpr float kRigReach = 2000.0f;
    constexpr uint32_t kRigMinVerts = 100;
    uint32_t verts = 0;
    for (const Cmd& c : b.cmds) {
        if (c.type != CmdType::Draw) continue;
        const PixelState& st = b.states[c.state];
        if (!st.view_space || (int)st.proj[6] != 0) continue;
        for (uint32_t v = 0; v < c.count; v++) {
            const float* p = b.verts[b.indices[c.first + v]].pos;
            if (fabsf(p[0]) > kRigReach || fabsf(p[1]) > kRigReach || fabsf(p[2]) > kRigReach)
                return false;
        }
        verts += c.count;
    }
    return verts >= kRigMinVerts;
}

void rig_log(const Batch& b, bool rig) {
    static uint32_t frame = 0;
    frame++;
    int n = 0;
    uint32_t verts = 0;
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    std::vector<uint32_t> tex;
    for (const Cmd& c : b.cmds) {
        if (c.type != CmdType::Draw) continue;
        const PixelState& st = b.states[c.state];
        if (!st.view_space || (int)st.proj[6] != 0) continue;
        n++;
        verts += c.count;
        for (uint32_t v = 0; v < c.count; v++) {
            const float* p = b.verts[b.indices[c.first + v]].pos;
            for (int a = 0; a < 3; a++) {
                if (p[a] < lo[a]) lo[a] = p[a];
                if (p[a] > hi[a]) hi[a] = p[a];
            }
        }
        for (int t = 0; t < 8; t++)
            if (st.tex_id[t] && std::find(tex.begin(), tex.end(), st.tex_id[t]) == tex.end())
                tex.push_back(st.tex_id[t]);
    }
    static int last_rig = -1;
    if ((int)rig != last_rig) {
        last_rig = rig;
        fprintf(stderr, "[rig] frame %u: detector -> %d\n", frame, (int)rig);
    }
    static int last_n = -1;
    static uint32_t last_v = ~0u;
    if (n != last_n || verts != last_v || frame % 30 == 0) {
        last_n = n;
        last_v = verts;
        fprintf(stderr, "[rig] frame %u: %d draws %u verts", frame, n, verts);
        if (n)
            fprintf(stderr, " x %.0f..%.0f y %.0f..%.0f z %.0f..%.0f, %zu textures", lo[0], hi[0], lo[1], hi[1], lo[2],
                    hi[2], tex.size());
        fprintf(stderr, "\n");
    }
}

}  // namespace gx
