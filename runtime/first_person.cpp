// First person: the eye on the player's ski. This is the game project's half of the
// renderer's eye hook (gx::render_set_eye_hook): where the ski is, which draws are the
// rider, and how the eye should follow the hull are all facts about Wave Race, read off
// each batch here and handed back as a view transform and a list of draws to leave out.
// It is installed from a static initializer, so linking this file is what turns the
// renderer's first person on for this game.
#include "first_person.h"
#include "pace.h"
#include "gx/render_gl.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace gx;

namespace {

// The anchor is in the ski's own frame: x to the right, y up, z forward, in game units
// from the hull's origin. The defaults put the eye at the rider's head; WR_FP_ANCHOR=x,y,z
// overrides them on a desktop, where nothing else configures this.
float g_fp_anchor[3] = {0.0f, 57.5f, -46.5f};
// Where the eye was last frame, in the world's own frame (the one the world matrix maps
// into view space): see first_person_eye. Invalid until a hull has been found since first
// person was switched on, and after any frame without one, so the eye starts where the
// rider is rather than easing in from wherever it was.
struct FpEye { bool valid; float pos[3]; float yaw, pitch, roll; };
FpEye g_fp_eye;
float g_fp_height_s = 0.0f, g_fp_yaw_s = 0.1f, g_fp_tilt = 0.6f, g_fp_tilt_s = 0.15f;
uint32_t g_last_frame = ~0u;

void mat4_identity(float* m) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

// ---------------------------------------------------------------------------
// First person.
//
// The eye is fixed to the player's ski, which means finding the ski in the batch: the
// vertices arrive in the chase camera's view space, and the only thing that says which
// of them are the ski, and where it is, is the position matrix each draw went through.
//
// What a race frame looks like from that side, read off a GCN_MTXLOG dump of Ocean City
// Harbor at speed. Every draw of the main scene goes through the world's view matrix --
// the course, the water, the other racers (they are transformed into the world on the
// CPU) -- except the player's racer, which the game places with matrices of its own: one
// for the hull and three for parts of the rider, in GX_PNMTX1..4. The hull is the one
// with the largest extent, some 80 units long against the rider's 40. Its model frame is
// X to the ski's left, Y up and Z forward: the matrix's columns come out as view-space
// -X, world up and the direction of travel. The racer's reflection is a separate 128x128
// pass with ten matrices of its own, mirrored, and is left alone.
//
// So: among the draws an eye replays, the matrix most vertices go through is the
// world's; every other rigid matrix near the camera is a piece of the racer; the piece
// with the biggest bounding box is the hull, and the rest are the rider, who is not
// drawn. A dolphin or a boat with a matrix of its own would be taken for the rider if it
// came within reach, so a piece must also be textured with something the reflection
// pass drew -- the one place the batch says what the racer looks like.
// ---------------------------------------------------------------------------
static bool mtx_equal(const float* a, const float* b) { return memcmp(a, b, 12 * sizeof(float)) == 0; }

static bool mtx_is_rigid(const float* m) {
    // Columns of unit length and orthogonal: a rotation, not a scaled billboard.
    for (int c = 0; c < 3; c++) {
        const float len2 = m[c] * m[c] + m[4 + c] * m[4 + c] + m[8 + c] * m[8 + c];
        if (fabsf(len2 - 1.0f) > 0.05f) return false;
    }
    const float xy = m[0] * m[1] + m[4] * m[5] + m[8] * m[9];
    const float yz = m[1] * m[2] + m[5] * m[6] + m[9] * m[10];
    return fabsf(xy) < 0.05f && fabsf(yz) < 0.05f;
}


// Inverse of the 3x3 part of a GX row-major 3x4 matrix, as a row-major 3x3.
static bool mtx3_inverse(const float* m, float r[9]) {
    const float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], k = m[10];
    const float A = e * k - f * h, B = f * g - d * k, C = d * h - e * g;
    const float det = a * A + b * B + c * C;
    if (fabsf(det) < 1e-6f) return false;
    const float s = 1.0f / det;
    r[0] = A * s; r[1] = (c * h - b * k) * s; r[2] = (b * f - c * e) * s;
    r[3] = B * s; r[4] = (a * k - c * g) * s; r[5] = (c * d - a * f) * s;
    r[6] = C * s; r[7] = (b * g - a * h) * s; r[8] = (a * e - b * d) * s;
    return true;
}

