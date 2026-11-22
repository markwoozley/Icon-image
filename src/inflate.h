#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

// Raw DEFLATE (RFC 1951) decoder. Decodes from `src` until the final block
// ends or `maxOut` bytes have been produced. Returns false on corrupt data.
bool RawInflate(const uint8_t* src, size_t srcLen, std::vector<uint8_t>& out,
                size_t maxOut = (size_t)-1);
