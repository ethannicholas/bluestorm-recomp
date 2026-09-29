// Host audio: mixes the AI DMA stream (32 kHz, from the AX HLE) with DVD streamed
// music (48 kHz DTK ADPCM read from the disc) and feeds SDL at 48 kHz.
#include "runtime.h"
#include "hw/dtk.h"
#include <SDL.h>
#include <algorithm>
#include <mutex>
#include <vector>

uint32_t ai_volume_left();
uint32_t ai_volume_right();
bool ai_stream_playing();
uint32_t ai_stream_rate();
void ai_stream_advance(uint32_t samples);

static constexpr int OUT_RATE = 48000;
void audio_wav_capture(const int16_t* s, int frames);
static SDL_AudioDeviceID g_dev;

// ---- AI DMA ring buffer (stereo L,R at 32 kHz) ----
static std::mutex g_ring_mutex;
static std::vector<int16_t> g_ring(32000 * 2);  // 1 second
static size_t g_ring_r, g_ring_w, g_ring_count;  // in frames
static double g_dma_pos;  // fractional read position for resampling

void audio_push_dma(const int16_t* samples_be, uint32_t frames) {
    std::lock_guard<std::mutex> lk(g_ring_mutex);
    size_t cap = g_ring.size() / 2;
    for (uint32_t i = 0; i < frames; i++) {
        // AX writes interleaved R, L (big endian)
        int16_t r = (int16_t)__builtin_bswap16((uint16_t)samples_be[2 * i]);
        int16_t l = (int16_t)__builtin_bswap16((uint16_t)samples_be[2 * i + 1]);
        if (g_ring_count == cap) { g_ring_r = (g_ring_r + 1) % cap; g_ring_count--; }  // drop oldest
        g_ring[2 * g_ring_w] = l;
        g_ring[2 * g_ring_w + 1] = r;
        g_ring_w = (g_ring_w + 1) % cap;
        g_ring_count++;
    }
}

// ---- DTK (DVD streaming ADPCM) ----
DtkState g_dtk;
std::mutex g_dtk_mutex;

static int16_t adp_decode(int32_t bits, int32_t q, int32_t& h1, int32_t& h2) {
    int32_t hist = 0;
    switch (q >> 4) {
    case 1: hist = h1 * 0x3c; break;
    case 2: hist = h1 * 0x73 - h2 * 0x34; break;
    case 3: hist = h1 * 0x62 - h2 * 0x37; break;
    }
    hist = std::clamp((hist + 0x20) >> 6, -0x200000, 0x1fffff);
    int32_t cur = (((int16_t)(bits << 12) >> (q & 0xf)) << 6) + hist;
    h2 = h1;
    h1 = cur;
    cur >>= 6;
    return (int16_t)std::clamp(cur, -0x8000, 0x7fff);
}

static std::vector<int16_t> g_dtk_buf;  // decoded stereo samples not yet consumed
static size_t g_dtk_buf_pos;
static double g_dtk_pos;  // fractional position for rate conversion

// Decode one 32-byte block (28 stereo samples). Caller holds g_dtk_mutex.
static bool dtk_decode_block() {
    if (!g_dtk.playing || g_dtk.len == 0) return false;
    if (g_dtk.cur >= g_dtk.start + g_dtk.len) {
        if (g_dtk.stop_at_end || g_dtk.next_len == 0) { g_dtk.playing = false; return false; }
        g_dtk.start = g_dtk.cur = g_dtk.next_start;
        g_dtk.len = g_dtk.next_len;
    }
    uint8_t blk[32];
    if (!iso_read(g_dtk.cur, blk, 32)) { g_dtk.playing = false; return false; }
    g_dtk.cur += 32;
    g_dtk_buf.erase(g_dtk_buf.begin(), g_dtk_buf.begin() + g_dtk_buf_pos);
    g_dtk_buf_pos = 0;
    for (int i = 0; i < 28; i++) {
        g_dtk_buf.push_back(adp_decode(blk[4 + i] & 0xF, blk[0], g_dtk.hist[0][0], g_dtk.hist[0][1]));
        g_dtk_buf.push_back(adp_decode(blk[4 + i] >> 4, blk[1], g_dtk.hist[1][0], g_dtk.hist[1][1]));
    }
    return true;
}

