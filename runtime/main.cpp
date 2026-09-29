// Entry point and SDL frontend: window, input, audio. The main thread executes GX
// batches produced by the guest thread.
#include "runtime.h"
#include "gx/render_gl.h"
#include "gx/gl.h"
#include "hw/pad.h"
#include <SDL.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <execinfo.h>
#include <thread>
#include <unistd.h>
#include <dirent.h>
#include <string>
#include <vector>

uint32_t boot_load(const char* iso_path);
void debug_dump_threads();
void debug_sampler_start();

static void on_signal(int sig) {
    signal(SIGALRM, [](int) { _exit(2); });
    alarm(2);  // never hang in here (stdio locks may be held by other threads)
    debug_dump_threads();
    _exit(1);
}

static void on_fault(int sig, siginfo_t* si, void*) {
    uintptr_t a = (uintptr_t)si->si_addr;
    if (a >= (uintptr_t)g_mem && a < (uintptr_t)g_mem + 0x40000000)
        fprintf(stderr, "\nFAULT: guest memory access at (addr & 0x3FFFFFFF) = %08lX\n", (unsigned long)(a - (uintptr_t)g_mem));
    else
        fprintf(stderr, "\nFAULT: host address %p (signal %d)\n", si->si_addr, sig);
    signal(SIGALRM, [](int) { _exit(2); });
    alarm(2);
    void* bt[64];
    int n = backtrace(bt, 64);
    backtrace_symbols_fd(bt, n, 2);
    debug_dump_threads();
    _exit(1);
}

bool audio_open();

// ---------------------------------------------------------------------------
// Input: keyboard + first game controller -> GC pad 1
// ---------------------------------------------------------------------------
static SDL_GameController* g_ctrl;

// Scripted input for testing: WR_INPUT="frame:BUTTON[+BUTTON]:dur,..." where frame is the
// presented-frame count at which to press, e.g. "900:START:10,1200:A:10". Stick: SL/SR/SU/SD.
struct ScriptedPress { uint32_t at, dur; uint16_t buttons; int sx, sy; };
static std::vector<ScriptedPress> g_script;
extern std::atomic<uint32_t> g_vi_retrace_count;

static void parse_script() {
    const char* e = getenv("WR_INPUT");
    if (!e) return;
    std::string s = e;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t end = s.find(',', pos);
        if (end == std::string::npos) end = s.size();
        std::string item = s.substr(pos, end - pos);
        pos = end + 1;
        size_t c1 = item.find(':'), c2 = item.rfind(':');
        if (c1 == std::string::npos || c2 == c1) continue;
        ScriptedPress p{(uint32_t)atoi(item.c_str()), (uint32_t)atoi(item.c_str() + c2 + 1), 0, 0, 0};
        std::string btns = item.substr(c1 + 1, c2 - c1 - 1);
        size_t bp = 0;
        while (bp <= btns.size()) {
            size_t be = btns.find('+', bp);
            if (be == std::string::npos) be = btns.size();
            std::string b = btns.substr(bp, be - bp);
            bp = be + 1;
            if (b == "A") p.buttons |= PAD_A; else if (b == "B") p.buttons |= PAD_B;
            else if (b == "X") p.buttons |= PAD_X; else if (b == "Y") p.buttons |= PAD_Y;
            else if (b == "Z") p.buttons |= PAD_Z; else if (b == "L") p.buttons |= PAD_L;
            else if (b == "R") p.buttons |= PAD_R; else if (b == "START") p.buttons |= PAD_START;
            else if (b == "UP") p.buttons |= PAD_UP; else if (b == "DOWN") p.buttons |= PAD_DOWN;
            else if (b == "LEFT") p.buttons |= PAD_LEFT; else if (b == "RIGHT") p.buttons |= PAD_RIGHT;
            else if (b == "SL") p.sx = -100; else if (b == "SR") p.sx = 100;
            else if (b == "SU") p.sy = 100; else if (b == "SD") p.sy = -100;
        }
        g_script.push_back(p);
    }
}

