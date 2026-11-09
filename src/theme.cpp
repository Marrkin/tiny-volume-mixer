#include "theme.hpp"

namespace tvm {
namespace {
bool LightPreference(const wchar_t* name) {
    DWORD value = 1, bytes = sizeof(value);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 name, RRF_RT_REG_DWORD, nullptr, &value, &bytes);
    return value != 0;
}
}
Palette ResolvePalette(ThemeMode mode) {
    HIGHCONTRASTW contrast{}; contrast.cbSize = sizeof(contrast);
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
        (contrast.dwFlags & HCF_HIGHCONTRASTON)) {
        const auto background = GetSysColor(COLOR_WINDOW), text = GetSysColor(COLOR_WINDOWTEXT);
        return {background, GetSysColor(COLOR_BTNFACE), text, text, text, GetSysColor(COLOR_HIGHLIGHT),
                background, GetSysColor(COLOR_HIGHLIGHT), text, GetSysColor(COLOR_GRAYTEXT), background == RGB(0,0,0)};
    }
    const bool dark = mode == ThemeMode::Dark || (mode == ThemeMode::System && !LightPreference(L"AppsUseLightTheme"));
    if (dark) return {RGB(24,24,24), RGB(43,43,43), RGB(242,242,242), RGB(169,169,169),
                      RGB(50,50,50), RGB(255,151,151), RGB(31,31,31), RGB(61,61,61),
                      RGB(248,248,248), RGB(143,143,143), true};
    return {RGB(249,249,249), RGB(224,224,224), RGB(28,28,28), RGB(95,95,95),
            RGB(219,219,219), RGB(169,30,38), RGB(237,237,237), RGB(210,210,210),
            RGB(62,62,62), RGB(139,139,139), false};
}
bool LightTaskbar() { return LightPreference(L"SystemUsesLightTheme"); }
const wchar_t* ThemeName(ThemeMode mode) {
    switch (mode) {
    case ThemeMode::Light: return L"Claro";
    case ThemeMode::Dark: return L"Escuro";
    default: return L"Automático (Windows)";
    }
}
} // namespace tvm
