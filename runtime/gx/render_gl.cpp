// GL back end for the GX pixel pipeline. Runs on the main thread.
//
// One source serves desktop OpenGL 3.3 core and OpenGL ES 3.2; where the two profiles
// differ, the difference is confined to the small block of helpers below.
#include "../runtime.h"
#include "render.h"
#include "render_gl.h"
#include "shadergen.h"
#include "gl.h"
#include <chrono>
#include <unordered_map>

bool write_png(const char* path, const uint8_t* rgba, int w, int h);

namespace gx {

static const int EFB_W = 640, EFB_H = 528;

// The renderer's half of WR_FRAMETIME (see xf.cpp): one line per batch with the time
// spent uploading textures, uploading the vertex buffer, and issuing the frame, plus how
// many times the full pixel state had to be re-applied. CPU time only: the GPU runs
// behind, so a long frame here is driver and submission cost, not fill.
static const bool g_frametime = getenv("WR_FRAMETIME") != nullptr;
static inline double ms_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

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
    GLint u_vr, u_view, u_screen_uv, u_screen_px, u_screen_ripple;
};

// Samples kept per hardware pixel, in each axis. See render_set_internal_scale.
static int g_scale = 2;

// VR eye state. When active, perspective batches are re-projected for the eye and the
// orthographic ones (the 2D HUD) are painted on a frame standing out in front of the
// game's camera -- see render_hud_frame.
static bool g_vr_active = false;
static float g_vr_proj[16], g_vr_view[16];
// The eye's view with the world's pitch taken out, for world geometry. The HUD frame uses
// g_vr_view untouched -- it is placed in the headset's space, not the game's.
static float g_vr_view_world[16];
static float g_world_pitch = 0.0f;
static float g_vr_hud[16];
// What the eye has drawn so far, standing in for the game's copy of the finished frame.
// There are two because the two users want different moments: the water refracts the
// scene as it stood *before* the water was drawn, and the spray refracts it after, water
// included. One texture serving both would quietly re-point the water's lookup -- and the
// game's own final composite, which samples the same whole-frame copy -- at whatever the
// spray grabbed later. See grab_eye().
struct EyeGrab { GLuint tex; int w, h; };
static EyeGrab g_eye_grab, g_spray_grab;
static int g_eye_w, g_eye_h;
static bool is_fullscreen_tex(uint32_t id);
static GLuint g_efb_fbo, g_efb_color, g_efb_depth;
// Copy of the EFB as it looked at the last present. The display copy is immediately
// followed by an EFB clear, so repainting has to come from here, not the live EFB.

static GLuint g_vao, g_vbo;
static GLuint g_copy_prog, g_copy_vao;
static GLint g_copy_u_src, g_copy_u_rect, g_copy_u_mode, g_copy_u_depth;
static GLuint g_blit_prog;
static GLint g_blit_u_src, g_blit_u_rect;
static GLuint g_copy_fbo;
static GLuint g_vs;
static std::unordered_map<ShaderKey, Program, ShaderKeyHash> g_programs;
// `grab` marks one of the spray's screen-space grabs; see is_grab_copy().
struct GlTex { GLuint tex; uint32_t w, h; bool efb; bool grab; uint32_t last_used; };
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

// ---------------------------------------------------------------------------
// Shader cache
//
// A TEV configuration is compiled the first time a draw uses it, on the render thread, in
// the middle of a frame. The race start brings in nine at once (spray, wake, the speed
// effects) and later stretches of a course add more: on this machine's compiler that
// frame took 60 ms against a 4 ms norm, and a mobile driver takes tens of milliseconds
// per program, so a burst of them is a visible hitch at a fixed spot in the course.
//
// So every key that gets compiled is written to a file, and the next run builds all of
// them in render_init before the game starts. Where the driver can hand back program
// binaries (GL ES 3.0 can; macOS reports no binary formats) those are stored too, which
// turns the second run's startup from compiling into loading. A binary is only trusted
// with the same driver and the same generated source: the file carries the GL strings
// and each record a hash of the GLSL its key generates, and either changing drops back
// to compiling that record and rewriting the file.
// ---------------------------------------------------------------------------
#if defined(WR_GL_ES) || defined(__APPLE__)
#define WR_HAVE_PROGRAM_BINARY 1
#else
// The desktop glad loader stops at 3.3; program binaries are 4.1.
#define WR_HAVE_PROGRAM_BINARY 0
#endif

static std::string g_shader_cache_path;
static bool g_binaries_supported = false;
static constexpr uint32_t kCacheMagic = 0x43535257;  // "WRSC"
static constexpr uint32_t kCacheVersion = 1;

static uint64_t hash_str(const char* s, uint64_t h = 1469598103934665603ull) {
    for (; s && *s; s++) h = (h ^ (uint8_t)*s) * 1099511628211ull;
    return h;
}

