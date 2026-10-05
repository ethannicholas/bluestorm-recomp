// GL back end for the GX pixel pipeline. Runs on the main thread.
//
// One source serves desktop OpenGL 3.3 core and OpenGL ES 3.2; where the two profiles
// differ, the difference is confined to the small block of helpers below.
#include "../runtime.h"
#include "render.h"
#include "render_gl.h"
#include "shadergen.h"
#include "gl.h"
#include <unordered_map>

bool write_png(const char* path, const uint8_t* rgba, int w, int h);

namespace gx {

static const int EFB_W = 640, EFB_H = 528;

// Logic-op blending does not exist in GL ES: there is no GL_COLOR_LOGIC_OP, no
// glLogicOp, and none of the GL_CLEAR..GL_SET enums. The game uses GX logic ops for
// only a few effects, so on ES they are skipped -- the draw writes through with
// blending off, which matches the common GX_LO_COPY case. Reproducing the rest would
// mean reading the framebuffer in the generated TEV shader via
// GL_EXT_shader_framebuffer_fetch.
#ifdef WR_GL_ES
static inline void set_logic_op_off() {}
static inline void set_logic_op(uint32_t) {}
// ES spells this with the float suffix; desktop GL 3.3 core only has the double form.
static inline void clear_depth(float d) { glClearDepthf(d); }
// ES has no sampler LOD bias. GX uses it to nudge mip selection, so skipping it can
// pick a slightly different mip level than hardware would.
static inline void set_lod_bias(GLuint, float) {}
#else
static const GLenum kLogicOp[16] = {GL_CLEAR, GL_AND, GL_AND_REVERSE, GL_COPY, GL_AND_INVERTED, GL_NOOP, GL_XOR, GL_OR,
                                    GL_NOR, GL_EQUIV, GL_INVERT, GL_OR_REVERSE, GL_COPY_INVERTED, GL_OR_INVERTED, GL_NAND, GL_SET};
static inline void set_logic_op_off() { glDisable(GL_COLOR_LOGIC_OP); }
static inline void set_logic_op(uint32_t mode) {
    glEnable(GL_COLOR_LOGIC_OP);
    glLogicOp(kLogicOp[mode & 15]);
}
static inline void clear_depth(float d) { glClearDepth(d); }
static inline void set_lod_bias(GLuint s, float bias) {
    glSamplerParameterf(s, GL_TEXTURE_LOD_BIAS, bias);
}
#endif

struct Program {
    GLuint prog;
    GLint u_proj, u_vp_a, u_vp_b, u_point_size, u_tex, u_reg, u_konst, u_texsize, u_indmtx, u_indscale,
        u_alpharef, u_fog, u_fogcolor, u_indcoordscale;
};

static int g_scale = 2;
static GLuint g_efb_fbo, g_efb_color, g_efb_depth;
static GLuint g_vao, g_vbo;
static GLuint g_copy_prog, g_copy_vao;
static GLint g_copy_u_src, g_copy_u_rect, g_copy_u_mode, g_copy_u_depth;
static GLuint g_blit_prog;
static GLint g_blit_u_src, g_blit_u_rect;
static GLuint g_copy_fbo;
static GLuint g_vs;
static std::unordered_map<ShaderKey, Program, ShaderKeyHash> g_programs;
struct GlTex { GLuint tex; uint32_t w, h; bool efb; };
static std::unordered_map<uint32_t, GlTex> g_textures;
static std::unordered_map<uint32_t, GLuint> g_samplers;
static int g_win_w, g_win_h;
bool g_cull_swap = false;

static GLuint compile(GLenum type, const std::string& src) {
    GLuint sh = glCreateShader(type);
    const char* p = src.c_str();
    glShaderSource(sh, 1, &p, nullptr);
    glCompileShader(sh);
    GLint ok;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
        fprintf(stderr, "%s\n", src.c_str());
        fatal("shader compile failed: %s", log);
    }
    return sh;
}

static GLuint link(GLuint vs, GLuint fs) {
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glBindAttribLocation(p, 0, "a_pos");
    glLinkProgram(p);
    GLint ok;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        fatal("program link failed: %s", log);
    }
    return p;
}

