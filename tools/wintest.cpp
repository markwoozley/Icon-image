// Windows-side test: wintest.exe <dll> <input> <out.bmp> [size]
// Registers the DLL, creates the handler via COM (as Explorer does), and
// saves the produced thumbnail as a 32-bit BMP.
#include <windows.h>
#include <objbase.h>
#include <shlwapi.h>
#include <thumbcache.h>
#include <cstdio>
#include <cstdlib>

static const CLSID CLSID_Cad = {0x7fba209c, 0x0dbc, 0x49ba, {0x8a, 0xbf, 0xee, 0x61, 0xff, 0x07, 0x0e, 0x5b}};
static const IID IID_Thumb = {0xe357fccd, 0xa995, 0x4576, {0xb0, 0x1f, 0x23, 0x46, 0x30, 0x15, 0x4e, 0x96}};

int wmain(int argc, wchar_t** argv) {
    if (argc < 4) return 2;
    UINT size = argc > 4 ? (UINT)_wtoi(argv[4]) : 256;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    HMODULE m = LoadLibraryW(argv[1]);
    if (!m) { std::printf("LoadLibrary failed %lu\n", GetLastError()); return 1; }
    auto reg = (HRESULT(WINAPI*)())GetProcAddress(m, "DllRegisterServer");
    std::printf("DllRegisterServer: 0x%08lx\n", reg ? reg() : E_FAIL);
    wchar_t val[256]; DWORD cb = sizeof(val);
    LONG r = RegGetValueW(HKEY_CURRENT_USER, L"Software\\Classes\\.stl\\ShellEx\\{e357fccd-a995-4576-b01f-234630154e96}",
                          nullptr, RRF_RT_REG_SZ, nullptr, val, &cb);
    std::printf("HKCU .stl handler: %ls (%ld)\n", r == 0 ? val : L"<missing>", r);

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
    BITMAPFILEHEADER bfh = {0x4D42, (DWORD)(sizeof(bfh) + sizeof(bih) + bytes), 0, 0, sizeof(bfh) + sizeof(bih)};
    FILE* f = _wfopen(argv[3], L"wb");
    fwrite(&bfh, sizeof(bfh), 1, f);
    fwrite(&bih, sizeof(bih), 1, f);
    fwrite(px, bytes, 1, f);
    fclose(f);
    tp->Release(); init->Release(); unk->Release(); stm->Release();
    CoUninitialize();
    return 0;
}
