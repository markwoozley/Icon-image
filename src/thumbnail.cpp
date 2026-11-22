#include "thumbnail.h"
#include "solidworks.h"
#include <cctype>
#include <cstdio>

namespace {

enum Fmt { kStep, kStl, kSolidWorks };

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
            if (ExtractSolidWorksPreview(data, len, encoded, log)) { kind = ThumbKind::EncodedImage; return true; }
            return false;
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
