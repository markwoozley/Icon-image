#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct PreviewCandidate {
    std::string source;
    std::vector<uint8_t> png;
};

// Collects every preview image SolidWorks embeds in .sldprt/.sldasm/.slddrw
// files (pre-2015 OLE compound files and the 2015+ container). Files often
// hold several (per configuration, older revisions, blank placeholders); the
// caller picks the best. `bmp` receives a legacy DIB preview as a .bmp file.
bool ExtractSolidWorksPreviews(const uint8_t* data, size_t len, std::vector<PreviewCandidate>& pngs,
                               std::vector<uint8_t>& bmp, std::string* log = nullptr);

bool LooksLikePng(const uint8_t* p, size_t n);
