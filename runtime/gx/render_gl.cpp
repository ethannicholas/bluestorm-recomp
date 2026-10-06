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
    GLint u_vr, u_view, u_hud_scale;
};

static int g_scale = 2;

// VR eye state. When active, perspective batches are re-projected for the eye and the
// orthographic ones (the 2D HUD) become a flat overlay rather than being projected
// into the world.
static bool g_vr_active = false;
static float g_vr_proj[16], g_vr_view[16];
static float g_vr_hud_scale = 0.55f;
static GLuint g_efb_fbo, g_efb_color, g_efb_depth;
// Copy of the EFB as it looked at the last present. The display copy is immediately
// followed by an EFB clear, so repainting has to come from here, not the live EFB.
static GLuint g_snap_fbo, g_snap_tex;
static bool g_frame_marker = false;
static bool g_dump_output = false;
static void dump_output(const char* kind);

// Where each draw of the current frame landed on screen, for WR_SEAM. Recorded as the
// frame is executed and printed only if that frame turns out to contain a seam, so the
// draw that produced the seam can be named without guessing at a frame number -- which
// the wall-clock timebase makes meaningless across runs anyway.
struct DrawExtent { int index; uint32_t verts; float x0, x1, y0, y1; bool ortho;
                    int sc_x0, sc_y0, sc_x1, sc_y1, off_x, off_y; };
