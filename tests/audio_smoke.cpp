// A real-device integration test. Only its own silent, temporary audio sessions
// are changed in the default mode. --linked-volume also changes output levels
// and restores the saved output/mute/session levels before returning.
#include "audio_engine.hpp"
#include "com_ptr.hpp"
#include <audioclient.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <algorithm>
#include <mmdeviceapi.h>
#include <cmath>
#include <functional>
#include <iostream>
#include <vector>

namespace {
constexpr GUID kTestSession1 = {0x50c348b4,0x8693,0x4a7d,{0xaf,0x6d,0x99,0x06,0x54,0x8f,0x41,0x5d}};
constexpr GUID kTestSession2 = {0x01f1bc2a,0x2b91,0x4c0a,{0x90,0x52,0x2c,0xd7,0x73,0x93,0xa1,0x71}};

bool Await(tvm::AudioEngine& engine, const std::function<bool(const tvm::AudioSnapshot&)>& predicate,
           DWORD timeout = 8000) {
    const auto deadline = GetTickCount64() + timeout;
    for (;;) {
        if (predicate(engine.Snapshot())) return true;
        const auto now = GetTickCount64();
        if (now >= deadline) return false;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, static_cast<DWORD>(deadline - now), QS_ALLINPUT);
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message); DispatchMessageW(&message);
        }
    }
}

struct Stream {
    tvm::ComPtr<IAudioClient> client;
    tvm::ComPtr<IAudioRenderClient> render;
    tvm::ComPtr<ISimpleAudioVolume> volume;
    ~Stream() { if (client) client->Stop(); }
    bool Open(IMMDevice* device, const GUID& session) {
        if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                    reinterpret_cast<void**>(client.put())))) return false;
        WAVEFORMATEX* format = nullptr;
        if (FAILED(client->GetMixFormat(&format))) return false;
        const auto hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1000000, 0, format, &session);
        CoTaskMemFree(format);
        if (FAILED(hr) || FAILED(client->GetService(IID_PPV_ARGS(render.put()))) ||
            FAILED(client->GetService(IID_PPV_ARGS(volume.put())))) return false;
        UINT32 frames = 0;
        BYTE* buffer = nullptr;
        if (FAILED(client->GetBufferSize(&frames)) || FAILED(render->GetBuffer(frames, &buffer))) return false;
        if (FAILED(render->ReleaseBuffer(frames, AUDCLNT_BUFFERFLAGS_SILENT))) return false;
        return SUCCEEDED(client->Start());
    }
};
}

namespace {
struct Producer {
    HANDLE input = nullptr;
    PROCESS_INFORMATION process{};
    ~Producer() {
        if (input) CloseHandle(input);
        if (process.hProcess) {
            WaitForSingleObject(process.hProcess, 10000);
            CloseHandle(process.hProcess);
            CloseHandle(process.hThread);
        }
    }
    bool Start(bool alternate = false) {
        SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
        HANDLE childInput = nullptr;
        if (!CreatePipe(&childInput, &input, &security, 0)) return false;
        SetHandleInformation(input, HANDLE_FLAG_INHERIT, 0);
        std::wstring path(32768, L'\0');
        path.resize(GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size())));
        auto command = L"\"" + path + (alternate ? L"\" --producer-aux" : L"\" --producer");
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = childInput;
        startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        const auto created = CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, TRUE,
                                            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
        CloseHandle(childInput);
        return created != FALSE;
    }
    void Send(char command) {
        DWORD written = 0;
        WriteFile(input, &command, 1, &written, nullptr);
    }
};

