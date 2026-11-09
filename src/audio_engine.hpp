#pragma once

#include "model.hpp"
#include <windows.h>
#include <memory>

namespace tvm {
inline constexpr UINT kAudioChanged = WM_APP + 1;

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    bool Start(HWND listener);
    void Stop();
    AudioSnapshot Snapshot();
    std::uint64_t SetVolume(const std::wstring& id, float scalar);
    void ToggleMute(const std::wstring& id);
    void Refresh();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace tvm
