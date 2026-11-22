#include "thumbnail.h"
#include "solidworks.h"
#include "png.h"
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdio>

namespace {

enum Fmt { kStep, kStl, kSolidWorks };

struct Analysis {
    int w = 0, h = 0;
    std::vector<uint8_t> rgb;  // opaque, 3 bytes per pixel
    uint8_t bg[3] = {255, 255, 255};
    size_t content = 0;  // pixels that differ from the background
    int x0 = 0, y0 = 0, x1 = -1, y1 = -1;  // content bounding box (inclusive)
};

// Decodes a preview, makes it opaque and finds the drawn part within it.
bool Analyze(const std::vector<uint8_t>& png, Analysis& a) {
    std::vector<uint8_t> rgba;
    if (!DecodePng(png.data(), png.size(), a.w, a.h, rgba)) return false;
    size_t n = (size_t)a.w * a.h, clear = 0;
    for (size_t i = 0; i < n; i++)
        if (rgba[i * 4 + 3] == 0) clear++;
    // Some SolidWorks versions write an alpha channel that is (almost) all
    // zero even though the colours are fine: ignore alpha in that case.
    bool ignoreAlpha = clear * 100 >= n * 90;
    a.rgb.resize(n * 3);
    for (size_t i = 0; i < n; i++) {
        unsigned al = ignoreAlpha ? 255 : rgba[i * 4 + 3];
        for (int c = 0; c < 3; c++) a.rgb[i * 3 + c] = (uint8_t)((rgba[i * 4 + c] * al + 255 * (255 - al) + 127) / 255);
    }
    const uint8_t* tl = &a.rgb[0];
    for (int c = 0; c < 3; c++) a.bg[c] = tl[c];
    a.x0 = a.w; a.y0 = a.h; a.x1 = -1; a.y1 = -1;
    for (int y = 0; y < a.h; y++) {
        for (int x = 0; x < a.w; x++) {
            const uint8_t* p = &a.rgb[((size_t)y * a.w + x) * 3];
            int d = std::max({std::abs(p[0] - a.bg[0]), std::abs(p[1] - a.bg[1]), std::abs(p[2] - a.bg[2])});
            if (d <= 30) continue;
            a.content++;
            a.x0 = std::min(a.x0, x); a.x1 = std::max(a.x1, x);
            a.y0 = std::min(a.y0, y); a.y1 = std::max(a.y1, y);
        }
    }
    return true;
}

// Crops to the part (plus a margin), scales into a size x size image.
void CropToImage(const Analysis& a, int size, Image& out) {
    double cx0 = 0, cy0 = 0, cw = a.w, ch = a.h;
    if (a.content > 0) {
        double bw = a.x1 - a.x0 + 1, bh = a.y1 - a.y0 + 1;
        double side = std::max(bw, bh) * 1.12 + 4;
        cx0 = (a.x0 + a.x1 + 1) * 0.5 - side / 2;
        cy0 = (a.y0 + a.y1 + 1) * 0.5 - side / 2;
        cw = ch = side;
    } else {
        double side = std::max(cw, ch);
        cx0 = (cw - side) / 2; cy0 = (ch - side) / 2;
        cw = ch = side;
    }
    out.width = out.height = size;
    out.bgra.assign((size_t)size * size * 4, 255);
    double scale = cw / size;  // source pixels per output pixel
    auto sample = [&](double sx, double sy, int c) {
        if (sx < 0 || sy < 0 || sx > a.w - 1 || sy > a.h - 1) return (double)a.bg[c];
        int ix = (int)sx, iy = (int)sy;
        int jx = std::min(ix + 1, a.w - 1), jy = std::min(iy + 1, a.h - 1);
        double fx = sx - ix, fy = sy - iy;
        auto px = [&](int x, int y) { return (double)a.rgb[((size_t)y * a.w + x) * 3 + c]; };
        return (px(ix, iy) * (1 - fx) + px(jx, iy) * fx) * (1 - fy) + (px(ix, jy) * (1 - fx) + px(jx, jy) * fx) * fy;
    };
    const int ss = 4;
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            double acc[3] = {0, 0, 0};
            for (int sy = 0; sy < ss; sy++)
                for (int sx = 0; sx < ss; sx++) {
                    double px = cx0 + (x + (sx + 0.5) / ss) * scale - 0.5;
                    double py = cy0 + (y + (sy + 0.5) / ss) * scale - 0.5;
                    for (int c = 0; c < 3; c++) acc[c] += sample(px, py, c);
                }
            uint8_t* o = &out.bgra[((size_t)y * size + x) * 4];
            o[0] = (uint8_t)std::lround(acc[2] / (ss * ss));
            o[1] = (uint8_t)std::lround(acc[1] / (ss * ss));
            o[2] = (uint8_t)std::lround(acc[0] / (ss * ss));
            o[3] = 255;
        }
    }
}

