#include "model.hpp"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <map>

namespace tvm {
namespace {
std::wstring Fold(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towlower(c));
    });
    return value;
}
}

std::wstring GroupId(const SessionInfo& session) {
    if (session.systemSounds) return L"@system";
    if (!session.appId.empty()) return L"app:" + Fold(session.appId);
    if (!session.executable.empty()) return L"exe:" + Fold(session.executable);
    // A missing process path must not merge unrelated or protected applications.
    if (session.processId != 0) return L"pid:" + std::to_wstring(session.processId);
    return L"session:" + session.instanceId;
}

std::vector<AppVolume> GroupSessions(const std::vector<SessionInfo>& sessions) {
    std::map<std::wstring, AppVolume> groups;
    for (const auto& session : sessions) {
        const auto level = AppSliderVolume(session.volume, session.outputVolume);
        const auto key = GroupId(session);
        const auto [it, inserted] = groups.try_emplace(key);
        auto& app = it->second;
        if (inserted) {
            app.id = key;
            app.name = session.name;
            app.executable = session.executable;
            app.appId = session.appId;
            app.volume = level;
            app.muted = session.muted;
        } else {
            app.mixed = app.mixed || std::abs(app.volume - level) > 0.005f ||
                        app.muted != session.muted;
            app.volume = std::max(app.volume, level);
            app.muted = app.muted && session.muted;
        }
        app.active = app.active || session.active;
        ++app.sessionCount;
    }
    std::vector<AppVolume> result;
    result.reserve(groups.size());
    for (auto& [key, app] : groups) result.push_back(std::move(app));
    std::sort(result.begin(), result.end(), [](const AppVolume& a, const AppVolume& b) {
        if (a.id == L"@system" || b.id == L"@system") return a.id == L"@system" && b.id != L"@system";
        const auto aName = Fold(a.name), bName = Fold(b.name);
        return aName == bName ? a.id < b.id : aName < bName;
    });
    return result;
}

float AppSliderVolume(float sessionVolume, float outputVolume) {
    if (!std::isfinite(sessionVolume) || !std::isfinite(outputVolume)) return 0.0f;
    return std::clamp(sessionVolume, 0.0f, 1.0f) * std::clamp(outputVolume, 0.0f, 1.0f);
}

float SessionVolumeForSlider(float sliderVolume, float outputVolume) {
    if (!std::isfinite(sliderVolume) || !std::isfinite(outputVolume) || outputVolume <= 0.0f) return 0.0f;
    return std::clamp(sliderVolume / std::min(outputVolume, 1.0f), 0.0f, 1.0f);
}

int VolumePercent(float scalar) {
    if (!std::isfinite(scalar)) return 0;
    return static_cast<int>(std::lround(std::clamp(scalar, 0.0f, 1.0f) * 100.0f));
}
} // namespace tvm
