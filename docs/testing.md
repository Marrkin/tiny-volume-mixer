# Validation

## Automated model and rendering checks

`ctest --preset release` runs deterministic checks for grouping multiple processes/sessions, executable path identity, protected-process fallbacks, system sounds, mixed volumes/mute, ordering, rounding, and invalid scalar values. They do not need a sound device. The same targets run in Windows CI. Rendering checks verify intermediate edge pixels from antialiasing, deterministic repeated frames, and readable text/thumb contrast in both themes. Grouping checks also cover Windows packaged application identities, output-adjusted levels across devices, linked slider conversions, zero-volume recovery, and preserving other app levels during an output increase.

## Core Audio integration

On a Windows desktop with an enabled default output:

```powershell
.\build\windows-x64\Release\tvm_audio_smoke.exe
```

This creates two independent **silent test streams**, after the engine has already enumerated sessions. It verifies creation notifications, grouping, external volume changes, writes to both sessions, mute/unmute, a manual rebind, stream release/recreation, and acknowledged volume commands. When another active render output is available, a second producer verifies discovery and real volume writes across default and non-default devices. It changes only its own temporary test sessions, never the master volume or another application's settings. Write requests stay below the corresponding output levels so this mode never raises an output. Exit code 77 means no default output or a zero-volume default output is available. It is built in CI but intentionally not run on hosted machines without audio devices.

The streams run in a child process to exercise the same separation as a real audio application. The test also verifies inactive state changes and removal when that process exits. Windows may retain inactive sessions while a process is still alive, so releasing a stream is not treated as a promise of immediate removal.

To exercise the linked sliders and real output writes explicitly:

```powershell
.\build\windows-x64\Release\tvm_audio_smoke.exe --linked-volume
```

This mode temporarily changes the default output and app levels. It saves and restores the original output volume/mute and all enumerated session levels, including when a check fails. It verifies proportional app levels, conversion back to relative session volume, an app raising its output while other apps hold their displayed levels, zero/recovery, external endpoint notifications, and raising an app from a zero output. Its streams remain silent. Run on a desktop when temporarily changing output levels is acceptable; it is not part of CI.

## Automated desktop checks

On an interactive Windows desktop, after building:

```powershell
.\scripts\test-desktop.ps1
# Optional: supply a different build and save captures of the app window.
.\scripts\test-desktop.ps1 -Executable .\build\cross\TinyVolumeMixer.exe -CaptureScreenshots
```

This exercises light/dark/automatic theme persistence and actual painted backgrounds, preservation of forced themes on Windows setting notifications, orientation, hotkey editing/collision feedback, opt-in startup and its quoted path, hide-on-close/focus-loss, and repeated open/orientation cycles. It launches a temporary copy, restores the app's previous preference key and named startup entry afterward, and leaves other startup entries alone. If the adjacent `tvm_audio_smoke.exe` is present, it also drags a real silent test session through 80 volume changes below the current output level, checks stable row/icon handles and the final value, and observes row removal on process exit. It also changes the silent sessions externally while the popup remains open and verifies from captured pixels that the painted thumb actually moves in both orientations; checking only `TBM_GETPOS` misses stale painted positions. No existing TinyVolumeMixer instance should be running. Native window-message checks cover UI handlers; the manual checklist below also covers physical input and taskbar behavior.

## Real Chrome discovery

```powershell
.\scripts\test-chrome.ps1
```

This launches Chrome in a disposable profile with a local Web Audio page at zero gain. It verifies a Chrome row when audio starts after the mixer and when an existing Chrome session is present at mixer startup. It does not alter any application volume or use the user's regular browser profile. Chrome is needed only for this optional desktop check.

`tests/audio_inventory.cpp` builds as `tvm_audio_inventory.exe`, a read-only diagnostic listing active outputs, session states, PIDs, resolved application names, and packaged identity availability. It is not installed with the portable app.

## Desktop checks before publishing

- Start normally and with `--background`; verify foreground/background behavior and a second launch opening the existing instance.
- Toggle the tray icon and Ctrl + Alt + M. Click outside, press Esc, and click X: the window hides and remains available in the tray. Exit from the tray menu ends the process.
- Play browser audio and music. Verify master/application changes against the classic Windows mixer in both directions, plus each mute button. With the master at 50, a session at 100 should display 50; a session at 40 should display 20. Raise an app past the master and verify the master follows while other apps keep their displayed levels. Test zero/recovery and media-key updates as well. Master tests affect the actual output: restore the previous value afterward.
- Open another app after launch. Pause, resume, and close streams. Verify new sessions arrive without a refresh and expired sessions disappear.
- Switch the default multimedia output, unplug a USB headset, and disable all outputs. Confirm rebinding, a clear empty state, and recovery after reconnecting. Also test launching while Windows Audio is stopped, then restarting it and clicking Retry.
- Switch between automatic/light/dark and change the Windows app theme while running; verify automatic updates, fixed overrides, readable settings, and tray contrast.
- Drag an application slider continuously in both orientations; labels/icons must remain stable and thumbs must have smooth edges.
- Change orientation repeatedly, scroll a long list, and check that 100% is at the top of vertical sliders. Use Tab, Shift+Tab, arrows, Page Up/Down, Home/End, Space, and Enter.
- Save a different hotkey, try an occupied combination, reopen the app, and verify persistence. Check that invalid combinations leave the previous binding active.
- Enable startup, verify the quoted executable path and `--background` in the named HKCU Run value, then disable it. Other startup entries must be unaffected.
- Check 100%, 150%, and 200% DPI, two monitors with different scaling, taskbars at different screen edges, and an Explorer restart. Verify placement, clipping, focus, and tray icon restoration.

## Resource measurements

Use a Release build and Task Manager or Process Explorer. Record Windows build, session count, time since startup, whether the window is visible, and both private bytes and working set. Measure idle CPU over at least one minute with the popup hidden and no volume/device/session changes. Compare again after repeatedly showing/hiding the popup and changing orientation; GDI handles, USER handles, threads, and private memory should settle instead of growing continuously.

Keep resource use low without enforcing a fixed RAM ceiling. Compare measurements under the same conditions; prioritize catching persistent handle/memory growth and unintended idle work.