int Produce(IMMDevice* device) {
    Stream first, second;
    if (!first.Open(device, kTestSession1) || !second.Open(device, kTestSession2)) return 1;
    char command = 0;
    DWORD read = 0;
    while (ReadFile(GetStdHandle(STD_INPUT_HANDLE), &command, 1, &read, nullptr) && read == 1) {
        switch (command) {
        case 'v':
            first.volume->SetMasterVolume(0.23f, nullptr);
            second.volume->SetMasterVolume(0.67f, nullptr);
            break;
        case '1':
            first.client->Stop();
            first.render.reset(); first.volume.reset(); first.client.reset();
            break;
        case '2':
            second.client->Stop();
            second.render.reset(); second.volume.reset(); second.client.reset();
            break;
        case 'r':
            first.client->Stop(); second.client->Stop();
            first.render.reset(); first.volume.reset(); first.client.reset();
            second.render.reset(); second.volume.reset(); second.client.reset();
            if (!first.Open(device,kTestSession1) || !second.Open(device,kTestSession2)) return 1;
            first.volume->SetMasterVolume(0.31f,nullptr);
            second.volume->SetMasterVolume(0.79f,nullptr);
            break;
        case 'q': return 0;
        }
    }
    return 0;
}

bool ConfirmVolumes(IMMDevice* device, DWORD pid, float expected) {
    tvm::ComPtr<IAudioSessionManager2> manager;
    tvm::ComPtr<IAudioSessionEnumerator> sessions;
    if (FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(manager.put()))) ||
        FAILED(manager->GetSessionEnumerator(sessions.put()))) return false;
    int count = 0, matched = 0;
    sessions->GetCount(&count);
    for (int i = 0; i < count; ++i) {
        tvm::ComPtr<IAudioSessionControl> control;
        tvm::ComPtr<IAudioSessionControl2> identity;
        tvm::ComPtr<ISimpleAudioVolume> volume;
        if (FAILED(sessions->GetSession(i, control.put())) ||
            FAILED(control->QueryInterface(IID_PPV_ARGS(identity.put())))) continue;
        DWORD process = 0; identity->GetProcessId(&process);
        if (process != pid) continue;
        if (FAILED(control->QueryInterface(IID_PPV_ARGS(volume.put())))) return false;
        float scalar = 0;
        if (FAILED(volume->GetMasterVolume(&scalar)) || std::abs(scalar - expected) >= 0.01f) return false;
        ++matched;
    }
    return matched == 2;
}

tvm::ComPtr<IMMDevice> AlternateDevice(IMMDeviceEnumerator* enumerator, IMMDevice* primary) {
    LPWSTR primaryId = nullptr; primary->GetId(&primaryId);
    const std::wstring id = primaryId ? primaryId : L""; CoTaskMemFree(primaryId);
    tvm::ComPtr<IMMDeviceCollection> devices;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, devices.put()))) return {};
    UINT count = 0; devices->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        tvm::ComPtr<IMMDevice> device;
        if (FAILED(devices->Item(i,device.put()))) continue;
        LPWSTR currentId = nullptr; device->GetId(&currentId);
        const bool match = currentId && id == currentId; CoTaskMemFree(currentId);
        if (!match) return device;
    }
    return {};
}

// The linked-volume mode deliberately exercises output writes. Restore every
// pre-existing session level as well as output volume/mute, including on failure.
struct SavedOutput {
    tvm::ComPtr<IAudioEndpointVolume> endpoint;
    std::vector<std::pair<tvm::ComPtr<ISimpleAudioVolume>, float>> sessions;
    float level = 0.0f;
    BOOL muted = FALSE;
    bool saved = false;
    bool Capture(IMMDevice* device) {
        tvm::ComPtr<IAudioSessionManager2> manager;
        tvm::ComPtr<IAudioSessionEnumerator> list;
        if (FAILED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                   reinterpret_cast<void**>(endpoint.put()))) ||
            FAILED(endpoint->GetMasterVolumeLevelScalar(&level)) || FAILED(endpoint->GetMute(&muted)) ||
            FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                   reinterpret_cast<void**>(manager.put()))) ||
            FAILED(manager->GetSessionEnumerator(list.put()))) return false;
        int count = 0;
        if (FAILED(list->GetCount(&count))) return false;
        for (int i = 0; i < count; ++i) {
            tvm::ComPtr<IAudioSessionControl> control;
            tvm::ComPtr<ISimpleAudioVolume> volume;
            float value = 0.0f;
            if (FAILED(list->GetSession(i, control.put())) ||
                FAILED(control->QueryInterface(IID_PPV_ARGS(volume.put()))) ||
                FAILED(volume->GetMasterVolume(&value))) return false;
            sessions.emplace_back(std::move(volume), value);
        }
        saved = true;
        return true;
    }
    bool Restore() {
        if (!saved) return true;
        float current = 0.0f;
        bool ok = SUCCEEDED(endpoint->GetMasterVolumeLevelScalar(&current));
        if (ok) ok = SUCCEEDED(endpoint->SetMasterVolumeLevelScalar(std::min(current, level), nullptr));
        for (const auto& [volume, value] : sessions) {
            if (FAILED(volume->SetMasterVolume(value, nullptr))) ok = false;
        }
        if (FAILED(endpoint->SetMute(muted, nullptr))) ok = false;
        if (FAILED(endpoint->SetMasterVolumeLevelScalar(level, nullptr))) ok = false;
        if (ok) saved = false;
        return ok;
    }
    ~SavedOutput() { if (!Restore()) std::cerr << "FAIL: restoring saved output/session levels\n"; }
};

