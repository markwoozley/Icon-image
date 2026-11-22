#include "solidworks.h"
#include "inflate.h"
#include <cstring>
#include <string>

namespace {

uint16_t U16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t U32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

// ---------------------------------------------------------------------------
// Minimal OLE / Compound File Binary reader (read-only, enough for SolidWorks).
// ---------------------------------------------------------------------------
class Cfb {
public:
    struct Entry {
        std::string name;
        int type;
        uint32_t start;
        uint64_t size;
    };

    bool Open(const uint8_t* d, size_t n) {
        static const uint8_t sig[8] = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};
        if (n < 512 || std::memcmp(d, sig, 8) != 0) return false;
        d_ = d; n_ = n;
        int shift = U16(d + 0x1E);
        int miniShift = U16(d + 0x20);
        if (shift < 7 || shift > 16 || miniShift < 2 || miniShift > shift) return false;
        sec_ = 1u << shift;
        miniSec_ = 1u << miniShift;
        miniCutoff_ = U32(d + 0x38);

        // Collect FAT sector numbers from the header DIFAT and DIFAT chain.
        std::vector<uint32_t> fatSectors;
        for (int i = 0; i < 109; i++) {
            uint32_t s = U32(d + 0x4C + i * 4);
            if (s >= 0xFFFFFFFA) break;
            fatSectors.push_back(s);
        }
        uint32_t difat = U32(d + 0x44);
        for (int guard = 0; difat < 0xFFFFFFFA && guard < 100000; guard++) {
            const uint8_t* p = Sector(difat);
            if (!p) return false;
            uint32_t per = sec_ / 4 - 1;
            for (uint32_t i = 0; i < per; i++) {
                uint32_t s = U32(p + i * 4);
                if (s < 0xFFFFFFFA) fatSectors.push_back(s);
            }
            difat = U32(p + per * 4);
        }
        for (uint32_t s : fatSectors) {
            const uint8_t* p = Sector(s);
            if (!p) return false;
            for (uint32_t i = 0; i < sec_ / 4; i++) fat_.push_back(U32(p + i * 4));
        }

        std::vector<uint8_t> dir;
        if (!ReadChain(U32(d + 0x30), (uint64_t)-1, dir)) return false;
        for (size_t off = 0; off + 128 <= dir.size(); off += 128) {
            const uint8_t* e = &dir[off];
            Entry en;
            int nameLen = U16(e + 64);
            for (int i = 0; i + 1 < nameLen && i < 64; i += 2) {
                uint16_t c = U16(e + i);
                if (!c) break;
                en.name.push_back(c < 128 ? (char)c : '?');
            }
            en.type = e[66];
            en.start = U32(e + 116);
            en.size = U32(e + 120) | ((uint64_t)(sec_ > 512 ? U32(e + 124) : 0) << 32);
            entries_.push_back(en);
        }
        if (entries_.empty() || entries_[0].type != 5) return false;

        // Mini stream (owned by the root entry) and mini FAT.
        ReadChain(entries_[0].start, entries_[0].size, miniStream_);
        std::vector<uint8_t> mf;
        ReadChain(U32(d + 0x3C), (uint64_t)-1, mf);
        for (size_t i = 0; i + 4 <= mf.size(); i += 4) miniFat_.push_back(U32(&mf[i]));
        return true;
    }

    const std::vector<Entry>& Entries() const { return entries_; }

    bool ReadStream(const Entry& e, std::vector<uint8_t>& out) {
        if (e.size > 512u * 1024 * 1024) return false;
        if (e.size < miniCutoff_) {
            out.clear();
            uint32_t s = e.start;
            for (size_t guard = 0; out.size() < e.size && s < 0xFFFFFFFA && guard < miniFat_.size() + 1; guard++) {
                uint64_t off = (uint64_t)s * miniSec_;
                if (off + miniSec_ > miniStream_.size()) return false;
                out.insert(out.end(), miniStream_.begin() + off, miniStream_.begin() + off + miniSec_);
                s = s < miniFat_.size() ? miniFat_[s] : 0xFFFFFFFE;
            }
            if (out.size() < e.size) return false;
            out.resize((size_t)e.size);
            return true;
        }
        return ReadChain(e.start, e.size, out) && out.size() == e.size;
    }

private:
    const uint8_t* Sector(uint32_t s) const {
        uint64_t off = ((uint64_t)s + 1) * sec_;
        if (off + sec_ > n_) return nullptr;
        return d_ + off;
    }

    bool ReadChain(uint32_t s, uint64_t size, std::vector<uint8_t>& out) const {
        out.clear();
        for (size_t guard = 0; s < 0xFFFFFFFA && out.size() < size; guard++) {
            if (guard > fat_.size()) return false;  // loop protection
            const uint8_t* p = Sector(s);
            if (!p) return false;
            out.insert(out.end(), p, p + sec_);
            s = s < fat_.size() ? fat_[s] : 0xFFFFFFFE;
        }
        if (size != (uint64_t)-1 && out.size() > size) out.resize((size_t)size);
        return true;
    }

