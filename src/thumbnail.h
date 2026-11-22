#pragma once
#include "mesh.h"

enum class ThumbKind { None, Rendered, EncodedImage };

// Produces a thumbnail for SolidWorks, STEP or STL data. `ext` (e.g. ".stl",
// may be empty) decides which reader is tried first; the others are tried
// afterwards in case the extension is wrong. For rendered formats `image` is
// filled; for SolidWorks the embedded preview is returned as an encoded
// PNG/BMP in `encoded`. If `log` is given, a human-readable report of what
// was tried is appended to it.
ThumbKind MakeThumbnail(const uint8_t* data, size_t len, int size, Image& image, std::vector<uint8_t>& encoded,
                        const std::string& ext = std::string(), std::string* log = nullptr);