int RunLinkedVolume(IMMDevice* device) {
    Producer producer;
    if (!producer.Start()) return 1;
    const auto window = CreateWindowExW(0, L"STATIC", L"TVM linked volume test", 0,
                                       0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    tvm::AudioEngine engine;
    int result = 0;
    const auto check = [&](bool passed, const char* label) {
        std::cout << (passed ? "PASS: " : "FAIL: ") << label << std::endl;
        if (!passed) result = 1;
        return passed;
    };
    std::wstring executable(32768, L'\0');
    executable.resize(GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size())));
    tvm::SessionInfo identity; identity.executable = executable;
    const auto id = tvm::GroupId(identity);
    const auto find = [&](const tvm::AudioSnapshot& s) -> const tvm::AppVolume* {
        for (const auto& app : s.apps) if (app.id == id) return &app;
        return nullptr;
    };
    if (!check(engine.Start(window) && Await(engine, [&](const auto& s) {
        const auto* app = find(s); return s.deviceAvailable && app && app->sessionCount == 2;
    }), "linked-volume test discovers its silent streams")) {
        engine.Stop(); DestroyWindow(window); return 1;
    }
    SavedOutput saved;
    if (!check(saved.Capture(device), "original output and session levels are saved")) {
        engine.Stop(); DestroyWindow(window); return 1;
    }
    producer.Send('v');
    const auto set = [&](const std::wstring& target, float level, int expectedMaster, int expectedApp) {
        const auto request = engine.SetVolume(target, level);
        return Await(engine, [&](const auto& s) {
            const auto* app = find(s);
            return s.lastAppliedCommand >= request && s.error.empty() && app &&
                tvm::VolumePercent(s.masterVolume) == expectedMaster &&
                tvm::VolumePercent(app->volume) == expectedApp;
        });
    };
    check(set(tvm::kMasterId, 0.5f, 50, 34), "master at 50 scales the 67-percent session to 34");
    check(set(id, 0.2f, 50, 20) && ConfirmVolumes(device, producer.process.dwProcessId, 0.4f),
          "app at 20 with master at 50 writes relative session level 40");
    const auto before = engine.Snapshot();
    check(set(id, 0.8f, 80, 80), "raising an app past the master raises the master to 80");
    const auto after = engine.Snapshot();
    bool held = true;
    for (const auto& old : before.apps) {
        if (old.id == id) continue;
        const auto it = std::find_if(after.apps.begin(), after.apps.end(), [&](const auto& app) { return app.id == old.id; });
        if (it != after.apps.end() && std::abs(it->volume - old.volume) > 0.005f) held = false;
    }
    check(held, "other apps retain their displayed levels when an app raises the master");
    check(set(tvm::kMasterId, 0.3f, 30, 30), "lowering the master pulls the app slider down");
    check(set(tvm::kMasterId, 0.0f, 0, 0), "master at zero brings app sliders to zero");
    check(set(tvm::kMasterId, 0.5f, 50, 50), "restoring the master from zero preserves the session ratio");
    check(SUCCEEDED(saved.endpoint->SetMasterVolumeLevelScalar(0.25f, nullptr)) &&
          Await(engine, [&](const auto& s) { const auto* app = find(s); return app &&
              tvm::VolumePercent(s.masterVolume) == 25 && tvm::VolumePercent(app->volume) == 25; }),
          "external output changes update app sliders through notifications");
    check(set(id, 0.0f, 25, 0) && ConfirmVolumes(device, producer.process.dwProcessId, 0.0f),
          "an app can be silenced without changing the master");
    check(set(tvm::kMasterId, 0.0f, 0, 0) && set(id, 0.15f, 15, 15),
          "raising an app from a zero master remains finite and raises its output");
    engine.Stop();
    check(saved.Restore(), "original output, mute, and session levels are restored");
    DestroyWindow(window);
    return result;
}

