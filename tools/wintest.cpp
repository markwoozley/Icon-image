// Windows-side test: wintest.exe <dll> <input> <out.bmp> [size]
// Registers the DLL, creates the handler via COM (as Explorer does), and
// saves the produced thumbnail as a 32-bit BMP.
#include <windows.h>
#include <objbase.h>
#include <shlwapi.h>
#include <thumbcache.h>
#include <shobjidl.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "../src/thumbnail.h"

static const CLSID CLSID_Cad = {0x7fba209c, 0x0dbc, 0x49ba, {0x8a, 0xbf, 0xee, 0x61, 0xff, 0x07, 0x0e, 0x5b}};
static const IID IID_Thumb = {0xe357fccd, 0xa995, 0x4576, {0xb0, 0x1f, 0x23, 0x46, 0x30, 0x15, 0x4e, 0x96}};

// Asks the Windows shell for the thumbnail exactly as File Explorer does
// (handler lookup, out-of-process host, thumbnail cache).
static void ShellCheck(const wchar_t* path, UINT size) {
    static const IID kIID_IShellItemImageFactory = {0xbcc18b79, 0xba16, 0x442f, {0x80, 0xc4, 0x8a, 0x59, 0xc3, 0x0c, 0x46, 0x3b}};
    static const CLSID kCLSID_LocalThumbnailCache = {0x50ef4544, 0xac9f, 0x4a8e, {0xb2, 0x1b, 0x8a, 0x26, 0x18, 0x0d, 0xb1, 0x3f}};
    static const IID kIID_IThumbnailCache = {0xf676c15d, 0x596a, 0x4ce2, {0x82, 0x34, 0x33, 0x99, 0x6f, 0x44, 0x5d, 0xb1}};
    std::printf("\n--- Windows shell check (what Explorer sees) ---\n");
    IShellItem* item = nullptr;
    HRESULT hr = SHCreateItemFromParsingName(path, nullptr, IID_IShellItem, (void**)&item);
    if (FAILED(hr)) { std::printf("open item failed: 0x%08lx\n", hr); return; }
    DWORD fa = GetFileAttributesW(path);
    std::printf("file attributes: 0x%08lx%s%s%s\n", fa, (fa & 0x00400000) ? "  [cloud: online-only]" : "",
                (fa & 0x00080000) ? "  [cloud: always keep on device]" : "", (fa & 0x00001000) ? "  [offline]" : "");
    SFGAOF attrs = 0;
    item->GetAttributes(SFGAO_FILESYSTEM | SFGAO_STORAGE | SFGAO_STREAM, &attrs);
    std::printf("item attributes: 0x%08lx\n", attrs);

    IThumbnailCache* cache = nullptr;
    hr = CoCreateInstance(kCLSID_LocalThumbnailCache, nullptr, CLSCTX_INPROC_SERVER, kIID_IThumbnailCache, (void**)&cache);
    if (SUCCEEDED(hr)) {
        ISharedBitmap* sb = nullptr;
        WTS_CACHEFLAGS flags = WTS_DEFAULT;
        // 0x8004B205 = "extraction pending": Windows is still working; wait and ask again.
        for (int tries = 0; tries < 30; tries++) {
            hr = cache->GetThumbnail(item, size, (WTS_FLAGS)(WTS_EXTRACT | (tries == 0 ? WTS_FORCEEXTRACTION : 0)), &sb,
                                     &flags, nullptr);
            if (hr != (HRESULT)0x8004B205) break;
            Sleep(500);
        }
        std::printf("thumbnail cache (forced re-extract): 0x%08lx%s\n", hr,
                    hr == (HRESULT)0x8004B200   ? "  (handler failed)"
                    : hr == (HRESULT)0x8004B205 ? "  (still pending after 15 s)"
                    : hr == (HRESULT)0x8004B206 ? "  (extraction blocked)"
                    : hr == (HRESULT)0x8004B207 ? "  (cloud file: storage provider has no thumbnail)"
                    : SUCCEEDED(hr)             ? "  OK"
                                                : "");
        if (sb) {
            SIZE sz = {};
            sb->GetSize(&sz);
            std::printf("  shell thumbnail size %ldx%ld\n", sz.cx, sz.cy);
            sb->Release();
        }
        cache->Release();
    } else {
        std::printf("thumbnail cache unavailable: 0x%08lx\n", hr);
    }

    IShellItemImageFactory* f = nullptr;
    hr = item->QueryInterface(kIID_IShellItemImageFactory, (void**)&f);
    if (SUCCEEDED(hr)) {
        SIZE sz = {(LONG)size, (LONG)size};
        HBITMAP b = nullptr;
        for (int tries = 0; tries < 20; tries++) {
            hr = f->GetImage(sz, SIIGBF_THUMBNAILONLY, &b);
            if (hr != (HRESULT)0x8004B205) break;
            Sleep(500);
        }
        std::printf("Explorer-style GetImage (thumbnail only): 0x%08lx %s\n", hr, SUCCEEDED(hr) ? "OK" : "FAILED");
        if (b) DeleteObject(b);
        f->Release();
    }
    item->Release();
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 4) return 2;
    UINT size = argc > 4 ? (UINT)_wtoi(argv[4]) : 256;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::printf("CAD thumbnail test v4\n");
    HMODULE m = LoadLibraryW(argv[1]);
    if (!m) { std::printf("LoadLibrary failed %lu\n", GetLastError()); return 1; }
    auto reg = (HRESULT(WINAPI*)())GetProcAddress(m, "DllRegisterServer");
    std::printf("DllRegisterServer: 0x%08lx\n", reg ? reg() : E_FAIL);
    wchar_t val[256]; DWORD cb = sizeof(val);
    LONG r = RegGetValueW(HKEY_CURRENT_USER, L"Software\\Classes\\.stl\\ShellEx\\{e357fccd-a995-4576-b01f-234630154e96}",
                          nullptr, RRF_RT_REG_SZ, nullptr, val, &cb);
    std::printf("HKCU .stl handler: %ls (%ld)\n", r == 0 ? val : L"<missing>", r);

    // Which thumbnail handler will Explorer actually use for this file type?
    {
        const wchar_t* dot = wcsrchr(argv[2], L'.');
        wchar_t clsid[128] = L"";
        DWORD n = 128;
        HRESULT ar = AssocQueryStringW(ASSOCF_INIT_DEFAULTTOSTAR, ASSOCSTR_SHELLEXTENSION, dot ? dot : L".x",
                                       L"{e357fccd-a995-4576-b01f-234630154e96}", clsid, &n);
        wchar_t prog[256] = L"";
        n = 256;
        AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_PROGID, dot ? dot : L".x", nullptr, prog, &n);
        std::printf("Explorer's handler for %ls (ProgID %ls): %ls %s\n", dot ? dot : L"?", prog,
                    SUCCEEDED(ar) ? clsid : L"<none>",
                    SUCCEEDED(ar) && _wcsicmp(clsid, L"{7FBA209C-0DBC-49BA-8ABF-EE61FF070E5B}") == 0 ? "(this add-on - good)"
                                                                                                 : "(NOT this add-on)");
    }
    IUnknown* unk = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_Cad, nullptr, CLSCTX_INPROC_SERVER, IID_IUnknown, (void**)&unk);
    std::printf("CoCreateInstance: 0x%08lx\n", hr);
    if (FAILED(hr)) return 1;
    IInitializeWithStream* init = nullptr;
    IThumbnailProvider* tp = nullptr;
    unk->QueryInterface(IID_IInitializeWithStream, (void**)&init);
    unk->QueryInterface(IID_Thumb, (void**)&tp);
    IStream* stm = nullptr;
    hr = SHCreateStreamOnFileEx(argv[2], STGM_READ | STGM_SHARE_DENY_NONE, 0, FALSE, nullptr, &stm);
    if (FAILED(hr)) { std::printf("open failed 0x%08lx\n", hr); return 1; }
    hr = init->Initialize(stm, STGM_READ);
    std::printf("Initialize: 0x%08lx\n", hr);
    HBITMAP bmp = nullptr;
    WTS_ALPHATYPE at;
    hr = tp->GetThumbnail(size, &bmp, &at);
    std::printf("GetThumbnail: 0x%08lx alpha=%d\n", hr, (int)at);

    // Diagnostic report: run the same reader code directly and explain.
    {
        std::vector<uint8_t> data;
        if (FILE* in = _wfopen(argv[2], L"rb")) {
            uint8_t buf[65536];
            size_t got;
            while ((got = fread(buf, 1, sizeof(buf), in)) > 0) data.insert(data.end(), buf, buf + got);
            fclose(in);
        }
        std::string ext;
        const wchar_t* dot = wcsrchr(argv[2], L'.');
        for (const wchar_t* c = dot; c && *c; c++) ext.push_back(*c < 128 ? (char)*c : '?');
        Image img;
        std::vector<uint8_t> enc;
        std::string log;
        MakeThumbnail(data.data(), data.size(), (int)size, img, enc, ext, &log);
        std::printf("\n--- report ---\n%s--------------\n", log.c_str());
        if (!enc.empty()) {
            // Save the raw embedded preview so it can be inspected.
            std::wstring raw = argv[3];
            size_t d = raw.rfind(L'.');
            if (d != std::wstring::npos) raw.resize(d);
            raw += (enc.size() > 1 && enc[1] == 'P') ? L".preview.png" : L".preview.bmp";
            if (FILE* o = _wfopen(raw.c_str(), L"wb")) { fwrite(enc.data(), 1, enc.size(), o); fclose(o); }
            std::printf("Raw preview saved as %ls\n", raw.c_str());
            if (enc.size() > 26 && enc[1] == 'P')
                std::printf("PNG: %ux%u, bit depth %u, colour type %u\n",
                            (enc[16] << 24) | (enc[17] << 16) | (enc[18] << 8) | enc[19],
                            (enc[20] << 24) | (enc[21] << 16) | (enc[22] << 8) | enc[23], enc[24], enc[25]);
        }
    }
    if (FAILED(hr)) return 1;
    BITMAP bm;
    GetObject(bmp, sizeof(bm), &bm);
    std::printf("bitmap %ldx%ld %dbpp\n", bm.bmWidth, bm.bmHeight, bm.bmBitsPixel);
    BITMAPINFOHEADER bih = {sizeof(bih), bm.bmWidth, -bm.bmHeight, 1, 32, BI_RGB};
    size_t bytes = (size_t)bm.bmWidth * bm.bmHeight * 4;
    void* px = std::malloc(bytes);
    HDC dc = GetDC(nullptr);
    GetDIBits(dc, bmp, 0, bm.bmHeight, px, (BITMAPINFO*)&bih, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    {
        size_t white = 0, clear = 0, total = (size_t)bm.bmWidth * bm.bmHeight;
        const BYTE* q = (const BYTE*)px;
        for (size_t i = 0; i < total; i++) {
            if (q[i * 4] > 245 && q[i * 4 + 1] > 245 && q[i * 4 + 2] > 245) white++;
            if (q[i * 4 + 3] == 0) clear++;
        }
        std::printf("thumbnail pixels: %zu%% near-white, %zu%% transparent\n", white * 100 / total, clear * 100 / total);
    }
    BITMAPFILEHEADER bfh = {0x4D42, (DWORD)(sizeof(bfh) + sizeof(bih) + bytes), 0, 0, sizeof(bfh) + sizeof(bih)};
    FILE* f = _wfopen(argv[3], L"wb");
    fwrite(&bfh, sizeof(bfh), 1, f);
    fwrite(&bih, sizeof(bih), 1, f);
    fwrite(px, bytes, 1, f);
    fclose(f);
    tp->Release(); init->Release(); unk->Release(); stm->Release();
    ShellCheck(argv[2], size);
    CoUninitialize();
    return 0;
}
