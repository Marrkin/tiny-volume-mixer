#include "app_identity.hpp"
#include "com_ptr.hpp"

#include <appmodel.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#include <vector>

namespace tvm {
namespace {
std::wstring FileName(const std::wstring& path) { return path.substr(path.find_last_of(L"\\/") + 1); }

std::wstring Description(const std::wstring& path) {
    if (path.empty()) return {};
    DWORD unused = 0;
    const auto size = GetFileVersionInfoSizeW(path.c_str(), &unused);
    if (size > 0 && size < 4 * 1024 * 1024) {
        std::vector<BYTE> info(size);
        if (GetFileVersionInfoW(path.c_str(), 0, size, info.data())) {
            struct Translation { WORD language; WORD codepage; };
            Translation* translation = nullptr;
            UINT bytes = 0;
            if (VerQueryValueW(info.data(), L"\\VarFileInfo\\Translation",
                reinterpret_cast<void**>(&translation), &bytes) && bytes >= sizeof(Translation)) {
                wchar_t key[96]{};
                swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\FileDescription",
                           translation->language, translation->codepage);
                wchar_t* value = nullptr; UINT length = 0;
                if (VerQueryValueW(info.data(), key, reinterpret_cast<void**>(&value), &length) && length > 1) {
                    return value;
                }
            }
        }
    }
    auto name = FileName(path);
    const auto dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos) name.resize(dot);
    return name;
}

AppIdentity ProcessIdentity(DWORD pid) {
    AppIdentity result;
    const auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return result;
    std::wstring path(32768, L'\0'); DWORD length = static_cast<DWORD>(path.size());
    if (QueryFullProcessImageNameW(process, 0, path.data(), &length)) result.executable.assign(path.data(), length);
    UINT32 characters = 0;
    if (GetApplicationUserModelId(process, &characters, nullptr) == ERROR_INSUFFICIENT_BUFFER && characters > 1) {
        std::vector<wchar_t> appId(characters);
        if (GetApplicationUserModelId(process, &characters, appId.data()) == ERROR_SUCCESS) result.appId = appId.data();
    }
    CloseHandle(process);
    if (!result.appId.empty()) {
        ComPtr<IShellItem> item;
        if (SUCCEEDED(SHCreateItemInKnownFolder(FOLDERID_AppsFolder, 0, result.appId.c_str(), IID_PPV_ARGS(item.put())))) {
            PWSTR name = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &name)) && name) result.name = name;
            CoTaskMemFree(name);
        }
    }
    if (result.name.empty()) result.name = Description(result.executable);
    result.name = FriendlyName(result.name);
    return result;
}

DWORD ParentProcess(DWORD pid) {
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
    DWORD parent = 0;
    if (Process32FirstW(snapshot, &entry)) {
        do { if (entry.th32ProcessID == pid) { parent = entry.th32ParentProcessID; break; } }
        while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    // Ignore a parent PID that has already exited and been reused.
    const auto childHandle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    const auto parentHandle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parent);
    FILETIME childCreated{}, parentCreated{}, exit{}, kernel{}, user{};
    const bool valid = childHandle && parentHandle && parent != pid &&
        GetProcessTimes(childHandle, &childCreated, &exit, &kernel, &user) &&
        GetProcessTimes(parentHandle, &parentCreated, &exit, &kernel, &user) &&
        CompareFileTime(&parentCreated, &childCreated) <= 0;
    if (childHandle) CloseHandle(childHandle);
    if (parentHandle) CloseHandle(parentHandle);
    return valid ? parent : 0;
}
}

std::wstring FriendlyName(std::wstring name) {
    if (_wcsicmp(name.c_str(), L"WhatsApp.Root") == 0) return L"WhatsApp";
    return name;
}

AppIdentity ResolveAppIdentity(DWORD pid, const std::wstring& sessionName) {
    auto result = ProcessIdentity(pid);
    if (_wcsicmp(FileName(result.executable).c_str(), L"msedgewebview2.exe") == 0) {
        // WebView2 is an embedded runtime. Attribute audio to the containing app
        // instead of merging unrelated embedded apps under the runtime's path.
        auto current = pid;
        for (int depth = 0; depth < 12; ++depth) {
            current = ParentProcess(current);
            if (!current) break;
            auto owner = ProcessIdentity(current);
            if (owner.executable.empty()) break;
            if (_wcsicmp(FileName(owner.executable).c_str(), L"msedgewebview2.exe") != 0) {
                result = std::move(owner); break;
            }
        }
        if (_wcsicmp(FileName(result.executable).c_str(), L"msedgewebview2.exe") == 0) {
            result.name = L"Aplicativo WebView2";
        }
    }
    if (result.name.empty() && !sessionName.empty() && sessionName.front() != L'@') result.name = FriendlyName(sessionName);
    if (result.name.empty()) result.name = L"Aplicativo (" + std::to_wstring(pid) + L")";
    return result;
}
} // namespace tvm