// Identifies the driver whose binaries the file holds.
static uint64_t driver_id() {
    uint64_t h = hash_str((const char*)glGetString(GL_VENDOR));
    h = hash_str((const char*)glGetString(GL_RENDERER), h);
    return hash_str((const char*)glGetString(GL_VERSION), h);
}

static void shader_cache_write_record(FILE* f, const ShaderKey& k, uint64_t src_hash, GLuint prog) {
    fwrite(&k, sizeof(k), 1, f);
    fwrite(&src_hash, sizeof(src_hash), 1, f);
    uint32_t fmt = 0, len = 0;
#if WR_HAVE_PROGRAM_BINARY
    std::vector<uint8_t> bin;
    if (g_binaries_supported) {
        GLint n = 0;
        glGetProgramiv(prog, GL_PROGRAM_BINARY_LENGTH, &n);
        if (n > 0) {
            bin.resize((size_t)n);
            GLenum e = 0;
            GLsizei got = 0;
            glGetProgramBinary(prog, n, &got, &e, bin.data());
            if (got > 0) { fmt = (uint32_t)e; len = (uint32_t)got; }
        }
    }
    fwrite(&fmt, sizeof(fmt), 1, f);
    fwrite(&len, sizeof(len), 1, f);
    if (len) fwrite(bin.data(), len, 1, f);
#else
    (void)prog;
    fwrite(&fmt, sizeof(fmt), 1, f);
    fwrite(&len, sizeof(len), 1, f);
#endif
}

static void shader_cache_write_header(FILE* f) {
    const uint32_t magic = kCacheMagic, ver = kCacheVersion;
    const uint64_t drv = driver_id();
    fwrite(&magic, sizeof(magic), 1, f);
    fwrite(&ver, sizeof(ver), 1, f);
    fwrite(&drv, sizeof(drv), 1, f);
}

// Compiles a program from source, or restores it from a binary when one is given and
// the driver accepts it. Returns 0 if the binary was refused.
static GLuint build_program(const ShaderKey& k, const std::string& src, const uint8_t* bin, uint32_t fmt, uint32_t len) {
#if WR_HAVE_PROGRAM_BINARY
    if (bin && len && g_binaries_supported) {
        GLuint p = glCreateProgram();
        glProgramBinary(p, (GLenum)fmt, bin, (GLsizei)len);
        GLint ok = 0;
        glGetProgramiv(p, GL_LINK_STATUS, &ok);
        if (ok) return p;
        glDeleteProgram(p);
        return 0;
    }
#else
    (void)bin; (void)fmt; (void)len;
#endif
    if (getenv("WR_DUMP_SHADERS")) fprintf(stderr, "---- shader %zu ----\n%s\n", g_programs.size(), src.c_str());
    GLuint fs = compile(GL_FRAGMENT_SHADER, src);
    GLuint p = glCreateProgram();
    glAttachShader(p, g_vs);
    glAttachShader(p, fs);
    glBindAttribLocation(p, 0, "a_pos");
#if WR_HAVE_PROGRAM_BINARY
    if (g_binaries_supported) glProgramParameteri(p, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
#endif
    glLinkProgram(p);
    GLint ok;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        fatal("program link failed: %s", log);
    }
    glDeleteShader(fs);
    (void)k;
    return p;
}