bool SolidWorksThumb(const uint8_t* data, size_t len, int size, Image& image, std::vector<uint8_t>& encoded,
                     std::string* log, ThumbKind& kind) {
    std::vector<PreviewCandidate> pngs;
    std::vector<uint8_t> bmp;
    if (!ExtractSolidWorksPreviews(data, len, pngs, bmp, log)) return false;
    int best = -1;
    Analysis bestA;
    for (size_t i = 0; i < pngs.size(); i++) {
        Analysis a;
        bool ok = Analyze(pngs[i].png, a);
        if (log) {
            char line[256];
            if (ok)
                std::snprintf(line, sizeof line, "  candidate %zu: %s, %zu bytes, %dx%d, %.1f%% drawn\n", i + 1,
                              pngs[i].source.c_str(), pngs[i].png.size(), a.w, a.h, 100.0 * a.content / ((double)a.w * a.h));
            else
                std::snprintf(line, sizeof line, "  candidate %zu: %s, %zu bytes, could not decode\n", i + 1,
                              pngs[i].source.c_str(), pngs[i].png.size());
            *log += line;
        }
        if (ok && (best < 0 || a.content > bestA.content)) { best = (int)i; bestA = std::move(a); }
    }
    if (best >= 0) {
        if (log) *log += "  using candidate " + std::to_string(best + 1) + "\n";
        CropToImage(bestA, size, image);
        kind = ThumbKind::Rendered;
        return true;
    }
    // Nothing decodable here: let Windows' own decoder try.
    if (!bmp.empty()) encoded = bmp;
    else if (!pngs.empty()) encoded = pngs[0].png;
    else return false;
    kind = ThumbKind::EncodedImage;
    return true;
}

bool TryFormat(Fmt f, bool byExt, const uint8_t* data, size_t len, int size, Image& image,
               std::vector<uint8_t>& encoded, std::string* log, ThumbKind& kind) {
    Scene scene;
    switch (f) {
        case kStep:
            if (!byExt && !LooksLikeStep(data, len)) return false;
            if (log) *log += std::string("Trying STEP") + (LooksLikeStep(data, len) ? "" : " (no ISO-10303-21 header)") + "\n";
            if (LoadStep(data, len, scene, log) && RenderScene(scene, size, image)) { kind = ThumbKind::Rendered; return true; }
            return false;
        case kStl:
            if (!byExt && !LooksLikeStl(data, len)) return false;
            if (log) *log += "Trying STL\n";
            if (LoadStl(data, len, scene, byExt) && RenderScene(scene, size, image)) { kind = ThumbKind::Rendered; return true; }
            if (log) *log += "STL: no usable triangles (" + std::to_string(scene.tris.size() / 9) + " read)\n";
            return false;
        case kSolidWorks:
            if (log) *log += "Trying SolidWorks preview\n";
            return SolidWorksThumb(data, len, size, image, encoded, log, kind);
    }
    return false;
}

}  // namespace

ThumbKind MakeThumbnail(const uint8_t* data, size_t len, int size, Image& image, std::vector<uint8_t>& encoded,
                        const std::string& extIn, std::string* log) {
    if (log) {
        char hex[64] = {0};
        for (size_t i = 0; i < 16 && i < len; i++) std::snprintf(hex + i * 3, 4, "%02x ", data[i]);
        *log += "File: " + std::to_string(len) + " bytes, extension '" + extIn + "', starts with: " + hex + "\n";
    }
    if (!data || len == 0) return ThumbKind::None;
    if (size > 2048) size = 2048;
    if (size < 16) size = 16;
    std::string ext;
    for (char c : extIn) ext.push_back((char)std::tolower((unsigned char)c));

    Fmt first = kSolidWorks;
    bool known = true;
    if (ext == ".step" || ext == ".stp") first = kStep;
    else if (ext == ".stl") first = kStl;
    else if (ext == ".sldprt" || ext == ".sldasm" || ext == ".slddrw") first = kSolidWorks;
    else known = false;

    ThumbKind kind = ThumbKind::None;
    if (known && TryFormat(first, true, data, len, size, image, encoded, log, kind)) return kind;
    // Content sniffing for the remaining formats.
    for (Fmt f : {kStep, kStl, kSolidWorks}) {
        if (known && f == first) continue;
        if (TryFormat(f, false, data, len, size, image, encoded, log, kind)) return kind;
    }
    if (log) *log += "No thumbnail could be produced.\n";
    return ThumbKind::None;
}
