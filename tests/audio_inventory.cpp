// Read-only diagnostic: enumerate render endpoints, live sessions, and resolved app names.
#include "app_identity.hpp"
#include "com_ptr.hpp"
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>
#include <iostream>

namespace {
std::string Utf8(const std::wstring& text) {
    const auto bytes = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(bytes, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), bytes, nullptr, nullptr);
    return result;
}
int Run() {
    tvm::ComPtr<IMMDeviceEnumerator> enumerator;
    tvm::ComPtr<IMMDeviceCollection> devices;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(enumerator.put()))) ||
        FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, devices.put()))) return 1;
    tvm::ComPtr<IMMDevice> defaultDevice;
    std::wstring defaultId;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, defaultDevice.put()))) {
        LPWSTR id = nullptr; defaultDevice->GetId(&id); if (id) defaultId = id; CoTaskMemFree(id);
    }
    UINT count = 0; devices->GetCount(&count);
    for (UINT index = 0; index < count; ++index) {
        tvm::ComPtr<IMMDevice> device;
        if (FAILED(devices->Item(index, device.put()))) continue;
        LPWSTR id = nullptr; device->GetId(&id);
        const bool isDefault = id && defaultId == id; CoTaskMemFree(id);
        tvm::ComPtr<IPropertyStore> properties;
        PROPVARIANT name; PropVariantInit(&name);
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, properties.put()))) properties->GetValue(PKEY_Device_FriendlyName, &name);
        std::cout << "OUTPUT: " << Utf8(name.vt == VT_LPWSTR ? name.pwszVal : L"Unknown") << (isDefault ? " [default]" : "") << '\n';
        PropVariantClear(&name);
        tvm::ComPtr<IAudioSessionManager2> manager;
        tvm::ComPtr<IAudioSessionEnumerator> sessions;
        if (FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(manager.put()))) ||
            FAILED(manager->GetSessionEnumerator(sessions.put()))) continue;
        int sessionCount = 0; sessions->GetCount(&sessionCount);
        for (int i = 0; i < sessionCount; ++i) {
            tvm::ComPtr<IAudioSessionControl> control;
            tvm::ComPtr<IAudioSessionControl2> identity;
            if (FAILED(sessions->GetSession(i, control.put())) || FAILED(control->QueryInterface(IID_PPV_ARGS(identity.put())))) continue;
            DWORD pid = 0; identity->GetProcessId(&pid);
            AudioSessionState state = AudioSessionStateExpired; control->GetState(&state);
            LPWSTR rawName = nullptr; control->GetDisplayName(&rawName);
            const auto app = tvm::ResolveAppIdentity(pid, rawName ? rawName : L""); CoTaskMemFree(rawName);
            std::cout << "  pid=" << pid << " state=" << static_cast<int>(state)
                      << " name=" << Utf8(app.name) << " packaged=" << !app.appId.empty() << '\n';
        }
    }
    return 0;
}
}
int main() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 1;
    const auto result = Run(); CoUninitialize(); return result;
}