static const Program& get_program(const ShaderKey& k) {
    auto it = g_programs.find(k);
    if (it != g_programs.end()) return it->second;
    std::string src = gen_pixel_shader(k);
    if (getenv("WR_DUMP_SHADERS")) fprintf(stderr, "---- shader %zu ----\n%s\n", g_programs.size(), src.c_str());
    GLuint fs = compile(GL_FRAGMENT_SHADER, src);
    Program pr{};
    pr.prog = link(g_vs, fs);
    glDeleteShader(fs);
    GLuint p = pr.prog;
    pr.u_proj = glGetUniformLocation(p, "u_proj");
    pr.u_vp_a = glGetUniformLocation(p, "u_vp_a");
    pr.u_vp_b = glGetUniformLocation(p, "u_vp_b");
    pr.u_point_size = glGetUniformLocation(p, "u_point_size");
    pr.u_tex = glGetUniformLocation(p, "u_tex");
    pr.u_reg = glGetUniformLocation(p, "u_reg");
    pr.u_konst = glGetUniformLocation(p, "u_konst");
    pr.u_texsize = glGetUniformLocation(p, "u_texsize");
    pr.u_indmtx = glGetUniformLocation(p, "u_indmtx");
    pr.u_indscale = glGetUniformLocation(p, "u_indscale");
    pr.u_alpharef = glGetUniformLocation(p, "u_alpharef");
    pr.u_fog = glGetUniformLocation(p, "u_fog");
    pr.u_fogcolor = glGetUniformLocation(p, "u_fogcolor");
    pr.u_indcoordscale = glGetUniformLocation(p, "u_indcoordscale");
    glUseProgram(p);
    GLint units[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    glUniform1iv(pr.u_tex, 8, units);
    return g_programs.emplace(k, pr).first->second;
}

static const char* kCopyVS = WR_GLSL_VERSION R"(
out vec2 v_uv;
uniform vec4 u_rect;  // source rect in normalized EFB coords (x0,y0,x1,y1), y down
void main() {
    vec2 p = vec2(gl_VertexID & 1, gl_VertexID >> 1);
    v_uv = mix(u_rect.xy, u_rect.zw, p);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// Converts EFB color (or depth) to the requested GX copy format. Output row 0 = top.
static const char* kCopyFS = WR_GLSL_VERSION R"(
in vec2 v_uv;
uniform sampler2D u_src;
uniform sampler2D u_depth;
uniform int u_mode;   // copy texture format (+16 intensity, +32 depth)
out vec4 o;
void main() {
    vec2 uv = vec2(v_uv.x, 1.0 - v_uv.y);
    vec4 c = texture(u_src, uv);
    if ((u_mode & 32) != 0) {
        float z = texture(u_depth, uv).r;
        uint zi = uint(z * 16777215.0);
        c = vec4(float((zi >> 16) & 255u), float((zi >> 8) & 255u), float(zi & 255u), 255.0) / 255.0;
        c = vec4(c.r, c.g, c.b, c.r);
    }
    int fmt = u_mode & 15;
    bool intensity = (u_mode & 16) != 0;
    if (intensity) {
        float y = clamp(dot(c.rgb, vec3(0.257, 0.504, 0.098)) + 16.0 / 255.0, 0.0, 1.0);
        c.rgb = vec3(y);
    }
    // Single-channel formats replicate into all components like the texture decoder does.
    if (fmt == 7) c = vec4(c.a);              // A8
    else if (fmt == 8) c = vec4(c.r);         // R8
    else if (fmt == 9) c = vec4(c.g);         // G8
    else if (fmt == 10) c = vec4(c.b);        // B8
    else if (fmt == 1 && !intensity) c = vec4(c.r);
    if (fmt == 4) c.a = 1.0;                  // RGB565
    o = c;
}
)";

static const char* kBlitFS = WR_GLSL_VERSION R"(
in vec2 v_uv;
uniform sampler2D u_src;
out vec4 o;
void main() { o = vec4(texture(u_src, vec2(v_uv.x, 1.0 - v_uv.y)).rgb, 1.0); }
)";