    const uint8_t* d_ = nullptr;
    size_t n_ = 0;
    uint32_t sec_ = 512, miniSec_ = 64, miniCutoff_ = 4096;
    std::vector<uint32_t> fat_, miniFat_;
    std::vector<uint8_t> miniStream_;
    std::vector<Entry> entries_;
};

// SolidWorks "Preview" streams are a DIB (BITMAPINFOHEADER + pixels), usually
// preceded by a 4-byte length. Wrap it in a BITMAPFILEHEADER to make a .bmp.
bool DibToBmp(const std::vector<uint8_t>& s, std::vector<uint8_t>& bmp) {
    size_t off = 0;
    if (s.size() > 8 && U32(&s[0]) != 40 && U32(&s[4]) == 40) off = 4;
    if (s.size() < off + 40 || U32(&s[off]) != 40) return false;
    const uint8_t* bih = &s[off];
    uint16_t bpp = U16(bih + 14);
    uint32_t clrUsed = U32(bih + 32);
    uint32_t compression = U32(bih + 16);
    uint32_t palette = clrUsed ? clrUsed : (bpp <= 8 ? (1u << bpp) : 0);
    uint32_t pixOff = 14 + 40 + palette * 4 + (compression == 3 ? 12 : 0);
    size_t dibLen = s.size() - off;
    bmp.assign(14, 0);
    bmp[0] = 'B'; bmp[1] = 'M';
    uint32_t total = (uint32_t)(14 + dibLen);
    std::memcpy(&bmp[2], &total, 4);
    std::memcpy(&bmp[10], &pixOff, 4);
    bmp.insert(bmp.end(), s.begin() + off, s.end());
    return true;
}

bool FromCfb(const uint8_t* data, size_t len, std::vector<uint8_t>& image) {
    Cfb cfb;
    if (!cfb.Open(data, len)) return false;
    const Cfb::Entry* png = nullptr;
    const Cfb::Entry* pngAny = nullptr;
    const Cfb::Entry* dib = nullptr;
    for (const auto& e : cfb.Entries()) {
        if (e.type != 2) continue;
        if (e.name == "PreviewPNG") png = &e;
        else if (!pngAny && e.name.size() >= 10 && e.name.compare(e.name.size() - 10, 10, "PreviewPNG") == 0) pngAny = &e;
        else if (e.name == "Preview") dib = &e;
    }
    for (const Cfb::Entry* e : {png, pngAny}) {
        if (e && cfb.ReadStream(*e, image) && LooksLikePng(image.data(), image.size())) return true;
    }
    std::vector<uint8_t> raw;
    if (dib && cfb.ReadStream(*dib, raw) && DibToBmp(raw, image)) return true;
    return false;
}

// ---------------------------------------------------------------------------
// SolidWorks 2015+ container: a sequence of sections, each starting with the
// marker 14 00 06 00 08 00, followed by
//   u32 ?, u32 ?, u32 compressedSize, u32 uncompressedSize, u32 nameLen,
//   name (each byte nibble-swapped), raw DEFLATE data.
// ---------------------------------------------------------------------------
bool FromSections(const uint8_t* data, size_t len, std::vector<uint8_t>& image) {
    static const uint8_t marker[6] = {0x14, 0x00, 0x06, 0x00, 0x08, 0x00};
    std::vector<uint8_t> best, tmp;
    bool bestNamed = false;
    for (size_t i = 0; i + 26 <= len; i++) {
        if (data[i] != 0x14 || std::memcmp(data + i, marker, 6) != 0) continue;
        uint32_t csize = U32(data + i + 14);
        uint32_t usize = U32(data + i + 18);
        uint32_t nameLen = U32(data + i + 22);
        if (nameLen == 0 || nameLen > 1024 || i + 26 + nameLen > len) continue;
        std::string name;
        for (uint32_t k = 0; k < nameLen; k++) {
            uint8_t c = data[i + 26 + k];
            name.push_back((char)(((c << 4) | (c >> 4)) & 0xFF));
        }
        bool named = name.size() >= 10 && name.compare(name.size() - 10, 10, "PreviewPNG") == 0;
        if (!named && name != "Preview") continue;
        size_t start = i + 26 + nameLen;
        if (usize < 8 || usize > 64u * 1024 * 1024 || csize > len - start) continue;
        tmp.clear();
        if (!RawInflate(data + start, csize, tmp, usize)) continue;
        if (!LooksLikePng(tmp.data(), tmp.size())) continue;
        // Prefer the stream named exactly "PreviewPNG"; otherwise take the largest.
        bool exact = name == "PreviewPNG";
        if (exact && !bestNamed) { best.swap(tmp); bestNamed = true; }
        else if (exact == bestNamed && tmp.size() > best.size()) best.swap(tmp);
        i = start + csize - 1;
    }
    if (best.empty()) return false;
    image.swap(best);
    return true;
}

}  // namespace

bool LooksLikePng(const uint8_t* p, size_t n) {
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    return n > 8 && std::memcmp(p, sig, 8) == 0;
}

bool ExtractSolidWorksPreview(const uint8_t* data, size_t len, std::vector<uint8_t>& image) {
    if (FromCfb(data, len, image)) return true;
    return FromSections(data, len, image);
}
