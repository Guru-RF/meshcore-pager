// Host shim: just enough of the Arduino API to compile the pager's protocol
// sources (src/meshcore.cpp, src/pagerclock.cpp and the headers they pull in)
// on a workstation, so tools/selftest.cpp exercises the SHIPPING code rather
// than a transcription of it. Nothing here is used on the device.
#pragma once

#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <sys/time.h>

#ifndef constrain
#define constrain(a, l, h) ((a) < (l) ? (l) : ((a) > (h) ? (h) : (a)))
#endif

static inline uint32_t millis() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (uint32_t)(tv.tv_sec * 1000ULL + tv.tv_usec / 1000ULL);
}

static inline void delay(uint32_t) {}

// A deterministic stand-in for the RP2040 ring oscillator. The selftest wants
// repeatable keys; the device must never use anything this predictable, which
// is why the real one is rp2040.hwrand32().
struct HostRp2040 {
    uint32_t state = 0x12345678u;
    uint32_t hwrand32() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }
};
extern HostRp2040 rp2040;
