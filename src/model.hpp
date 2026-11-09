#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tvm {
inline constexpr wchar_t kMasterId[] = L"@master";

struct SessionInfo {
    std::wstring instanceId;
    std::wstring executable;
    std::wstring name;
    std::uint32_t processId = 0;
    float volume = 0.0f;
    bool muted = false;
    bool active = false;
    bool systemSounds = false;
    std::wstring appId{};
};

struct AppVolume {
    std::wstring id;
    std::wstring name;
    std::wstring executable;
    float volume = 0.0f;
    bool muted = false;
    bool active = false;
    bool mixed = false;
    std::size_t sessionCount = 0;
    std::wstring appId{};
};

struct AudioSnapshot {
    bool deviceAvailable = false;
    std::wstring deviceName;
    float masterVolume = 0.0f;
    bool masterMuted = false;
    std::vector<AppVolume> apps;
    std::wstring error;
    std::uint64_t lastAppliedCommand = 0;
};

std::wstring GroupId(const SessionInfo& session);
std::vector<AppVolume> GroupSessions(const std::vector<SessionInfo>& sessions);
int VolumePercent(float scalar);
} // namespace tvm