int Run(const std::string& mode) {
    tvm::ComPtr<IMMDeviceEnumerator> enumerator;
    tvm::ComPtr<IMMDevice> device;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(enumerator.put()))) ||
        FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, device.put()))) {
        std::cout << "SKIP: no default render device.\n";
        return 77;
    }
    if (mode == "--producer") return Produce(device.get());
    if (mode == "--linked-volume") return RunLinkedVolume(device.get());
    if (mode == "--producer-aux") {
        const auto alternate = AlternateDevice(enumerator.get(),device.get());
        if (!alternate) return 77;
        return Produce(alternate.get()) == 0 ? 0 : 77;
    }
    tvm::ComPtr<IAudioEndpointVolume> endpoint;
    float outputLevel = 0.0f;
    if (FAILED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                               reinterpret_cast<void**>(endpoint.put()))) ||
        FAILED(endpoint->GetMasterVolumeLevelScalar(&outputLevel)) || outputLevel <= 0.0f) {
        std::cout << "SKIP: a nonzero output is needed for session-only checks.\n";
        return 77;
    }
    const auto expectedPercent = [outputLevel](float relative) {
        return tvm::VolumePercent(tvm::AppSliderVolume(relative, outputLevel));
    };
    int result = 0;
    const auto window = CreateWindowExW(0, L"STATIC", L"TVM audio test", 0,
                                        0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    tvm::AudioEngine engine;
    const auto check = [&result](bool passed, const char* label) {
        std::cout << (passed ? "PASS: " : "FAIL: ") << label << std::endl;
        if (!passed) result = 1;
        return passed;
    };
    check(engine.Start(window), "MTA worker starts");
    check(Await(engine, [](const auto& s) { return s.deviceAvailable; }), "default output is discovered");
    Producer producer;
    if (check(producer.Start(), "separate process creates two silent test streams after enumeration")) {
        std::wstring executable(32768, L'\0');
        executable.resize(GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size())));
        tvm::SessionInfo identity; identity.executable = executable;
        const auto id = tvm::GroupId(identity);
        const auto find = [&id](const tvm::AudioSnapshot& snapshot) -> const tvm::AppVolume* {
            for (const auto& app : snapshot.apps) if (app.id == id) return &app;
            return nullptr;
        };
        if (check(Await(engine, [&](const auto& s) { const auto* app = find(s); return app && app->sessionCount == 2; }),
                  "creation callbacks discover and group both sessions")) {
            producer.Send('v');
            check(Await(engine, [&](const auto& s) {
                const auto* app = find(s); return app && tvm::VolumePercent(app->volume) == expectedPercent(0.67f);
            }), "external volume changes reach the UI snapshot");
            engine.SetVolume(id, outputLevel * 0.41f);
            check(Await(engine, [&](const auto& s) {
                const auto* app = find(s); return app && !app->mixed && tvm::VolumePercent(app->volume) == expectedPercent(0.41f);
            }), "one slider updates every session in the group");
            check(ConfirmVolumes(device.get(), producer.process.dwProcessId, 0.41f),
                  "Core Audio confirms the volume in both streams");
            engine.ToggleMute(id);
            check(Await(engine, [&](const auto& s) { const auto* app = find(s); return app && app->muted; }),
                  "group mute is applied");
            engine.SetVolume(id, outputLevel * 0.52f);
            check(Await(engine, [&](const auto& s) {
                const auto* app = find(s); return app && !app->muted && tvm::VolumePercent(app->volume) == expectedPercent(0.52f);
            }), "raising a slider restores sound");
            engine.Refresh();
            engine.SetVolume(id, outputLevel * 0.37f);
            check(Await(engine, [&](const auto& s) {
                const auto* app = find(s);
                return app && app->sessionCount == 2 && tvm::VolumePercent(app->volume) == expectedPercent(0.37f);
            }), "writes after rebinding reach both sessions without duplication");
            producer.Send('r');
            check(Await(engine, [&](const auto& s) {
                const auto* app = find(s); return app && app->sessionCount == 2 && tvm::VolumePercent(app->volume) == expectedPercent(0.79f);
            }), "releasing and reopening streams restores the existing application's controls");
            const auto request = engine.SetVolume(id,outputLevel * 0.65f);
            check(Await(engine, [&](const auto& s) {
                const auto* app = find(s);
                return s.lastAppliedCommand >= request && app && !app->mixed && tvm::VolumePercent(app->volume) == expectedPercent(0.65f);
            }), "volume acknowledgments reflect the latest command after stream reuse");
            const auto alternate = AlternateDevice(enumerator.get(),device.get());
            if (alternate) {
                Producer auxiliary;
                if (check(auxiliary.Start(true), "silent producer starts on a non-default output")) {
                    const bool ready = Await(engine,[&](const auto& s) {
                        const auto* app = find(s);
                        return (app && app->sessionCount == 4) || WaitForSingleObject(auxiliary.process.hProcess,0) == WAIT_OBJECT_0;
                    });
                    DWORD exitCode = STILL_ACTIVE; GetExitCodeProcess(auxiliary.process.hProcess,&exitCode);
                    if (exitCode == 77) std::cout << "SKIP: additional output does not accept shared streams." << std::endl;
                    else if (check(ready && exitCode == STILL_ACTIVE, "sessions on default and non-default outputs share one application row")) {
                        tvm::ComPtr<IAudioEndpointVolume> otherEndpoint;
                        float otherLevel = 0.0f;
                        const bool readable = SUCCEEDED(alternate->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL,
                            nullptr, reinterpret_cast<void**>(otherEndpoint.put()))) &&
                            SUCCEEDED(otherEndpoint->GetMasterVolumeLevelScalar(&otherLevel));
                        check(readable, "additional output volume is readable");
                        const float chosen = std::min(outputLevel, otherLevel) * 0.61f;
                        engine.SetVolume(id,chosen);
                        check(Await(engine,[&](const auto& s) {
                            const auto* app = find(s);
                            return app && app->sessionCount == 4 && !app->mixed && tvm::VolumePercent(app->volume) == tvm::VolumePercent(chosen);
                        }), "one control adjusts sessions across render outputs");
                        check(ConfirmVolumes(alternate.get(),auxiliary.process.dwProcessId,tvm::SessionVolumeForSlider(chosen,otherLevel)),
                              "Core Audio confirms writes on the non-default output");
                        auxiliary.Send('q');
                        check(Await(engine,[&](const auto& s) {
                            const auto* app = find(s); return app && app->sessionCount == 2;
                        }), "closing one producer preserves the remaining application's sessions");
                    }
                    auxiliary.Send('q');
                }
            } else std::cout << "SKIP: only one render output is available." << std::endl;
            producer.Send('1');
            producer.Send('2');
            check(Await(engine, [&](const auto& s) {
                const auto* app = find(s); return !app || !app->active;
            }), "stopped streams no longer appear active");
            producer.Send('q');
            check(Await(engine, [&](const auto& s) { return find(s) == nullptr; }),
                  "the application row disappears when the producer exits");
        }
        producer.Send('q');
    }
    engine.Stop();
    DestroyWindow(window);
    return result;
}
} // namespace

int main(int argc, char** argv) {
    const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) return 1;
    const auto result = Run(argc > 1 ? argv[1] : "");
    CoUninitialize();
    return result;
}
