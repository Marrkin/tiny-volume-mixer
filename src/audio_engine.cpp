#include "audio_engine.hpp"
#include "com_ptr.hpp"
#include "app_identity.hpp"

#include <audiopolicy.h>
#include <endpointvolume.h>
#include <propkeydef.h> // DEFINE_PROPERTYKEY must precede the device property keys in the Windows SDK.
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <propvarutil.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <map>
#include <new>
#include <thread>
#include <vector>

namespace tvm {
namespace {
constexpr UINT kWork = WM_APP + 20;
constexpr GUID kContext = {0xa46df083, 0xb292, 0x4fe1, {0x9a, 0xe8, 0x75, 0x44, 0x28, 0x5f, 0x42, 0x14}};
enum class WorkKind { NewSession, Values, Metadata, Remove, Device, Volume, Mute, Refresh, ProcessExit, Stop };

struct Work {
    explicit Work(WorkKind kindIn) : kind(kindIn) {}
    WorkKind kind;
    std::uint64_t generation = 0;
    std::uint64_t token = 0;
    ComPtr<IAudioSessionControl> control;
    std::wstring id;
    float volume = 0.0f;
    std::uint64_t request = 0;
};

// Callbacks only retain a reference and post a message. They never enter the UI,
// acquire our locks, unregister a sink, or perform audio queries.
struct Dispatcher {
    std::atomic<DWORD> threadId{0};
    std::atomic<unsigned> posters{0};