static void apply_script(PadState& p) {
    uint32_t vi = gx::g_frames_submitted.load();  // script time base: presented game frames
    for (auto& s : g_script) {
        if (vi >= s.at && vi < s.at + s.dur) {
            if (getenv("WR_INPUT_LOG")) fprintf(stderr, "[input] frame %u press %04X held=%04X pressed=%04X\n", vi, s.buttons,
                                                             mem_r32(0x80345F44), mem_r32(0x80345F64));
            p.buttons |= s.buttons;
            if (s.sx) p.stick_x = (uint8_t)(128 + s.sx);
            if (s.sy) p.stick_y = (uint8_t)(128 + s.sy);
            if (s.buttons & PAD_L) p.trig_l = 255;
            if (s.buttons & PAD_R) p.trig_r = 255;
        }
    }
}

static uint8_t axis_to_u8(int v, bool invert) {
    float f = v / 32767.0f;
    if (invert) f = -f;
    int r = 128 + (int)(f * 100.0f);
    return (uint8_t)std::clamp(r, 0, 255);
}

static void update_pad() {
    PadState p;
    p.connected = true;
    const uint8_t* k = SDL_GetKeyboardState(nullptr);
    if (k[SDL_SCANCODE_RETURN]) p.buttons |= PAD_START;
    if (k[SDL_SCANCODE_X]) p.buttons |= PAD_A;
    if (k[SDL_SCANCODE_Z]) p.buttons |= PAD_B;
    if (k[SDL_SCANCODE_C]) p.buttons |= PAD_X;
    if (k[SDL_SCANCODE_S]) p.buttons |= PAD_Y;
    if (k[SDL_SCANCODE_D]) p.buttons |= PAD_Z;
    if (k[SDL_SCANCODE_Q]) { p.buttons |= PAD_L; p.trig_l = 255; }
    if (k[SDL_SCANCODE_W]) { p.buttons |= PAD_R; p.trig_r = 255; }
    if (k[SDL_SCANCODE_I]) p.buttons |= PAD_UP;
    if (k[SDL_SCANCODE_K]) p.buttons |= PAD_DOWN;
    if (k[SDL_SCANCODE_J]) p.buttons |= PAD_LEFT;
    if (k[SDL_SCANCODE_L]) p.buttons |= PAD_RIGHT;
    int sx = 0, sy = 0;
    if (k[SDL_SCANCODE_LEFT]) sx -= 100;
    if (k[SDL_SCANCODE_RIGHT]) sx += 100;
    if (k[SDL_SCANCODE_UP]) sy += 100;
    if (k[SDL_SCANCODE_DOWN]) sy -= 100;
    p.stick_x = (uint8_t)(128 + sx);
    p.stick_y = (uint8_t)(128 + sy);
    if (g_ctrl) {
        auto b = [&](SDL_GameControllerButton btn) { return SDL_GameControllerGetButton(g_ctrl, btn); };
        if (b(SDL_CONTROLLER_BUTTON_A)) p.buttons |= PAD_A;
        if (b(SDL_CONTROLLER_BUTTON_X)) p.buttons |= PAD_B;
        if (b(SDL_CONTROLLER_BUTTON_B)) p.buttons |= PAD_X;
        if (b(SDL_CONTROLLER_BUTTON_Y)) p.buttons |= PAD_Y;
        if (b(SDL_CONTROLLER_BUTTON_START)) p.buttons |= PAD_START;
        if (b(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) p.buttons |= PAD_Z;
        if (b(SDL_CONTROLLER_BUTTON_DPAD_UP)) p.buttons |= PAD_UP;
        if (b(SDL_CONTROLLER_BUTTON_DPAD_DOWN)) p.buttons |= PAD_DOWN;
        if (b(SDL_CONTROLLER_BUTTON_DPAD_LEFT)) p.buttons |= PAD_LEFT;
        if (b(SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) p.buttons |= PAD_RIGHT;
        int lx = SDL_GameControllerGetAxis(g_ctrl, SDL_CONTROLLER_AXIS_LEFTX);
        int ly = SDL_GameControllerGetAxis(g_ctrl, SDL_CONTROLLER_AXIS_LEFTY);
        if (abs(lx) > 4000 || abs(ly) > 4000) { p.stick_x = axis_to_u8(lx, false); p.stick_y = axis_to_u8(ly, true); }
        p.cstick_x = axis_to_u8(SDL_GameControllerGetAxis(g_ctrl, SDL_CONTROLLER_AXIS_RIGHTX), false);
        p.cstick_y = axis_to_u8(SDL_GameControllerGetAxis(g_ctrl, SDL_CONTROLLER_AXIS_RIGHTY), true);
        int lt = SDL_GameControllerGetAxis(g_ctrl, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
        int rt = SDL_GameControllerGetAxis(g_ctrl, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
        if (lt > 1000) { p.trig_l = (uint8_t)(lt * 255 / 32767); if (lt > 30000) p.buttons |= PAD_L; }
        if (rt > 1000) { p.trig_r = (uint8_t)(rt * 255 / 32767); if (rt > 30000) p.buttons |= PAD_R; }
    }
    apply_script(p);
    pad_set_state(0, p);
}

// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    // Default: the image the build was configured with, else the first .iso in ./rom
    std::string iso_default = WR_DEFAULT_ISO;
    if (access(iso_default.c_str(), R_OK) != 0) {
        iso_default.clear();
        if (DIR* d = opendir("rom")) {
            while (dirent* e = readdir(d)) {
                std::string n = e->d_name;
                if (n.size() > 4 && n.substr(n.size() - 4) == ".iso") { iso_default = "rom/" + n; break; }
            }
            closedir(d);
        }
    }
    const char* iso = iso_default.c_str();
    bool headless = false, hidden = false;
    int scale = 2;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--log-all")) for (auto& e : g_log_enabled) e = true;
        else if (!strcmp(argv[i], "--sample")) debug_sampler_start();
        else if (!strcmp(argv[i], "--headless")) headless = true;
        else if (!strcmp(argv[i], "--hidden")) hidden = true;
        else if (!strcmp(argv[i], "--cull-swap")) gx::g_cull_swap = true;
        else if (!strncmp(argv[i], "--scale=", 8)) scale = atoi(argv[i] + 8);
        else if (!strncmp(argv[i], "--dump-dir=", 11)) gx::g_dump_dir = argv[i] + 11;
        else if (!strncmp(argv[i], "--dump-every=", 13)) gx::g_dump_every = atoi(argv[i] + 13);
        else if (argv[i][0] != '-') iso = argv[i];
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    struct sigaction sa = {};
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);

    if (!*iso || access(iso, R_OK) != 0) {
        fprintf(stderr, "No game image found. Put your Wave Race: Blue Storm (USA) .iso in rom/ or pass its path.\n");
        return 1;
    }
    mem_init();
    timing_init();
    parse_script();

    if (headless) {
        uint32_t entry = boot_load(iso);
        threads_start_boot(entry);
        for (;;) {
            PadState p;
            p.connected = true;
            apply_script(p);
            pad_set_state(0, p);
            auto b = gx::take_batch(5);  // discard
            (void)b;
        }
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) fatal("SDL_Init: %s", SDL_GetError());
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_Window* win = SDL_CreateWindow("Wave Race: Blue Storm (recompiled)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       1280, 960, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI |
                                       (hidden ? SDL_WINDOW_HIDDEN : 0));
    if (!win) fatal("SDL_CreateWindow: %s", SDL_GetError());
    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    if (!ctx) fatal("SDL_GL_CreateContext: %s", SDL_GetError());
    SDL_GL_SetSwapInterval(0);
    LOG(LOG_GX, "GL: %s / %s", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    gx::render_init(scale);

    audio_open();

    uint32_t entry = boot_load(iso);
    threads_start_boot(entry);

    bool running = true;
    uint32_t frames = 0;
    auto t0 = std::chrono::steady_clock::now();
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = false;
            else if (e.type == SDL_CONTROLLERDEVICEADDED && !g_ctrl) g_ctrl = SDL_GameControllerOpen(e.cdevice.which);
            else if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_ESCAPE) running = false;
        }
        update_pad();
        int dw, dh;
        SDL_GL_GetDrawableSize(win, &dw, &dh);
        gx::render_set_window_size(dw, dh);
        auto b = gx::take_batch(4);
        if (!b) continue;
        if (gx::render_execute(*b)) {
            // macOS (GL on Metal) only presents correctly with the window framebuffer bound.
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            SDL_GL_SwapWindow(win);
            frames++;
            auto now = std::chrono::steady_clock::now();
            double secs = std::chrono::duration<double>(now - t0).count();
            if (secs >= 2.0) {
                char title[128];
                snprintf(title, sizeof(title), "Wave Race: Blue Storm (recompiled) - %.1f fps", frames / secs);
                SDL_SetWindowTitle(win, title);
                frames = 0;
                t0 = now;
            }
        }
    }
    threads_request_quit();
    _exit(0);
}
