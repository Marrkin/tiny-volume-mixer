#pragma once
#include <windows.h>
#include <string>

namespace tvm {
struct AppIdentity {
    std::wstring executable;
    std::wstring name;
    std::wstring appId;
};
AppIdentity ResolveAppIdentity(DWORD processId, const std::wstring& sessionName = {});
std::wstring FriendlyName(std::wstring name);
} // namespace tvm
