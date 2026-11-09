# Initial validation — 2026-11-09

Environment: Windows x64, build `10.0.26300.9711`, desktop at 125% scaling, Realtek default multimedia output. Source was cross-compiled from WSL with LLVM-MinGW `20261104` (Clang 23.1.5), CMake 4.4.5, Ninja 1.13.2, Release configuration. The produced PE x64 executables were then run on the actual Windows host.

## Results

- Clean compile with the enabled Clang warnings; portable executable approximately 860 KiB. Import inspection found only Windows system DLLs, with no third-party runtime DLLs.
- CTest model suite: passed.
- Core Audio integration: passed discovery of sessions created after enumeration, two-session grouping, external changes, writes confirmed through Core Audio in both streams, mute/unmute, fresh enumerator rebinding, inactive state updates, and row removal after the producer process exited.
- Desktop checks: passed orientation persistence, native hotkey editing, saved settings, quoted startup command, startup removal, collision feedback with the old binding retained, hide-on-close, continued background process, shortcut handler, and hide-on-focus-loss.
- After 40 open/orientation cycles: GDI handles remained `31 → 31`; USER handles remained `36 → 36`. The process exited cleanly. Saved user preferences and the named startup entry were restored after testing.
- Real window captures are in [screenshots](screenshots/v0.1/horizontal.png), including the [vertical view](screenshots/v0.1/vertical.png) and [settings](screenshots/v0.1/settings.png).

## Idle measurement

The app started with `--background`, settled for eight seconds, then remained hidden without test-generated audio or UI activity for 65.108 seconds.

| Metric | Recorded value |
| --- | ---: |
| Process CPU consumed during the interval | 0 ms |
| CPU as a fraction of one core | 0% during this sample |
| Private memory | 2,240,512 bytes (2.14 MiB) |
| Working set | 14,745,600 bytes (14.06 MiB) |
| Threads | 4 |
| Exit code | 0 |

These are historical v0.1 measurements. The subsequent requirements removed a fixed RAM budget. The idle working set exceeded the original brief's ideal 10 MB. Private memory is a different metric and should not be used to claim that the working-set target was reached. These figures are one machine/sample, not a resource guarantee.

## Remaining validation boundaries

The MSVC presets and Windows CI/release workflow are prepared; MSVC was not installed locally, and that workflow has not been run from this checkout. The local executable is the LLVM-MinGW build. Physical output switching/unplugging, Explorer restart, actual sign-in startup, multiple DPI monitors, and screen-reader behavior still need the manual checks described in [testing](testing.md). No GitHub Release was published during this implementation.
