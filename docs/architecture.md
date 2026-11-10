# Architecture

The executable contains a Win32 popup/tray UI, preferences stored in the current user's registry, and a Core Audio worker. It uses only the C++ standard library and Windows SDK.

## Audio ownership and notifications

The UI initializes COM as STA. `AudioEngine` starts a dedicated MTA thread, creates its message queue, and initializes `IMMDeviceEnumerator`. Audio COM interfaces are created, queried, and released on that worker. Only value snapshots cross into the UI.

The worker subscribes to:

- `IMMNotificationClient` for default output and device changes.
- `IAudioEndpointVolumeCallback` for master volume and mute.
- `IAudioSessionNotification` for newly created sessions.
- `IAudioSessionEvents` for per-session volume, metadata, state, and disconnection.

Callbacks retain a new session when needed and post commands without taking the engine's snapshot mutex or calling into the UI. They never unregister notifications. Commands run sequentially on the MTA. `GetCount` is called after registering the creation sink to enable notifications; subsequent discovery retains the callback's session pointer instead of relying on repeated enumeration. See Microsoft's [session enumeration guidance](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nn-audiopolicy-iaudiosessionenumerator) and [callback requirements](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nn-audiopolicy-iaudiosessionevents).

Endpoint changes unregister the old sinks, clear old sessions, increment a generation counter, and bind every active render endpoint. Volume callbacks monitor every active output; the master slider controls the default `eRender/eMultimedia` endpoint. Queued notifications from previous generations are discarded. An absent output is a normal empty state. Manual refresh also retries binding if the audio service was unavailable at launch.

During shutdown, the dispatcher stops accepting posts, waits for in-flight posters on the owning thread, unregisters notifications, and releases queued session references before `CoUninitialize`. Callback objects use atomic reference counts and hold their dispatcher independently of the engine lifetime.

Expired session controls remain subscribed because Windows can reactivate them without a fresh creation callback. They are omitted from the visible snapshot while expired. One registered wait per accessible source process removes its retained sessions on exit. Both waits and audio sinks post to the worker; no timer scans processes or sessions. Disconnected sessions are unregistered immediately.

## Grouping

Each session is keyed by its output token plus unique instance identifier, so initial enumeration and concurrent creation callbacks cannot add duplicates. Packaged apps are grouped by their Windows application identity; otherwise the process's full executable path identifies the group, case insensitively. Windows Shell supplies packaged display names and icons. WebView2 process ancestry is followed to its embedding app, with process creation times checked to avoid using a recycled parent PID. An explicit fallback normalizes `WhatsApp.Root` if Shell metadata is unavailable. Protected processes fall back to process IDs; anonymous sessions fall back to instance IDs. System sounds have a separate identity.

Session levels remain relative Core Audio values. Each visible app slider uses `session volume × output slider volume`, matching the classic Windows mixer. These are slider percentages, not measurements of acoustic loudness: the endpoint scalar uses an audio-tapered curve. The group presents the maximum output-adjusted session level, an all-muted flag, and an any-active flag. Sessions on different outputs use their own output levels.

Changing the master writes only the default endpoint. Its app sliders move proportionally without rewriting session ratios, including through zero. Endpoint callbacks also keep them aligned when Windows, media keys, or another mixer changes the output.

Moving an app slider converts the requested displayed level back to a relative session level on each affected output. If the request exceeds an output's current level, that output rises to the requested level. Other live apps on that output are first attenuated to preserve their displayed levels, matching SndVol's behavior. Their previous levels are restored if this preparation or the endpoint write fails. A successful output increase is followed by writes to the target sessions. App writes unmute their sessions above zero; mute buttons preserve levels. A partial API failure is surfaced in the footer, and the next snapshot reflects actual Core Audio values.

## UI and settings

The window uses GDI text, the built-in Windows GDI+ antialiaser for curves/glyphs, and Common Controls trackbars for input and keyboard behavior. Row text, sliders, and icon buttons paint through offscreen buffers. A volume update invalidates only the percentage region and changed slider; unchanged names and icons are not erased or reset. Rows are reused by application key when other apps arrive/leave. Native thumb hit regions are sized to cover the rendered circular thumb. No fonts or third-party graphics libraries are bundled. DPI scaling follows the tray monitor, and the popup is clamped to that monitor's work area. Long lists scroll inside the popup.

Volume commands carry increasing request IDs. Snapshots acknowledge applied commands, keeping stale callbacks from pulling a locally dragged thumb backward. Errors release the optimistic state and expose actual Core Audio values.

Light/dark palettes are selected by a persisted automatic/light/dark preference. Automatic mode reads the Windows app theme and updates on `WM_SETTINGCHANGE`, `WM_THEMECHANGED`, and `WM_SYSCOLORCHANGE`. An explicit theme stays fixed, while high-contrast system colors take priority. Tray glyph contrast follows the taskbar theme independently.

Volume notifications update existing controls. Hidden windows retain no painting activity. There is no audio sampling, timer, periodic session scan, telemetry, update checker, or network code.

`RegisterHotKey` reserves the proposed combination before releasing the previous one, so a collision leaves the current shortcut intact. Startup is opt-in using the current user's Run key. A named local mutex keeps a single instance per Windows session, and a registered window message brings the existing popup forward.
