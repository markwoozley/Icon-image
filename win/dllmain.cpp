// Windows Explorer thumbnail handler (IThumbnailProvider) for SolidWorks,
// STEP and STL files. Registers per-user (HKCU) so no admin rights needed.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <thumbcache.h>
#include <wincodec.h>
#include <olectl.h>
#include <new>
#include <string>
#include <vector>

#include "../src/thumbnail.h"

// {7FBA209C-0DBC-49BA-8ABF-EE61FF070E5B}
static const CLSID CLSID_CadThumbnailProvider = {
    0x7fba209c, 0x0dbc, 0x49ba, {0x8a, 0xbf, 0xee, 0x61, 0xff, 0x07, 0x0e, 0x5b}};
// IThumbnailProvider (not exported by every SDK's uuid library).
static const IID kIID_IThumbnailProvider = {
    0xe357fccd, 0xa995, 0x4576, {0xb0, 0x1f, 0x23, 0x46, 0x30, 0x15, 0x4e, 0x96}};
static const wchar_t kClsidStr[] = L"{7FBA209C-0DBC-49BA-8ABF-EE61FF070E5B}";
static const wchar_t kThumbHandlerKey[] = L"{e357fccd-a995-4576-b01f-234630154e96}";
static const wchar_t* kExtensions[] = {L".stl", L".step", L".stp", L".sldprt", L".sldasm", L".slddrw"};

static HMODULE g_module = nullptr;
static LONG g_refs = 0;

namespace {

const ULONGLONG kMaxFileBytes = 1024ull * 1024 * 1024;  // 1 GB

class ThumbnailProvider : public IInitializeWithStream, public IThumbnailProvider {
public:
    ThumbnailProvider() { InterlockedIncrement(&g_refs); }
    virtual ~ThumbnailProvider() { InterlockedDecrement(&g_refs); }

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == kIID_IThumbnailProvider) {
            *ppv = static_cast<IThumbnailProvider*>(this);
        } else if (riid == IID_IInitializeWithStream) {
            *ppv = static_cast<IInitializeWithStream*>(this);
        } else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
    IFACEMETHODIMP_(ULONG) Release() override {
        ULONG r = InterlockedDecrement(&refs_);
        if (!r) delete this;
        return r;
    }

    // IInitializeWithStream
    IFACEMETHODIMP Initialize(IStream* stream, DWORD) override {
        if (!stream) return E_INVALIDARG;
        if (!data_.empty()) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        STATSTG st = {};
        if (SUCCEEDED(stream->Stat(&st, STATFLAG_DEFAULT))) {
            if (st.pwcsName) {
                // Keep the file extension; it tells us which reader to use.
                const wchar_t* dot = wcsrchr(st.pwcsName, L'.');
                if (dot)
                    for (const wchar_t* c = dot; *c && ext_.size() < 16; c++) ext_.push_back(*c < 128 ? (char)*c : '?');
                CoTaskMemFree(st.pwcsName);
            }
            if (st.cbSize.QuadPart > kMaxFileBytes) return E_FAIL;
        }
        try {
            if (st.cbSize.QuadPart) data_.reserve((size_t)st.cbSize.QuadPart);
            BYTE buf[1 << 16];
            for (;;) {
                ULONG got = 0;
                HRESULT hr = stream->Read(buf, sizeof(buf), &got);
                if (got) data_.insert(data_.end(), buf, buf + got);
                if (FAILED(hr)) return hr;
                if (got == 0 || hr == S_FALSE) break;
                if (data_.size() > kMaxFileBytes) return E_FAIL;
            }
        } catch (...) {
            return E_OUTOFMEMORY;
        }
        return data_.empty() ? E_FAIL : S_OK;
    }

    // IThumbnailProvider
    IFACEMETHODIMP GetThumbnail(UINT cx, HBITMAP* phbmp, WTS_ALPHATYPE* alpha) override {
        if (!phbmp || !alpha) return E_POINTER;
        *phbmp = nullptr;
        *alpha = WTSAT_UNKNOWN;
        try {
            Image img;
            std::vector<uint8_t> encoded;
            ThumbKind k = MakeThumbnail(data_.data(), data_.size(), (int)cx, img, encoded, ext_);
            if (k == ThumbKind::Rendered) {
                *alpha = WTSAT_ARGB;
                return MakeBitmap(img.width, img.height, img.bgra.data(), phbmp);
            }
            if (k == ThumbKind::EncodedImage) {
                *alpha = WTSAT_RGB;
                return DecodeImage(encoded, cx, phbmp);
            }
        } catch (...) {
            return E_FAIL;
        }
        return E_FAIL;
    }

private:
    static HRESULT MakeBitmap(int w, int h, const uint8_t* bgra, HBITMAP* out) {
        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
        bmi.bmiHeader.biWidth = w;
        bmi.bmiHeader.biHeight = -h;  // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP bmp = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bmp) return E_OUTOFMEMORY;
        if (bgra) memcpy(bits, bgra, (size_t)w * h * 4);
        *out = bmp;
        return S_OK;
    }

