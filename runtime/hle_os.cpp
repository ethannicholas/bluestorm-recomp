// HLE replacements for a few SDK OS functions (debug output).
#include "runtime.h"

std::string guest_str(uint32_t addr, size_t max) {
    std::string s;
    if (!addr) return "(null)";
    for (size_t i = 0; i < max; i++) {
        char ch = (char)mem_r8(addr + (uint32_t)i);
        if (!ch) break;
        s.push_back(ch);
    }
    return s;
}

// printf-style formatting with PowerPC EABI varargs: integer args in r[first_gpr..10],
// floating args in f[first_fpr..8], overflow on the stack.
std::string guest_format(CPU* c, uint32_t fmt_addr, int first_gpr, int first_fpr) {
    std::string fmt = guest_str(fmt_addr), out;
    int gi = first_gpr, fi = first_fpr;
    uint32_t stack = c->r[1] + 8;
    auto next_int = [&]() -> uint32_t {
        if (gi <= 10) return c->r[gi++];
        uint32_t v = mem_r32(stack); stack += 4; return v;
    };
    auto next_i64 = [&]() -> uint64_t {
        if (!(gi & 1)) gi++;  // 64-bit args occupy an aligned pair starting at an odd register (r3,r5,r7,r9)
        if (gi <= 9) { uint64_t v = ((uint64_t)c->r[gi] << 32) | c->r[gi + 1]; gi += 2; return v; }
        stack = (stack + 7) & ~7u;
        uint64_t v = ((uint64_t)mem_r32(stack) << 32) | mem_r32(stack + 4); stack += 8; return v;
    };
    auto next_dbl = [&]() -> double {
        if (fi <= 8) return c->f[fi++].d;
        stack = (stack + 7) & ~7u;
        uint64_t u = ((uint64_t)mem_r32(stack) << 32) | mem_r32(stack + 4); stack += 8;
        double d; memcpy(&d, &u, 8); return d;
    };
    for (size_t i = 0; i < fmt.size(); i++) {
        if (fmt[i] != '%') { out.push_back(fmt[i]); continue; }
        size_t j = i + 1;
        std::string spec = "%";
        while (j < fmt.size() && strchr("-+ #0123456789.*", fmt[j])) {
            if (fmt[j] == '*') spec += std::to_string((int32_t)next_int());
            else spec.push_back(fmt[j]);
            j++;
        }
        int lng = 0;
        while (j < fmt.size() && strchr("hlLqjzt", fmt[j])) { if (fmt[j] == 'l' || fmt[j] == 'L' || fmt[j] == 'q') lng++; j++; }
        if (j >= fmt.size()) break;
        char conv = fmt[j];
        char buf[512];
        switch (conv) {
        case 'd': case 'i':
            if (lng >= 2) snprintf(buf, sizeof(buf), (spec + "lld").c_str(), (long long)next_i64());
            else snprintf(buf, sizeof(buf), (spec + "d").c_str(), (int32_t)next_int());
            out += buf; break;
        case 'u': case 'x': case 'X': case 'o':
            if (lng >= 2) snprintf(buf, sizeof(buf), (spec + "ll" + conv).c_str(), (unsigned long long)next_i64());
            else snprintf(buf, sizeof(buf), (spec + conv).c_str(), next_int());
            out += buf; break;
        case 'c': snprintf(buf, sizeof(buf), (spec + "c").c_str(), (int)next_int()); out += buf; break;
        case 'p': snprintf(buf, sizeof(buf), "0x%08x", next_int()); out += buf; break;
        case 's': snprintf(buf, sizeof(buf), (spec + "s").c_str(), guest_str(next_int()).c_str()); out += buf; break;
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G':
            snprintf(buf, sizeof(buf), (spec + conv).c_str(), next_dbl()); out += buf; break;
        case '%': out.push_back('%'); break;
        default: out += spec; out.push_back(conv); break;
        }
        i = j;
    }
    return out;
}

extern "C" void hle_OSReport(CPU* c) {
    std::string s = guest_format(c, c->r[3], 4, 1);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    LOG(LOG_OS, "%s", s.c_str());
}