// Sets out.view_to_eye, the view-to-eye transform, for a rider standing on a hull placed by
// `hm`, given the world matrix `wm`.
//
// Two attempts bracket what is wanted here. Fixed rigidly to the hull, every slap of a
// wave went straight into the viewer's head, at the game's 30 Hz. Levelled outright, with
// the height eased over 0.3 s, it was worse the other way: a camera flown along the
// course, cut off from the water, the waves doing nothing to it -- and since the eased
// height lagged the hull's, the ski rose through the viewer on every crest.
//
// So the eye's *position* is the rider's head on the hull exactly as it lies: no lag,
// which is the only thing that keeps the ski under the viewer, and the waves lift and
// drop it as they lift and drop him. What is eased is its *orientation*, in the world
// (the hull's pose is taken out of the chase camera's frame, which moves every frame
// too, through the world matrix; the world's up is its Y):
//
// - the heading follows the hull's over fp_yaw_s;
// - pitch and roll follow the hull's over fp_tilt_s, scaled by fp_tilt -- 1 rides with
//   the hull, 0 keeps the horizon level. Easing takes out the 30 Hz jolts while the
//   swell itself still comes through.
//
// fp_height_s eases the height as well, for anyone who wants it; it is 0 by default,
// for the reason above.
static void first_person_eye(const float* wm, const float* hm, const float anchor[3], gx::EyeOverride& out) {
    mat4_identity(out.view_to_eye);
    float wi[9];
    if (!mtx3_inverse(wm, wi)) return;
    auto to_world_dir = [&](float x, float y, float z, float out[3]) {
        for (int r = 0; r < 3; r++) out[r] = wi[r * 3] * x + wi[r * 3 + 1] * y + wi[r * 3 + 2] * z;
    };
    // The hull in the world. Its model frame is X to the ski's left, Y up and Z forward.
    float hp[3], hx[3], hy[3], hz[3];
    to_world_dir(hm[3] - wm[3], hm[7] - wm[7], hm[11] - wm[11], hp);
    to_world_dir(hm[0], hm[4], hm[8], hx);
    to_world_dir(hm[1], hm[5], hm[9], hy);
    to_world_dir(hm[2], hm[6], hm[10], hz);
    // Where the head is: on the hull as it actually lies. Anchor x is to the right, the
    // hull's -X.
    float target[3];
    for (int i = 0; i < 3; i++)
        target[i] = hp[i] - hx[i] * anchor[0] + hy[i] * anchor[1] + hz[i] * anchor[2];
    auto clamp1 = [](float v) { return v < -1.0f ? -1.0f : v > 1.0f ? 1.0f : v; };
    const float target_yaw = atan2f(hz[0], hz[2]);
    // Nose up is positive pitch; the right side rising is positive roll.
    const float target_pitch = asinf(clamp1(hz[1]));
    const float target_roll = asinf(clamp1(-hx[1]));

    const float dt = wr::frame_dt();  // once per game frame
    auto rate = [dt](float tau) { return tau > 0.0f ? 1.0f - expf(-dt / tau) : 1.0f; };
    FpEye& e = g_fp_eye;
    const float dx = target[0] - e.pos[0], dz = target[2] - e.pos[2];
    // A respawn after a crash moves the ski a long way in one frame; follow it there.
    if (!e.valid || dx * dx + dz * dz > 300.0f * 300.0f) {
        e.valid = true;
        memcpy(e.pos, target, sizeof(e.pos));
        e.yaw = target_yaw;
        e.pitch = target_pitch;
        e.roll = target_roll;
    } else {
        e.pos[0] = target[0];
        e.pos[2] = target[2];
        e.pos[1] += (target[1] - e.pos[1]) * rate(g_fp_height_s);
        float dyaw = target_yaw - e.yaw;
        while (dyaw > 3.14159265f) dyaw -= 6.2831853f;
        while (dyaw < -3.14159265f) dyaw += 6.2831853f;
        e.yaw += dyaw * rate(g_fp_yaw_s);
        e.pitch += (target_pitch - e.pitch) * rate(g_fp_tilt_s);
        e.roll += (target_roll - e.roll) * rate(g_fp_tilt_s);
    }
    // The eye's axes in the world: heading, then the scaled pitch, then the scaled roll
    // about the forward axis. Then into view space, where the vertices are.
    const float sy = sinf(e.yaw), cy = cosf(e.yaw);
    const float p = e.pitch * g_fp_tilt, r = e.roll * g_fp_tilt;
    const float sp = sinf(p), cp = cosf(p), sr = sinf(r), cr = cosf(r);
    const float fwd_w[3] = {sy * cp, sp, cy * cp};
    // Level right, which is square to the pitched forward too; up = right x forward.
    const float r0[3] = {-cy, 0.0f, sy};
    const float u0[3] = {r0[1] * fwd_w[2] - r0[2] * fwd_w[1], r0[2] * fwd_w[0] - r0[0] * fwd_w[2],
                         r0[0] * fwd_w[1] - r0[1] * fwd_w[0]};
    float right_w[3], up_w[3];
    for (int i = 0; i < 3; i++) {
        right_w[i] = r0[i] * cr + u0[i] * sr;
        up_w[i] = u0[i] * cr - r0[i] * sr;
    }
    auto to_view_dir = [&](const float v[3], float out[3]) {
        for (int r = 0; r < 3; r++) out[r] = wm[r * 4] * v[0] + wm[r * 4 + 1] * v[1] + wm[r * 4 + 2] * v[2];
    };
    float right[3], up[3], fwd[3], eye[3];
    to_view_dir(right_w, right);
    to_view_dir(up_w, up);
    to_view_dir(fwd_w, fwd);
    to_view_dir(e.pos, eye);
    eye[0] += wm[3]; eye[1] += wm[7]; eye[2] += wm[11];
    const float back[3] = {-fwd[0], -fwd[1], -fwd[2]};
    // The HUD's counter-tilt: the eye's level frame (same heading, the world's up) in the
    // eye's own coordinates. Column j is level axis j -- right, up, back -- seen from the
    // eye's right, up and back.
    {
        const float lvl[3][3] = {{-cy, 0.0f, sy}, {0.0f, 1.0f, 0.0f}, {-sy, 0.0f, -cy}};
        const float back_w[3] = {-fwd_w[0], -fwd_w[1], -fwd_w[2]};
        const float* eye_ax[3] = {right_w, up_w, back_w};
        mat4_identity(out.hud_to_eye);
        for (int j = 0; j < 3; j++)
            for (int i = 0; i < 3; i++)
                out.hud_to_eye[j * 4 + i] = eye_ax[i][0] * lvl[j][0] + eye_ax[i][1] * lvl[j][1] +
                                         eye_ax[i][2] * lvl[j][2];
    }
    float* C = out.view_to_eye;
    memset(C, 0, 16 * sizeof(float));
    C[0] = right[0]; C[4] = right[1]; C[8] = right[2];
    C[1] = up[0];    C[5] = up[1];    C[9] = up[2];
    C[2] = back[0];  C[6] = back[1];  C[10] = back[2];
    C[12] = -(right[0] * eye[0] + right[1] * eye[1] + right[2] * eye[2]);
    C[13] = -(up[0] * eye[0] + up[1] * eye[1] + up[2] * eye[2]);
    C[14] = -(back[0] * eye[0] + back[1] * eye[1] + back[2] * eye[2]);
    C[15] = 1.0f;
}