void render_init(int internal_scale) {
    g_scale = internal_scale;
    g_vs = compile(GL_VERTEX_SHADER, gen_vertex_shader());

    glGenFramebuffers(1, &g_efb_fbo);
    glGenTextures(1, &g_efb_color);
    glBindTexture(GL_TEXTURE_2D, g_efb_color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, EFB_W * g_scale, EFB_H * g_scale, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenTextures(1, &g_efb_depth);
    glBindTexture(GL_TEXTURE_2D, g_efb_depth);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, EFB_W * g_scale, EFB_H * g_scale, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, g_efb_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_efb_color, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, g_efb_depth, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) fatal("EFB framebuffer incomplete");
    glClearColor(0, 0, 0, 1);
    clear_depth(1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glGenVertexArrays(1, &g_vao);
    glBindVertexArray(g_vao);
    glGenBuffers(1, &g_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    const GLsizei stride = sizeof(GpuVertex);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(GpuVertex, pos));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, (void*)offsetof(GpuVertex, col[0]));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, (void*)offsetof(GpuVertex, col[1]));
    for (int i = 0; i < 8; i++) {
        glEnableVertexAttribArray(3 + i);
        glVertexAttribPointer(3 + i, 3, GL_FLOAT, GL_FALSE, stride, (void*)(offsetof(GpuVertex, tex) + i * 12));
    }

    GLuint cvs = compile(GL_VERTEX_SHADER, kCopyVS);
    g_copy_prog = link(cvs, compile(GL_FRAGMENT_SHADER, kCopyFS));
    g_copy_u_src = glGetUniformLocation(g_copy_prog, "u_src");
    g_copy_u_depth = glGetUniformLocation(g_copy_prog, "u_depth");
    g_copy_u_rect = glGetUniformLocation(g_copy_prog, "u_rect");
    g_copy_u_mode = glGetUniformLocation(g_copy_prog, "u_mode");
    g_blit_prog = link(cvs, compile(GL_FRAGMENT_SHADER, kBlitFS));
    g_blit_u_src = glGetUniformLocation(g_blit_prog, "u_src");
    g_blit_u_rect = glGetUniformLocation(g_blit_prog, "u_rect");
    glGenVertexArrays(1, &g_copy_vao);
    glGenFramebuffers(1, &g_copy_fbo);
#ifndef WR_GL_ES
    // Desktop core profile needs this to honour gl_PointSize; ES always does.
    glEnable(GL_PROGRAM_POINT_SIZE);
#endif
}

void render_set_window_size(int w, int h) { g_win_w = w; g_win_h = h; }

// ---------------------------------------------------------------------------
static void upload_texture(const TexData& t) {
    GlTex g{};
    glGenTextures(1, &g.tex);
    glBindTexture(GL_TEXTURE_2D, g.tex);
    g.w = t.width; g.h = t.height;
    uint32_t w = t.width, h = t.height;
    for (size_t l = 0; l < t.levels.size(); l++) {
        glTexImage2D(GL_TEXTURE_2D, (GLint)l, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, t.levels[l].data());
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, (GLint)t.levels.size() - 1);
    g_textures[t.id] = g;
}

static GLuint get_sampler(uint32_t mode0, uint32_t mode1, uint32_t levels) {
    uint32_t key = (mode0 & 0x3FFFFF) ^ (mode1 << 22) ^ (levels << 30);
    auto it = g_samplers.find(key);
    if (it != g_samplers.end()) return it->second;
    GLuint s;
    glGenSamplers(1, &s);
    static const GLenum wrap[4] = {GL_CLAMP_TO_EDGE, GL_REPEAT, GL_MIRRORED_REPEAT, GL_REPEAT};
    glSamplerParameteri(s, GL_TEXTURE_WRAP_S, wrap[mode0 & 3]);
    glSamplerParameteri(s, GL_TEXTURE_WRAP_T, wrap[(mode0 >> 2) & 3]);
    bool mag_lin = (mode0 >> 4) & 1;
    uint32_t minf = (mode0 >> 5) & 7;
    bool min_lin = minf & 4;
    uint32_t mip = minf & 3;
    glSamplerParameteri(s, GL_TEXTURE_MAG_FILTER, mag_lin ? GL_LINEAR : GL_NEAREST);
    GLenum mf;
    if (!mip || levels <= 1) mf = min_lin ? GL_LINEAR : GL_NEAREST;
    else if (mip == 1) mf = min_lin ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST;
    else mf = min_lin ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR;
    glSamplerParameteri(s, GL_TEXTURE_MIN_FILTER, mf);
    int8_t bias = (int8_t)((mode0 >> 9) & 0xFF);
    set_lod_bias(s, bias / 32.0f);
    glSamplerParameterf(s, GL_TEXTURE_MIN_LOD, (mode1 & 0xFF) / 16.0f);
    glSamplerParameterf(s, GL_TEXTURE_MAX_LOD, ((mode1 >> 8) & 0xFF) / 16.0f);
    g_samplers[key] = s;
    return s;
}