    bool Post(std::unique_ptr<Work> work) {
        if (!work) return false;
        posters.fetch_add(1);
        const auto id = threadId.load();
        const bool posted = id != 0 && PostThreadMessageW(id, kWork, 0,
                                            reinterpret_cast<LPARAM>(work.get())) != FALSE;
        if (posted) work.release();
        posters.fetch_sub(1);
        return posted;
    }
    void Notify(WorkKind kind, std::uint64_t generation, std::uint64_t token = 0) {
        auto work = std::unique_ptr<Work>(new (std::nothrow) Work(kind));
        if (!work) return;
        work->generation = generation;
        work->token = token;
        Post(std::move(work));
    }
    void Close() {
        threadId.store(0);
        // Only the owning worker waits, during shutdown. A callback never waits.
        while (posters.load() != 0) SwitchToThread();
    }
};

template<class Interface> class Callback : public Interface {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** value) override {
        if (!value) return E_POINTER;
        *value = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(Interface)) {
            *value = static_cast<Interface*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto remaining = --references_;
        if (remaining == 0) delete this;
        return remaining;
    }
protected:
    virtual ~Callback() = default;
private:
    std::atomic<ULONG> references_{1};
};

class DeviceSink final : public Callback<IMMNotificationClient> {
public:
    explicit DeviceSink(std::shared_ptr<Dispatcher> dispatch) : dispatch_(std::move(dispatch)) {}
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eRender && role == eMultimedia) Changed();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { Changed(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { Changed(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { Changed(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override {
        Changed(); return S_OK;
    }
private:
    void Changed() { dispatch_->Notify(WorkKind::Device, 0); }
    std::shared_ptr<Dispatcher> dispatch_;
};

class CreatedSink final : public Callback<IAudioSessionNotification> {
public:
    CreatedSink(std::shared_ptr<Dispatcher> dispatch, std::uint64_t generation, std::uint64_t device)
        : dispatch_(std::move(dispatch)), generation_(generation), device_(device) {}
    HRESULT STDMETHODCALLTYPE OnSessionCreated(IAudioSessionControl* session) override {
        auto work = std::unique_ptr<Work>(new (std::nothrow) Work(WorkKind::NewSession));
        if (!work) return S_OK;
        work->generation = generation_; work->token = device_;
        work->control = ComPtr<IAudioSessionControl>(session);
        dispatch_->Post(std::move(work));
        return S_OK;
    }
private:
    std::shared_ptr<Dispatcher> dispatch_;
    std::uint64_t generation_;
    std::uint64_t device_;
};

class SessionSink final : public Callback<IAudioSessionEvents> {
public:
    SessionSink(std::shared_ptr<Dispatcher> dispatch, std::uint64_t generation, std::uint64_t token)
        : dispatch_(std::move(dispatch)), generation_(generation), token_(token) {}
    HRESULT STDMETHODCALLTYPE OnDisplayNameChanged(LPCWSTR, LPCGUID) override {
        Notify(WorkKind::Metadata); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnIconPathChanged(LPCWSTR, LPCGUID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnSimpleVolumeChanged(float, BOOL, LPCGUID) override {
        Notify(WorkKind::Values); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnChannelVolumeChanged(DWORD, float[], DWORD, LPCGUID) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnGroupingParamChanged(LPCGUID, LPCGUID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnStateChanged(AudioSessionState) override {
        // Expired sessions can reactivate. Keep the sink until disconnection or
        // process exit so browsers can resume without a new creation callback.
        Notify(WorkKind::Values);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnSessionDisconnected(AudioSessionDisconnectReason) override {
        Notify(WorkKind::Remove); return S_OK;
    }
private:
    void Notify(WorkKind kind) { dispatch_->Notify(kind, generation_, token_); }
    std::shared_ptr<Dispatcher> dispatch_;
    std::uint64_t generation_;
    std::uint64_t token_;
};

class EndpointSink final : public Callback<IAudioEndpointVolumeCallback> {
public:
    EndpointSink(std::shared_ptr<Dispatcher> dispatch, std::uint64_t generation)
        : dispatch_(std::move(dispatch)), generation_(generation) {}
    HRESULT STDMETHODCALLTYPE OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA) override {
        dispatch_->Notify(WorkKind::Values, generation_); return S_OK;
    }
private:
    std::shared_ptr<Dispatcher> dispatch_;
    std::uint64_t generation_;
};

std::wstring ErrorText(const wchar_t* operation, HRESULT result) {
    wchar_t code[16]{};
    swprintf_s(code, L"0x%08X", static_cast<unsigned>(result));
    return std::wstring(operation) + L" (" + code + L").";
}

struct ProcessWatch {
    HANDLE process = nullptr, wait = nullptr;
    std::shared_ptr<Dispatcher> dispatch;
    std::uint64_t generation = 0;
    DWORD pid = 0;
    ~ProcessWatch() {
        if (wait) UnregisterWaitEx(wait, INVALID_HANDLE_VALUE);
        if (process) CloseHandle(process);
    }
    static void CALLBACK Exited(void* context, BOOLEAN) {
        const auto* watch = static_cast<ProcessWatch*>(context);
        watch->dispatch->Notify(WorkKind::ProcessExit, watch->generation, watch->pid);
    }
};
} // namespace

struct AudioEngine::Impl {
    struct Session {
        ComPtr<IAudioSessionControl2> control;
        ComPtr<ISimpleAudioVolume> volume;
        ComPtr<IAudioSessionEvents> sink;
        SessionInfo info;
        std::uint64_t token = 0;
    };

    HWND listener = nullptr;
    std::thread worker;
    std::shared_ptr<Dispatcher> dispatch = std::make_shared<Dispatcher>();
    std::mutex snapshotMutex;
    AudioSnapshot snapshot;
    std::atomic<bool> pending{false};
    std::atomic<std::uint64_t> nextRequest{0};
    std::uint64_t lastAppliedCommand = 0;

    // Everything below is confined to the audio worker.
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMNotificationClient> deviceSink;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioEndpointVolume> endpoint;
    ComPtr<IAudioEndpointVolumeCallback> endpointSink;
    struct Output {
        ComPtr<IMMDevice> device;
        ComPtr<IAudioSessionManager2> manager;
        ComPtr<IAudioSessionNotification> sink;
        std::uint64_t token = 0;
    };
    std::vector<Output> outputs;
    std::vector<Session> sessions;
    std::map<DWORD, std::unique_ptr<ProcessWatch>> watches;
    std::uint64_t generation = 0;
    std::uint64_t nextToken = 0;
    std::wstring deviceName;
    std::wstring error;

    void Publish() {
        AudioSnapshot current;
        current.deviceAvailable = static_cast<bool>(endpoint);
        current.deviceName = deviceName;
        current.error = error;
        current.lastAppliedCommand = lastAppliedCommand;
        if (endpoint) {
            BOOL muted = FALSE;
            auto hr = endpoint->GetMasterVolumeLevelScalar(&current.masterVolume);
            if (SUCCEEDED(hr)) hr = endpoint->GetMute(&muted);
            if (FAILED(hr)) {
                current.deviceAvailable = false;
                current.error = ErrorText(L"Não foi possível ler o volume da saída", hr);
            }
            current.masterMuted = muted != FALSE;
        }
        std::vector<SessionInfo> infos;
        for (auto& session : sessions) {
            AudioSessionState state = AudioSessionStateExpired;
            BOOL muted = FALSE;
            if (FAILED(session.control->GetState(&state)) || state == AudioSessionStateExpired) continue;
            if (FAILED(session.volume->GetMasterVolume(&session.info.volume)) ||
                FAILED(session.volume->GetMute(&muted))) continue;
            session.info.muted = muted != FALSE;
            session.info.active = state == AudioSessionStateActive;
            infos.push_back(session.info);
        }
        current.apps = GroupSessions(infos);
        {
            const std::lock_guard lock(snapshotMutex);
            snapshot = std::move(current);
        }
        if (!pending.exchange(true) && !PostMessageW(listener, kAudioChanged, 0, 0)) pending.store(false);
    }

    void ReadMetadata(Session& session) {
        if (session.info.systemSounds) { session.info.name = L"Sons do sistema"; return; }
        LPWSTR displayName = nullptr;
        session.control->GetDisplayName(&displayName);
        const auto identity = ResolveAppIdentity(session.info.processId, displayName ? displayName : L"");
        CoTaskMemFree(displayName);
        session.info.executable = identity.executable;
        session.info.name = identity.name;
        session.info.appId = identity.appId;
    }

    void WatchProcess(DWORD pid) {
        if (pid == 0 || watches.contains(pid)) return;
        auto watch = std::make_unique<ProcessWatch>();
        watch->process = OpenProcess(SYNCHRONIZE, FALSE, pid);
        if (!watch->process) return; // Protected processes still retain their audio event sink.
        watch->dispatch = dispatch; watch->generation = generation; watch->pid = pid;
        if (!RegisterWaitForSingleObject(&watch->wait, watch->process, ProcessWatch::Exited,
                                         watch.get(), INFINITE, WT_EXECUTEONLYONCE)) return;
        watches.emplace(pid, std::move(watch));
    }

    void RemoveProcess(DWORD pid) {
        for (auto it = sessions.begin(); it != sessions.end();) {
            if (it->info.processId == pid) {
                it->control->UnregisterAudioSessionNotification(it->sink.get());
                it = sessions.erase(it);
            } else ++it;
        }
        watches.erase(pid);
    }

    void AddSession(IAudioSessionControl* control, std::uint64_t deviceToken) {
        if (!control) return;
        Session session;
        if (FAILED(control->QueryInterface(IID_PPV_ARGS(session.control.put()))) ||
            FAILED(control->QueryInterface(IID_PPV_ARGS(session.volume.put())))) return;
        AudioSessionState state = AudioSessionStateExpired;
        if (FAILED(session.control->GetState(&state))) return;
        LPWSTR id = nullptr;
        if (FAILED(session.control->GetSessionInstanceIdentifier(&id)) || !id) return;
        session.info.instanceId = std::to_wstring(deviceToken) + L":" + id;
        CoTaskMemFree(id);
        for (const auto& existing : sessions) {
            if (existing.info.instanceId == session.info.instanceId) return;
        }
        DWORD pid = 0;
        session.control->GetProcessId(&pid);
        session.info.processId = pid;
        session.info.systemSounds = session.control->IsSystemSoundsSession() == S_OK;
        ReadMetadata(session);
        session.token = ++nextToken;
        session.sink.attach(new SessionSink(dispatch, generation, session.token));
        const auto hr = session.control->RegisterAudioSessionNotification(session.sink.get());
        if (FAILED(hr)) {
            error = ErrorText(L"Não foi possível acompanhar uma sessão de áudio", hr);
            return;
        }
        sessions.push_back(std::move(session));
        WatchProcess(pid);
    }

    void RemoveSession(std::uint64_t token) {
        const auto it = std::find_if(sessions.begin(), sessions.end(), [token](const Session& session) {
            return session.token == token;
        });
        if (it != sessions.end()) {
            it->control->UnregisterAudioSessionNotification(it->sink.get());
            const auto pid = it->info.processId;
            sessions.erase(it);
            if (std::none_of(sessions.begin(), sessions.end(), [pid](const Session& s) { return s.info.processId == pid; })) {
                watches.erase(pid);
            }
        }
    }

    void ClearDevice() {
        ++generation; // Drop queued events belonging to the previous endpoint.
        for (auto& output : outputs) {
            if (output.manager && output.sink) output.manager->UnregisterSessionNotification(output.sink.get());
        }
        for (auto& session : sessions) {
            session.control->UnregisterAudioSessionNotification(session.sink.get());
        }
        sessions.clear(); watches.clear(); outputs.clear();
        if (endpoint && endpointSink) endpoint->UnregisterControlChangeNotify(endpointSink.get());
        endpointSink.reset();
        endpoint.reset();
        device.reset();
        deviceName.clear();
    }

    void BindDevice() {
        ClearDevice();
        error.clear();
        if (!enumerator) {
            auto hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      IID_PPV_ARGS(enumerator.put()));
            if (FAILED(hr)) {
                error = ErrorText(L"O serviço de áudio não está disponível", hr);
                Publish(); return;
            }
            deviceSink.attach(new DeviceSink(dispatch));
            hr = enumerator->RegisterEndpointNotificationCallback(deviceSink.get());
            if (FAILED(hr)) {
                deviceSink.reset(); enumerator.reset();
                error = ErrorText(L"Não foi possível acompanhar os dispositivos de áudio", hr);
                Publish(); return;
            }
        }
        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, device.put()))) {
            ComPtr<IPropertyStore> properties;
            if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, properties.put()))) {
                PROPVARIANT name; PropVariantInit(&name);
                if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &name)) &&
                    name.vt == VT_LPWSTR && name.pwszVal) deviceName = name.pwszVal;
                PropVariantClear(&name);
            }
            auto hr = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                       reinterpret_cast<void**>(endpoint.put()));
            if (SUCCEEDED(hr)) {
                endpointSink.attach(new EndpointSink(dispatch, generation));
                hr = endpoint->RegisterControlChangeNotify(endpointSink.get());
            }
            if (FAILED(hr)) error = ErrorText(L"Não foi possível acompanhar o volume geral", hr);
        }
        // Apps can be explicitly routed to a non-default render endpoint. Observe
        // every active output; only the master slider refers to the default one.
        ComPtr<IMMDeviceCollection> devices;
        auto hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, devices.put());
        if (FAILED(hr)) {
            error = ErrorText(L"Não foi possível listar as saídas de áudio", hr); Publish(); return;
        }
        UINT deviceCount = 0; devices->GetCount(&deviceCount);
        for (UINT index = 0; index < deviceCount; ++index) {
            Output output; output.token = ++nextToken;
            if (FAILED(devices->Item(index, output.device.put()))) continue;
            hr = output.device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                         reinterpret_cast<void**>(output.manager.put()));
            if (FAILED(hr)) { error = ErrorText(L"Não foi possível acessar uma saída de áudio", hr); continue; }
            output.sink.attach(new CreatedSink(dispatch, generation, output.token));
            hr = output.manager->RegisterSessionNotification(output.sink.get());
            if (FAILED(hr)) { error = ErrorText(L"Não foi possível acompanhar novos aplicativos", hr); continue; }
            outputs.push_back(std::move(output));
            auto& bound = outputs.back();
            ComPtr<IAudioSessionEnumerator> list;
            hr = bound.manager->GetSessionEnumerator(list.put());
            int count = 0;
            if (SUCCEEDED(hr)) hr = list->GetCount(&count); // Enables creation notifications.
            if (FAILED(hr)) { error = ErrorText(L"Não foi possível listar as sessões de áudio", hr); continue; }
            for (int sessionIndex = 0; sessionIndex < count; ++sessionIndex) {
                ComPtr<IAudioSessionControl> control;
                if (SUCCEEDED(list->GetSession(sessionIndex, control.put()))) AddSession(control.get(), bound.token);
            }
        }
        Publish();
    }

    void Apply(const Work& work) {
        HRESULT result = S_OK;
        if (work.id == kMasterId && endpoint) {
            if (work.kind == WorkKind::Volume) {
                result = endpoint->SetMasterVolumeLevelScalar(work.volume, &kContext);
                if (SUCCEEDED(result) && work.volume > 0.0f) result = endpoint->SetMute(FALSE, &kContext);
            } else {
                BOOL muted = FALSE;
                result = endpoint->GetMute(&muted);
                if (SUCCEEDED(result)) result = endpoint->SetMute(!muted, &kContext);
            }
        } else {
            std::vector<Session*> targets;
            bool allMuted = true;
            for (auto& session : sessions) {
                if (GroupId(session.info) != work.id) continue;
                AudioSessionState state = AudioSessionStateExpired;
                if (FAILED(session.control->GetState(&state)) || state == AudioSessionStateExpired) continue;
                BOOL muted = FALSE;
                const auto hr = session.volume->GetMute(&muted);
                if (FAILED(hr)) { result = hr; continue; }
                allMuted = allMuted && muted != FALSE;
                targets.push_back(&session);
            }
            if (targets.empty()) result = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
            for (auto* session : targets) {
                auto hr = work.kind == WorkKind::Volume
                    ? session->volume->SetMasterVolume(work.volume, &kContext)
                    : session->volume->SetMute(!allMuted, &kContext);
                if (SUCCEEDED(hr) && work.kind == WorkKind::Volume && work.volume > 0.0f) {
                    hr = session->volume->SetMute(FALSE, &kContext);
                }
                if (FAILED(hr)) result = hr;
            }
        }
        error = FAILED(result) ? ErrorText(L"Não foi possível alterar o volume", result) : L"";
        lastAppliedCommand = std::max(lastAppliedCommand, work.request);
        Publish();
    }

    void Run(HANDLE ready) {
        MSG message{};
        PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
        dispatch->threadId.store(GetCurrentThreadId());
        const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        SetEvent(ready);
        if (FAILED(initialized)) {
            error = ErrorText(L"Não foi possível iniciar o áudio", initialized);
            Publish();
            dispatch->Close();
            return;
        }
        try {
            BindDevice();
            bool stopping = false;
            while (!stopping && GetMessageW(&message, nullptr, 0, 0) > 0) {
                if (message.message != kWork) continue;
                const auto work = std::unique_ptr<Work>(reinterpret_cast<Work*>(message.lParam));
                if (!work) continue;
                switch (work->kind) {
                case WorkKind::Stop: stopping = true; break;
                case WorkKind::Device: BindDevice(); break;
                case WorkKind::Refresh:
                    // A manual retry also replaces an enumerator left by a service restart.
                    if (enumerator && deviceSink) {
                        enumerator->UnregisterEndpointNotificationCallback(deviceSink.get());
                    }
                    deviceSink.reset(); enumerator.reset();
                    BindDevice(); break;
                case WorkKind::Volume:
                case WorkKind::Mute: Apply(*work); break;
                default:
                    if (work->generation != generation) break;
                    if (work->kind == WorkKind::NewSession) AddSession(work->control.get(), work->token);
                    if (work->kind == WorkKind::ProcessExit) RemoveProcess(static_cast<DWORD>(work->token));
                    if (work->kind == WorkKind::Remove) RemoveSession(work->token);
                    if (work->kind == WorkKind::Metadata) {
                        for (auto& session : sessions) {
                            if (session.token == work->token) ReadMetadata(session);
                        }
                    }
                    Publish();
                    break;
                }
            }
        } catch (...) {
            error = L"Ocorreu um erro ao acompanhar o áudio. Reinicie o TinyVolumeMixer.";
            Publish();
        }
        dispatch->Close();
        ClearDevice();
        if (enumerator && deviceSink) enumerator->UnregisterEndpointNotificationCallback(deviceSink.get());
        deviceSink.reset();
        enumerator.reset();
        // Release retained creation notifications on this MTA, before CoUninitialize.
        while (PeekMessageW(&message, nullptr, kWork, kWork, PM_REMOVE)) {
            delete reinterpret_cast<Work*>(message.lParam);
        }
        CoUninitialize();
    }
};

