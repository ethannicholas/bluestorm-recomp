// Generates GLSL for the GX pixel pipeline (TEV) from BP state.
#include "shadergen.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <cstdlib>

namespace gx {

ShaderKey make_shader_key(const PixelState& st) {
    ShaderKey k;
    memset(&k, 0, sizeof(k));
    const uint32_t* bp = st.bp;
    k.genmode = bp[0x00] & ~0xC000u;  // without cull mode
    uint32_t nstages = ((bp[0x00] >> 10) & 15) + 1;
    uint32_t nind = (bp[0x00] >> 16) & 7;
    for (uint32_t i = 0; i < (nstages + 1) / 2; i++) k.order[i] = bp[0x28 + i];
    for (uint32_t i = 0; i < nstages; i++) {
        k.cenv[i] = bp[0xC0 + 2 * i];
        k.aenv[i] = bp[0xC1 + 2 * i];
        k.indcmd[i] = bp[0x10 + i];
    }
    for (int i = 0; i < 8; i++) k.ksel[i] = bp[0xF6 + i];
    k.alpha_func = (bp[0xF3] >> 16) & 0xFF;
    k.iref = nind ? bp[0x27] & ((1u << (6 * nind)) - 1) : 0;
    k.fog = (bp[0xF1] >> 20) & 0xF;  // proj bit + fsel
    k.num_texgens = st.num_texgens;
    k.efb_has_alpha = (bp[0x43] & 7) == 1;
    uint32_t used = 0;
    for (uint32_t s = 0; s < nstages; s++) {
        uint32_t order = bp[0x28 + s / 2] >> ((s & 1) * 12);
        if (order & 0x40) used |= 1u << (order & 7);
    }
    for (uint32_t i = 0; i < nind; i++) used |= 1u << ((bp[0x27] >> (6 * i)) & 7);
    for (int m = 0; m < 8; m++)
        if ((used & (1u << m)) && st.tex_is_efb[m]) k.efb_tex_mask |= 1u << m;
    return k;
}

static const char* kColorIn[16] = {
    "prev.rgb", "prev.aaa", "c0.rgb", "c0.aaa", "c1.rgb", "c1.aaa", "c2.rgb", "c2.aaa",
    "tex.rgb", "tex.aaa", "ras.rgb", "ras.aaa", "ivec3(255)", "ivec3(128)", "konst.rgb", "ivec3(0)"};
static const char* kAlphaIn[8] = {"prev.a", "c0.a", "c1.a", "c2.a", "tex.a", "ras.a", "konst.a", "0"};
static const char* kDest[4] = {"prev", "c0", "c1", "c2"};

static std::string konst_color(uint32_t sel) {
    static const int frac[8] = {255, 223, 191, 159, 128, 96, 64, 32};
    char b[64];
    if (sel < 8) { snprintf(b, sizeof(b), "ivec3(%d)", frac[sel]); return b; }
    if (sel >= 12 && sel <= 15) { snprintf(b, sizeof(b), "u_konst[%u].rgb", sel - 12); return b; }
    if (sel >= 16) {
        static const char* comp = "rgba";
        snprintf(b, sizeof(b), "ivec3(u_konst[%u].%c)", (sel - 16) & 3, comp[(sel - 16) >> 2]);
        return b;
    }
    return "ivec3(255)";
}
static std::string konst_alpha(uint32_t sel) {
    static const int frac[8] = {255, 223, 191, 159, 128, 96, 64, 32};
    char b[64];
    if (sel < 8) { snprintf(b, sizeof(b), "%d", frac[sel]); return b; }
    if (sel >= 16) {
        static const char* comp = "rgba";
        snprintf(b, sizeof(b), "u_konst[%u].%c", (sel - 16) & 3, comp[(sel - 16) >> 2]);
        return b;
    }
    return "255";
}

static std::string swizzle(const ShaderKey& k, uint32_t table) {
    // Swap table n lives in ksel[2n] (r,g) and ksel[2n+1] (b,a).
    static const char* comp = "rgba";
    uint32_t a = k.ksel[table * 2], b = k.ksel[table * 2 + 1];
    std::string s;
    s += comp[a & 3];
    s += comp[(a >> 2) & 3];
    s += comp[b & 3];
    s += comp[(b >> 2) & 3];
    return s;
}

static const char* kCompare[8] = {"false", "(%s < %s)", "(%s == %s)", "(%s <= %s)", "(%s > %s)", "(%s != %s)", "(%s >= %s)", "true"};

std::string gen_vertex_shader() {
    return R"(#version 410 core
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec4 a_col0;
layout(location = 2) in vec4 a_col1;
layout(location = 3) in vec3 a_tex[8];
uniform mat4 u_proj;
uniform vec4 u_vp_a;   // x: 2*(offx-342)/w - 1, y: 2*sx/w, z: 2*(offy-342)/h - 1, w: 2*sy/h
uniform vec4 u_vp_b;   // x: 2*offz/16777215 - 1, y: 2*sz/16777215
uniform float u_point_size;
out vec4 v_col0;
out vec4 v_col1;
out vec3 v_tex[8];
void main() {
    vec4 clip = u_proj * vec4(a_pos, 1.0);
    gl_Position.x = u_vp_a.x * clip.w + u_vp_a.y * clip.x;
    gl_Position.y = -(u_vp_a.z * clip.w + u_vp_a.w * clip.y);
    gl_Position.z = u_vp_b.x * clip.w + u_vp_b.y * clip.z;
    gl_Position.w = clip.w;
    gl_PointSize = u_point_size;
    v_col0 = a_col0;
    v_col1 = a_col1;
    for (int i = 0; i < 8; i++) v_tex[i] = a_tex[i];
}
)";
}