static inline int32_t sx11(uint32_t v) { return (int32_t)(v << 21) >> 21; }

static float fog_float(uint32_t v) {
    uint32_t mant = v & 0x7FF, exp = (v >> 11) & 0xFF, sign = (v >> 19) & 1;
    uint32_t bits = (sign << 31) | (exp << 23) | (mant << 12);
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

static const GLenum kBlendSrc[8] = {GL_ZERO, GL_ONE, GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA};
static const GLenum kBlendDst[8] = {GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA};
static const GLenum kDepthFunc[8] = {GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS};
static void apply_state(const PixelState& st, int prim) {
    const uint32_t* bp = st.bp;
    ShaderKey key = make_shader_key(st);
    const Program& pr = get_program(key);
    glUseProgram(pr.prog);

    // Projection
    float P[16] = {0};
    const float* p = st.proj;
    if ((int)p[6] == 0) {  // perspective
        P[0] = p[0]; P[8] = p[1]; P[5] = p[2]; P[9] = p[3]; P[10] = p[4]; P[14] = p[5]; P[11] = -1.0f;
    } else {
        P[0] = p[0]; P[12] = p[1]; P[5] = p[2]; P[13] = p[3]; P[10] = p[4]; P[14] = p[5]; P[15] = 1.0f;
    }
    glUniformMatrix4fv(pr.u_proj, 1, GL_FALSE, P);
    const float* vp = st.viewport;  // sx, sy, sz, ox, oy, oz
    float vpa[4] = {2.0f * (vp[3] - 342.0f) / EFB_W - 1.0f, 2.0f * vp[0] / EFB_W, 2.0f * (vp[4] - 342.0f) / EFB_H - 1.0f, 2.0f * vp[1] / EFB_H};
    float vpb[4] = {2.0f * vp[5] / 16777215.0f - 1.0f, 2.0f * vp[2] / 16777215.0f, 0, 0};
    glUniform4fv(pr.u_vp_a, 1, vpa);
    glUniform4fv(pr.u_vp_b, 1, vpb);
    float psize = ((bp[0x22] >> 8) & 0xFF) / 6.0f * g_scale;
    glUniform1f(pr.u_point_size, psize < 1 ? 1 : psize);

    // TEV registers
    GLint regs[16], kon[16];
    for (int r = 0; r < 4; r++) {
        uint32_t ra = st.tev_reg[r][0], bg = st.tev_reg[r][1];
        regs[r * 4 + 0] = sx11(ra); regs[r * 4 + 3] = sx11(ra >> 12);
        regs[r * 4 + 2] = sx11(bg); regs[r * 4 + 1] = sx11(bg >> 12);
        uint32_t ka = st.tev_konst[r][0], kb = st.tev_konst[r][1];
        kon[r * 4 + 0] = ka & 0xFF; kon[r * 4 + 3] = (ka >> 12) & 0xFF;
        kon[r * 4 + 2] = kb & 0xFF; kon[r * 4 + 1] = (kb >> 12) & 0xFF;
    }
    glUniform4iv(pr.u_reg, 4, regs);
    glUniform4iv(pr.u_konst, 4, kon);
    GLint aref[2] = {(GLint)(bp[0xF3] & 0xFF), (GLint)((bp[0xF3] >> 8) & 0xFF)};
    glUniform2iv(pr.u_alpharef, 1, aref);
    // Indirect matrices
    GLint im[18];
    GLint isc[3];
    for (int m = 0; m < 3; m++) {
        uint32_t r0 = bp[0x06 + 3 * m], r1 = bp[0x07 + 3 * m], r2 = bp[0x08 + 3 * m];
        im[m * 6 + 0] = sx11(r0); im[m * 6 + 1] = sx11(r1); im[m * 6 + 2] = sx11(r2);
        im[m * 6 + 3] = sx11(r0 >> 11); im[m * 6 + 4] = sx11(r1 >> 11); im[m * 6 + 5] = sx11(r2 >> 11);
        isc[m] = (int)(((r0 >> 22) & 3) | (((r1 >> 22) & 3) << 2) | (((r2 >> 22) & 3) << 4)) - 17;
    }
    glUniform3iv(pr.u_indmtx, 6, im);
    glUniform1iv(pr.u_indscale, 3, isc);
    float ics[8];
    for (int i = 0; i < 4; i++) {
        uint32_t ss = bp[0x25 + i / 2] >> ((i & 1) * 8);
        ics[i * 2] = 1.0f / (float)(1u << (ss & 15));
        ics[i * 2 + 1] = 1.0f / (float)(1u << ((ss >> 4) & 15));
    }
    glUniform2fv(pr.u_indcoordscale, 4, ics);
    // Fog
    float fog[4] = {fog_float(bp[0xEE]), fog_float(bp[0xF1]), (float)(bp[0xEF] & 0xFFFFFF), (float)(bp[0xF0] & 0x1F)};
    glUniform4fv(pr.u_fog, 1, fog);
    float fogc[3] = {((bp[0xF2] >> 16) & 0xFF) / 255.0f, ((bp[0xF2] >> 8) & 0xFF) / 255.0f, (bp[0xF2] & 0xFF) / 255.0f};
    glUniform3fv(pr.u_fogcolor, 1, fogc);

    // Textures
    float tsz[16];
    for (int m = 0; m < 8; m++) {
        tsz[m * 2] = tsz[m * 2 + 1] = 1.0f;
        glActiveTexture(GL_TEXTURE0 + m);
        uint32_t id = st.tex_id[m];
        auto it = id ? g_textures.find(id) : g_textures.end();
        if (it == g_textures.end()) { glBindTexture(GL_TEXTURE_2D, 0); continue; }
        glBindTexture(GL_TEXTURE_2D, it->second.tex);
        uint32_t base = m < 4 ? 0x80 + m : 0xA0 + (m - 4);
        uint32_t img0 = bp[base + 8];
        tsz[m * 2] = (float)((img0 & 0x3FF) + 1);
        tsz[m * 2 + 1] = (float)(((img0 >> 10) & 0x3FF) + 1);
        GLint maxl = 0;
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, &maxl);
        glBindSampler(m, get_sampler(bp[base], bp[base + 4], it->second.efb ? 1 : (uint32_t)maxl + 1));
    }
    glUniform2fv(pr.u_texsize, 8, tsz);

    // Blend / logic op
    uint32_t bm = bp[0x41];
    bool blend = bm & 1, logic = (bm >> 1) & 1, sub = (bm >> 11) & 1;
    if (sub) {
        glEnable(GL_BLEND);
        set_logic_op_off();
        glBlendEquation(GL_FUNC_REVERSE_SUBTRACT);
        glBlendFunc(GL_ONE, GL_ONE);
    } else if (blend) {
        glEnable(GL_BLEND);
        set_logic_op_off();
        glBlendEquation(GL_FUNC_ADD);
        glBlendFunc(kBlendSrc[(bm >> 8) & 7], kBlendDst[(bm >> 5) & 7]);
    } else if (logic) {
        glDisable(GL_BLEND);
        set_logic_op(bm >> 12);
    } else {
        glDisable(GL_BLEND);
        set_logic_op_off();
    }
    bool has_alpha = (bp[0x43] & 7) == 1;
    GLboolean cw = (bm >> 3) & 1, aw = ((bm >> 4) & 1) && has_alpha;
    glColorMask(cw, cw, cw, aw);
    // Depth
    uint32_t zm = bp[0x40];
    if (zm & 1) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(kDepthFunc[(zm >> 1) & 7]);
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    glDepthMask((zm >> 4) & 1);
    // Cull
    uint32_t cull = (bp[0x00] >> 14) & 3;
    static bool nocull = getenv("WR_NOCULL") != nullptr;
    if (prim != 0 || cull == 0 || nocull) glDisable(GL_CULL_FACE);
    else {
        glEnable(GL_CULL_FACE);
        if (cull == 3) glCullFace(GL_FRONT_AND_BACK);
        else {
            // GX: 1 = cull front, 2 = cull back. GX front faces are clockwise with y down,
            // which is counter-clockwise after our y flip (GL's default front face).
            bool back = cull == 2;
            if (g_cull_swap) back = !back;
            glCullFace(back ? GL_BACK : GL_FRONT);
        }
    }
    // Scissor (EFB coords, y down)
    int xoff = (int)((bp[0x59] & 0x3FF) << 1) - 342 * 0, yoff = (int)(((bp[0x59] >> 10) & 0x3FF) << 1);
    (void)xoff; (void)yoff;
    int x0 = (int)(bp[0x20] >> 12 & 0x7FF) - 342, y0 = (int)(bp[0x20] & 0x7FF) - 342;
    int x1 = (int)(bp[0x21] >> 12 & 0x7FF) - 341, y1 = (int)(bp[0x21] & 0x7FF) - 341;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > EFB_W) x1 = EFB_W;
    if (y1 > EFB_H) y1 = EFB_H;
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    glEnable(GL_SCISSOR_TEST);
    glScissor(x0 * g_scale, (EFB_H - y1) * g_scale, (x1 - x0) * g_scale, (y1 - y0) * g_scale);
}

