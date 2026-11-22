// Command-line harness: thumbtest <input> <output-prefix> [size]
// Writes <prefix>.pam (rendered) or <prefix>.png/.bmp (SolidWorks preview).
#include "../src/thumbnail.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: %s input outprefix [size]\n", argv[0]); return 2; }
    std::ifstream in(argv[1], std::ios::binary);
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    int size = argc > 3 ? std::atoi(argv[3]) : 256;
    Image img;
    std::vector<uint8_t> enc;
    auto t0 = std::chrono::steady_clock::now();
    ThumbKind k = MakeThumbnail(data.data(), data.size(), size, img, enc);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::string out = argv[2];
    if (k == ThumbKind::Rendered) {
        std::FILE* f = std::fopen((out + ".pam").c_str(), "wb");
        std::fprintf(f, "P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", img.width, img.height);
        for (size_t i = 0; i < img.bgra.size(); i += 4) {
            uint8_t a = img.bgra[i + 3];
            auto un = [&](uint8_t c) { return (uint8_t)(a ? std::min(255, c * 255 / a) : 0); };
            uint8_t px[4] = {un(img.bgra[i + 2]), un(img.bgra[i + 1]), un(img.bgra[i]), a};
            std::fwrite(px, 1, 4, f);
        }
        std::fclose(f);
        std::printf("%s: rendered %dx%d in %.1f ms\n", argv[1], img.width, img.height, ms);
    } else if (k == ThumbKind::EncodedImage) {
        bool png = enc.size() > 4 && enc[1] == 'P';
        std::FILE* f = std::fopen((out + (png ? ".png" : ".bmp")).c_str(), "wb");
        std::fwrite(enc.data(), 1, enc.size(), f);
        std::fclose(f);
        std::printf("%s: embedded %s preview (%zu bytes) in %.1f ms\n", argv[1], png ? "PNG" : "BMP", enc.size(), ms);
    } else {
        std::printf("%s: FAILED (%.1f ms)\n", argv[1], ms);
        return 1;
    }
    return 0;
}
