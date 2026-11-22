#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

// Extracts the preview image SolidWorks embeds in .sldprt/.sldasm/.slddrw
// files. Works for both the pre-2015 OLE compound-file format and the newer
// (2015+) proprietary container. On success `image` holds an encoded PNG or
// BMP file that the caller can hand to an image decoder.
bool ExtractSolidWorksPreview(const uint8_t* data, size_t len, std::vector<uint8_t>& image);

bool LooksLikePng(const uint8_t* p, size_t n);