    // Makes the preview opaque. Some SolidWorks versions store the preview
    // with an alpha channel that is (almost) entirely zero even though the
    // colours are valid; in that case the alpha channel is ignored. Otherwise
    // transparent areas are blended onto white.
    static void FlattenOntoWhite(BYTE* px, size_t count) {
        size_t clear = 0;
        for (size_t i = 0; i < count; i++)
            if (px[i * 4 + 3] == 0) clear++;
        bool ignoreAlpha = clear * 100 >= count * 90;
        for (size_t i = 0; i < count; i++) {
            BYTE* p = px + i * 4;
            if (!ignoreAlpha) {
                unsigned a = p[3];
                for (int c = 0; c < 3; c++) p[c] = (BYTE)((p[c] * a + 255 * (255 - a) + 127) / 255);
            }
            p[3] = 255;
        }
    }

    // Decodes a PNG/BMP with WIC, scaled so its longest side is at most cx.
    static HRESULT DecodeImage(const std::vector<uint8_t>& enc, UINT cx, HBITMAP* out) {
        IWICImagingFactory* factory = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory,
                                      (void**)&factory);
        if (FAILED(hr)) return hr;
        IWICStream* stream = nullptr;
        IWICBitmapDecoder* decoder = nullptr;
        IWICBitmapFrameDecode* frame = nullptr;
        IWICFormatConverter* conv = nullptr;
        IWICBitmapScaler* scaler = nullptr;
        hr = factory->CreateStream(&stream);
        if (SUCCEEDED(hr)) hr = stream->InitializeFromMemory((BYTE*)enc.data(), (DWORD)enc.size());
        if (SUCCEEDED(hr)) hr = factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder);
        if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
        if (SUCCEEDED(hr)) hr = factory->CreateFormatConverter(&conv);
        if (SUCCEEDED(hr))
            hr = conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                  WICBitmapPaletteTypeCustom);
        UINT w = 0, h = 0;
        if (SUCCEEDED(hr)) hr = conv->GetSize(&w, &h);
        IWICBitmapSource* src = conv;
        if (SUCCEEDED(hr) && w && h && (w > cx || h > cx)) {
            double s = (double)cx / (w > h ? w : h);
            UINT nw = (UINT)(w * s + 0.5), nh = (UINT)(h * s + 0.5);
            if (nw < 1) nw = 1;
            if (nh < 1) nh = 1;
            hr = factory->CreateBitmapScaler(&scaler);
            if (SUCCEEDED(hr)) hr = scaler->Initialize(conv, nw, nh, WICBitmapInterpolationModeFant);
            if (SUCCEEDED(hr)) { src = scaler; w = nw; h = nh; }
        }
        if (SUCCEEDED(hr) && (!w || !h)) hr = E_FAIL;
        if (SUCCEEDED(hr)) {
            HBITMAP bmp = nullptr;
            hr = MakeBitmap((int)w, (int)h, nullptr, &bmp);
            if (SUCCEEDED(hr)) {
                DIBSECTION ds;
                GetObject(bmp, sizeof(ds), &ds);
                hr = src->CopyPixels(nullptr, w * 4, w * h * 4, (BYTE*)ds.dsBm.bmBits);
                if (SUCCEEDED(hr)) {
                    FlattenOntoWhite((BYTE*)ds.dsBm.bmBits, (size_t)w * h);
                    *out = bmp;
                }
                else DeleteObject(bmp);
            }
        }
        if (scaler) scaler->Release();
        if (conv) conv->Release();
        if (frame) frame->Release();
        if (decoder) decoder->Release();
        if (stream) stream->Release();
        factory->Release();
        return hr;
    }

    LONG refs_ = 1;
    std::vector<uint8_t> data_;
    std::string ext_;
};

class ClassFactory : public IClassFactory {
public:
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { InterlockedIncrement(&g_refs); return 2; }
    IFACEMETHODIMP_(ULONG) Release() override { InterlockedDecrement(&g_refs); return 1; }
    IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (outer) return CLASS_E_NOAGGREGATION;
        ThumbnailProvider* p = new (std::nothrow) ThumbnailProvider();
        if (!p) return E_OUTOFMEMORY;
        HRESULT hr = p->QueryInterface(riid, ppv);
        p->Release();
        return hr;
    }
    IFACEMETHODIMP LockServer(BOOL lock) override {
        if (lock) InterlockedIncrement(&g_refs);
        else InterlockedDecrement(&g_refs);
        return S_OK;
    }
};

