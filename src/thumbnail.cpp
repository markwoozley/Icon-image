#include "thumbnail.h"
#include "solidworks.h"

ThumbKind MakeThumbnail(const uint8_t* data, size_t len, int size, Image& image, std::vector<uint8_t>& encoded) {
    if (!data || len == 0) return ThumbKind::None;
    if (size > 2048) size = 2048;
    if (size < 16) size = 16;
    Scene scene;
    if (LooksLikeStep(data, len)) {
        if (LoadStep(data, len, scene) && RenderScene(scene, size, image)) return ThumbKind::Rendered;
        return ThumbKind::None;
    }
    if (LooksLikeStl(data, len)) {
        if (LoadStl(data, len, scene) && RenderScene(scene, size, image)) return ThumbKind::Rendered;
        return ThumbKind::None;
    }
    if (ExtractSolidWorksPreview(data, len, encoded)) return ThumbKind::EncodedImage;
    return ThumbKind::None;
}
