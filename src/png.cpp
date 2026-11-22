#include "png.h"
#include "inflate.h"
#include <cstdlib>
#include <cstring>

static uint32_t BE32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

bool DecodePng(const uint8_t* d, size_t n, int& w, int& h, std::vector<uint8_t>& rgba) {
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (n < 33 || std::memcmp(d, sig, 8) != 0) return false;
    size_t off = 8;
    std::vector<uint8_t> idat, palette, trns;
    int depth = 0, ctype = 0, interlace = 0;
    w = h = 0;
    while (off + 12 <= n) {
        uint32_t len = BE32(d + off);
        if (len > n - off - 12) return false;
        const uint8_t* type = d + off + 4;
        const uint8_t* body = d + off + 8;
        if (!std::memcmp(type, "IHDR", 4) && len >= 13) {
            w = (int)BE32(body); h = (int)BE32(body + 4);
            depth = body[8]; ctype = body[9]; interlace = body[12];
        } else if (!std::memcmp(type, "PLTE", 4)) {
            palette.assign(body, body + len);
        } else if (!std::memcmp(type, "tRNS", 4)) {
            trns.assign(body, body + len);
        } else if (!std::memcmp(type, "IDAT", 4)) {
            idat.insert(idat.end(), body, body + len);
        } else if (!std::memcmp(type, "IEND", 4)) {
            break;
        }
        off += 12 + len;
    }
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384 || interlace != 0 || idat.size() < 2) return false;
    int channels;
    switch (ctype) {
        case 0: channels = 1; break;
        case 2: channels = 3; break;
        case 3: channels = 1; break;
        case 4: channels = 2; break;
        case 6: channels = 4; break;
        default: return false;
    }
    if (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16) return false;
    size_t bitsPerPixel = (size_t)channels * depth;
    size_t stride = ((size_t)w * bitsPerPixel + 7) / 8;
    size_t bpp = (bitsPerPixel + 7) / 8;
    std::vector<uint8_t> raw;
    size_t need = (stride + 1) * h;
    if (!RawInflate(idat.data() + 2, idat.size() - 2, raw, need) || raw.size() < need) return false;

    std::vector<uint8_t> prev(stride, 0), cur(stride);
    rgba.assign((size_t)w * h * 4, 255);
    for (int y = 0; y < h; y++) {
        const uint8_t* src = &raw[(stride + 1) * y];
        int filter = src[0];
        src++;
        for (size_t i = 0; i < stride; i++) {
            int a = i >= bpp ? cur[i - bpp] : 0;
            int b = prev[i];
            int c = i >= bpp ? prev[i - bpp] : 0;
            int x = src[i], v;
            switch (filter) {
                case 0: v = x; break;
                case 1: v = x + a; break;
                case 2: v = x + b; break;
                case 3: v = x + ((a + b) >> 1); break;
                case 4: {
                    int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
                    v = x + ((pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c));
                    break;
                }
                default: return false;
            }
            cur[i] = (uint8_t)v;
        }
        uint8_t* out = &rgba[(size_t)y * w * 4];
        for (int x = 0; x < w; x++) {
            int s[4] = {0, 0, 0, 255};
            for (int ch = 0; ch < channels; ch++) {
                int val;
                if (depth == 8) val = cur[(size_t)x * channels + ch];
                else if (depth == 16) val = cur[((size_t)x * channels + ch) * 2];
                else {
                    size_t bit = (size_t)x * depth;
                    int raw8 = (cur[bit / 8] >> (8 - depth - (bit % 8))) & ((1 << depth) - 1);
                    val = ctype == 3 ? raw8 : raw8 * 255 / ((1 << depth) - 1);
                }
                s[ch] = val;
            }
            uint8_t* o = out + (size_t)x * 4;
            if (ctype == 3) {
                int i = s[0];
                if ((size_t)i * 3 + 2 < palette.size()) { o[0] = palette[i * 3]; o[1] = palette[i * 3 + 1]; o[2] = palette[i * 3 + 2]; }
                o[3] = (size_t)i < trns.size() ? trns[i] : 255;
            } else if (channels <= 2) {
                o[0] = o[1] = o[2] = (uint8_t)s[0];
                o[3] = channels == 2 ? (uint8_t)s[1] : 255;
            } else {
                o[0] = (uint8_t)s[0]; o[1] = (uint8_t)s[1]; o[2] = (uint8_t)s[2];
                o[3] = channels == 4 ? (uint8_t)s[3] : 255;
            }
        }
        std::swap(prev, cur);
    }
    return true;
}