static std::vector<DrawExtent> g_draw_extents;
static const Batch* g_seam_batch = nullptr;  // the draw the seam probe is reporting on
static const Cmd* g_seam_cmd = nullptr;
static int g_seam_x = -1, g_seam_y = -1;  // where the previous frame's seam was
static bool g_seam_trace = false;  // trace the next frame draw by draw
static GLuint g_vao, g_vbo;
static GLuint g_copy_prog, g_copy_vao;
static GLint g_copy_u_src, g_copy_u_rect, g_copy_u_mode, g_copy_u_depth;
static GLuint g_blit_prog;
static GLint g_blit_u_src, g_blit_u_rect;
static GLuint g_copy_fbo;
static GLuint g_vs;
static std::unordered_map<ShaderKey, Program, ShaderKeyHash> g_programs;
struct GlTex { GLuint tex; uint32_t w, h; bool efb; uint32_t last_used; };
static std::unordered_map<uint32_t, GlTex> g_textures;
// Frames counted here rather than reusing the GX frame counter, so eviction works the
// same for any frontend. Textures the game stops using are released: a race streams
// them continuously, and without this both the GL objects and the decoded copies grow
// without bound until the device runs out of memory and crawls.
static uint32_t g_render_frame;
static constexpr uint32_t kTexIdleFrames = 240;  // ~8 s at 30 fps
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
    pr.u_vr = glGetUniformLocation(p, "u_vr");
    pr.u_view = glGetUniformLocation(p, "u_view");
    pr.u_hud_scale = glGetUniformLocation(p, "u_hud_scale");
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
    glGenFramebuffers(1, &g_snap_fbo);
    glGenTextures(1, &g_snap_tex);
    glBindTexture(GL_TEXTURE_2D, g_snap_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, EFB_W * g_scale, EFB_H * g_scale, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindFramebuffer(GL_FRAMEBUFFER, g_snap_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_snap_tex, 0);

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
    g.last_used = g_render_frame;
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
static bool samples_fullscreen_copy(const PixelState& st);

// The origin that the scissor box and the viewport are both measured from, in EFB pixels.
//
// GX stores it halved, so GXSetScissorBoxOffset(0, 0) reads back as 342 -- which is where
// the 342 hardcoded here comes from. Reading the register instead would be more faithful
// in general, and both the scissor and the viewport would have to use it or geometry and
// clipping disagree and nearly everything is scissored away. It is not done, because this
// game's register reads 340 throughout, and switching to it shifts the whole picture two
// pixels right with nothing to say which is correct. The constant is what has been
// checked against reference footage, so it stays until there is a reason to move it.
static void scissor_offset(const uint32_t*, int& xoff, int& yoff) {
    xoff = 342;
    yoff = 342;
}

static void apply_state(const PixelState& st, int prim) {
    int xoff, yoff;
    scissor_offset(st.bp, xoff, yoff);
    const uint32_t* bp = st.bp;
    ShaderKey key = make_shader_key(st);
    const Program& pr = get_program(key);
    glUseProgram(pr.prog);

    // Projection. In VR a perspective batch is world geometry and gets the eye's
    // projection instead of the game's; an orthographic one is a 2D element and keeps
    // the game's, drawn as an overlay.
    float P[16] = {0};
    const float* p = st.proj;
    const bool perspective = (int)p[6] == 0;
    if (perspective) {
        P[0] = p[0]; P[8] = p[1]; P[5] = p[2]; P[9] = p[3]; P[10] = p[4]; P[14] = p[5]; P[11] = -1.0f;
    } else {
        P[0] = p[0]; P[12] = p[1]; P[5] = p[2]; P[13] = p[3]; P[10] = p[4]; P[14] = p[5]; P[15] = 1.0f;
    }
    // Note: a draw sampling a copy of the whole frame (the water surface is one) must
    // stay in the world, however tempting its screen-space origin makes the overlay path
    // look. That path is in NDC and so is head-locked -- which is what the HUD wants and
    // the ocean emphatically does not. Sending the water through it pinned the water, and
    // the racer baked into the copy, to the viewer's face while the real racer went on
    // moving in the world. The seam at the billboard's edge is the lesser problem.
    if (g_vr_active && perspective) {
        // WR_EYE_GAMEPROJ keeps the game's own frustum and applies only the head
        // transform, which tells apart "the eye sees less than it should" from "the game
        // never drew anything out there".
        static const bool game_proj = getenv("WR_EYE_GAMEPROJ") != nullptr;
        static bool logged = false;
        if (!logged && getenv("WR_EYELOG")) {
            logged = true;
            fprintf(stderr, "[eye] game fov: x=%.1fdeg y=%.1fdeg (p0=%f p2=%f)\n",
                    2.0f * atanf(1.0f / p[0]) * 57.2958f, 2.0f * atanf(1.0f / p[2]) * 57.2958f,
                    p[0], p[2]);
        }
        glUniformMatrix4fv(pr.u_proj, 1, GL_FALSE, game_proj ? P : g_vr_proj);
        glUniformMatrix4fv(pr.u_view, 1, GL_FALSE, g_vr_view);
        glUniform1i(pr.u_vr, 1);
    } else {
        glUniformMatrix4fv(pr.u_proj, 1, GL_FALSE, P);
        glUniform1i(pr.u_vr, g_vr_active ? 2 : 0);
        glUniform1f(pr.u_hud_scale, g_vr_hud_scale);
    }
    const float* vp = st.viewport;  // sx, sy, sz, ox, oy, oz
    float vpa[4] = {2.0f * (vp[3] - xoff) / EFB_W - 1.0f, 2.0f * vp[0] / EFB_W, 2.0f * (vp[4] - yoff) / EFB_H - 1.0f, 2.0f * vp[1] / EFB_H};
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
        it->second.last_used = g_render_frame;
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
    // Scissor (EFB coords, y down). See scissor_offset() for why 342 is not a constant.
    int x0 = (int)(bp[0x20] >> 12 & 0x7FF) - xoff, y0 = (int)(bp[0x20] & 0x7FF) - yoff;
    int x1 = (int)(bp[0x21] >> 12 & 0x7FF) - xoff + 1, y1 = (int)(bp[0x21] & 0x7FF) - yoff + 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > EFB_W) x1 = EFB_W;
    if (y1 > EFB_H) y1 = EFB_H;
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    if (g_vr_active) {
        // The scissor rect is in EFB coordinates, which say nothing about an eye's
        // render target.
        glDisable(GL_SCISSOR_TEST);
        return;
    }
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
        // The same target redrawn each frame keeps its id, so reuse the texture it
        // already has rather than allocating and freeing one per copy per frame.
        GlTex t{};
        auto old = g_textures.find(c.tex_id);
        const bool reuse = old != g_textures.end() && old->second.efb &&
                           old->second.w == c.dst_w && old->second.h == c.dst_h;
        if (reuse) {
            t = old->second;
            glBindTexture(GL_TEXTURE_2D, t.tex);
        } else {
            glGenTextures(1, &t.tex);
            t.w = c.dst_w; t.h = c.dst_h; t.efb = true;
            glBindTexture(GL_TEXTURE_2D, t.tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, c.dst_w * g_scale, c.dst_h * g_scale, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        }
        t.last_used = g_render_frame;
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
        if (!reuse && old != g_textures.end()) glDeleteTextures(1, &old->second.tex);
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

// Where the finished frame is blitted. 0 is the window's framebuffer; a VR frontend
// points this at one of its swapchain images instead.
static GLuint g_output_fbo = 0;
// The most recent presented rect, so the output can be refreshed without the game
// having produced a new frame (a VR compositor wants one every display frame, which is
// far more often than this game renders).
static EfbCopyCmd g_last_present{};
static bool g_have_present = false;

static void blit_to_output(const EfbCopyCmd& c, GLuint src_tex) {
    glBindFramebuffer(GL_FRAMEBUFFER, g_output_fbo);
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
    glBindTexture(GL_TEXTURE_2D, src_tex);
    glBindSampler(0, 0);
    glUniform1i(g_blit_u_src, 0);
    float rect[4] = {(float)c.src_x / EFB_W, (float)(c.src_y + c.src_h) / EFB_H, (float)(c.src_x + c.src_w) / EFB_W, (float)c.src_y / EFB_H};
    glUniform4fv(g_blit_u_rect, 1, rect);
    glBindVertexArray(g_copy_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(g_vao);

    // A row of cells across the top, all the same colour, that colour derived from the
    // frame number. Every cell is written by the same blit, so in a screenshot they must
    // all match. If they do not, the image the compositor showed was assembled from more
    // than one frame, and the colours say which -- which settles whether a seam is
    // something this renderer drew or something that happened after it.
    if (g_frame_marker) {
        const uint32_t n = g_present_count;
        glEnable(GL_SCISSOR_TEST);
        glClearColor(((n * 37) % 256) / 255.0f, ((n * 91) % 256) / 255.0f,
                     ((n * 151) % 256) / 255.0f, 1.0f);
        const int cells = 16, hgt = g_win_h / 48 > 4 ? g_win_h / 48 : 4;
        for (int i = 0; i < cells; i++) {
            glScissor(i * g_win_w / cells, g_win_h - hgt, g_win_w / cells - 2, hgt);
            glClear(GL_COLOR_BUFFER_BIT);
        }
        glDisable(GL_SCISSOR_TEST);
    }
}

static void present(const EfbCopyCmd& c) {
    g_present_count++;
    // WR_DUMP_RANGE=a-b restricts dumping to a window of presented frames, so a short
    // stretch can be captured every single frame. Flicker is only visible frame by frame.
    static int range_lo = -1, range_hi = -1;
    static bool range_parsed = false;
    if (!range_parsed) {
        range_parsed = true;
        if (const char* s = getenv("WR_DUMP_RANGE")) {
            range_lo = atoi(s);
            const char* dash = strchr(s, '-');
            range_hi = dash ? atoi(dash + 1) : range_lo;
        }
    }
    const bool in_range = range_lo < 0 || ((int)g_present_count >= range_lo &&
                                           (int)g_present_count <= range_hi);
    if (g_dump_dir && g_dump_every && in_range && g_present_count % g_dump_every == 0) dump_efb(c);
    // WR_SEAM=1 dumps only frames that contain a full-height vertical edge somewhere in
    // the interior. The character-select seam appears for a few frames somewhere in a
    // long session, and a fixed dump interval will not land on it; this looks for the
    // fault itself rather than for a moment someone guessed at.
    static const bool seam_hunt = getenv("WR_SEAM") != nullptr;
    if (seam_hunt && g_dump_dir) {
        const int w = c.src_w * g_scale, h = c.src_h * g_scale;
        std::vector<uint8_t> px((size_t)w * h * 4);
        glReadPixels(c.src_x * g_scale, (EFB_H - c.src_y - c.src_h) * g_scale, w, h, GL_RGBA,
                     GL_UNSIGNED_BYTE, px.data());
        int best_x = -1, best_rows = 0;
        for (int x = 40; x < w - 40; x++) {
            int rows = 0;
            for (int y = 0; y < h; y++) {
                const uint8_t* a = &px[((size_t)y * w + x) * 4];
                const int d = abs(a[0] - a[4]) + abs(a[1] - a[5]) + abs(a[2] - a[6]);
                if (d > 40) rows++;
            }
            if (rows > best_rows) { best_rows = rows; best_x = x; }
        }
        if (best_rows > h * 6 / 10) {
            fprintf(stderr, "[seam] frame %u: x=%d spans %d of %d rows\n", g_present_count,
                    best_x, best_rows, h);
            // Arm the per-command probe for the next frame, once, now that the seam's
            // column is known. The detector reads a 640x480 window starting at EFB row
            // (EFB_H - 480), so its x index is already an absolute EFB column.
            static bool armed = false;
            if (!armed) {
                armed = true;
                g_seam_x = best_x / g_scale;
                g_seam_trace = true;
                fprintf(stderr, "[seam] probing every command of the next frame at x=%d\n",
                        g_seam_x);
            }
            for (auto& d : g_draw_extents)
                fprintf(stderr, "[seam]   draw %3d verts=%5u x=%6.1f..%-6.1f y=%6.1f..%-6.1f %s "
                        "sc=%d,%d..%d,%d off=%d,%d\n",
                        d.index, d.verts, d.x0, d.x1, d.y0, d.y1, d.ortho ? "2D" : "3D",
                        d.sc_x0, d.sc_y0, d.sc_x1, d.sc_y1, d.off_x, d.off_y);
            dump_efb(c);
        }
    }
    g_last_present = c;
    g_have_present = true;
    // Keep a copy before the display copy's clear wipes the EFB, so a later repaint
    // has something to show. Without this, every frame the game did not produce would
    // repaint a cleared EFB -- black -- which strobes against the frames it did.
    const GLint fw = EFB_W * g_scale, fh = EFB_H * g_scale;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_efb_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_snap_fbo);
    glBlitFramebuffer(0, 0, fw, fh, 0, 0, fw, fh, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    blit_to_output(c, g_efb_color);
    dump_output("p");
}

// Reads back whatever blit_to_output just wrote -- in the headset, the swapchain image
// the compositor will sample.
//
// This is the only view of a *repaint*: present() runs just for the frames the game
// produced, and in the headset more than half of all display frames are repaints of the
// previous one. A fault that lives in those frames leaves no trace in an EFB dump, so
// "the EFB is clean" says nothing about them. `kind` distinguishes the two.
static void dump_output(const char* kind) {
    if (!g_dump_dir || !g_dump_output) return;
    // Writing a 1024x768 PNG on every display frame at 72 Hz would cost more than the
    // game does and shift the timing being observed, so this samples a few scanlines,
    // looks for a vertical edge, and only writes an image when it finds one.
    static const int kRows = 8;
    static std::vector<uint8_t> rows;
    rows.resize((size_t)g_win_w * kRows * 4);
    for (int i = 0; i < kRows; i++)
        glReadPixels(0, g_win_h * (i + 1) / (kRows + 1), g_win_w, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                     &rows[(size_t)i * g_win_w * 4]);
    // Compare across a four-pixel baseline, not adjacent columns: the EFB is upscaled
    // 640 -> 1024 with linear filtering, which spreads a one-pixel step over two columns
    // and would hide it from an adjacent-pixel test.
    int best_x = -1, best_n = 0;
    for (int x = 16; x < g_win_w - 17; x++) {
        int n = 0;
        for (int i = 0; i < kRows; i++) {
            const uint8_t* a = &rows[((size_t)i * g_win_w + x - 2) * 4];
            const uint8_t* b = &rows[((size_t)i * g_win_w + x + 2) * 4];
            if (abs(a[0] - b[0]) + abs(a[1] - b[1]) + abs(a[2] - b[2]) > 40) n++;
        }
        if (n > best_n) { best_n = n; best_x = x; }
    }
    if (best_n < kRows - 2) return;
    static uint32_t seq = 0;
    std::vector<uint8_t> px((size_t)g_win_w * g_win_h * 4), fl(px.size());
    glReadPixels(0, 0, g_win_w, g_win_h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    const size_t stride = (size_t)g_win_w * 4;
    for (int y = 0; y < g_win_h; y++)
        memcpy(&fl[y * stride], &px[(size_t)(g_win_h - 1 - y) * stride], stride);
    for (size_t i = 3; i < fl.size(); i += 4) fl[i] = 255;
    char path[512];
    snprintf(path, sizeof(path), "%s/out_%05u_%s%05u_x%d.png", g_dump_dir, seq++, kind,
             g_present_count, best_x);
    write_png(path, fl.data(), g_win_w, g_win_h);
}
void render_set_output_fbo(unsigned fbo) { g_output_fbo = (GLuint)fbo; }
uint32_t present_count() { return g_present_count; }
void render_set_frame_marker(bool on) { g_frame_marker = on; }
void render_set_dump_output(bool on) { g_dump_output = on; }

bool render_repaint() {
    if (!g_have_present) return false;
    blit_to_output(g_last_present, g_snap_tex);
    dump_output("r");
    return true;
}

// Executes a batch. Returns true if it contained a Present.
void render_set_vr_eye(const float proj[16], const float view[16], float hud_scale) {
    memcpy(g_vr_proj, proj, sizeof(g_vr_proj));
    memcpy(g_vr_view, view, sizeof(g_vr_view));
    g_vr_hud_scale = hud_scale;
}

static bool execute_batch(Batch& b, bool do_present);

// The size of the frame the game actually scans out, which is the display copy's. It is
// not the EFB's size: the EFB is 640x528 and the game displays 640x480 of it.
static void display_size(const Batch& b, uint32_t& w, uint32_t& h) {
    w = EFB_W;
    h = EFB_H;
    for (auto& c : b.cmds)
        if (c.type == CmdType::EfbCopy && c.copy.to_xfb) { w = c.copy.dst_w; h = c.copy.dst_h; }
}

// A copy that takes the whole displayed frame is the game compositing its finished
// image: a post pass reads the EFB out and draws it straight back as a screen-filling
// quad. Smaller copies are real scene content -- the water reflection, the sprite sheet
// the spray uses -- and the eye pass needs them.
static bool is_fullscreen_copy(const EfbCopyCmd& c, uint32_t dw, uint32_t dh) {
    return !c.to_xfb && c.dst_w >= dw && c.dst_h >= dh;
}

// Marks the commands an eye must not replay.
//
// An off-screen pass draws into a target and then copies it out -- a reflection, the
// sheet the spray uses. Its draws are in that target's space, so re-aiming them at an
// eye is meaningless, and the first eye has already produced all of them against the
// EFB. Everything else -- the main scene, the composite over it, the HUD -- is replayed.
//
// What marks such a pass is that its copy *clears* the EFB, since the next pass needs it
// empty. A copy that does not clear is a grab: the game lifting a piece of the live
// scene to texture with, which is how the spray is done -- at speed it takes some fifty
// 32x32 and 64x64 rects from scattered screen positions, each preceded by no draws of
// its own. Treating every copy as ending a pass instead throws away whatever draws
// happen to sit in front of the first grab, and the water surface is among them: the
// ocean disappeared the moment the racer was fast enough to throw spray.
static void mark_offscreen_passes(const Batch& b, std::vector<uint8_t>& skip) {
    uint32_t dw, dh;
    display_size(b, dw, dh);
    skip.assign(b.cmds.size(), 0);
    size_t pass_start = 0;
    for (size_t i = 0; i < b.cmds.size(); i++) {
        if (b.cmds[i].type != CmdType::EfbCopy) continue;
        const EfbCopyCmd& c = b.cmds[i].copy;
        if (!c.to_xfb && c.clear && !is_fullscreen_copy(c, dw, dh))
            for (size_t j = pass_start; j <= i; j++) skip[j] = 1;
        pass_start = i + 1;
    }
}

// Texture ids holding a copy of a whole frame.
//
// A draw sampling one is screen-space, so in an eye it is a flat billboard rather than
// something in the world -- it leaves a faint rectangular seam where its edges fall. It
// is tempting to drop those draws, but in this game the water surface is one of them:
// dropping it leaves the seabed showing through bare sand instead of blue-green water,
// which is far worse than the seam. WR_EYE_SKIPCOMP drops them anyway, for comparing.
static std::vector<uint32_t> g_fullscreen_tex;

// These ids are stable across frames -- an EFB copy keeps the id its destination address
// was registered under -- so the set only ever needs adding to.
static void note_fullscreen_copies(const Batch& b) {
    uint32_t dw, dh;
    display_size(b, dw, dh);
    for (auto& c : b.cmds) {
        if (c.type != CmdType::EfbCopy || !is_fullscreen_copy(c.copy, dw, dh) || !c.copy.tex_id)
            continue;
        bool known = false;
        for (uint32_t id : g_fullscreen_tex) known |= (id == c.copy.tex_id);
        if (!known) g_fullscreen_tex.push_back(c.copy.tex_id);
    }
}

static bool samples_fullscreen_copy(const PixelState& st) {
    for (int i = 0; i < 8; i++) {
        if (!st.tex_is_efb[i] || !st.tex_id[i]) continue;
        for (uint32_t id : g_fullscreen_tex)
            if (id == st.tex_id[i]) return true;
    }
    return false;
}

// Draw one eye's view of a batch into `fbo`.
//
// The vertex buffer, the CPU-side transform in xf.cpp and any render-to-texture results
// are all shared between the eyes -- only the uniforms and the draw calls are repeated,
// which is what makes stereo affordable here. Pass do_copies for the first eye only;
// the second reuses what it produced.
bool render_execute_eye(Batch& b, unsigned fbo, int w, int h, bool do_copies) {
    // The scene samples textures the game produces by copying them back out of the EFB:
    // the water reflection, the sprite sheet the spray uses. An eye pass never draws into
    // the EFB, so on its own it would copy out an empty one -- which is what left the ski
    // untextured and put a black quad on the water. So the first eye runs the whole frame
    // flat into the EFB exactly as the hardware would, minus the scanout, and the eyes
    // then re-project only the main scene on top of correct textures.
    if (do_copies) {
        g_vr_active = false;
        execute_batch(b, false);  // which also notes this batch's whole-frame copies
    }
    static std::vector<uint8_t> skip;
    mark_offscreen_passes(b, skip);
    // WR_EYELOG=1 reports how a frame was split, which is the only way to tell a scene
    // rendered at the wrong field of view from a composite quad standing in for one.
    static const bool eyelog = getenv("WR_EYELOG") != nullptr;
    int n_drawn = 0, n_skipped = 0;
    if (eyelog && do_copies) {
        uint32_t dw, dh;
        display_size(b, dw, dh);
        fprintf(stderr, "[eye] f%u cmds=%zu display=%ux%u\n", g_render_frame, b.cmds.size(),
                dw, dh);
        size_t pass_start = 0;
        for (size_t i = 0; i < b.cmds.size(); i++) {
            if (b.cmds[i].type != CmdType::EfbCopy) continue;
            int nd = 0;
            for (size_t j = pass_start; j < i; j++) nd += b.cmds[j].type == CmdType::Draw;
            const EfbCopyCmd& cc = b.cmds[i].copy;
            fprintf(stderr, "[eye]   copy %ux%u xfb=%d tex=%u full=%d draws=%d clr=%d%d src=%u,%u+%ux%u %s\n",
                    cc.dst_w, cc.dst_h, (int)cc.to_xfb, cc.tex_id,
                    (int)is_fullscreen_copy(cc, dw, dh), nd, (int)cc.clear,
                    (int)cc.clear_color, cc.src_x, cc.src_y, cc.src_w, cc.src_h,
                    skip[i] ? "SKIP" : "replay");
            pass_start = i + 1;
        }
    }

    g_vr_active = true;
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)fbo);
    glViewport(0, 0, w, h);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(1, 1, 1, 1);
    glDepthMask(GL_TRUE);
    glClearColor(0, 0, 0, 1);
    clear_depth(1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);

    uint32_t cur_state = UINT32_MAX;
    int cur_prim = -1;
    for (size_t i = 0; i < b.cmds.size(); i++) {
        Cmd& c = b.cmds[i];
        // Copies and the present are the first eye's business, done against the EFB.
        if (c.type != CmdType::Draw) {
            cur_state = UINT32_MAX;
            continue;
        }
        if (skip[i]) { n_skipped++; continue; }
        // WR_EYE_SKIPCOMP drops the screen-space passes entirely, for comparing against
        // drawing them flat across the eye. Dropping the water one leaves bare seabed.
        static const bool skipcomp = getenv("WR_EYE_SKIPCOMP") != nullptr;
        if (skipcomp && samples_fullscreen_copy(b.states[c.state])) { n_skipped++; continue; }
        n_drawn++;
        if (c.state != cur_state || c.prim != cur_prim) {
            apply_state(b.states[c.state], c.prim);
            // apply_state binds the EFB's scissor and viewport expectations; the eye
            // target overrides both.
            glViewport(0, 0, w, h);
            glDisable(GL_SCISSOR_TEST);
            cur_state = c.state;
            cur_prim = c.prim;
        }
        static const GLenum mode[3] = {GL_TRIANGLES, GL_LINES, GL_POINTS};
        glDrawArrays(mode[c.prim], c.first, c.count);
    }
    if (eyelog && do_copies)
        fprintf(stderr, "[eye]   drawn=%d skipped_composite=%d\n", n_drawn, n_skipped);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    g_vr_active = false;
    return true;
}

// Release GL textures the game has stopped using.
//
// EFB copies are included. A copy to an address it already holds reuses its texture, so
// the fixed targets never come through here, but the spray copies to rotating addresses
// and would otherwise strand a texture per sprite per frame. They are given a longer
// idle period than the guest-side cache so that an address is always forgotten there
// first: a draw can then never reach an id whose texture has already gone.
static void evict_textures() {
    if ((g_render_frame & 63) != 0) return;
    for (auto it = g_textures.begin(); it != g_textures.end();) {
        const uint32_t idle = it->second.efb ? 4 * kTexIdleFrames : kTexIdleFrames;
        if (g_render_frame - it->second.last_used > idle) {
            glDeleteTextures(1, &it->second.tex);
            it = g_textures.erase(it);
        } else {
            ++it;
        }
    }
}

// Reads back the column the previous frame's seam was on, and reports the first command
// after which the discontinuity is present. Sampling only draws was not enough: the seam
// was never there after any draw, which leaves the copies -- and the scissored clear each
// one performs -- as the only remaining suspects.
static void seam_probe(const char* what, int a, int bval) {
    if (!g_seam_trace || g_seam_x <= 0) return;
    const int sx = g_seam_x * g_scale, hh = EFB_H * g_scale;
    static std::vector<uint8_t> col;
    col.resize((size_t)hh * 2 * 4);
    // The detector compares column x with x+1, so the probe has to read that same pair.
    glReadPixels(sx, 0, 2, hh, GL_RGBA, GL_UNSIGNED_BYTE, col.data());
    int rows = 0;
    for (int y = 0; y < hh; y++) {
        const uint8_t* p = &col[(size_t)y * 8];
        if (abs(p[0] - p[4]) + abs(p[1] - p[5]) + abs(p[2] - p[6]) > 40) rows++;
    }
    if (!strcmp(what, "before-present"))
        fprintf(stderr, "[seam] probe at x=%d before present: %d of %d rows differ\n", g_seam_x,
                rows, hh);
    if (rows > hh / 3) {
        fprintf(stderr, "[seam] appears after %s(%d,%d) on %d of %d rows\n", what, a, bval, rows,
                hh);
        if (g_seam_batch && g_seam_cmd) {
            // The raw inputs for the offending draw: what the game actually submitted,
            // before this renderer's interpretation of it.
            const Batch& bb = *g_seam_batch;
            const Cmd& cc = *g_seam_cmd;
            const PixelState& st = bb.states[cc.state];
            fprintf(stderr, "[seam]   proj type=%d [%g %g %g %g %g %g]\n", (int)st.proj[6],
                    st.proj[0], st.proj[1], st.proj[2], st.proj[3], st.proj[4], st.proj[5]);
            fprintf(stderr, "[seam]   viewport sx=%g sy=%g ox=%g oy=%g\n", st.viewport[0],
                    st.viewport[1], st.viewport[3], st.viewport[4]);
            const uint32_t cmode0 = st.bp[0x41], genmode = st.bp[0x00], atest = st.bp[0xF3];
            fprintf(stderr, "[seam]   cmode0=%06X blend=%d logic=%d src=%u dst=%u subtract=%d\n",
                    cmode0, (int)(cmode0 & 1), (int)((cmode0 >> 1) & 1),
                    (cmode0 >> 8) & 7, (cmode0 >> 5) & 7, (int)((cmode0 >> 11) & 1));
            fprintf(stderr, "[seam]   genmode=%06X tevstages=%u texgens=%u  alpha=%06X\n", genmode,
                    ((genmode >> 10) & 0xF) + 1, (genmode >> 4) & 0xF, atest);
            for (int i = 0; i < 4; i++)
                fprintf(stderr, "[seam]   tev_reg[%d]=%08X,%08X konst=%08X,%08X\n", i,
                        st.tev_reg[i][0], st.tev_reg[i][1], st.tev_konst[i][0], st.tev_konst[i][1]);
            // Write the sampled textures out so the alpha that drives the blend can be
            // looked at rather than inferred. GLES has no glGetTexImage, so each one is
            // attached to a scratch framebuffer and read back.
            if (g_dump_dir) {
                GLuint fbo = 0;
                glGenFramebuffers(1, &fbo);
                glBindFramebuffer(GL_FRAMEBUFFER, fbo);
                for (int i = 0; i < 8; i++) {
                    auto it = g_textures.find(st.tex_id[i]);
                    if (!st.tex_id[i] || it == g_textures.end()) continue;
                    const int tw = it->second.w, th = it->second.h;
                    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                           it->second.tex, 0);
                    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
                        continue;
                    std::vector<uint8_t> t((size_t)tw * th * 4);
                    glReadPixels(0, 0, tw, th, GL_RGBA, GL_UNSIGNED_BYTE, t.data());
                    char path[512];
                    snprintf(path, sizeof(path), "%s/seamtex_%d_%u.png", g_dump_dir, i,
                             st.tex_id[i]);
                    write_png(path, t.data(), tw, th);
                    fprintf(stderr, "[seam]   wrote %s (%dx%d)\n", path, tw, th);
                }
                glBindFramebuffer(GL_FRAMEBUFFER, g_efb_fbo);
                glDeleteFramebuffers(1, &fbo);
            }
            for (int i = 0; i < 8; i++) {
                if (!st.tex_id[i]) continue;
                auto it = g_textures.find(st.tex_id[i]);
                fprintf(stderr, "[seam]   tex%d id=%u%s %s %ux%u\n", i, st.tex_id[i],
                        st.tex_is_efb[i] ? " (efb copy)" : "",
                        it == g_textures.end() ? "MISSING FROM THE GL CACHE" : "present",
                        it == g_textures.end() ? 0 : it->second.w,
                        it == g_textures.end() ? 0 : it->second.h);
            }
            for (uint32_t v = cc.first; v < cc.first + cc.count && v < bb.verts.size(); v++)
                fprintf(stderr, "[seam]   vert %u pos=(%g, %g, %g)\n", v - cc.first,
                        bb.verts[v].pos[0], bb.verts[v].pos[1], bb.verts[v].pos[2]);
        }
        if (!g_draw_extents.empty()) {
            const DrawExtent& d = g_draw_extents.back();
            fprintf(stderr, "[seam]   that draw: verts=%u x=%.1f..%.1f y=%.1f..%.1f %s "
                    "scissor=%d,%d..%d,%d\n", d.verts, d.x0, d.x1, d.y0, d.y1,
                    d.ortho ? "2D" : "3D", d.sc_x0, d.sc_y0, d.sc_x1, d.sc_y1);
        }
        g_seam_trace = false;
    }
}

// WR_QUADPROBE=1 reports every full-height orthographic quad and, crucially, whether it
// changed any pixels: the framebuffer under it is read back before and after the draw.
//
// It exists to compare two platforms without assuming they do the same thing. The same
// source renders this game correctly on desktop GL and with a hard vertical band on GLES,
// and the band's edge is one of these quads. Whether that quad is submitted at all, what
// it samples, and whether it has any visible effect are three separate questions, and all
// three are answered here in a form that can be diffed between machines.
struct QuadProbe {
    bool pending = false;
    int x = 0, y = 0, w = 0, h = 0;
    uint32_t tex = 0, texw = 0, texh = 0, cmode = 0;
    float x0 = 0, x1 = 0, y0 = 0, y1 = 0;
    std::vector<uint8_t> before, after;
};
static QuadProbe g_qp;

static void quad_probe_begin(const DrawExtent& e, const PixelState& st) {
    g_qp.pending = false;
    if (!e.ortho || e.verts != 6 || (e.y1 - e.y0) < 400.0f) return;
    int x = (int)floorf(e.x0), y_top = (int)floorf(e.y0);
    int w = (int)ceilf(e.x1) - x, h = (int)ceilf(e.y1) - y_top;
    if (x < 0) { w += x; x = 0; }
    if (y_top < 0) { h += y_top; y_top = 0; }
    if (x + w > EFB_W) w = EFB_W - x;
    if (y_top + h > EFB_H) h = EFB_H - y_top;
    if (w <= 0 || h <= 0) return;
    g_qp.x = x * g_scale;
    g_qp.y = (EFB_H - y_top - h) * g_scale;  // glReadPixels counts rows from the bottom
    g_qp.w = w * g_scale;
    g_qp.h = h * g_scale;
    g_qp.x0 = e.x0; g_qp.x1 = e.x1; g_qp.y0 = e.y0; g_qp.y1 = e.y1;
    g_qp.cmode = st.bp[0x41];
    g_qp.tex = st.tex_id[0];
    auto it = g_textures.find(g_qp.tex);
    g_qp.texw = it == g_textures.end() ? 0 : it->second.w;
    g_qp.texh = it == g_textures.end() ? 0 : it->second.h;
    g_qp.before.resize((size_t)g_qp.w * g_qp.h * 4);
    glReadPixels(g_qp.x, g_qp.y, g_qp.w, g_qp.h, GL_RGBA, GL_UNSIGNED_BYTE, g_qp.before.data());
    g_qp.pending = true;
}

static void quad_probe_end() {
    if (!g_qp.pending) return;
    g_qp.pending = false;
    g_qp.after.resize(g_qp.before.size());
    glReadPixels(g_qp.x, g_qp.y, g_qp.w, g_qp.h, GL_RGBA, GL_UNSIGNED_BYTE, g_qp.after.data());
    size_t changed = 0;
    long long sum = 0;
    for (size_t i = 0; i + 3 < g_qp.before.size(); i += 4) {
        const int d = abs(g_qp.before[i] - g_qp.after[i]) + abs(g_qp.before[i + 1] - g_qp.after[i + 1]) +
                      abs(g_qp.before[i + 2] - g_qp.after[i + 2]);
        if (d > 2) changed++;
        sum += d;
    }
    const size_t px = g_qp.before.size() / 4;
    fprintf(stderr, "[quad] f%u x=%.0f..%.0f y=%.0f..%.0f tex=%u(%ux%u) cmode=%06X "
            "changed=%zu/%zu (%.1f%%) meandelta=%.2f\n",
            g_render_frame, g_qp.x0, g_qp.x1, g_qp.y0, g_qp.y1, g_qp.tex, g_qp.texw, g_qp.texh,
            g_qp.cmode, changed, px, px ? 100.0 * (double)changed / (double)px : 0.0,
            px ? (double)sum / (double)px : 0.0);
}

// Runs a batch into the EFB the way the hardware would. With do_present false the final
// scanout is skipped but everything else -- including every render-to-texture copy -- still
// happens, which is how the stereo path obtains the textures its eye passes sample.
static bool execute_batch(Batch& b, bool do_present) {
    g_render_frame++;
    evict_textures();
    for (auto& t : b.new_textures) upload_texture(*t);
    glBindFramebuffer(GL_FRAMEBUFFER, g_efb_fbo);
    glViewport(0, 0, EFB_W * g_scale, EFB_H * g_scale);
    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER, b.verts.size() * sizeof(GpuVertex), b.verts.data(), GL_STREAM_DRAW);
    note_fullscreen_copies(b);
    static const bool batchlog = getenv("WR_EYELOG") != nullptr;
    if (batchlog) {
        int nd = 0, nc = 0, np = 0;
        for (auto& c : b.cmds) {
            nd += c.type == CmdType::Draw;
            nc += c.type == CmdType::EfbCopy;
            np += c.type == CmdType::Present;
        }
        fprintf(stderr, "[batch] f%u cmds=%zu draws=%d copies=%d present=%d verts=%zu\n",
                g_render_frame, b.cmds.size(), nd, nc, np, b.verts.size());
    }
    bool presented = false;
    uint32_t cur_state = UINT32_MAX;
    int cur_prim = -1;
    // WR_NO_COMP drops draws that sample a copy of the whole frame, in the flat path too.
    // The duplicate racer on the water is visible without any of the VR code, so this is
    // how to tell whether that draw is responsible for it.
    static const bool no_comp = getenv("WR_NO_COMP") != nullptr;
    static const bool only_comp = getenv("WR_ONLY_COMP") != nullptr;
    static const bool complog = getenv("WR_COMPLOG") != nullptr;
    static const bool quadprobe = getenv("WR_QUADPROBE") != nullptr;
    static const bool seam_record = getenv("WR_SEAM") != nullptr || quadprobe;
    if (seam_record) g_draw_extents.clear();
    int draw_index = 0;
    for (auto& c : b.cmds) {
        switch (c.type) {
        case CmdType::Draw: {
            const bool comp = (no_comp || complog || only_comp) &&
                              samples_fullscreen_copy(b.states[c.state]);
            if (only_comp && !comp) break;
            // WR_DRAWLOG=<frame> lists every draw in one frame with its index, so a
            // specific piece of geometry can be found and then skipped by index.
            static const uint32_t drawlog = getenv("WR_DRAWLOG") ? atoi(getenv("WR_DRAWLOG")) : 0;
            // WR_DRAW_SKIP=a-b drops a range of draw indices, to attribute a piece of the
            // image to the draws that made it.
            static int skip_lo = -1, skip_hi = -1;
            static bool skip_parsed = false;
            if (!skip_parsed) {
                skip_parsed = true;
                if (const char* s = getenv("WR_DRAW_SKIP")) {
                    skip_lo = atoi(s);
                    const char* dash = strchr(s, '-');
                    skip_hi = dash ? atoi(dash + 1) : skip_lo;
                }
            }
            if (drawlog && g_render_frame == drawlog) {
                const PixelState& st = b.states[c.state];
                fprintf(stderr, "[draw] %d verts=%u st=%u texgens=%u", draw_index, c.count,
                        c.state, st.num_texgens);
                for (int i = 0; i < 8; i++)
                    if (st.tex_id[i]) fprintf(stderr, " t%d=%u%s", i, st.tex_id[i],
                                              st.tex_is_efb[i] ? "*" : "");
                fprintf(stderr, "\n");
            }
            const int this_draw = draw_index++;
            if (seam_record) {
                // Mirror of the vertex shader's flat path: project, then apply GX's
                // viewport transform, and take the screen-space bounds of the draw.
                const PixelState& st = b.states[c.state];
                const float* p = st.proj;
                const bool ortho = (int)p[6] != 0;
                const float* vp = st.viewport;
                int sxo, syo;
                scissor_offset(st.bp, sxo, syo);
                const float ax = 2.0f * (vp[3] - sxo) / EFB_W - 1.0f, bx = 2.0f * vp[0] / EFB_W;
                const float ay = 2.0f * (vp[4] - syo) / EFB_H - 1.0f, by = 2.0f * vp[1] / EFB_H;
                DrawExtent e{this_draw, c.count, 1e9f, -1e9f, 1e9f, -1e9f, ortho, 0, 0, 0, 0, 0, 0};
                e.off_x = sxo; e.off_y = syo;
                e.sc_x0 = (int)(st.bp[0x20] >> 12 & 0x7FF) - sxo;
                e.sc_y0 = (int)(st.bp[0x20] & 0x7FF) - syo;
                e.sc_x1 = (int)(st.bp[0x21] >> 12 & 0x7FF) - sxo + 1;
                e.sc_y1 = (int)(st.bp[0x21] & 0x7FF) - syo + 1;
                for (uint32_t v = c.first; v < c.first + c.count && v < b.verts.size(); v++) {
                    const float* q = b.verts[v].pos;
                    float cx, cy, cw;
                    if (ortho) {
                        // apply_state builds P[0]=p[0], P[12]=p[1], P[5]=p[2], P[13]=p[3],
                        // so with w=1 the translate is a plain add, not a z term.
                        cx = p[0] * q[0] + p[1];
                        cy = p[2] * q[1] + p[3];
                        cw = 1.0f;
                    } else {
                        cx = p[0] * q[0] + p[1] * q[2];
                        cy = p[2] * q[1] + p[3] * q[2];
                        cw = -q[2];
                    }
                    if (fabsf(cw) < 1e-6f) continue;
                    const float nx = ax + bx * (cx / cw), ny = ay + by * (cy / cw);
                    const float sx = (nx * 0.5f + 0.5f) * EFB_W, sy = (ny * 0.5f + 0.5f) * EFB_H;
                    e.x0 = fminf(e.x0, sx); e.x1 = fmaxf(e.x1, sx);
                    e.y0 = fminf(e.y0, sy); e.y1 = fmaxf(e.y1, sy);
                }
                if (e.x0 < 1e8f) g_draw_extents.push_back(e);
                if (quadprobe && e.x0 < 1e8f) quad_probe_begin(e, st);
            }
            if (skip_lo >= 0 && this_draw >= skip_lo && this_draw <= skip_hi) break;
            // WR_NO_EFBTEX drops draws that sample a partial EFB copy -- here, the copy of
            // the water surface that the game tints submerged geometry with. Unlike a draw
            // index this is stable from frame to frame, which matters because the game's
            // timebase is wall-clock driven and frame N is not the same moment twice.
            static const bool no_efbtex = getenv("WR_NO_EFBTEX") != nullptr;
            if (no_efbtex) {
                const PixelState& st = b.states[c.state];
                bool partial = false;
                for (int i = 0; i < 8; i++)
                    if (st.tex_is_efb[i] && st.tex_id[i]) partial = true;
                if (partial && !samples_fullscreen_copy(st)) break;
            }
            if (complog) {
                // Every draw that samples a render-to-texture result, not just the
                // whole-frame ones: the duplicate racer is in one of these layers and
                // the whole-frame one turned out to be innocent.
                const PixelState& st = b.states[c.state];
                bool any_efb = false;
                for (int i = 0; i < 8; i++) any_efb |= st.tex_is_efb[i] && st.tex_id[i];
                if (any_efb) {
                    fprintf(stderr, "[efb] f%u verts=%u texgens=%u cols=%u full=%d", g_render_frame,
                            c.count, st.num_texgens, st.num_colors, (int)comp);
                    for (int i = 0; i < 8; i++)
                        if (st.tex_id[i]) fprintf(stderr, " t%d=%u%s", i, st.tex_id[i],
                                                  st.tex_is_efb[i] ? "*" : "");
                    fprintf(stderr, "\n");
                }
            }
            if (comp && no_comp) break;
            if (c.state != cur_state || c.prim != cur_prim) {
                apply_state(b.states[c.state], c.prim);
                cur_state = c.state;
                cur_prim = c.prim;
            }
            static const GLenum mode[3] = {GL_TRIANGLES, GL_LINES, GL_POINTS};
            glDrawArrays(mode[c.prim], c.first, c.count);
            // With a seam's position known from the previous frame, read one scanline
            // after each draw and name the first draw that puts the discontinuity there.
            // Neither the geometry extents nor the scissor rects accounted for it, so the
            // only way left is to watch the pixels change.
            quad_probe_end();
            g_seam_batch = &b;
            g_seam_cmd = &c;
            seam_probe("draw", this_draw, (int)c.count);
            g_seam_batch = nullptr;
            g_seam_cmd = nullptr;
            break;
        }
        case CmdType::EfbCopy:
            do_efb_copy(c.copy);
            glViewport(0, 0, EFB_W * g_scale, EFB_H * g_scale);
            seam_probe(c.copy.to_xfb ? "xfb-copy" : "copy", (int)c.copy.dst_w, (int)c.copy.dst_h);
            cur_state = UINT32_MAX;
            break;
        case CmdType::Present:
            seam_probe("before-present", 0, 0);
            if (do_present) present(c.copy);
            presented = true;
            glBindFramebuffer(GL_FRAMEBUFFER, g_efb_fbo);
            glViewport(0, 0, EFB_W * g_scale, EFB_H * g_scale);
            cur_state = UINT32_MAX;
            break;
        }
    }
    return presented;
}

bool render_execute(Batch& b) { return execute_batch(b, true); }

}  // namespace gx
