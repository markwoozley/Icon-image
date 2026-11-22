// Compact raw-DEFLATE decoder, modelled on zlib's "puff" reference decoder.
#include "inflate.h"
#include <cstring>

namespace {

struct State {
    const uint8_t* in;
    size_t inLen, inPos = 0;
    uint32_t bitBuf = 0;
    int bitCnt = 0;
    std::vector<uint8_t>* out;
    size_t maxOut;
    bool full = false;
};

struct Huffman {
    short count[16];
    short symbol[288];
};

bool Bits(State& s, int need, int& val) {
    uint32_t v = s.bitBuf;
    while (s.bitCnt < need) {
        if (s.inPos >= s.inLen) return false;
        v |= (uint32_t)s.in[s.inPos++] << s.bitCnt;
        s.bitCnt += 8;
    }
    s.bitBuf = v >> need;
    s.bitCnt -= need;
    val = (int)(v & ((1u << need) - 1));
    return true;
}

bool Put(State& s, uint8_t b) {
    if (s.out->size() >= s.maxOut) { s.full = true; return false; }
    s.out->push_back(b);
    return true;
}

bool Stored(State& s) {
    s.bitBuf = 0; s.bitCnt = 0;
    if (s.inPos + 4 > s.inLen) return false;
    unsigned len = s.in[s.inPos] | (s.in[s.inPos + 1] << 8);
    unsigned nlen = s.in[s.inPos + 2] | (s.in[s.inPos + 3] << 8);
    s.inPos += 4;
    if (len != (~nlen & 0xffff)) return false;
    if (s.inPos + len > s.inLen) return false;
    for (unsigned i = 0; i < len; i++)
        if (!Put(s, s.in[s.inPos++])) return false;
    return true;
}

// Returns symbol or -1 on error.
int Decode(State& s, const Huffman& h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        int b;
        if (!Bits(s, 1, b)) return -1;
        code |= b;
        int count = h.count[len];
        if (code - count < first) return h.symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

// Returns 0 for a complete code, >0 for incomplete, <0 for over-subscribed.
int Construct(Huffman& h, const short* length, int n) {
    std::memset(h.count, 0, sizeof(h.count));
    for (int i = 0; i < n; i++) h.count[length[i]]++;
    if (h.count[0] == n) return 0;
    int left = 1;
    for (int len = 1; len <= 15; len++) {
        left <<= 1;
        left -= h.count[len];
        if (left < 0) return left;
    }
    short offs[16];
    offs[1] = 0;
    for (int len = 1; len < 15; len++) offs[len + 1] = offs[len] + h.count[len];
    for (int i = 0; i < n; i++)
        if (length[i] != 0) h.symbol[offs[length[i]]++] = (short)i;
    return left;
}

const short kLBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                          35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const short kLExt[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                         3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const short kDBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                          193, 257, 385, 513, 769, 1025, 1537, 2049, 3073,
                          4097, 6145, 8193, 12289, 16385, 24577};
const short kDExt[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                         7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool Codes(State& s, const Huffman& lencode, const Huffman& distcode) {
    for (;;) {
        int sym = Decode(s, lencode);
        if (sym < 0) return false;
        if (sym < 256) {
            if (!Put(s, (uint8_t)sym)) return false;
        } else if (sym == 256) {
            return true;
        } else {
            sym -= 257;
            if (sym >= 29) return false;
            int e;
            if (!Bits(s, kLExt[sym], e)) return false;
            int len = kLBase[sym] + e;
            int dsym = Decode(s, distcode);
            if (dsym < 0 || dsym >= 30) return false;
            if (!Bits(s, kDExt[dsym], e)) return false;
            size_t dist = (size_t)kDBase[dsym] + e;
            if (dist > s.out->size()) return false;
            for (int i = 0; i < len; i++) {
                uint8_t b = (*s.out)[s.out->size() - dist];
                if (!Put(s, b)) return false;
            }
        }
    }
}

bool Fixed(State& s) {
    static Huffman lencode, distcode;
    static bool built = false;
    if (!built) {
        short lengths[288];
        int i = 0;
        for (; i < 144; i++) lengths[i] = 8;
        for (; i < 256; i++) lengths[i] = 9;
        for (; i < 280; i++) lengths[i] = 7;
        for (; i < 288; i++) lengths[i] = 8;
        Construct(lencode, lengths, 288);
        for (i = 0; i < 30; i++) lengths[i] = 5;
        Construct(distcode, lengths, 30);
        built = true;
    }
    return Codes(s, lencode, distcode);
}

bool Dynamic(State& s) {
    static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    int nlen, ndist, ncode;
    if (!Bits(s, 5, nlen) || !Bits(s, 5, ndist) || !Bits(s, 4, ncode)) return false;
    nlen += 257; ndist += 1; ncode += 4;
    if (nlen > 286 || ndist > 30) return false;
    short lengths[320] = {0};
    for (int i = 0; i < ncode; i++) {
        int v;
        if (!Bits(s, 3, v)) return false;
        lengths[order[i]] = (short)v;
    }
    Huffman lencode, distcode;
    if (Construct(lencode, lengths, 19) != 0) return false;
    int index = 0;
    while (index < nlen + ndist) {
        int sym = Decode(s, lencode);
        if (sym < 0) return false;
        if (sym < 16) {
            lengths[index++] = (short)sym;
        } else {
            int len = 0, rep;
            if (sym == 16) {
                if (index == 0) return false;
                len = lengths[index - 1];
                if (!Bits(s, 2, rep)) return false;
                rep += 3;
            } else if (sym == 17) {
                if (!Bits(s, 3, rep)) return false;
                rep += 3;
            } else {
                if (!Bits(s, 7, rep)) return false;
                rep += 11;
            }
            if (index + rep > nlen + ndist) return false;
            while (rep--) lengths[index++] = (short)len;
        }
    }
    if (lengths[256] == 0) return false;
    int err = Construct(lencode, lengths, nlen);
    if (err < 0 || (err > 0 && nlen - lencode.count[0] != 1)) return false;
    err = Construct(distcode, lengths + nlen, ndist);
    if (err < 0 || (err > 0 && ndist - distcode.count[0] != 1)) return false;
    return Codes(s, lencode, distcode);
}

}  // namespace

bool RawInflate(const uint8_t* src, size_t srcLen, std::vector<uint8_t>& out, size_t maxOut) {
    State s;
    s.in = src;
    s.inLen = srcLen;
    s.out = &out;
    s.maxOut = maxOut;
    int last, type;
    do {
        if (!Bits(s, 1, last) || !Bits(s, 2, type)) return false;
        bool ok;
        if (type == 0) ok = Stored(s);
        else if (type == 1) ok = Fixed(s);
        else if (type == 2) ok = Dynamic(s);
        else return false;
        if (!ok) return s.full;  // hitting maxOut counts as success
    } while (!last);
    return true;
}