AudioEngine::AudioEngine() : impl_(std::make_unique<Impl>()) {}
AudioEngine::~AudioEngine() { Stop(); }

bool AudioEngine::Start(HWND listener) {
    if (impl_->worker.joinable()) return false;
    impl_->listener = listener;
    const auto ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ready) return false;
    try { impl_->worker = std::thread([this, ready] { impl_->Run(ready); }); }
    catch (...) { CloseHandle(ready); return false; }
    WaitForSingleObject(ready, INFINITE);
    CloseHandle(ready);
    return true;
}

void AudioEngine::Stop() {
    if (!impl_->worker.joinable()) return;
    impl_->dispatch->Notify(WorkKind::Stop, 0);
    impl_->worker.join();
}

AudioSnapshot AudioEngine::Snapshot() {
    const std::lock_guard lock(impl_->snapshotMutex);
    impl_->pending.store(false);
    return impl_->snapshot;
}

std::uint64_t AudioEngine::SetVolume(const std::wstring& id, float scalar) {
    if (!std::isfinite(scalar)) return 0;
    auto work = std::make_unique<Work>(WorkKind::Volume);
    work->id = id;
    work->volume = std::clamp(scalar, 0.0f, 1.0f);
    const auto request = work->request = ++impl_->nextRequest;
    return impl_->dispatch->Post(std::move(work)) ? request : 0;
}
void AudioEngine::ToggleMute(const std::wstring& id) {
    auto work = std::make_unique<Work>(WorkKind::Mute);
    work->id = id;
    impl_->dispatch->Post(std::move(work));
}
void AudioEngine::Refresh() { impl_->dispatch->Notify(WorkKind::Refresh, 0); }
} // namespace tvm