// The eye hook: finds the racer in `b`, and fills in the eye and the draws to hide.
// `skip` marks the off-screen passes, as the renderer leaves them.
static bool eye_hook(const Batch& b, const std::vector<uint8_t>& skip, uint32_t frame, gx::EyeOverride& out) {
    out.hide.clear();
    mat4_identity(out.hud_to_eye);
    // A gap in the frames means first person was off in between: start the eye where the
    // rider is rather than easing in from wherever it was.
    if (frame != g_last_frame + 1) g_fp_eye.valid = false;
    g_last_frame = frame;
    static const bool fplog = getenv("WR_FPLOG") != nullptr;
    // The distinct matrices the main scene's world draws go through, with how many
    // vertices each carries.
    struct Group { const float* m; uint32_t verts; float lo[3], hi[3]; bool racer; };
    std::vector<Group> groups;
    std::vector<uint32_t> group_of(b.cmds.size(), UINT32_MAX);
    for (size_t i = 0; i < b.cmds.size(); i++) {
        const Cmd& c = b.cmds[i];
        if (c.type != CmdType::Draw || skip[i]) continue;
        const PixelState& st = b.states[c.state];
        if ((int)st.proj[6] != 0 || st.view_space) continue;
        const float* m = &b.mtxs[c.mtx * 12];
        uint32_t g = UINT32_MAX;
        for (size_t k = 0; k < groups.size(); k++)
            if (mtx_equal(groups[k].m, m)) { g = (uint32_t)k; break; }
        if (g == UINT32_MAX) {
            g = (uint32_t)groups.size();
            groups.push_back({m, 0, {1e30f, 1e30f, 1e30f}, {-1e30f, -1e30f, -1e30f}, false});
        }
        groups[g].verts += c.count;
        group_of[i] = g;
    }
    if (groups.empty()) {
        return false;
    }
    uint32_t world = 0;
    for (size_t k = 1; k < groups.size(); k++)
        if (groups[k].verts > groups[world].verts) world = (uint32_t)k;
    // Within reach of the camera: the chase camera keeps the hull some 150 units ahead.
    constexpr float kReach = 600.0f;
    auto near_camera = [](const float* m) {
        return m[3] * m[3] + m[7] * m[7] + m[11] * m[11] < kReach * kReach;
    };
    // What the racer is textured with: whatever an off-screen pass drew near the camera
    // through a rigid matrix that is not the world's, which is the reflection pass and
    // nothing else. The 480x480 pass is the sky through a rotation at 10,000 units, and
    // the reflection reads it as the environment, so the sky's texture is on the racer
    // whichever way the set is built; what keeps the sky itself out is its size, below.
    std::vector<uint32_t> racer_tex;
    for (size_t i = 0; i < b.cmds.size(); i++) {
        const Cmd& c = b.cmds[i];
        if (c.type != CmdType::Draw || !skip[i]) continue;
        const PixelState& st = b.states[c.state];
        const float* m = &b.mtxs[c.mtx * 12];
        if (st.view_space || !near_camera(m) || !mtx_is_rigid(m) || mtx_equal(m, groups[world].m))
            continue;
        for (int t = 0; t < 8; t++)
            if (st.tex_id[t] && !st.tex_is_efb[t]) racer_tex.push_back(st.tex_id[t]);
    }
    // A piece of the racer: rigid, within reach of the camera, textured like one, and no
    // bigger than a ski. No reflection pass means no racer. Each of those was learned
    // from a false positive: without the texture test a menu's full-screen quad, drawn
    // with a perspective projection through a matrix turned a quarter turn, passed as a
    // hull 640 units across; and without the size cap the sky did, a rotation at the
    // origin 8,000 units wide wearing the same environment map the ski's paint reflects.
    constexpr float kPieceMax = 300.0f;
    for (size_t k = 0; k < groups.size() && !racer_tex.empty(); k++) {
        Group& g = groups[k];
        if (k == world || !mtx_is_rigid(g.m) || !near_camera(g.m)) continue;
        bool textured = false;
        for (size_t i = 0; i < b.cmds.size() && !textured; i++) {
            if (group_of[i] != k) continue;
            const PixelState& st = b.states[b.cmds[i].state];
            for (int t = 0; t < 8 && !textured; t++)
                for (uint32_t id : racer_tex) if (id == st.tex_id[t]) { textured = true; break; }
        }
        g.racer = textured;
    }
    // Each piece's extent in its own frame -- across, up and along its matrix's axes --
    // which is what tells a hull from a buoy: the hull is 28 units across and 82 long,
    // a buoy about 90 every way. The columns are unit length to within what
    // mtx_is_rigid allows, so projecting onto them is good enough for a size.
    for (size_t i = 0; i < b.cmds.size(); i++) {
        const uint32_t k = group_of[i];
        if (k == UINT32_MAX || !groups[k].racer) continue;
        const Cmd& c = b.cmds[i];
        const float* m = groups[k].m;
        for (uint32_t v = 0; v < c.count; v++) {
            const float* p = b.verts[b.indices[c.first + v]].pos;
            const float d[3] = {p[0] - m[3], p[1] - m[7], p[2] - m[11]};
            for (int a = 0; a < 3; a++) {
                const float e = d[0] * m[a] + d[1] * m[4 + a] + d[2] * m[8 + a];
                if (e < groups[k].lo[a]) groups[k].lo[a] = e;
                if (e > groups[k].hi[a]) groups[k].hi[a] = e;
            }
        }
    }
    auto size2 = [](const Group& g) {
        float d2 = 0.0f;
        for (int a = 0; a < 3; a++) d2 += (g.hi[a] - g.lo[a]) * (g.hi[a] - g.lo[a]);
        return d2;
    };
    for (auto& g : groups)
        if (g.racer && size2(g) > kPieceMax * kPieceMax) g.racer = false;
    // The hull is the longest piece that is at least twice as long as it is wide. The
    // course intro flies the camera past a buoy, which is rigid, within reach, textured
    // like the reflection pass's buoys and bigger than the hull; it was the hull for two
    // seconds before the shape was asked for. The rider is every piece placed near the
    // hull: the rider's parts have their origins within 40 units of the hull's.
    uint32_t hull = UINT32_MAX;
    float best = -1.0f;
    for (size_t k = 0; k < groups.size(); k++) {
        const Group& g = groups[k];
        if (!g.racer) continue;
        const float across = g.hi[0] - g.lo[0], along = g.hi[2] - g.lo[2];
        if (along >= 2.0f * across && along > best) { best = along; hull = (uint32_t)k; }
    }
    if (hull == UINT32_MAX) {
        if (fplog) fprintf(stderr, "[fp] f%u no racer: %zu matrices, world has %u verts, %zu racer textures\n",
                           frame, groups.size(), groups[world].verts, racer_tex.size());
        g_fp_eye.valid = false;
        return false;
    }
    // Which draws are the rider. Two kinds, and the first version only knew one:
    //
    // - His body is skinned by the game on the CPU and drawn through the *world* matrix,
    //   like the course. Nothing about its matrix says it is his, so it is found by what
    //   it is textured with -- what the reflection pass draws him in -- and by where it is.
    // - His head is a rigid piece of its own, beside the ski's: the hull, the steering
    //   pole and the handlebar each have a matrix too (the pole hinges, so it moves
    //   against the hull). The head is the one that shares a texture with the body -- the
    //   skin -- which none of the ski's pieces do.
    //
    // Hiding every rigid piece but the hull, which is what the first version did, took the
    // handlebars off the ski and left the rider standing on it headless.
    const float* hm = groups[hull].m;
    constexpr float kRiderReach = 90.0f;   // from the hull's origin, in game units
    auto near_hull = [&](const Cmd& c) {
        float sum[3] = {0.0f, 0.0f, 0.0f};
        for (uint32_t v = 0; v < c.count; v++) {
            const float* p = b.verts[b.indices[c.first + v]].pos;
            for (int a = 0; a < 3; a++) sum[a] += p[a];
        }
        const float n = c.count ? (float)c.count : 1.0f;
        const float dx = sum[0] / n - hm[3], dy = sum[1] / n - hm[7], dz = sum[2] / n - hm[11];
        return dx * dx + dy * dy + dz * dz < kRiderReach * kRiderReach;
    };
    auto in_set = [](const std::vector<uint32_t>& set, uint32_t id) {
        for (uint32_t x : set) if (x == id) return true;
        return false;
    };
    out.hide.assign(b.cmds.size(), 0);
    // The body starts from the draws wearing something the reflection pass drew, and then
    // takes in every nearby world draw that shares a texture with what it has so far,
    // until nothing more joins. The second step is not optional: a few of the body's
    // draws are textured only with the rider's shading maps (four of them, which the game
    // loads after his model), and the reflection pass never uses those. Matching on the
    // reflection's textures alone left those draws in -- strips of the rider hanging in
    // the air where he had been. Each of them also wears a map the rest of the body does.
    std::vector<uint32_t> body_tex;
    int body_draws = 0;
    std::vector<uint8_t> near(b.cmds.size(), 0);
    for (size_t i = 0; i < b.cmds.size(); i++)
        if (group_of[i] == world && near_hull(b.cmds[i])) near[i] = 1;
    auto take = [&](size_t i) {
        const PixelState& st = b.states[b.cmds[i].state];
        out.hide[i] = 1;
        body_draws++;
        for (int t = 0; t < 8; t++)
            if (st.tex_id[t] && !st.tex_is_efb[t] && !in_set(body_tex, st.tex_id[t])) body_tex.push_back(st.tex_id[t]);
    };
    for (size_t i = 0; i < b.cmds.size(); i++) {
        if (!near[i]) continue;
        const PixelState& st = b.states[b.cmds[i].state];
        for (int t = 0; t < 8; t++)
            if (st.tex_id[t] && !st.tex_is_efb[t] && in_set(racer_tex, st.tex_id[t])) { take(i); break; }
    }
    for (bool grew = true; grew;) {
        grew = false;
        for (size_t i = 0; i < b.cmds.size(); i++) {
            if (!near[i] || out.hide[i]) continue;
            const PixelState& st = b.states[b.cmds[i].state];
            for (int t = 0; t < 8; t++)
                if (st.tex_id[t] && !st.tex_is_efb[t] && in_set(body_tex, st.tex_id[t])) {
                    take(i);
                    grew = true;
                    break;
                }
        }
    }
    // The hull's own textures are the ski's, whatever else wears them.
    std::vector<uint32_t> hull_tex;
    for (size_t i = 0; i < b.cmds.size(); i++) {
        if (group_of[i] != hull) continue;
        const PixelState& st = b.states[b.cmds[i].state];
        for (int t = 0; t < 8; t++) if (st.tex_id[t]) hull_tex.push_back(st.tex_id[t]);
    }
    std::vector<uint8_t> rider_piece(groups.size(), 0);
    for (size_t k = 0; k < groups.size(); k++) {
        const Group& g = groups[k];
        if (!g.racer || k == hull) continue;
        const float dx = g.m[3] - hm[3], dy = g.m[7] - hm[7], dz = g.m[11] - hm[11];
        if (dx * dx + dy * dy + dz * dz > kRiderReach * kRiderReach) continue;
        for (size_t i = 0; i < b.cmds.size() && !rider_piece[k]; i++) {
            if (group_of[i] != k) continue;
            const PixelState& st = b.states[b.cmds[i].state];
            for (int t = 0; t < 8; t++)
                if (st.tex_id[t] && in_set(body_tex, st.tex_id[t]) && !in_set(hull_tex, st.tex_id[t])) {
                    rider_piece[k] = 1;
                    break;
                }
        }
    }
    int hidden = body_draws;
    for (size_t i = 0; i < b.cmds.size(); i++) {
        const uint32_t k = group_of[i];
        if (k != UINT32_MAX && rider_piece[k]) { out.hide[i] = 1; hidden++; }
    }
    first_person_eye(groups[world].m, hm, g_fp_anchor, out);
    if (fplog) {
        for (size_t k = 0; k < groups.size(); k++) {
            const Group& g = groups[k];
            if (!g.racer || k == hull) continue;
            fprintf(stderr, "[fp]   %s piece at (%.1f, %.1f, %.1f) across/up/along %.0fx%.0fx%.0f, %u verts\n",
                    rider_piece[k] ? "rider" : "ski", g.m[3], g.m[7], g.m[11],
                    g.hi[0] - g.lo[0], g.hi[1] - g.lo[1], g.hi[2] - g.lo[2], g.verts);
        }
        const float* m = groups[hull].m;
        fprintf(stderr, "[fp] f%u hull at (%.1f, %.1f, %.1f) fwd=(%.2f, %.2f, %.2f) up=(%.2f, %.2f, %.2f)"
                " across/up/along %.0fx%.0fx%.0f, %u verts; eye at world (%.1f, %.1f, %.1f) yaw %.1f;"
                " %d body draws (%zu textures), %d hidden in all\n",
                frame, m[3], m[7], m[11], m[2], m[6], m[10], m[1], m[5], m[9],
                groups[hull].hi[0] - groups[hull].lo[0], groups[hull].hi[1] - groups[hull].lo[1],
                groups[hull].hi[2] - groups[hull].lo[2], groups[hull].verts,
                g_fp_eye.pos[0], g_fp_eye.pos[1], g_fp_eye.pos[2], g_fp_eye.yaw * 57.2958f,
                body_draws, body_tex.size(), hidden);
    }
    return true;
}

const bool s_installed = [] {
    if (const char* a = getenv("WR_FP_ANCHOR")) sscanf(a, "%f,%f,%f", &g_fp_anchor[0], &g_fp_anchor[1], &g_fp_anchor[2]);
    render_set_eye_hook(eye_hook);
    return true;
}();

}  // namespace

namespace wr {

void first_person_configure(const float anchor[3], float height_s, float yaw_s, float tilt, float tilt_s) {
    memcpy(g_fp_anchor, anchor, sizeof(g_fp_anchor));
    g_fp_height_s = height_s;
    g_fp_yaw_s = yaw_s;
    g_fp_tilt = tilt;
    g_fp_tilt_s = tilt_s;
}

}  // namespace wr
