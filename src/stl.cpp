#include "mesh.h"
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>

static bool IsAsciiStl(const uint8_t* d, size_t n) {
    size_t i = 0;
    while (i < n && std::isspace(d[i])) i++;
    if (n - i < 6 || std::strncmp((const char*)d + i, "solid", 5) != 0) return false;
    // Binary files may also start with "solid"; require a "facet" keyword early on.
    size_t lim = n < 4096 ? n : 4096;
    for (size_t k = i; k + 5 <= lim; k++)
        if (std::memcmp(d + k, "facet", 5) == 0) return true;
    return n - i < 64;  // empty ascii solid
}

static bool IsBinaryStl(const uint8_t* d, size_t n) {
    if (n < 84) return false;
    uint32_t count;
    std::memcpy(&count, d + 80, 4);
    return (uint64_t)count * 50 + 84 <= n && (uint64_t)count * 50 + 84 + 1024 >= n;
}

bool LooksLikeStl(const uint8_t* d, size_t n) { return IsAsciiStl(d, n) || IsBinaryStl(d, n); }

bool LoadStl(const uint8_t* d, size_t n, Scene& scene, bool force) {
    // `force`: the file is known to be STL (by extension), so accept binary
    // files whose triangle count in the header is wrong.
    if ((IsBinaryStl(d, n) || force) && !IsAsciiStl(d, n) && n >= 84 + 50) {
        uint32_t count;
        std::memcpy(&count, d + 80, 4);
        if ((uint64_t)count * 50 + 84 > n || count == 0) count = (uint32_t)((n - 84) / 50);
        scene.tris.reserve(scene.tris.size() + (size_t)count * 9);
        for (uint32_t t = 0; t < count; t++) {
            const uint8_t* p = d + 84 + (size_t)t * 50 + 12;
            float v[9];
            std::memcpy(v, p, 36);
            bool ok = true;
            for (float f : v) ok = ok && std::isfinite(f);
            if (ok) scene.tris.insert(scene.tris.end(), v, v + 9);
        }
        return !scene.tris.empty();
    }
    if (!IsAsciiStl(d, n)) return false;
    // ASCII: collect every "vertex x y z"; each consecutive three form a triangle.
    std::string buf;
    float v[9];
    int k = 0;
    size_t i = 0;
    while (i + 6 < n) {
        if (d[i] == 'v' && std::memcmp(d + i, "vertex", 6) == 0) {
            i += 6;
            for (int c = 0; c < 3; c++) {
                while (i < n && std::isspace(d[i])) i++;
                size_t s = i;
                while (i < n && !std::isspace(d[i])) i++;
                buf.assign((const char*)d + s, i - s);
                v[k * 3 + c] = std::strtof(buf.c_str(), nullptr);
            }
            if (++k == 3) {
                scene.tris.insert(scene.tris.end(), v, v + 9);
                k = 0;
            }
        } else {
            i++;
        }
    }
    return !scene.tris.empty();
}