static void do_efb_copy(const EfbCopyCmd& c) {
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    set_logic_op_off();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(1, 1, 1, 1);
    if (!c.to_xfb && c.tex_id) {
        GlTex t{};
        glGenTextures(1, &t.tex);
        t.w = c.dst_w; t.h = c.dst_h; t.efb = true;
        glBindTexture(GL_TEXTURE_2D, t.tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, c.dst_w * g_scale, c.dst_h * g_scale, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, g_copy_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.tex, 0);
        glViewport(0, 0, c.dst_w * g_scale, c.dst_h * g_scale);
        glUseProgram(g_copy_prog);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, g_efb_color);
        glBindSampler(0, 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, g_efb_depth);
        glBindSampler(1, 0);
        glUniform1i(g_copy_u_src, 0);
        glUniform1i(g_copy_u_depth, 1);
        // Destination texel row 0 = top of the source rect.
        float rect[4] = {(float)c.src_x / EFB_W, (float)c.src_y / EFB_H,
                         (float)(c.src_x + c.src_w) / EFB_W, (float)(c.src_y + c.src_h) / EFB_H};
        glUniform4fv(g_copy_u_rect, 1, rect);
        glUniform1i(g_copy_u_mode, (int)c.format | (c.depth ? 32 : 0));
        glBindVertexArray(g_copy_vao);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        auto old = g_textures.find(c.tex_id);
        if (old != g_textures.end()) glDeleteTextures(1, &old->second.tex);
        g_textures[c.tex_id] = t;
        glBindFramebuffer(GL_FRAMEBUFFER, g_efb_fbo);
        glBindVertexArray(g_vao);
    }
    if (c.clear) {
        glBindFramebuffer(GL_FRAMEBUFFER, g_efb_fbo);
        glEnable(GL_SCISSOR_TEST);
        glScissor(c.src_x * g_scale, (EFB_H - c.src_y - c.src_h) * g_scale, c.src_w * g_scale, c.src_h * g_scale);
        GLbitfield bits = 0;
        glColorMask(c.clear_color, c.clear_color, c.clear_color, c.clear_alpha);
        if (c.clear_color || c.clear_alpha) bits |= GL_COLOR_BUFFER_BIT;
        if (c.clear_z) { bits |= GL_DEPTH_BUFFER_BIT; glDepthMask(GL_TRUE); }
        glClearColor(((c.clear_rgba >> 24) & 0xFF) / 255.0f, ((c.clear_rgba >> 16) & 0xFF) / 255.0f,
                     ((c.clear_rgba >> 8) & 0xFF) / 255.0f, (c.clear_rgba & 0xFF) / 255.0f);
        clear_depth(c.clear_z_value / 16777215.0f);
        if (bits) glClear(bits);
        glColorMask(1, 1, 1, 1);
    }
}

