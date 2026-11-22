#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

// Minimal PNG decoder (non-interlaced; all colour types; 8/16-bit and
// palette). Output is 8-bit RGBA, top-down.
bool DecodePng(const uint8_t* data, size_t len, int& w, int& h, std::vector<uint8_t>& rgba);
