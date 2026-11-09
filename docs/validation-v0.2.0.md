# v0.2.0 validation — 2026-11-09

Built with LLVM-MinGW/Clang in Release mode and run on the Windows x64 host, build `10.0.26300.9711`, at 125% DPI. MSVC CI remains configured but was not run from this checkout.

## Changes validated

- Light, dark, and automatic theme choices persist. Captured backgrounds match the selected palette, automatic matches the current Windows app theme, and an explicit light override survives a Windows setting-change notification. The tray glyph reads the taskbar theme separately.
- Rendering checks verify antialiased edge coverage, identical repeated frames, readable text contrast, and visible thumbs in both themes. The circular thumb's native hit region now covers its rendered width.
- The desktop check performs 80 real volume changes against its own silent audio producer. Application row/icon controls retain their handles and the slider settles at the final command instead of accepting stale readback. Names/icons are not reset by routine volume notifications; changed regions paint through buffers.
- Both native Chrome discovery checks passed: a real Chrome session created after the mixer starts, and an existing Chrome session when the mixer starts. Chrome used a disposable profile and a zero-gain local Web Audio page. No browser/application/master volume was changed by that check.
- The local inventory showed `msedgewebview2` audio belonging to WhatsApp. Both sessions now use the Windows packaged identity, the display name **WhatsApp**, and its Shell icon. They share one control instead of appearing as separate WebView2 and WhatsApp.Root applications.
- Real Core Audio tests passed for late session discovery, grouped writes/mute, stream reuse, latest-command acknowledgments, fresh enumerator rebinding, inactive state changes, and removal when the producer exits.
- The additional-output test ran successfully on Realtek (default) and HDMI (non-default). Four silent sessions across two processes/outputs shared one app row. Core Audio reads confirmed the chosen volume on the non-default output. Closing the auxiliary producer retained the remaining sessions.
- Hotkey conflict handling, startup enable/disable, quoted startup paths, focus-loss hiding, close-to-tray behavior, and preference restoration passed again. Forty open/orientation cycles held GDI handles at **71 → 71**; USER handles changed **43 → 44**. Shutdown completed normally.

## Reproduce

```powershell
cmake --preset windows-x64
cmake --build --preset release
ctest --preset release
.\build\windows-x64\Release\tvm_audio_smoke.exe
.\scripts\test-desktop.ps1 -CaptureScreenshots
.\scripts\test-chrome.ps1
```

The two CTest suites cover model/group identity and rendering/contrast. Core Audio and interactive checks require a real Windows desktop and suitable devices. The Chrome check additionally requires Chrome. The desktop script restores the app's preference key and named startup value; the audio tests write only to their own silent producers.

## Boundaries

The original report did not establish one definitive cause of Chrome's absence. This revision addresses both incomplete output coverage and missed reactivation of expired sessions, with real Chrome and multi-output checks above. Physical hotplug/default-device switching, mixed-DPI monitor transitions, OS theme changes made through Windows Settings, Explorer restart, screen readers, and actual sign-in startup remain part of the manual checklist in [testing](testing.md).

Resource use remains a design concern, with no fixed RAM ceiling. The historical v0.1 resource sample is retained separately and does not describe this revision. No release was published to GitHub during these changes.