const char* g_dump_dir = nullptr;
int g_dump_every = 0;
static uint32_t g_present_count;

static void dump_efb(const EfbCopyCmd& c) {
    int w = c.src_w * g_scale, h = c.src_h * g_scale;
    std::vector<uint8_t> px((size_t)w * h * 4), flipped((size_t)w * h * 4);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_efb_fbo);
    glReadPixels(c.src_x * g_scale, (EFB_H - c.src_y - c.src_h) * g_scale, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    for (int y = 0; y < h; y++) {
        memcpy(&flipped[(size_t)y * w * 4], &px[(size_t)(h - 1 - y) * w * 4], (size_t)w * 4);
        for (int x = 0; x < w; x++) flipped[((size_t)y * w + x) * 4 + 3] = 255;
    }
    char path[512];
    snprintf(path, sizeof(path), "%s/frame_%05u.png", g_dump_dir, g_present_count);
    write_png(path, flipped.data(), w, h);
}

static void present(const EfbCopyCmd& c) {
    g_present_count++;
    if (g_dump_dir && g_dump_every && g_present_count % g_dump_every == 0) dump_efb(c);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    set_logic_op_off();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(1, 1, 1, 1);
    glViewport(0, 0, g_win_w, g_win_h);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    // Letterbox to 4:3
    int w = g_win_w, h = g_win_h;
    int vw = w, vh = w * 3 / 4;
    if (vh > h) { vh = h; vw = h * 4 / 3; }
    glViewport((w - vw) / 2, (h - vh) / 2, vw, vh);
    glUseProgram(g_blit_prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_efb_color);
    glBindSampler(0, 0);
    glUniform1i(g_blit_u_src, 0);
    float rect[4] = {(float)c.src_x / EFB_W, (float)(c.src_y + c.src_h) / EFB_H, (float)(c.src_x + c.src_w) / EFB_W, (float)c.src_y / EFB_H};
    glUniform4fv(g_blit_u_rect, 1, rect);
    glBindVertexArray(g_copy_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(g_vao);
    if (g_dump_dir && getenv("WR_DUMP_WINDOW") && g_dump_every && g_present_count % g_dump_every == 0) {
        std::vector<uint8_t> px((size_t)g_win_w * g_win_h * 4), fl(px.size());
        glReadPixels(0, 0, g_win_w, g_win_h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        for (int y = 0; y < g_win_h; y++) memcpy(&fl[(size_t)y * g_win_w * 4], &px[(size_t)(g_win_h - 1 - y) * g_win_w * 4], (size_t)g_win_w * 4);
        for (size_t i = 3; i < fl.size(); i += 4) fl[i] = 255;
        char path[512];
        snprintf(path, sizeof(path), "%s/window_%05u.png", g_dump_dir, g_present_count);
        write_png(path, fl.data(), g_win_w, g_win_h);
        fprintf(stderr, "[win] %dx%d glError=%x\n", g_win_w, g_win_h, glGetError());
    }
}

// Executes a batch. Returns true if it contained a Present.
bool render_execute(Batch& b) {
    for (auto& t : b.new_textures) upload_texture(*t);
    glBindFramebuffer(GL_FRAMEBUFFER, g_efb_fbo);
    glViewport(0, 0, EFB_W * g_scale, EFB_H * g_scale);
    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER, b.verts.size() * sizeof(GpuVertex), b.verts.data(), GL_STREAM_DRAW);
    bool presented = false;
    uint32_t cur_state = UINT32_MAX;
    int cur_prim = -1;
    for (auto& c : b.cmds) {
        switch (c.type) {
        case CmdType::Draw: {
            if (c.state != cur_state || c.prim != cur_prim) {
                apply_state(b.states[c.state], c.prim);
                cur_state = c.state;
                cur_prim = c.prim;
            }
            static const GLenum mode[3] = {GL_TRIANGLES, GL_LINES, GL_POINTS};
            glDrawArrays(mode[c.prim], c.first, c.count);
            break;
        }
        case CmdType::EfbCopy:
            do_efb_copy(c.copy);
            glViewport(0, 0, EFB_W * g_scale, EFB_H * g_scale);
            cur_state = UINT32_MAX;
            break;
        case CmdType::Present:
            present(c.copy);
            presented = true;
            glBindFramebuffer(GL_FRAMEBUFFER, g_efb_fbo);
            glViewport(0, 0, EFB_W * g_scale, EFB_H * g_scale);
            cur_state = UINT32_MAX;
            break;
        }
    }
    return presented;
}

}  // namespace gx
