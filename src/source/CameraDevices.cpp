#include "CameraDevices.hpp"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dshow.h>

// System import libs for DirectShow device enumeration (no vcpkg dependency).
#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

namespace quadmapper {

namespace {
std::string bstrToUtf8(BSTR bstr) {
    if (!bstr) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, bstr, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return {};
    std::string out(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, bstr, -1, out.data(), len, nullptr, nullptr);
    return out;
}
} // namespace

std::vector<CameraDevice> enumerateCameraDevices() {
    std::vector<CameraDevice> devices;

    // CoInitialize may already have been called on this thread; tolerate both.
    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool didInit = SUCCEEDED(hrInit);

    ICreateDevEnum* devEnum = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&devEnum)))) {
        IEnumMoniker* enumMoniker = nullptr;
        HRESULT hr = devEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &enumMoniker, 0);
        if (hr == S_OK && enumMoniker) {
            IMoniker* moniker = nullptr;
            while (enumMoniker->Next(1, &moniker, nullptr) == S_OK) {
                IPropertyBag* bag = nullptr;
                if (SUCCEEDED(moniker->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&bag)))) {
                    VARIANT var;
                    VariantInit(&var);
                    if (SUCCEEDED(bag->Read(L"FriendlyName", &var, nullptr)) && var.vt == VT_BSTR) {
                        std::string name = bstrToUtf8(var.bstrVal);
                        if (!name.empty()) {
                            devices.push_back(CameraDevice{name});
                        }
                    }
                    VariantClear(&var);
                    bag->Release();
                }
                moniker->Release();
            }
            enumMoniker->Release();
        }
        devEnum->Release();
    }

    if (didInit) {
        CoUninitialize();
    }
    return devices;
}

} // namespace quadmapper

#else // !_WIN32

namespace quadmapper {
std::vector<CameraDevice> enumerateCameraDevices() { return {}; }
} // namespace quadmapper

#endif