std::string gen_pixel_shader(const ShaderKey& k) {
    std::string s;
    char buf[1024];
    auto W = [&](const char* fmt, auto... args) {
        snprintf(buf, sizeof(buf), fmt, args...);
        s += buf;
    };
    uint32_t nstages = ((k.genmode >> 10) & 15) + 1;
    uint32_t nind = (k.genmode >> 16) & 7;

    s += "#version 410 core\n";
    s += "in vec4 v_col0;\nin vec4 v_col1;\nin vec3 v_tex[8];\n";
    s += "uniform sampler2D u_tex[8];\n";
    s += "uniform ivec4 u_reg[4];\nuniform ivec4 u_konst[4];\n";
    s += "uniform vec2 u_texsize[8];\n";
    s += "uniform ivec3 u_indmtx[6];\n";  // 3 matrices x 2 rows (11-bit signed entries)
    s += "uniform int u_indscale[3];\n";
    s += "uniform ivec2 u_alpharef;\n";
    s += "uniform vec4 u_fog;\n";       // A, C, bmag, bshift
    s += "uniform vec3 u_fogcolor;\n";
    s += "layout(location = 0) out vec4 o_color;\n";
    s += "ivec4 sample_tex(int m, vec2 uv) { return ivec4(round(texture(u_tex[m], uv) * 255.0)); }\n";
    s += "void main() {\n";
    s += "  ivec4 prev = u_reg[0], c0 = u_reg[1], c1 = u_reg[2], c2 = u_reg[3];\n";
    s += "  ivec4 col0 = ivec4(round(v_col0 * 255.0)), col1 = ivec4(round(v_col1 * 255.0));\n";
    s += "  ivec4 tex = ivec4(0), ras = ivec4(0), konst = ivec4(0);\n";
    s += "  int alphabump = 0;\n";
    s += "  vec2 ind_prev = vec2(0.0);\n";

    // texcoords (projective divide)
    for (uint32_t i = 0; i < 8; i++) {
        if (i < k.num_texgens) W("  vec2 uv%u = v_tex[%u].xy / (v_tex[%u].z == 0.0 ? 1.0 : v_tex[%u].z);\n", i, i, i, i);
        else W("  vec2 uv%u = vec2(0.0);\n", i);
    }
    // indirect stages
    for (uint32_t i = 0; i < nind; i++) {
        uint32_t map = (k.iref >> (6 * i)) & 7, coord = (k.iref >> (6 * i + 3)) & 7;
        W("  ivec4 indtex%u = sample_tex(%u, uv%u * u_indscalef(%u));\n", i, map, coord, i);
    }

    for (uint32_t st = 0; st < nstages; st++) {
        uint32_t order = k.order[st / 2] >> ((st & 1) * 12);
        uint32_t map = order & 7, coord = (order >> 3) & 7;
        bool tex_en = (order >> 6) & 1;
        uint32_t chan = (order >> 7) & 7;
        uint32_t cenv = k.cenv[st], aenv = k.aenv[st];
        uint32_t ind = k.indcmd[st];
        W("  // stage %u\n  {\n", st);
        // ---- indirect texture offset ----
        W("    vec2 tc = uv%u * u_texsize[%u];\n", coord, map);
        uint32_t bt = ind & 3, fmt = (ind >> 2) & 3, bias = (ind >> 4) & 7, bs = (ind >> 7) & 3, mid = (ind >> 9) & 15;
        uint32_t sw = (ind >> 13) & 7, tw = (ind >> 16) & 7;
        bool fb = (ind >> 20) & 1;
        if (bt < nind && (mid != 0 || bs != 0)) {
            static const int mask[4] = {255, 31, 15, 7};
            W("    ivec3 icrd = indtex%u.abg & %d;\n", bt, mask[fmt]);
            if (bs) {
                static const char* bsc[4] = {"", "a", "b", "g"};
                static const int bmask[4] = {0xF8, 0xE0, 0xF0, 0xF8};
                W("    alphabump = indtex%u.%s & %d;\n", bt, bsc[bs], bmask[fmt]);
            }
            int b = fmt == 0 ? -128 : 1;
            if (bias & 1) W("    icrd.x += %d;\n", b);
            if (bias & 2) W("    icrd.y += %d;\n", b);
            if (bias & 4) W("    icrd.z += %d;\n", b);
            if (mid >= 1 && mid <= 3) {
                uint32_t m = mid - 1;
                W("    vec2 ioff = vec2(dot(vec3(u_indmtx[%u]), vec3(icrd)), dot(vec3(u_indmtx[%u]), vec3(icrd))) * exp2(float(u_indscale[%u])) / 1024.0;\n",
                  m * 2, m * 2 + 1, m);
            } else if (mid >= 5 && mid <= 7) {
                W("    vec2 ioff = tc.xx * vec2(icrd.xy) / 256.0 * exp2(float(u_indscale[%u]));\n", mid - 5);
            } else if (mid >= 9 && mid <= 11) {
                W("    vec2 ioff = tc.yy * vec2(icrd.xy) / 256.0 * exp2(float(u_indscale[%u]));\n", mid - 9);
            } else {
                W("    vec2 ioff = vec2(0.0);\n");
            }
        } else {
            W("    vec2 ioff = vec2(0.0);\n");
        }
        static const float wrapsz[8] = {0, 256, 128, 64, 32, 16, 0.001f, 0};
        if (sw == 6) W("    tc.x = 0.0;\n");
        else if (sw) W("    tc.x = mod(tc.x, %.1f);\n", wrapsz[sw]);
        if (tw == 6) W("    tc.y = 0.0;\n");
        else if (tw) W("    tc.y = mod(tc.y, %.1f);\n", wrapsz[tw]);
        if (fb) W("    ioff += ind_prev;\n");
        W("    ind_prev = ioff;\n");
        W("    tc += ioff;\n");
        // ---- texture ----
        if (tex_en) {
            uint32_t tswap = (aenv >> 2) & 3;
            W("    tex = sample_tex(%u, tc / u_texsize[%u]).%s;\n", map, map, swizzle(k, tswap).c_str());
        } else {
            W("    tex = ivec4(255);\n");
        }
        // ---- rasterized color ----
        {
            uint32_t rswap = aenv & 3;
            std::string sw_ = swizzle(k, rswap);
            switch (chan) {
            case 0: W("    ras = col0.%s;\n", sw_.c_str()); break;
            case 1: W("    ras = col1.%s;\n", sw_.c_str()); break;
            case 5: W("    ras = ivec4(alphabump);\n"); break;
            case 6: W("    ras = ivec4(alphabump | (alphabump >> 5));\n"); break;
            default: W("    ras = ivec4(0);\n"); break;
            }
        }
        // ---- konst ----
        {
            uint32_t ks = k.ksel[st / 2];
            uint32_t kc = (st & 1) ? (ks >> 14) & 31 : (ks >> 4) & 31;
            uint32_t ka = (st & 1) ? (ks >> 19) & 31 : (ks >> 9) & 31;
            W("    konst = ivec4(%s, %s);\n", konst_color(kc).c_str(), konst_alpha(ka).c_str());
        }
        // ---- color combiner ----
        {
            uint32_t d = cenv & 15, c = (cenv >> 4) & 15, b = (cenv >> 8) & 15, a = (cenv >> 12) & 15;
            uint32_t bias_ = (cenv >> 16) & 3, op = (cenv >> 18) & 1, clamp = (cenv >> 19) & 1, scale = (cenv >> 20) & 3, dest = (cenv >> 22) & 3;
            W("    ivec3 ca = %s & 255, cb = %s & 255, cc = %s & 255, cd = %s;\n", kColorIn[a], kColorIn[b], kColorIn[c], kColorIn[d]);
            if (bias_ != 3) {
                static const char* biast[3] = {"", " + 128", " - 128"};
                static const char* sl[4] = {"", " << 1", " << 2", ""};
                static const char* lb[4] = {"", " + 128", "", " + 127"};
                W("    ivec3 cr = (((cd%s)%s) %c (((((ca << 8) + (cb - ca) * (cc + (cc >> 7)))%s)%s) >> 8))%s;\n",
                  biast[bias_], sl[scale], op ? '-' : '+', sl[scale], lb[2 * op + (scale != 3 ? 1 : 0)], scale == 3 ? " >> 1" : "");
            } else {
                uint32_t mode = (scale << 1) | op;
                switch (mode) {
                case 0: W("    ivec3 cr = cd + ((ca.r > cb.r) ? cc : ivec3(0));\n"); break;
                case 1: W("    ivec3 cr = cd + ((ca.r == cb.r) ? cc : ivec3(0));\n"); break;
                case 2: W("    ivec3 cr = cd + (((ca.g << 8 | ca.r) > (cb.g << 8 | cb.r)) ? cc : ivec3(0));\n"); break;
                case 3: W("    ivec3 cr = cd + (((ca.g << 8 | ca.r) == (cb.g << 8 | cb.r)) ? cc : ivec3(0));\n"); break;
                case 4: W("    ivec3 cr = cd + (((ca.b << 16 | ca.g << 8 | ca.r) > (cb.b << 16 | cb.g << 8 | cb.r)) ? cc : ivec3(0));\n"); break;
                case 5: W("    ivec3 cr = cd + (((ca.b << 16 | ca.g << 8 | ca.r) == (cb.b << 16 | cb.g << 8 | cb.r)) ? cc : ivec3(0));\n"); break;
                case 6: W("    ivec3 cr = cd + ivec3(greaterThan(ca, cb)) * cc;\n"); break;
                case 7: W("    ivec3 cr = cd + ivec3(equal(ca, cb)) * cc;\n"); break;
                }
            }
            if (clamp) W("    %s.rgb = clamp(cr, 0, 255);\n", kDest[dest]);
            else W("    %s.rgb = clamp(cr, -1024, 1023);\n", kDest[dest]);
        }
        // ---- alpha combiner ----
        {
            uint32_t d = (aenv >> 4) & 7, c = (aenv >> 7) & 7, b = (aenv >> 10) & 7, a = (aenv >> 13) & 7;
            uint32_t bias_ = (aenv >> 16) & 3, op = (aenv >> 18) & 1, clamp = (aenv >> 19) & 1, scale = (aenv >> 20) & 3, dest = (aenv >> 22) & 3;
            W("    int aa = %s & 255, ab = %s & 255, ac = %s & 255, ad = %s;\n", kAlphaIn[a], kAlphaIn[b], kAlphaIn[c], kAlphaIn[d]);
            if (bias_ != 3) {
                static const char* biast[3] = {"", " + 128", " - 128"};
                static const char* sl[4] = {"", " << 1", " << 2", ""};
                static const char* lb[4] = {"", " + 128", "", " + 127"};
                W("    int ar = (((ad%s)%s) %c (((((aa << 8) + (ab - aa) * (ac + (ac >> 7)))%s)%s) >> 8))%s;\n",
                  biast[bias_], sl[scale], op ? '-' : '+', sl[scale], lb[2 * op + (scale == 3 ? 1 : 0)], scale == 3 ? " >> 1" : "");
            } else {
                uint32_t mode = (scale << 1) | op;
                switch (mode) {
                case 0: W("    int ar = ad + ((ca.r > cb.r) ? ac : 0);\n"); break;
                case 1: W("    int ar = ad + ((ca.r == cb.r) ? ac : 0);\n"); break;
                case 2: W("    int ar = ad + (((ca.g << 8 | ca.r) > (cb.g << 8 | cb.r)) ? ac : 0);\n"); break;
                case 3: W("    int ar = ad + (((ca.g << 8 | ca.r) == (cb.g << 8 | cb.r)) ? ac : 0);\n"); break;
                case 4: W("    int ar = ad + (((ca.b << 16 | ca.g << 8 | ca.r) > (cb.b << 16 | cb.g << 8 | cb.r)) ? ac : 0);\n"); break;
                case 5: W("    int ar = ad + (((ca.b << 16 | ca.g << 8 | ca.r) == (cb.b << 16 | cb.g << 8 | cb.r)) ? ac : 0);\n"); break;
                case 6: W("    int ar = ad + ((aa > ab) ? ac : 0);\n"); break;
                case 7: W("    int ar = ad + ((aa == ab) ? ac : 0);\n"); break;
                }
            }
            if (clamp) W("    %s.a = clamp(ar, 0, 255);\n", kDest[dest]);
            else W("    %s.a = clamp(ar, -1024, 1023);\n", kDest[dest]);
        }
        s += "  }\n";
    }
    s += "  ivec4 outc = prev & 255;\n";

    // alpha test
    {
        uint32_t f0 = k.alpha_func & 7, f1 = (k.alpha_func >> 3) & 7, logic = (k.alpha_func >> 6) & 3;
        char t0[128], t1[128];
        snprintf(t0, sizeof(t0), kCompare[f0], "outc.a", "u_alpharef.x");
        snprintf(t1, sizeof(t1), kCompare[f1], "outc.a", "u_alpharef.y");
        static const char* lop[4] = {"&&", "||", "!=", "=="};
        if (!(f0 == 7 && f1 == 7 && logic == 0))
            W("  if (!(%s %s %s)) discard;\n", t0, lop[logic], t1);
    }
    // fog
    {
        uint32_t fsel = k.fog & 7;
        bool ortho = (k.fog >> 3) & 1;
        if (fsel >= 2) {
            s += "  float zs = gl_FragCoord.z * 16777215.0;\n";
            if (ortho) s += "  float ze = u_fog.x * (zs / 16777215.0);\n";
            else s += "  float ze = (u_fog.x * 16777216.0) / (u_fog.z - floor(zs / exp2(u_fog.w)));\n";
            s += "  float fog = clamp(ze - u_fog.y, 0.0, 1.0);\n";
            switch (fsel) {
            case 4: s += "  fog = 1.0 - exp2(-8.0 * fog);\n"; break;
            case 5: s += "  fog = 1.0 - exp2(-8.0 * fog * fog);\n"; break;
            case 6: s += "  fog = exp2(-8.0 * (1.0 - fog));\n"; break;
            case 7: s += "  fog = exp2(-8.0 * (1.0 - fog) * (1.0 - fog));\n"; break;
            default: break;
            }
            s += "  vec3 fc = mix(vec3(outc.rgb), u_fogcolor * 255.0, fog);\n";
            s += "  outc.rgb = ivec3(round(fc));\n";
        }
    }
    s += "  o_color = vec4(outc) / 255.0;\n";
    if (getenv("WR_FLAT")) s += "  o_color = vec4(1.0, 0.0, 1.0, 1.0);\n";
    s += "}\n";

    // Indirect coordinate scale helper: emitted before main via string substitution.
    std::string helper =
        "uniform vec2 u_indcoordscale[4];\n"
        "vec2 u_indscalef(int i) { return u_indcoordscale[i]; }\n";
    size_t pos = s.find("ivec4 sample_tex");
    s.insert(pos, helper);
    return s;
}

}  // namespace gx
