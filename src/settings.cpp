#include "settings.hpp"

#include <array>
#include <vector>
#include <utility>

namespace tvm {
namespace {
constexpr auto kSettingsKey = L"Software\\TinyVolumeMixer";
constexpr auto kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr auto kRunName = L"TinyVolumeMixer";

DWORD ReadDword(HKEY key, const wchar_t* name, DWORD fallback) {
    DWORD value = 0, size = sizeof(value);
    return RegGetValueW(key, nullptr, name, RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS
        ? value : fallback;
}
std::wstring StartupCommand() { return L"\"" + ExecutablePath() + L"\" --background"; }
}

Settings LoadSettings() {
    Settings result;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
        result.vertical = ReadDword(key, L"Vertical", 0) != 0;
        const auto theme = ReadDword(key, L"Theme", 0);
        if (theme <= static_cast<DWORD>(ThemeMode::Dark)) result.theme = static_cast<ThemeMode>(theme);
        const auto modifiers = ReadDword(key, L"HotkeyModifiers", result.modifiers);
        const auto vk = ReadDword(key, L"HotkeyKey", result.key);
        if (vk > 0 && vk < 0xFF && vk != VK_F12 && modifiers != 0 &&
            (modifiers & ~(MOD_ALT | MOD_CONTROL | MOD_SHIFT)) == 0) {
            result.modifiers = modifiers;
            result.key = vk;
        }
        RegCloseKey(key);
    }
    return result;
}

LSTATUS SaveSettings(const Settings& settings) {
    HKEY key = nullptr;
    auto result = RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0,
                                  KEY_SET_VALUE, nullptr, &key, nullptr);
    if (result != ERROR_SUCCESS) return result;
    const std::array<std::pair<const wchar_t*, DWORD>, 4> values{{
        {L"Vertical", settings.vertical ? 1u : 0u},
        {L"HotkeyModifiers", settings.modifiers}, {L"HotkeyKey", settings.key},
        {L"Theme", static_cast<DWORD>(settings.theme)}
    }};
    for (const auto& [name, value] : values) {
        result = RegSetValueExW(key, name, 0, REG_DWORD,
                               reinterpret_cast<const BYTE*>(&value), sizeof(value));
        if (result != ERROR_SUCCESS) break;
    }
    RegCloseKey(key);
    return result;
}

std::wstring ExecutablePath() {
    std::vector<wchar_t> buffer(512);
    for (;;) {
        const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) return {};
        if (length < buffer.size()) return {buffer.data(), length};
        buffer.resize(buffer.size() * 2);
    }
}

bool StartupEnabled() {
    DWORD bytes = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunName, RRF_RT_REG_SZ,
                    nullptr, nullptr, &bytes) != ERROR_SUCCESS) return false;
    std::vector<wchar_t> value(bytes / sizeof(wchar_t) + 1, L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunName, RRF_RT_REG_SZ,
                    nullptr, value.data(), &bytes) != ERROR_SUCCESS) return false;
    return _wcsicmp(value.data(), StartupCommand().c_str()) == 0;
}

LSTATUS SetStartupEnabled(bool enabled) {
    if (!enabled) {
        const auto result = RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunName);
        return result == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : result;
    }
    HKEY key = nullptr;
    auto result = RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0,
                                  KEY_SET_VALUE, nullptr, &key, nullptr);
    if (result != ERROR_SUCCESS) return result;
    const auto command = StartupCommand();
    result = RegSetValueExW(key, kRunName, 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
                           static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return result;
}

std::wstring HotkeyText(const Settings& settings) {
    std::wstring result;
    if (settings.modifiers & MOD_CONTROL) result += L"Ctrl + ";
    if (settings.modifiers & MOD_ALT) result += L"Alt + ";
    if (settings.modifiers & MOD_SHIFT) result += L"Shift + ";
    auto scan = MapVirtualKeyW(settings.key, MAPVK_VK_TO_VSC);
    if (settings.key == VK_INSERT || settings.key == VK_DELETE || settings.key == VK_HOME ||
        settings.key == VK_END || settings.key == VK_PRIOR || settings.key == VK_NEXT ||
        (settings.key >= VK_LEFT && settings.key <= VK_DOWN)) scan |= 0x100;
    wchar_t name[64]{};
    if (GetKeyNameTextW(static_cast<LONG>(scan << 16), name, 64) > 0) result += name;
    else result += static_cast<wchar_t>(settings.key);
    return result;
}

std::wstring WindowsError(DWORD code) {
    wchar_t* message = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                   FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
                   reinterpret_cast<wchar_t*>(&message), 0, nullptr);
    std::wstring result = message ? message : L"Erro " + std::to_wstring(code);
    LocalFree(message);
    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n')) result.pop_back();
    return result;
}
} // namespace tvm
