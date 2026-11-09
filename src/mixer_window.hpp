#pragma once

#include "audio_engine.hpp"
#include "settings.hpp"
#include <memory>
#include <vector>

namespace tvm {
inline constexpr wchar_t kWindowClass[] = L"TinyVolumeMixer.Popup";
inline constexpr wchar_t kShowMessage[] = L"TinyVolumeMixer.Show.v1";

class MixerWindow {
public:
    explicit MixerWindow(HINSTANCE instance);
    ~MixerWindow();
    bool Create();
    int Run(bool background);
private:
    struct Row;
    static LRESULT CALLBACK WindowProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK ViewportProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMessage(UINT, WPARAM, LPARAM);
    void Show();
    void Hide(bool deactivated = false);
    void AddTrayIcon();
    void TrayMenu();
    void UpdateAudio();
    void SyncRows(bool force = false);
    void LayoutPopup();
    void LayoutChildren();
    void PositionRows();
    void Scroll(int amount);
    void Paint();
    void DrawButton(const DRAWITEMSTRUCT& draw);
    void UpdateFonts();
    void ShowSettings(bool show);
    void BuildSettings();
    void ApplySettings();
    void ToggleOrientation();
    void ThemeMenu();
    void ApplyTheme();
    void UpdateTrayTheme();
    void Feedback(std::wstring text, bool error);
    void Tooltip(HWND control, const wchar_t* text);
    int Px(int value) const;

    HINSTANCE instance_;
    HWND window_ = nullptr;
    HWND viewport_ = nullptr;
    HWND tooltip_ = nullptr;
    HWND orientationButton_ = nullptr;
    HWND themeButton_ = nullptr;
    HWND settingsButton_ = nullptr;
    HWND closeButton_ = nullptr;
    HWND retryButton_ = nullptr;
    HWND hotkeyControl_ = nullptr;
    HWND startupControl_ = nullptr;
    HWND saveButton_ = nullptr;
    std::vector<HWND> settingsControls_;
    std::vector<std::unique_ptr<Row>> rows_;
    HICON icon_ = nullptr;
    HICON trayIcon_ = nullptr;
    ULONG_PTR graphicsToken_ = 0;
    HFONT font_ = nullptr;
    HFONT boldFont_ = nullptr;
    HFONT smallFont_ = nullptr;
    HBRUSH background_ = nullptr;
    AudioEngine audio_;
    AudioSnapshot snapshot_;
    Settings settings_;
    Palette colors_{};
    UINT dpi_ = 96;
    UINT taskbarCreated_ = 0;
    UINT showMessage_ = 0;
    int hotkeyId_ = 1;
    int scrollOffset_ = 0;
    bool hotkeyRegistered_ = false;
    bool settingsView_ = false;
    bool trayAdded_ = false;
    bool menuOpen_ = false;
    bool layingOut_ = false;
    bool audioReady_ = false;
    bool themeInitialized_ = false;
    int trayLight_ = -1;
    ULONGLONG deactivatedAt_ = 0;
    std::wstring feedback_;
    bool feedbackError_ = false;
};
} // namespace tvm
