#pragma once
#include "mesh.h"

enum class ThumbKind { None, Rendered, EncodedImage };

// Produces a thumbnail for SolidWorks, STEP or STL data (format is detected
// from content). For rendered formats `image` is filled; for SolidWorks the
// embedded preview is returned as an encoded PNG/BMP in `encoded`.
ThumbKind MakeThumbnail(const uint8_t* data, size_t len, int size, Image& image, std::vector<uint8_t>& encoded);