ClassFactory g_factory;

// --- Registration helpers ---------------------------------------------------
bool SetString(HKEY root, const std::wstring& path, const wchar_t* name, const std::wstring& value) {
    HKEY k;
    if (RegCreateKeyExW(root, path.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return false;
    LONG r = RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)value.c_str(), (DWORD)((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

std::wstring GetString(HKEY root, const std::wstring& path) {
    wchar_t buf[512];
    DWORD size = sizeof(buf);
    if (RegGetValueW(root, path.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return L"";
    return buf;
}

// Deletes our handler key only if it still points at this CLSID.
void RemoveIfOurs(const std::wstring& path) {
    if (_wcsicmp(GetString(HKEY_CURRENT_USER, path).c_str(), kClsidStr) == 0)
        RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str());
}

std::vector<std::wstring> HandlerKeys(const wchar_t* ext) {
    std::wstring base = L"Software\\Classes\\";
    std::vector<std::wstring> keys = {
        base + ext + L"\\ShellEx\\" + kThumbHandlerKey,
        base + L"SystemFileAssociations\\" + ext + L"\\ShellEx\\" + kThumbHandlerKey,
    };
    // Also cover the ProgID the extension is associated with (e.g. one
    // installed by a CAD program), which Explorer may consult first.
    std::vector<std::wstring> progIds = {GetString(HKEY_CLASSES_ROOT, ext)};
    std::wstring fileExts = std::wstring(L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\") + ext;
    HKEY k;
    wchar_t buf[512];
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, (fileExts + L"\\UserChoice").c_str(), L"ProgId", RRF_RT_REG_SZ, nullptr, buf,
                     &size) == ERROR_SUCCESS)
        progIds.push_back(buf);
    for (const std::wstring& sub : {std::wstring(ext) + L"\\OpenWithProgids", fileExts + L"\\OpenWithProgids"}) {
        HKEY root = sub[0] == L'.' ? HKEY_CLASSES_ROOT : HKEY_CURRENT_USER;
        if (RegOpenKeyExW(root, sub.c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) continue;
        for (DWORD i = 0;; i++) {
            DWORD n = 512;
            if (RegEnumValueW(k, i, buf, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            progIds.push_back(buf);
        }
        RegCloseKey(k);
    }
    for (auto& id : progIds) {
        if (id.empty() || id.find(L'\\') != std::wstring::npos) continue;
        std::wstring key = base + id + L"\\ShellEx\\" + kThumbHandlerKey;
        bool dup = false;
        for (auto& e : keys) dup = dup || _wcsicmp(e.c_str(), key.c_str()) == 0;
        if (!dup) keys.push_back(key);
    }
    return keys;
}

}  // namespace

extern "C" BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = inst;
        DisableThreadLibraryCalls(inst);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (clsid != CLSID_CadThumbnailProvider) return CLASS_E_CLASSNOTAVAILABLE;
    return g_factory.QueryInterface(riid, ppv);
}

STDAPI DllCanUnloadNow() { return g_refs == 0 ? S_OK : S_FALSE; }

STDAPI DllRegisterServer() {
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(g_module, path, MAX_PATH)) return HRESULT_FROM_WIN32(GetLastError());
    std::wstring clsidKey = std::wstring(L"Software\\Classes\\CLSID\\") + kClsidStr;
    bool ok = SetString(HKEY_CURRENT_USER, clsidKey, nullptr, L"CAD Thumbnail Provider (SolidWorks/STEP/STL)") &&
              SetString(HKEY_CURRENT_USER, clsidKey + L"\\InprocServer32", nullptr, path) &&
              SetString(HKEY_CURRENT_USER, clsidKey + L"\\InprocServer32", L"ThreadingModel", L"Apartment");
    for (const wchar_t* ext : kExtensions)
        for (auto& key : HandlerKeys(ext)) ok = SetString(HKEY_CURRENT_USER, key, nullptr, kClsidStr) && ok;
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return ok ? S_OK : SELFREG_E_CLASS;
}

STDAPI DllUnregisterServer() {
    for (const wchar_t* ext : kExtensions)
        for (auto& key : HandlerKeys(ext)) RemoveIfOurs(key);
    RegDeleteTreeW(HKEY_CURRENT_USER, (std::wstring(L"Software\\Classes\\CLSID\\") + kClsidStr).c_str());
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}
