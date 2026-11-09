#pragma once
#include <windows.h>

namespace tvm {
enum class ThemeMode : DWORD { System = 0, Light = 1, Dark = 2 };
struct Palette {
    COLORREF background, track, text, secondary, border, error;
    COLORREF button, buttonPressed, thumb, thumbMuted;
    bool dark;
    bool operator==(const Palette&) const = default;
};
Palette ResolvePalette(ThemeMode mode);
bool LightTaskbar();
const wchar_t* ThemeName(ThemeMode mode);
} // namespace tvm
