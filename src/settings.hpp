#pragma once

#include <windows.h>
#include <string>
#include "theme.hpp"

namespace tvm {
struct Settings {
    bool vertical = false;
    UINT modifiers = MOD_CONTROL | MOD_ALT;
    UINT key = 'M';
    ThemeMode theme = ThemeMode::System;
};

Settings LoadSettings();
LSTATUS SaveSettings(const Settings& settings);
std::wstring ExecutablePath();
bool StartupEnabled();
LSTATUS SetStartupEnabled(bool enabled);
std::wstring HotkeyText(const Settings& settings);
std::wstring WindowsError(DWORD code);
} // namespace tvm