static const Program& register_program(const ShaderKey& k, GLuint p) {
    Program pr{};
    pr.prog = p;
    pr.u_proj = glGetUniformLocation(p, "u_proj");
    pr.u_vp_a = glGetUniformLocation(p, "u_vp_a");
    pr.u_vp_b = glGetUniformLocation(p, "u_vp_b");
    pr.u_point_size = glGetUniformLocation(p, "u_point_size");
    pr.u_vr = glGetUniformLocation(p, "u_vr");
    pr.u_view = glGetUniformLocation(p, "u_view");
    pr.u_screen_uv = glGetUniformLocation(p, "u_screen_uv");
    pr.u_screen_px = glGetUniformLocation(p, "u_screen_px");
    pr.u_screen_ripple = glGetUniformLocation(p, "u_screen_ripple");
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

void render_set_shader_cache(const char* path) { g_shader_cache_path = path ? path : ""; }

// Builds every program the file remembers. Called once from render_init.
static void shader_cache_load() {
    if (g_shader_cache_path.empty()) return;
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<uint8_t> data;
    if (FILE* f = fopen(g_shader_cache_path.c_str(), "rb")) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n > 0) {
            data.resize((size_t)n);
            if (fread(data.data(), 1, (size_t)n, f) != (size_t)n) data.clear();
        }
        fclose(f);
    }
    if (data.empty()) return;
    const uint8_t* p = data.data();
    const uint8_t* end = p + data.size();
    auto take = [&](void* dst, size_t n) {
        if ((size_t)(end - p) < n) return false;
        memcpy(dst, p, n);
        p += n;
        return true;
    };
    uint32_t magic = 0, ver = 0;
    uint64_t drv = 0;
    if (!take(&magic, 4) || !take(&ver, 4) || !take(&drv, 8) || magic != kCacheMagic || ver != kCacheVersion) {
        fprintf(stderr, "[shaders] ignoring unrecognised cache %s\n", g_shader_cache_path.c_str());
        return;
    }
    const bool same_driver = drv == driver_id();
    int from_binary = 0, compiled = 0;
    bool stale = !same_driver;
    while (p < end) {
        ShaderKey k;
        uint64_t src_hash;
        uint32_t fmt, len;
        if (!take(&k, sizeof(k)) || !take(&src_hash, 8) || !take(&fmt, 4) || !take(&len, 4)) { stale = true; break; }
        const uint8_t* bin = p;
        if ((size_t)(end - p) < len) { stale = true; break; }
        p += len;
        if (g_programs.count(k)) { stale = true; continue; }  // a duplicate, written by a crash mid-append
        std::string src = gen_pixel_shader(k);
        const bool bin_ok = same_driver && len && hash_str(src.c_str()) == src_hash;
        GLuint prog = bin_ok ? build_program(k, src, bin, fmt, len) : 0;
        if (prog) from_binary++;
        else { prog = build_program(k, src, nullptr, 0, 0); compiled++; stale = true; }
        register_program(k, prog);
    }
    // Anything that could not be used as stored is replaced: the whole file is rewritten
    // from the programs now in hand, with fresh binaries where the driver gives them.
    if (stale || (g_binaries_supported && from_binary == 0 && compiled > 0)) {
        if (FILE* f = fopen(g_shader_cache_path.c_str(), "wb")) {
            shader_cache_write_header(f);
            for (auto& kv : g_programs) shader_cache_write_record(f, kv.first, hash_str(gen_pixel_shader(kv.first).c_str()), kv.second.prog);
            fclose(f);
        }
    }
    fprintf(stderr, "[shaders] %d programs from cache (%d from binaries, %d compiled) in %.0f ms\n",
            from_binary + compiled, from_binary, compiled, ms_since(t0));
}

static const Program& get_program(const ShaderKey& k) {
    auto it = g_programs.find(k);
    if (it != g_programs.end()) return it->second;
    const auto t0 = std::chrono::steady_clock::now();
    std::string src = gen_pixel_shader(k);
    GLuint p = build_program(k, src, nullptr, 0, 0);
    const Program& pr = register_program(k, p);
    if (g_frametime) fprintf(stderr, "[shaders] compiled program %zu in %.1f ms\n", g_programs.size(), ms_since(t0));
    if (!g_shader_cache_path.empty()) {
        // Appended rather than rewritten, so a crash later in the run keeps what was
        // learned so far. A missing or truncated file gets a header first.
        FILE* f = fopen(g_shader_cache_path.c_str(), "ab");
        if (f) {
            fseek(f, 0, SEEK_END);
            if (ftell(f) == 0) shader_cache_write_header(f);
            shader_cache_write_record(f, k, hash_str(src.c_str()), p);
            fclose(f);
        }
    }
    return pr;
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
    g_scale = internal_scale < 1 ? 1
                                 : (internal_scale > kMaxInternalScale ? kMaxInternalScale
                                                                       : internal_scale);
    g_vs = compile(GL_VERTEX_SHADER, gen_vertex_shader());
#if WR_HAVE_PROGRAM_BINARY
    {
        GLint n = 0;
        glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &n);
        g_binaries_supported = n > 0;
    }
#endif
    shader_cache_load();

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

