// Host shim: the sliver of the FatFS API the pager's identity store uses,
// backed by ordinary files under ./.selftest-fs/.
#pragma once

#include <Arduino.h>
#include <sys/stat.h>

class File {
public:
    FILE *f = nullptr;
    explicit operator bool() const { return f != nullptr; }
    size_t read(uint8_t *buf, size_t n) { return f ? fread(buf, 1, n, f) : 0; }
    size_t write(const uint8_t *buf, size_t n) { return f ? fwrite(buf, 1, n, f) : 0; }
    void   close() { if (f) { fclose(f); f = nullptr; } }
    bool   available() { return f && !feof(f); }
    size_t readBytesUntil(char, char *, size_t) { return 0; }
    void   println(const char *) {}
};

class FatFSClass {
public:
    bool begin() { mkdir(".selftest-fs", 0755); return true; }
    void format() {}
    File open(const char *path, const char *mode) {
        char real[256];
        snprintf(real, sizeof(real), ".selftest-fs/%s", path[0] == '/' ? path + 1 : path);
        File h;
        h.f = fopen(real, mode);
        return h;
    }
    bool remove(const char *path) {
        char real[256];
        snprintf(real, sizeof(real), ".selftest-fs/%s", path[0] == '/' ? path + 1 : path);
        return ::remove(real) == 0;
    }
};
extern FatFSClass FatFS;