// ---- SDL callback ----
static void audio_callback(void*, uint8_t* stream, int len) {
    int16_t* out = (int16_t*)stream;
    int frames = len / 4;
    // DMA part: resample 32k -> 48k
    {
        std::lock_guard<std::mutex> lk(g_ring_mutex);
        size_t cap = g_ring.size() / 2;
        // Keep latency bounded: if more than ~120ms is buffered, skip ahead.
        while (g_ring_count > 32000 * 12 / 100) { g_ring_r = (g_ring_r + 1) % cap; g_ring_count--; }
        const double step = 32000.0 / OUT_RATE;
        for (int i = 0; i < frames; i++) {
            int32_t l = 0, r = 0;
            if (g_ring_count >= 2) {
                size_t a = g_ring_r, b = (g_ring_r + 1) % cap;
                double f = g_dma_pos;
                l = (int32_t)(g_ring[2 * a] * (1 - f) + g_ring[2 * b] * f);
                r = (int32_t)(g_ring[2 * a + 1] * (1 - f) + g_ring[2 * b + 1] * f);
                g_dma_pos += step;
                while (g_dma_pos >= 1.0 && g_ring_count > 1) {
                    g_dma_pos -= 1.0;
                    g_ring_r = (g_ring_r + 1) % cap;
                    g_ring_count--;
                }
            }
            out[2 * i] = (int16_t)l;
            out[2 * i + 1] = (int16_t)r;
        }
    }
    // DTK streaming music
    if (ai_stream_playing()) {
        std::lock_guard<std::mutex> lk(g_dtk_mutex);
        const double step = (double)ai_stream_rate() / OUT_RATE;
        int32_t vl = (int32_t)ai_volume_left(), vr = (int32_t)ai_volume_right();
        static double counter_frac;
        counter_frac += frames * step;
        uint32_t elapsed = (uint32_t)counter_frac;
        counter_frac -= elapsed;
        for (int i = 0; i < frames; i++) {
            if (g_dtk_buf_pos + 2 > g_dtk_buf.size() && !dtk_decode_block()) break;
            int32_t l = g_dtk_buf[g_dtk_buf_pos], r = g_dtk_buf[g_dtk_buf_pos + 1];
            out[2 * i] = (int16_t)std::clamp(out[2 * i] + l * vl / 255, -32768, 32767);
            out[2 * i + 1] = (int16_t)std::clamp(out[2 * i + 1] + r * vr / 255, -32768, 32767);
            g_dtk_pos += step;
            while (g_dtk_pos >= 1.0) {
                g_dtk_pos -= 1.0;
                g_dtk_buf_pos += 2;
                if (g_dtk_buf_pos + 2 > g_dtk_buf.size() && !dtk_decode_block()) break;
            }
        }
        ai_stream_advance(elapsed);
    }
    audio_wav_capture(out, frames);
}

static FILE* g_wav;
static uint32_t g_wav_bytes;
static void wav_header(FILE* f, uint32_t data_bytes) {
    auto w32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
    auto w16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f); w32(36 + data_bytes); fwrite("WAVEfmt ", 1, 8, f);
    w32(16); w16(1); w16(2); w32(OUT_RATE); w32(OUT_RATE * 4); w16(4); w16(16);
    fwrite("data", 1, 4, f); w32(data_bytes);
    fseek(f, 0, SEEK_END);
}
void audio_wav_capture(const int16_t* s, int frames) {
    if (!g_wav) return;
    fwrite(s, 4, frames, g_wav);
    g_wav_bytes += frames * 4;
    wav_header(g_wav, g_wav_bytes);
}

bool audio_open() {
    if (const char* p = getenv("WR_WAV")) { g_wav = fopen(p, "wb"); if (g_wav) wav_header(g_wav, 0); }
    SDL_AudioSpec want{}, have{};
    want.freq = OUT_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 512;
    want.callback = audio_callback;
    g_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (!g_dev) {
        LOG(LOG_AI, "audio: cannot open device: %s", SDL_GetError());
        return false;
    }
    SDL_PauseAudioDevice(g_dev, 0);
    return true;
}