// Change how many samples the EFB keeps per hardware pixel, between frames.
//
// The two VR views want different answers. Theater shows the EFB itself, blown up to fill
// a panel wider than the frame was ever drawn for, so every extra sample is detail the
// viewer sees. Stereo never shows it: there it holds only what the eye passes sample out
// of it -- the water reflection, the sheet the spray is cut from -- and the pass that
// fills it is a whole extra scene render on top of the two eyes. So the scale follows the
// view, which means changing it while the game runs.
//
// Nothing in the EFB has to survive the change: it is cleared and redrawn every frame.
// The textures EFB copies land in do not get off so lightly -- they are allocated at
// dst * scale but matched for reuse by their logical size alone, so a survivor would be
// reused at the old resolution and quietly sample wrong. They go with the EFB.
void render_set_internal_scale(int scale) {
    if (scale < 1) scale = 1;
    if (scale > kMaxInternalScale) scale = kMaxInternalScale;
    if (scale == g_scale) return;
    g_scale = scale;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_efb_color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, EFB_W * g_scale, EFB_H * g_scale, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindTexture(GL_TEXTURE_2D, g_efb_depth);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, EFB_W * g_scale, EFB_H * g_scale, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    for (auto it = g_textures.begin(); it != g_textures.end();) {
        if (it->second.efb) {
            glDeleteTextures(1, &it->second.tex);
            it = g_textures.erase(it);
        } else {
            ++it;
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, g_efb_fbo);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        fatal("EFB framebuffer incomplete at internal scale %d", g_scale);
}

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
// GX stores it halved and biased by 342, so GXSetScissorBoxOffset(0, 0) reads back as 342.
// This game sets (-2, -2) -- the register reads 340 -- and moves every viewport by the same
// (-2, -2), so on hardware the two cancel and the picture sits at the EFB's origin.
// Assuming 342 instead left every pass two pixels up and left of where the game believes
// it is. That shows wherever the game samples a copy of the EFB at screen coordinates it
// worked out itself: the water looked its refraction up two pixels out of register, which
// stood a water-tinted second copy of the racer beside the real one, and the last two
// columns and rows of every pass were never drawn.
static void scissor_offset(const uint32_t* bp, int& xoff, int& yoff) {
    xoff = (int)(bp[0x59] & 0x3FF) * 2;
    yoff = (int)((bp[0x59] >> 10) & 0x3FF) * 2;
}

// c = a * b, column-major, element (row r, column k) at m[k * 4 + r].
static void mat4_mul(const float* a, const float* b, float* c) {
    for (int k = 0; k < 4; k++)
        for (int r = 0; r < 4; r++) {
            float sum = 0;
            for (int i = 0; i < 4; i++) sum += a[i * 4 + r] * b[k * 4 + i];
            c[k * 4 + r] = sum;
        }
}

// The water surface refracts by looking the finished frame up at the screen position the
// game computed for each of its vertices. In an eye those positions are the flat view's,
// and the copy is the flat view's too, so water the eye can see beyond the game's own
// 60-degree frustum samples off the edge of the copy and clamps -- the scene smeared down
// the sea in streaks, the racer among it.
//
// Nothing about that is fixable by moving the lookup around, because the data is not in
// the copy. So the eye grabs what it has drawn itself, which covers exactly what the eye
// can see, and the fragment samples it at its own position rather than at the flat view's.
// The indirect stage that ripples the lookup still applies on top, so the water keeps its
// wobble. Costs one full-target copy per eye, taken only on a frame that has such a
// draw -- and the spray wants the same thing at a later moment, so a frame throwing
// spray pays for two. See is_grab_tex().
static void grab_eye(EyeGrab& g, int w, int h) {
    if (!g.tex || g.w != w || g.h != h) {
        if (!g.tex) glGenTextures(1, &g.tex);
        glBindTexture(GL_TEXTURE_2D, g.tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        g.w = w;
        g.h = h;
    } else {
        glBindTexture(GL_TEXTURE_2D, g.tex);
    }
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h);
}

// The spray is composited from screen-space grabs -- the same mechanism as the water
// surface, one size down. At speed it lifts some fifty small rects out of the live scene
// and redraws each as a billboard sampling its own rect, so on hardware a droplet
// refracts exactly the pixels it covers and its edges cannot be seen. An eye re-projects
// the billboard but not the rect, which belongs to the flat view, so the patch agrees
// with nothing behind it and reads as a square with a piece of scene in it.
//
// So these lookups are taken over the way the water's are: the texture becomes the eye's
// own grab and the coordinate becomes the fragment's own position, which is the one place
// the pixels the droplet wants actually are. What survives of the game's own lookup is
// the indirect offset that distorts it -- the droplet's whole visible character, since
// the undistorted part is by construction the background it sits on.
//
// Unlike the whole-frame copies these go to addresses that rotate, so a fresh texture id
// appears for every sprite of every frame; a set of ids would grow without bound and be
// scanned per draw. The flag rides on the texture instead.
static bool is_grab_tex(uint32_t id) {
    // WR_EYE_SPRAY=0 restores the stale lookup, which is the only way to see from inside
    // the headset what this changed.
    static const bool off = getenv("WR_EYE_SPRAY") && atoi(getenv("WR_EYE_SPRAY")) == 0;
    if (off || !id) return false;
    auto it = g_textures.find(id);
    return it != g_textures.end() && it->second.grab;
}

// Whether an eye takes this texture's lookups over at all: a copy of the whole frame
// (the water surface samples one) or one of the spray's grabs.
static bool is_eye_screen_tex(uint32_t id) {
    return (g_eye_grab.tex && is_fullscreen_tex(id)) || (g_spray_grab.tex && is_grab_tex(id));
}

static void apply_state(const PixelState& st, int prim) {
    int xoff, yoff;
    scissor_offset(st.bp, xoff, yoff);
    const uint32_t* bp = st.bp;
    ShaderKey key = make_shader_key(st);
    const Program& pr = get_program(key);
    glUseProgram(pr.prog);

    // Projection. In VR a perspective batch is world geometry and gets the eye's
    // projection instead of the game's; an orthographic one is a 2D element and goes on
    // the HUD frame -- and so does a perspective batch the game placed in view space
    // itself, which is 3D but is no more part of the course than the lap counter is.
    // That is the countdown light rig: left in the world it stands in the water between
    // the viewer and the racer, because the game means it to hang in front of the camera.
    float P[16] = {0};
    const float* p = st.proj;
    const bool perspective = (int)p[6] == 0;
    // Something the game placed in view space but drew with a perspective frustum is a 3D
    // object held in front of the camera, not a 2D overlay. Painting it on the HUD frame
    // flattens it -- that frame's Z column is zero, so every vertex lands on one plane --
    // which throws away the object's own depth. The countdown rig lost the occlusion that
    // hides the lamp behind its lens, and the lamps showed through as white squares.
    //
    // So it keeps the eye's projection and stays 3D. It takes g_vr_view rather than
    // g_vr_view_world: the world pitch is taken out of the *world* to level the sea, and
    // applying it to something attached to the camera is what tilted the rig by 23
    // degrees. Without the correction it hangs in front of the viewer the way the game
    // means it to, and the head still moves within that.
    //
    // WR_EYE_VS3D=0 puts it back on the HUD frame, which is where it was.
    static const bool vs3d = !(getenv("WR_EYE_VS3D") && atoi(getenv("WR_EYE_VS3D")) == 0);
    const bool view_space_3d = perspective && st.view_space && vs3d;
    const bool on_hud_frame = (!perspective || st.view_space) && !view_space_3d;
    if (perspective) {
        P[0] = p[0]; P[8] = p[1]; P[5] = p[2]; P[9] = p[3]; P[10] = p[4]; P[14] = p[5]; P[11] = -1.0f;
    } else {
        P[0] = p[0]; P[12] = p[1]; P[5] = p[2]; P[13] = p[3]; P[10] = p[4]; P[14] = p[5]; P[15] = 1.0f;
    }
    // Note: a draw sampling a copy of the whole frame (the water surface is one) must
    // stay in the world, however tempting its screen-space origin makes the overlay path
    // look. Sending the water through it put the water, and the racer baked into the
    // copy, on a flat panel hanging in front of the camera while the real racer went on
    // moving in the world. The seam at the billboard's edge is the lesser problem.
    if (g_vr_active && !on_hud_frame) {
        // A camera-placed 3D object is viewed with the head transform but without the
        // world's pitch correction; see view_space_3d above.
        const float* view = view_space_3d ? g_vr_view : g_vr_view_world;
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
        glUniformMatrix4fv(pr.u_view, 1, GL_FALSE, view);
        glUniform1i(pr.u_vr, 1);
    } else if (g_vr_active) {
        // A HUD element in an eye. The game's own projection already puts its frame in
        // [-1,1], so that is where the chain picks up -- for the perspective rig too,
        // since the frame's matrix carries clip w through and the divide lands it on
        // the plane just the same.
        //
        // GX's viewport transform is deliberately not in the chain: it places the frame
        // within the 640x528 EFB, and an eye's render target is not the EFB -- the same
        // reason the scissor rect is dropped below. Including it would map the EFB
        // rather than the 480 lines the game displays, leaving the HUD a few per cent
        // small and off centre.
        //
        // Every term is constant for the draw, so the whole chain folds into one matrix
        // here and the shader is left with a single multiply.
        float a[16], b[16], M[16];
        mat4_mul(g_vr_hud, P, a);   // the game's 2D frame, placed in view space
        mat4_mul(g_vr_view, a, b);  // that frame seen from this eye
        mat4_mul(g_vr_proj, b, M);
        glUniformMatrix4fv(pr.u_proj, 1, GL_FALSE, M);
        glUniform1i(pr.u_vr, 2);
    } else {
        glUniformMatrix4fv(pr.u_proj, 1, GL_FALSE, P);
        glUniform1i(pr.u_vr, 0);
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

    // Which texgens feed a copy of the whole frame. In an eye those lookups are taken
    // over: the texture becomes the eye's own grab and the coordinate becomes the
    // fragment's own position, since the game's coordinate belongs to a view this eye is
    // not looking from. See grab_eye().
    uint32_t screen_uv = 0;
    if (g_vr_active) {
        const uint32_t nstg = ((bp[0x00] >> 10) & 15) + 1;
        for (uint32_t s = 0; s < nstg; s++) {
            const uint32_t order = bp[0x28 + s / 2] >> ((s & 1) * 12);
            const uint32_t map = order & 7;
            if ((order & 0x40) && st.tex_is_efb[map] && st.tex_id[map] &&
                is_eye_screen_tex(st.tex_id[map]))
                screen_uv |= 1u << ((order >> 3) & 7);
        }
    }
    glUniform1i(pr.u_screen_uv, (GLint)screen_uv);
    glUniform2f(pr.u_screen_px, g_eye_w ? 1.0f / (float)g_eye_w : 0.0f,
                g_eye_h ? 1.0f / (float)g_eye_h : 0.0f);

    // The indirect offset that distorts a substituted lookup is an absolute displacement
    // in the copy's texels, and the copy's texels are EFB pixels. The eye grab's are not:
    // the eye sees a wider field across more pixels, so one of its pixels covers a
    // different angle. Both grids are pixels over an angle, so the conversion is the
    // ratio of their pixels per unit of frustum tangent -- the game's from its own
    // viewport and projection, the eye's from the target size and the headset's.
    //
    // Without it the droplets keep a displacement of a few pixels on a target several
    // times the EFB's width, which is a fraction of the distortion the game asked for:
    // each one degenerates into an almost exact copy of its own background and the spray
    // disappears rather than reading wrongly.
    //
    // The y term is negated because the two grids run opposite ways. An EFB copy's row 0
    // is the top of its source rect, so +t walks down the screen; the eye grab comes
    // straight off the render target, so its row 0 is the bottom and +t walks up -- which
    // is also the direction gl_FragCoord.y counts, and why the undistorted part of the
    // lookup needs no flip of its own.
    float ripple[2] = {1.0f, 1.0f};
    // WR_EYE_RIPPLE=0 keeps the substitution but leaves the offset unconverted, which is
    // what the water surface shipped with and separates "the lookup is in the wrong
    // space" from "the distortion is the wrong size".
    static const bool no_ripple = getenv("WR_EYE_RIPPLE") && atoi(getenv("WR_EYE_RIPPLE")) == 0;
    if (screen_uv && perspective && !no_ripple) {
        const float gx = fabsf(vp[0]) * p[0], gy = fabsf(vp[1]) * p[2];
        if (gx > 0.0f) ripple[0] = 0.5f * (float)g_eye_w * g_vr_proj[0] / gx;
        if (gy > 0.0f) ripple[1] = -0.5f * (float)g_eye_h * g_vr_proj[5] / gy;
    }
    glUniform2fv(pr.u_screen_ripple, 1, ripple);

    // Textures
    float tsz[16];
    for (int m = 0; m < 8; m++) {
        tsz[m * 2] = tsz[m * 2 + 1] = 1.0f;
        glActiveTexture(GL_TEXTURE0 + m);
        uint32_t id = st.tex_id[m];
        if (screen_uv && st.tex_is_efb[m] && id && is_eye_screen_tex(id)) {
            const EyeGrab& g = is_grab_tex(id) ? g_spray_grab : g_eye_grab;
            // The grab carries its own filtering; a sampler object would override it.
            glBindTexture(GL_TEXTURE_2D, g.tex);
            glBindSampler(m, 0);
            tsz[m * 2] = (float)g.w;
            tsz[m * 2 + 1] = (float)g.h;
            continue;
        }
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
    // In an eye the HUD is a quad out in the world rather than something laid over the
    // finished image, so the game's depth state no longer places it: the scene it is meant
    // to sit over is mostly nearer than the frame, and every element of the HUD is on one
    // plane. It is submitted last, so submission order is the layering.
    if (g_vr_active && on_hud_frame) {
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
    }
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
    // Scissor (EFB coords, y down), measured from the same origin as the viewport.
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
        //
        // WR_EYE_SCISSORLOG reports the draws whose rect is not the whole frame, which
        // are the ones this is throwing away clipping for. Anything the game relies on
        // the scissor to hide is drawn in full in an eye.
        static const bool slog = getenv("WR_EYE_SCISSORLOG") != nullptr;
        if (slog && (x0 > 0 || y0 > 0 || x1 < EFB_W || y1 < EFB_H)) {
            static int shown;
            if (shown++ < 40)
                fprintf(stderr, "[scissor] f%u rect=%d,%d..%d,%d (frame is 0,0..%d,%d)\n",
                        g_render_frame, x0, y0, x1, y1, EFB_W, EFB_H);
        }
        glDisable(GL_SCISSOR_TEST);
        return;
    }
    glEnable(GL_SCISSOR_TEST);
    glScissor(x0 * g_scale, (EFB_H - y1) * g_scale, (x1 - x0) * g_scale, (y1 - y0) * g_scale);
}

// A copy the spray composites from. It neither scans out nor clears -- a clearing copy
// ends an off-screen pass, and a grab deliberately leaves the scene it lifted from intact
// -- and it is small: at speed the spray takes 32x32 and 64x64 rects, while the other
// partial copies in this game (the water reflection, the rect the submerged tint samples)
// are far larger, which is what keeps them out of this. See is_grab_tex().
static constexpr uint32_t kGrabMax = 64;
static bool is_grab_copy(const EfbCopyCmd& c) {
    return !c.to_xfb && !c.clear && c.dst_w <= kGrabMax && c.dst_h <= kGrabMax;
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
        // Set after the reuse path, which carries the previous copy's flags in.
        t.grab = is_grab_copy(c);
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
    blit_to_output(c, g_efb_color);
}

void render_set_output_fbo(unsigned fbo) { g_output_fbo = (GLuint)fbo; }
uint32_t present_count() { return g_present_count; }


// Executes a batch. Returns true if it contained a Present.
void render_set_world_pitch(float pitch_rad) { g_world_pitch = pitch_rad; }

void render_set_vr_eye(const float proj[16], const float view[16], const float hud[16]) {
    memcpy(g_vr_proj, proj, sizeof(g_vr_proj));
    memcpy(g_vr_view, view, sizeof(g_vr_view));
    memcpy(g_vr_hud, hud, sizeof(g_vr_hud));
    if (g_world_pitch == 0.0f) {
        memcpy(g_vr_view_world, view, sizeof(g_vr_view_world));
        return;
    }
    // The camera looks down by `pitch`, so the world's up arrives at (0, cos, sin) in the
    // vertices' own frame. Rotating about X by -pitch takes it back to (0, 1, 0), which is
    // the headset's up, and the sea with it.
    const float c = cosf(g_world_pitch), s = sinf(g_world_pitch);
    float R[16] = {0};
    R[0] = 1.0f;
    R[5] = c;  R[6] = -s;
    R[9] = s;  R[10] = c;
    R[15] = 1.0f;
    mat4_mul(view, R, g_vr_view_world);
}

// The frame the HUD is painted on in stereo: a quad `dist` game units ahead of the game's
// camera, `scale` of the vertical field of view tall and 4:3 wide, with the game's own 2D
// frame mapped onto it corner to corner. It is anchored to the camera rather than to the
// head, so it frames the race while the viewer looks forward and stays where it is when
// they turn to look at something else.
//
// `height` lifts its centre off the forward axis and `pitch_rad` leans the top away, both
// because where a panel wants to hang is a question about a person and not about geometry.
// At 0 and 0 the frame stands vertical and centred on the axis, which is where the eye was
// at the moment the runtime fixed its LOCAL space.
//
// Writing NDC straight into the eye -- what this replaced -- cannot work in stereo. The
// headset's per-eye frustums are asymmetric, so one NDC position is a different direction
// in each eye; there is no depth at which the two images agree, and they never fuse.
void render_hud_frame(float dist, float tan_half_fovy, float scale, float height,
                      float pitch_rad, float out[16]) {
    const float half_h = dist * tan_half_fovy * scale;
    const float half_w = half_h * 4.0f / 3.0f;   // the game's frame is 4:3
    const float c = cosf(pitch_rad), s = sinf(pitch_rad);
    memset(out, 0, 16 * sizeof(float));
    // X: the frame's own right, which the tilt leaves alone.
    out[0] = half_w;
    // Y: its up, leaned back by the pitch. GX clip space and view space both point Y up.
    out[5] = half_h * c;
    out[6] = -half_h * s;
    // W: the centre, `height` up from the forward axis and `dist` down it (-Z forward).
    out[13] = height;
    out[14] = -dist;
    out[15] = 1.0f;
    // The Z column stays zero: every element lands on the plane of the frame. What that
    // costs is the overlay's own depth ordering, which is why the eye path drops the
    // depth test for these draws and lets submission order do the layering.
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

static bool is_fullscreen_tex(uint32_t id) {
    for (uint32_t t : g_fullscreen_tex)
        if (t == id) return true;
    return false;
}

static bool samples_fullscreen_copy(const PixelState& st) {
    for (int i = 0; i < 8; i++)
        if (st.tex_is_efb[i] && st.tex_id[i] && is_fullscreen_tex(st.tex_id[i])) return true;
    return false;
}

static bool samples_grab_copy(const PixelState& st) {
    for (int i = 0; i < 8; i++)
        if (st.tex_is_efb[i] && st.tex_id[i] && is_grab_tex(st.tex_id[i])) return true;
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
    int n_drawn = 0, n_skipped = 0, n_spray = 0;
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
    g_eye_w = w;
    g_eye_h = h;
    bool grabbed = false, grabbed_spray = false;
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
        // The first draw that wants the finished frame is the moment to take it: the
        // scene behind the water is in the target by now and the water is not yet.
        if (!grabbed && samples_fullscreen_copy(b.states[c.state])) {
            grabbed = true;
            grab_eye(g_eye_grab, w, h);
            cur_state = UINT32_MAX;   // the grab left its own texture bound
        }
        // The spray composites itself over the finished scene, the water included, so the
        // grab above -- taken deliberately before the water, which is what the water
        // itself needs -- is a layer short by the time the droplets draw. Re-take it at
        // the first of them, once, and every droplet in the frame then refracts the scene
        // as it actually stands. Costs a second full-target copy per eye on any frame
        // that throws spray, which at speed is every frame.
        if (samples_grab_copy(b.states[c.state])) {
            // How a droplet actually addresses its grab, which decides what the
            // substitution has to carry over: the indirect matrix id per stage (1..3 is
            // a static offset in texels, 5..11 scales the coordinate itself) and the
            // wrap, which folds the coordinate into the copy's own size.
            if (eyelog && do_copies && !grabbed_spray) {
                const PixelState& st = b.states[c.state];
                const uint32_t nstg = ((st.bp[0x00] >> 10) & 15) + 1;
                fprintf(stderr, "[eye]   spray draw: stages=%u nind=%u", nstg,
                        (st.bp[0x00] >> 16) & 7);
                for (uint32_t t = 0; t < nstg; t++) {
                    const uint32_t ic = st.bp[0x10 + t];
                    fprintf(stderr, " s%u[mid=%u sw=%u tw=%u bt=%u]", t, (ic >> 9) & 15,
                            (ic >> 13) & 7, (ic >> 16) & 7, ic & 3);
                }
                fprintf(stderr, "\n");
            }
            if (!grabbed_spray) {
                grabbed_spray = true;
                grab_eye(g_spray_grab, w, h);
                cur_state = UINT32_MAX;
            }
            n_spray++;
        }
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
        fprintf(stderr, "[eye]   drawn=%d skipped_composite=%d spray=%d\n", n_drawn,
                n_skipped, n_spray);
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

// Runs a batch into the EFB the way the hardware would. With do_present false the final
// scanout is skipped but everything else -- including every render-to-texture copy -- still
// happens, which is how the stereo path obtains the textures its eye passes sample.
static bool execute_batch(Batch& b, bool do_present) {
    g_render_frame++;
    evict_textures();
    const auto t_start = std::chrono::steady_clock::now();
    for (auto& t : b.new_textures) upload_texture(*t);
    const double ms_tex = g_frametime ? ms_since(t_start) : 0.0;
    glBindFramebuffer(GL_FRAMEBUFFER, g_efb_fbo);
    glViewport(0, 0, EFB_W * g_scale, EFB_H * g_scale);
    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER, b.verts.size() * sizeof(GpuVertex), b.verts.data(), GL_STREAM_DRAW);
    const double ms_vbo = g_frametime ? ms_since(t_start) - ms_tex : 0.0;
    uint32_t n_apply = 0;
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
                // The projection type and the view-space depth are what decide a draw's
                // fate in an eye: a perspective batch is re-projected as world geometry,
                // anything else goes on the HUD frame. And a draw a few tens of units
                // from the camera is in front of the viewer's face either way.
                float zlo = 1e30f, zhi = -1e30f;
                for (uint32_t v = 0; v < c.count; v++) {
                    const float z = b.verts[c.first + v].pos[2];
                    if (z < zlo) zlo = z;
                    if (z > zhi) zhi = z;
                }
                fprintf(stderr, "[draw] %d verts=%u st=%u texgens=%u proj=%c z=%.0f..%.0f",
                        draw_index, c.count, c.state, st.num_texgens,
                        (int)st.proj[6] == 0 ? 'p' : 'o', zlo, zhi);
                for (int i = 0; i < 8; i++)
                    if (st.tex_id[i]) fprintf(stderr, " t%d=%u%s", i, st.tex_id[i],
                                              st.tex_is_efb[i] ? "*" : "");
                fprintf(stderr, "\n");
            }
            const int this_draw = draw_index++;
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
                n_apply++;
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
            if (do_present) present(c.copy);
            presented = true;
            glBindFramebuffer(GL_FRAMEBUFFER, g_efb_fbo);
            glViewport(0, 0, EFB_W * g_scale, EFB_H * g_scale);
            cur_state = UINT32_MAX;
            break;
        }
    }
    if (g_frametime)
        fprintf(stderr, "[rt] f%u render %6.2fms (tex %5.2f vbo %5.2f)  applies %4u  programs %zu\n",
                g_render_frame, ms_since(t_start), ms_tex, ms_vbo, n_apply, g_programs.size());
    return presented;
}

bool render_execute(Batch& b) { return execute_batch(b, true); }

}  // namespace gx
