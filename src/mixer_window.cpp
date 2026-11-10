#include "mixer_window.hpp"
#include "../resources/resource.h"
#include "painting.hpp"
#include "com_ptr.hpp"

#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellscalingapi.h>
#include <uxtheme.h>
#include <windowsx.h>
#include <algorithm>
#include <iterator>
#include <string_view>

namespace tvm {
namespace {
constexpr UINT kTray = WM_APP + 2;
constexpr int kOrientation = 101, kSettings = 102, kClose = 103, kRetry = 104;
constexpr int kSave = 105, kStartup = 106, kHotkey = 107, kTheme = 109;
constexpr int kSystemTheme = 301, kLightTheme = 302, kDarkTheme = 303;
constexpr int kOpenMenu = 201, kSettingsMenu = 202, kRefreshMenu = 203, kExitMenu = 204;
constexpr wchar_t kRowClass[] = L"TinyVolumeMixer.Row";
constexpr wchar_t kViewportClass[] = L"TinyVolumeMixer.Viewport";

void Fill(HDC dc, const RECT& rect, COLORREF color) {
    SetDCBrushColor(dc, color);
    FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}
void Text(HDC dc, HFONT font, COLORREF color, std::wstring_view text, RECT rect,
          UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS) {
    const auto old = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, text.data(), static_cast<int>(text.size()), &rect, flags | DT_NOPREFIX);
    SelectObject(dc, old);
}
void Rounded(HDC dc, RECT rect, int diameter, COLORREF color) { SmoothRounded(dc, rect, diameter, color); }
void Speaker(HDC dc, RECT rect, bool muted, COLORREF color) {
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    const float u = static_cast<float>(rect.right - rect.left) / 15.0f;
    const float cx = static_cast<float>(rect.left + rect.right)/2.0f;
    const float cy = static_cast<float>(rect.top + rect.bottom)/2.0f;
    Gdiplus::Pen pen(DrawingColor(color), std::max(1.0f, u));
    pen.SetStartCap(Gdiplus::LineCapRound); pen.SetEndCap(Gdiplus::LineCapRound);
    const Gdiplus::PointF outline[]{{cx-5*u,cy-2*u},{cx-3*u,cy-2*u},{cx,cy-5*u},
        {cx,cy+5*u},{cx-3*u,cy+2*u},{cx-5*u,cy+2*u},{cx-5*u,cy-2*u}};
    graphics.DrawLines(&pen, outline, static_cast<int>(std::size(outline)));
    if (muted) {
        graphics.DrawLine(&pen, cx+2*u,cy-2*u,cx+6*u,cy+2*u);
        graphics.DrawLine(&pen, cx+6*u,cy-2*u,cx+2*u,cy+2*u);
    } else {
        graphics.DrawArc(&pen, cx-u,cy-3*u,5*u,6*u,-65.0f,130.0f);
        graphics.DrawArc(&pen, cx-2*u,cy-5*u,9*u,10*u,-65.0f,130.0f);
    }
}
HWND Button(HWND parent, HINSTANCE instance, int id, const wchar_t* name) {
    return CreateWindowExW(0, L"BUTTON", name,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
}
HICON ApplicationIcon(const AppVolume& app, int size) {
    if (!app.appId.empty()) {
        ComPtr<IShellItemImageFactory> image;
        if (SUCCEEDED(SHCreateItemInKnownFolder(FOLDERID_AppsFolder, 0, app.appId.c_str(), IID_PPV_ARGS(image.put())))) {
            HBITMAP bitmap = nullptr;
            if (SUCCEEDED(image->GetImage({size,size}, SIIGBF_ICONONLY, &bitmap))) {
                BITMAP info{}; GetObjectW(bitmap, sizeof(info), &info);
                const auto stride = ((info.bmWidth + 15)/16)*2;
                const std::vector<BYTE> bits(static_cast<std::size_t>(stride*info.bmHeight), 0);
                const auto mask = CreateBitmap(info.bmWidth, info.bmHeight, 1, 1, bits.data());
                ICONINFO icon{}; icon.fIcon = TRUE; icon.hbmColor = bitmap; icon.hbmMask = mask;
                const auto result = mask ? CreateIconIndirect(&icon) : nullptr;
                if (mask) DeleteObject(mask);
                DeleteObject(bitmap);
                if (result) return result;
            }
        }
    }
    HICON icon = nullptr;
    if (!app.executable.empty()) ExtractIconExW(app.executable.c_str(), 0, &icon, nullptr, 1);
    return icon;
}
} // namespace

struct MixerWindow::Row {
    MixerWindow* app;
    HWND window = nullptr, slider = nullptr, mute = nullptr;
    AppVolume value;
    HICON icon = nullptr;
    std::wstring muteText;
    const std::wstring tooltipText;
    bool tracking = false;
    std::uint64_t pendingVolume = 0;

    Row(MixerWindow* owner, AppVolume data)
        : app(owner), value(std::move(data)), tooltipText(value.name) {
        window = CreateWindowExW(WS_EX_CONTROLPARENT, kRowClass, value.name.c_str(),
            WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 0, 0, app->viewport_, nullptr, app->instance_, this);
        mute = Button(window, app->instance_, 1, L"Silenciar");
        slider = CreateWindowExW(0, TRACKBAR_CLASSW, value.name.c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_NOTICKS | TBS_FIXEDLENGTH |
            (app->settings_.vertical ? TBS_VERT : TBS_HORZ),
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(2), app->instance_, nullptr);
        SendMessageW(slider, TBM_SETRANGE, FALSE, MAKELPARAM(0, 100));
        SendMessageW(slider, TBM_SETLINESIZE, 0, 1);
        SendMessageW(slider, TBM_SETPAGESIZE, 0, 10);
        // The native thumb's drag-axis size is roughly half this length. Match
        // its hit region to our 18-DIP circular thumb, not a narrow old arrow.
        SendMessageW(slider, TBM_SETTHUMBLENGTH, app->Px(36), 0);
        SetWindowTheme(slider, L"", L"");
        SetWindowSubclass(slider, SliderProc, 1, reinterpret_cast<DWORD_PTR>(this));
        icon = ApplicationIcon(value, app->Px(24));
        app->Tooltip(mute, L"Silenciar / ativar som");
        app->Tooltip(slider, tooltipText.c_str());
        Update(value);
    }
    ~Row() {
        for (const auto control : {mute, slider}) {
            TOOLINFOW info{}; info.cbSize = sizeof(info);
            info.uFlags = TTF_IDISHWND;
            info.hwnd = app->window_;
            info.uId = reinterpret_cast<UINT_PTR>(control);
            SendMessageW(app->tooltip_, TTM_DELTOOLW, 0, reinterpret_cast<LPARAM>(&info));
        }
        if (IsWindow(window)) DestroyWindow(window);
        if (icon) DestroyIcon(icon);
    }
    RECT PercentRect() const {
        RECT rect{}; GetClientRect(window, &rect);
        return app->settings_.vertical
            ? RECT{0, rect.bottom - app->Px(32), rect.right, rect.bottom}
            : RECT{rect.right - app->Px(90), 0, rect.right, app->Px(30)};
    }
    void UpdateMuteName() {
        const auto name = (value.muted ? L"Ativar som de " : L"Silenciar ") + value.name;
        if (name != muteText) { muteText = name; SetWindowTextW(mute, name.c_str()); }
    }
    void Update(const AppVolume& data) {
        const auto previous = value;
        const bool local = pendingVolume > app->snapshot_.lastAppliedCommand && app->snapshot_.error.empty();
        value = data;
        if (local) { value.volume = previous.volume; value.muted = previous.muted; }
        else pendingVolume = 0;
        const int percent = VolumePercent(value.volume);
        const int position = app->settings_.vertical ? 100 - percent : percent;
        if (!tracking && GetCapture() != slider && SendMessageW(slider, TBM_GETPOS, 0, 0) != position) {
            // Recalculate the native thumb rectangle as well as the logical
            // value: custom painting uses TBM_GETTHUMBRECT for its position.
            SendMessageW(slider, TBM_SETPOS, TRUE, position);
        }
        UpdateMuteName();
        const bool enabled = value.id != kMasterId || app->snapshot_.deviceAvailable;
        if ((IsWindowEnabled(slider) != FALSE) != enabled) EnableWindow(slider, enabled);
        if ((IsWindowEnabled(mute) != FALSE) != enabled) EnableWindow(mute, enabled);
        if (previous.name != value.name) InvalidateRect(window, nullptr, FALSE);
        else if (VolumePercent(previous.volume) != percent || previous.muted != value.muted) {
            const auto rect = PercentRect(); InvalidateRect(window, &rect, FALSE);
        }
        if (previous.muted != value.muted) {
            InvalidateRect(mute, nullptr, FALSE); InvalidateRect(slider, nullptr, FALSE);
        }
    }
    static LRESULT CALLBACK SliderProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp,
                                       UINT_PTR id, DWORD_PTR) {
        if (message == WM_ERASEBKGND) return 1;
        if (message == WM_NCDESTROY) RemoveWindowSubclass(hwnd, SliderProc, id);
        return DefSubclassProc(hwnd, message, wp, lp);
    }
    void Layout(int width, int height) {
        const auto s = [this](int x) { return app->Px(x); };
        if (app->settings_.vertical) {
            MoveWindow(mute, (width - s(34))/2, s(8), s(34), s(34), TRUE);
            MoveWindow(slider, (width - s(36))/2, s(82), s(36), height - s(120), TRUE);
        } else {
            MoveWindow(mute, s(2), s(15), s(34), s(34), TRUE);
            MoveWindow(slider, s(46), s(26), width - s(48), s(36), TRUE);
        }
    }
    void Paint() {
        PAINTSTRUCT paint{};
        const auto target = BeginPaint(window, &paint);
        RECT rect{}; GetClientRect(window, &rect);
        {
        PaintBuffer buffer(target, rect); const auto dc = buffer.get();
        Fill(dc, rect, app->colors_.background);
        const auto s = [this](int x) { return app->Px(x); };
        auto percent = std::to_wstring(VolumePercent(value.volume)) + L"%";
        if (value.muted) percent = L"Mudo · " + percent;
        if (app->settings_.vertical) {
            Text(dc, app->boldFont_, app->colors_.text, value.name,
                 {s(5), s(47), rect.right - s(5), s(76)}, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            Text(dc, app->smallFont_, value.muted ? app->colors_.secondary : app->colors_.text, percent,
                 {s(2), rect.bottom - s(30), rect.right - s(2), rect.bottom - s(6)}, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else {
            Text(dc, app->boldFont_, app->colors_.text, value.name, {s(46), s(2), rect.right - s(85), s(30)});
            Text(dc, app->smallFont_, app->colors_.secondary, percent, {rect.right - s(85), s(2), rect.right - s(5), s(30)},
                 DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }
        }
        EndPaint(window, &paint);
    }
    LRESULT DrawSlider(NMCUSTOMDRAW& draw) {
        if (draw.dwDrawStage != CDDS_PREPAINT) return CDRF_DODEFAULT;
        RECT client{}, channel{}, thumb{};
        GetClientRect(slider, &client);
        SendMessageW(slider, TBM_GETCHANNELRECT, 0, reinterpret_cast<LPARAM>(&channel));
        SendMessageW(slider, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&thumb));
        PaintBuffer buffer(draw.hdc, client);
        const auto dc = buffer.get();
        Fill(dc, client, app->colors_.background);
        const auto s = [this](int x) { return app->Px(x); };
        const int x = (thumb.left + thumb.right)/2;
        const int y = (thumb.top + thumb.bottom)/2;
        const RECT track = app->settings_.vertical
            ? RECT{x - s(8), channel.left, x + s(8), channel.right}
            : RECT{channel.left, y - s(8), channel.right, y + s(8)};
        Rounded(dc, track, s(16), app->colors_.track);
        const int radius = s(9);
        Rounded(dc, {x - radius, y - radius, x + radius, y + radius}, radius * 2,
                value.muted ? app->colors_.thumbMuted : app->colors_.thumb);
        if (GetFocus() == slider && !(SendMessageW(slider, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS)) {
            SetTextColor(dc, app->colors_.secondary);
            DrawFocusRect(dc, &client);
        }
        // Draw in one pass. The standard trackbar still owns input, hit testing,
        // keyboard navigation, range, and its accessibility value provider.
        return CDRF_SKIPDEFAULT;
    }
    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
        auto* row = reinterpret_cast<Row*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            row = static_cast<Row*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            row->window = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(row));
        }
        if (!row) return DefWindowProcW(hwnd, message, wp, lp);
        switch (message) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: row->Paint(); return 0;
        case WM_SIZE: row->Layout(LOWORD(lp), HIWORD(lp)); return 0;
        case WM_COMMAND:
            if (LOWORD(wp) == 1 && HIWORD(wp) == BN_CLICKED) {
                row->pendingVolume = 0; row->app->audio_.ToggleMute(row->value.id);
            }
            return 0;
        case WM_NOTIFY:
            if (reinterpret_cast<NMHDR*>(lp)->code == NM_CUSTOMDRAW) {
                return row->DrawSlider(*reinterpret_cast<NMCUSTOMDRAW*>(lp));
            }
            break;
        case WM_HSCROLL:
        case WM_VSCROLL: {
            const int pos = static_cast<int>(SendMessageW(row->slider, TBM_GETPOS, 0, 0));
            const int percent = row->app->settings_.vertical ? 100 - pos : pos;
            row->tracking = LOWORD(wp) == TB_THUMBTRACK;
            const bool wasMuted = row->value.muted;
            const int oldPercent = VolumePercent(row->value.volume);
            row->value.volume = static_cast<float>(percent) / 100.0f;
            if (percent > 0) row->value.muted = false;
            row->pendingVolume = row->app->audio_.SetVolume(row->value.id, row->value.volume);
            if (oldPercent != percent || wasMuted != row->value.muted) {
                const auto rect = row->PercentRect(); InvalidateRect(hwnd, &rect, FALSE);
            }
            if (wasMuted != row->value.muted) { row->UpdateMuteName(); InvalidateRect(row->mute, nullptr, FALSE); }
            return 0;
        }
        case WM_DRAWITEM: {
            const auto& draw = *reinterpret_cast<DRAWITEMSTRUCT*>(lp);
            PaintBuffer buffer(draw.hDC, draw.rcItem); const auto dc = buffer.get();
            Fill(dc, draw.rcItem, row->app->colors_.background);
            Rounded(dc, draw.rcItem, row->app->Px(12),
                    (draw.itemState & ODS_SELECTED) ? row->app->colors_.buttonPressed : row->app->colors_.button);
            auto rect = draw.rcItem;
            InflateRect(&rect, -row->app->Px(7), -row->app->Px(7));
            if (row->icon && !row->value.muted) {
                DrawIconEx(dc, rect.left, rect.top, row->icon,
                           rect.right - rect.left, rect.bottom - rect.top, 0, nullptr, DI_NORMAL);
            } else Speaker(dc, rect, row->value.muted, row->value.muted ? row->app->colors_.error : row->app->colors_.secondary);
            if (draw.itemState & ODS_FOCUS) DrawFocusRect(dc, &draw.rcItem);
            return TRUE;
        }
        case WM_MOUSEWHEEL: return SendMessageW(row->app->viewport_, message, wp, lp);
        }
        return DefWindowProcW(hwnd, message, wp, lp);
    }
};

MixerWindow::MixerWindow(HINSTANCE instance) : instance_(instance), settings_(LoadSettings()) {
    colors_ = ResolvePalette(settings_.theme);
    background_ = CreateSolidBrush(colors_.background);
}
MixerWindow::~MixerWindow() {
    audio_.Stop();
    rows_.clear();
    if (trayAdded_) {
        NOTIFYICONDATAW data{}; data.cbSize = sizeof(data); data.hWnd = window_; data.uID = 1;
        Shell_NotifyIconW(NIM_DELETE, &data);
    }
    if (hotkeyRegistered_) UnregisterHotKey(window_, hotkeyId_);
    if (IsWindow(window_)) DestroyWindow(window_);
    for (const auto font : {font_, boldFont_, smallFont_}) if (font) DeleteObject(font);
    if (background_) DeleteObject(background_);
    if (icon_) DestroyIcon(icon_);
    if (trayIcon_) DestroyIcon(trayIcon_);
    if (graphicsToken_) Gdiplus::GdiplusShutdown(graphicsToken_);
}
int MixerWindow::Px(int value) const { return MulDiv(value, static_cast<int>(dpi_), 96); }

bool MixerWindow::Create() {
    Gdiplus::GdiplusStartupInput graphics;
    if (Gdiplus::GdiplusStartup(&graphicsToken_, &graphics, nullptr) != Gdiplus::Ok) return false;
    icon_ = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
    WNDCLASSEXW cls{}; cls.cbSize = sizeof(cls);
    cls.hInstance = instance_;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hIcon = icon_; cls.hIconSm = icon_;
    cls.lpfnWndProc = WindowProc;
    cls.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&cls)) return false;
    cls.hIcon = nullptr; cls.hIconSm = nullptr;
    cls.lpszClassName = kViewportClass; cls.lpfnWndProc = ViewportProc;
    if (!RegisterClassExW(&cls)) return false;
    cls.lpszClassName = kRowClass; cls.lpfnWndProc = Row::Proc;
    if (!RegisterClassExW(&cls)) return false;

    window_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_CONTROLPARENT,
        kWindowClass, L"TinyVolumeMixer", WS_POPUP | WS_THICKFRAME | WS_CLIPCHILDREN,
        0, 0, 440, 300, nullptr, nullptr, instance_, this);
    if (!window_) return false;
    dpi_ = GetDpiForWindow(window_);
    UpdateFonts();
    ApplyTheme();
    tooltip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
        WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT,
        CW_USEDEFAULT, CW_USEDEFAULT, window_, nullptr, instance_, nullptr);
    SendMessageW(tooltip_, TTM_SETMAXTIPWIDTH, 0, Px(320));
    orientationButton_ = Button(window_, instance_, kOrientation, L"Alternar orientação das barras");
    themeButton_ = Button(window_, instance_, kTheme, L"Tema: automático, claro ou escuro");
    settingsButton_ = Button(window_, instance_, kSettings, L"Configurações");
    closeButton_ = Button(window_, instance_, kClose, L"Ocultar mixer");
    Tooltip(orientationButton_, L"Alternar barras horizontais / verticais");
    Tooltip(themeButton_, L"Tema: automático (Windows), claro ou escuro");
    Tooltip(settingsButton_, L"Configurações / voltar ao mixer");
    Tooltip(closeButton_, L"Ocultar (Esc). O mixer continua na bandeja.");
    viewport_ = CreateWindowExW(WS_EX_CONTROLPARENT, kViewportClass, nullptr,
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 0, 0, window_, nullptr, instance_, this);
    retryButton_ = Button(viewport_, instance_, kRetry, L"Tentar novamente");
    ShowWindow(retryButton_, SW_HIDE);
    taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
    showMessage_ = RegisterWindowMessageW(kShowMessage);
    AddTrayIcon();
    hotkeyRegistered_ = RegisterHotKey(window_, hotkeyId_, settings_.modifiers | MOD_NOREPEAT, settings_.key) != FALSE;
    if (!hotkeyRegistered_) Feedback(L"Atalho em uso. Escolha outra combinação em Configurações.", true);
    if (!audio_.Start(window_)) return false;
    return true;
}

int MixerWindow::Run(bool background) {
    if (!background) Show();
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (IsWindowVisible(window_) && message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) {
            Hide(); continue;
        }
        if (!IsWindowVisible(window_) || !IsDialogMessageW(window_, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    return static_cast<int>(message.wParam);
}

void MixerWindow::Tooltip(HWND control, const wchar_t* text) {
    TOOLINFOW info{}; info.cbSize = sizeof(info);
    info.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    info.hwnd = window_;
    info.uId = reinterpret_cast<UINT_PTR>(control);
    info.lpszText = const_cast<wchar_t*>(text);
    SendMessageW(tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
}

void MixerWindow::AddTrayIcon() {
    NOTIFYICONDATAW data{}; data.cbSize = sizeof(data);
    data.hWnd = window_; data.uID = 1;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    data.uCallbackMessage = kTray; data.hIcon = trayIcon_ ? trayIcon_ : icon_;
    wcscpy_s(data.szTip, L"TinyVolumeMixer — clique para abrir");
    trayAdded_ = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
    if (trayAdded_) {
        data.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &data);
    }
}

void MixerWindow::Show() {
    feedback_.clear();
    if (!hotkeyRegistered_) Feedback(L"Atalho em uso. Altere-o nas configurações.", true);
    if (settingsView_) ShowSettings(false);
    snapshot_ = audio_.Snapshot();
    SyncRows();
    LayoutPopup();
    ShowWindow(window_, SW_SHOW);
    SetForegroundWindow(window_);
    SetFocus(rows_.empty() ? settingsButton_ : rows_.front()->slider);
}
void MixerWindow::Hide(bool deactivated) {
    if (deactivated) deactivatedAt_ = GetTickCount64();
    ShowWindow(window_, SW_HIDE);
}

void MixerWindow::TrayMenu() {
    menuOpen_ = true;
    const auto menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kOpenMenu, L"Abrir mixer");
    AppendMenuW(menu, MF_STRING, kSettingsMenu, L"Configurações");
    AppendMenuW(menu, MF_STRING, kRefreshMenu, L"Atualizar dispositivos");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kExitMenu, L"Sair");
    POINT point{}; GetCursorPos(&point);
    SetForegroundWindow(window_);
    const auto selected = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                         point.x, point.y, 0, window_, nullptr);
    DestroyMenu(menu);
    menuOpen_ = false;
    PostMessageW(window_, WM_NULL, 0, 0);
    switch (selected) {
    case kOpenMenu: Show(); break;
    case kSettingsMenu: Show(); ShowSettings(true); break;
    case kRefreshMenu: audio_.Refresh(); Show(); break;
    case kExitMenu: PostQuitMessage(0); break;
    default:
        if (GetForegroundWindow() != window_) Hide();
        break;
    }
}

void MixerWindow::UpdateAudio() {
    const bool first = !audioReady_;
    auto next = audio_.Snapshot();
    const bool footerChanged = snapshot_.deviceName != next.deviceName || snapshot_.error != next.error ||
                               snapshot_.deviceAvailable != next.deviceAvailable;
    snapshot_ = std::move(next);
    audioReady_ = true;
    if (IsWindowVisible(window_) && !settingsView_) SyncRows();
    if (first) InvalidateRect(viewport_, nullptr, FALSE);
    if (footerChanged) {
        RECT rect{}; GetClientRect(window_, &rect); rect.top = rect.bottom - Px(36);
        InvalidateRect(window_, &rect, FALSE);
    }
}

void MixerWindow::SyncRows(bool force) {
    std::vector<AppVolume> values;
    if (snapshot_.deviceAvailable) {
        values.push_back({kMasterId, L"Volume geral", L"", snapshot_.masterVolume,
                          snapshot_.masterMuted, false, false, 1});
    }
    values.insert(values.end(), snapshot_.apps.begin(), snapshot_.apps.end());
    bool changed = force || values.size() != rows_.size();
    if (!changed) {
        for (std::size_t i = 0; i < values.size(); ++i) changed = changed || values[i].id != rows_[i]->value.id;
    }
    if (changed) {
        std::wstring focused;
        bool focusedMute = false;
        for (const auto& row : rows_) {
            if (GetFocus() == row->slider || GetFocus() == row->mute) {
                focused = row->value.id; focusedMute = GetFocus() == row->mute;
            }
        }
        auto previous = std::move(rows_);
        rows_.clear();
        for (auto& value : values) {
            auto found = std::find_if(previous.begin(), previous.end(), [&](const auto& row) {
                return row && !force && row->value.id == value.id;
            });
            if (found != previous.end()) {
                (*found)->Update(value); rows_.push_back(std::move(*found));
            } else rows_.push_back(std::make_unique<Row>(this, std::move(value)));
        }
        previous.clear();
        LayoutPopup();
        for (const auto& row : rows_) {
            if (row->value.id == focused) SetFocus(focusedMute ? row->mute : row->slider);
        }
    } else {
        for (std::size_t i = 0; i < values.size(); ++i) rows_[i]->Update(values[i]);
    }
    const bool retry = rows_.empty() && audioReady_;
    if ((IsWindowVisible(retryButton_) != FALSE) != retry) ShowWindow(retryButton_, retry ? SW_SHOW : SW_HIDE);
    if (changed) InvalidateRect(viewport_, nullptr, FALSE);
}

void MixerWindow::UpdateFonts() {
    for (const auto font : {font_, boldFont_, smallFont_}) if (font) DeleteObject(font);
    const auto make = [this](int height, int weight) {
        return CreateFontW(-Px(height), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    };
    font_ = make(13, FW_NORMAL);
    boldFont_ = make(13, FW_SEMIBOLD);
    smallFont_ = make(11, FW_NORMAL);
}

void MixerWindow::LayoutPopup() {
    if (layingOut_ || !viewport_) return;
    layingOut_ = true;
    NOTIFYICONIDENTIFIER identifier{}; identifier.cbSize = sizeof(identifier);
    identifier.hWnd = window_; identifier.uID = 1;
    RECT anchor{};
    if (FAILED(Shell_NotifyIconGetRect(&identifier, &anchor))) {
        POINT cursor{}; GetCursorPos(&cursor);
        MONITORINFO info{}; info.cbSize = sizeof(info);
        GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &info);
        anchor = {info.rcWork.right - 1, info.rcWork.bottom - 1, info.rcWork.right, info.rcWork.bottom};
    }
    const auto monitor = MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{}; info.cbSize = sizeof(info); GetMonitorInfoW(monitor, &info);
    UINT xDpi = 96, yDpi = 96;
    GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &xDpi, &yDpi);
    if (dpi_ != xDpi) {
        dpi_ = xDpi; UpdateFonts();
        rows_.clear();
        // SyncRows observes layingOut_, so rebuilding controls cannot recurse here.
        SyncRows(true);
        if (settingsView_) BuildSettings();
    }
    const int availableWidth = info.rcWork.right - info.rcWork.left - Px(16);
    const int availableHeight = info.rcWork.bottom - info.rcWork.top - Px(16);
    int width = Px(440), height = Px(300);
    if (settingsView_) {
        width = Px(440); height = Px(316);
    } else if (settings_.vertical && !rows_.empty()) {
        const auto columns = static_cast<int>(std::min<std::size_t>(rows_.size(), 6));
        width = std::max(Px(360), columns * Px(104) + Px(24)); height = Px(366);
    } else {
        const auto count = static_cast<int>(std::min<std::size_t>(rows_.size(), 6));
        height = Px(94) + std::max(Px(154), count * Px(66) + (count == 1 ? Px(72) : 0));
    }
    width = std::min(width, availableWidth); height = std::min(height, availableHeight);
    const int margin = Px(8);
    int x = anchor.right - width, y = info.rcWork.bottom - height - margin;
    if (anchor.bottom <= info.rcWork.top) y = info.rcWork.top + margin;
    if (anchor.right <= info.rcWork.left) x = info.rcWork.left + margin;
    x = std::clamp<LONG>(x, info.rcWork.left + margin, std::max(info.rcWork.left + margin, info.rcWork.right - width - margin));
    y = std::clamp<LONG>(y, info.rcWork.top + margin, std::max(info.rcWork.top + margin, info.rcWork.bottom - height - margin));
    SetWindowPos(window_, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE);
    LayoutChildren();
    layingOut_ = false;
    InvalidateRect(window_, nullptr, FALSE);
}

void MixerWindow::LayoutChildren() {
    if (!viewport_) return;
    RECT rect{}; GetClientRect(window_, &rect);
    const auto width = rect.right;
    MoveWindow(closeButton_, width - Px(42), Px(12), Px(28), Px(28), TRUE);
    MoveWindow(settingsButton_, width - Px(74), Px(12), Px(28), Px(28), TRUE);
    MoveWindow(themeButton_, width - Px(106), Px(12), Px(28), Px(28), TRUE);
    MoveWindow(orientationButton_, width - Px(138), Px(12), Px(28), Px(28), TRUE);
    ShowWindow(orientationButton_, settingsView_ ? SW_HIDE : SW_SHOW);
    MoveWindow(viewport_, Px(12), Px(58), width - Px(24), std::max(1L, rect.bottom - Px(94)), TRUE);
    PositionRows();
}

void MixerWindow::PositionRows() {
    RECT rect{}; GetClientRect(viewport_, &rect);
    const bool vertical = settings_.vertical;
    const int bar = vertical ? SB_HORZ : SB_VERT;
    const int total = static_cast<int>(rows_.size()) * Px(vertical ? 104 : 66);
    const int page = vertical ? rect.right : rect.bottom;
    scrollOffset_ = std::clamp(scrollOffset_, 0, std::max(0, total - page));
    SCROLLINFO scroll{}; scroll.cbSize = sizeof(scroll); scroll.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    scroll.nMin = 0; scroll.nMax = std::max(0, total - 1);
    scroll.nPage = static_cast<UINT>(std::max(0, page)); scroll.nPos = scrollOffset_;
    ShowScrollBar(viewport_, vertical ? SB_VERT : SB_HORZ, FALSE);
    SetScrollInfo(viewport_, bar, &scroll, TRUE);
    ShowScrollBar(viewport_, bar, total > page && !settingsView_);
    GetClientRect(viewport_, &rect);
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        const int at = static_cast<int>(i) * Px(vertical ? 104 : 66) - scrollOffset_;
        MoveWindow(rows_[i]->window, vertical ? at : 0, vertical ? 0 : at,
                   vertical ? Px(104) : rect.right, vertical ? rect.bottom : Px(66), TRUE);
    }
    MoveWindow(retryButton_, std::max(0L, (rect.right - Px(160))/2), Px(100), Px(160), Px(32), TRUE);
    InvalidateRect(viewport_, nullptr, FALSE);
}
void MixerWindow::Scroll(int amount) { scrollOffset_ += amount; PositionRows(); }

void MixerWindow::Paint() {
    PAINTSTRUCT paint{}; const auto target = BeginPaint(window_, &paint);
    RECT rect{}; GetClientRect(window_, &rect);
    const auto dc = CreateCompatibleDC(target);
    const auto bitmap = CreateCompatibleBitmap(target, std::max(1L, rect.right), std::max(1L, rect.bottom));
    const auto oldBitmap = SelectObject(dc, bitmap);
    Fill(dc, rect, colors_.background);
    Text(dc, boldFont_, colors_.text, settingsView_ ? L"Configurações" : L"TinyVolumeMixer",
         {Px(18), Px(12), rect.right - Px(146), Px(42)});
    Fill(dc, {Px(16), Px(51), rect.right - Px(16), Px(52)}, colors_.border);
    const auto footer = settingsView_ ? L"TinyVolumeMixer · v1.0.0" :
        !feedback_.empty() ? feedback_ : !snapshot_.error.empty() ? snapshot_.error :
        snapshot_.deviceAvailable ? L"Saída padrão: " + snapshot_.deviceName : L"TinyVolumeMixer · v1.0.0";
    const bool error = !settingsView_ && (!feedback_.empty() ? feedbackError_ : !snapshot_.error.empty());
    Text(dc, smallFont_, error ? colors_.error : colors_.secondary, footer,
         {Px(18), rect.bottom - Px(30), rect.right - Px(18), rect.bottom - Px(7)});
    BitBlt(target, 0, 0, rect.right, rect.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBitmap); DeleteObject(bitmap); DeleteDC(dc);
    EndPaint(window_, &paint);
}

void MixerWindow::DrawButton(const DRAWITEMSTRUCT& draw) {
    PaintBuffer buffer(draw.hDC, draw.rcItem); const auto dc = buffer.get();
    Fill(dc, draw.rcItem, colors_.background);
    if (draw.itemState & ODS_SELECTED) Rounded(dc, draw.rcItem, Px(8), colors_.track);
    if (draw.CtlID == kSave || draw.CtlID == kRetry) {
        Rounded(dc, draw.rcItem, Px(8), (draw.itemState & ODS_SELECTED) ? colors_.buttonPressed : colors_.button);
        Text(dc, boldFont_, colors_.text, draw.CtlID == kSave ? L"Salvar alterações" : L"Tentar novamente",
             draw.rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else {
        const int x = (draw.rcItem.left + draw.rcItem.right)/2;
        const int y = (draw.rcItem.top + draw.rcItem.bottom)/2;
        const auto pen = CreatePen(PS_SOLID, std::max(1, Px(1)), colors_.secondary);
        const auto oldPen = SelectObject(dc, pen);
        if (draw.CtlID == kClose) {
            MoveToEx(dc, x-Px(4), y-Px(4), nullptr); LineTo(dc, x+Px(5), y+Px(5));
            MoveToEx(dc, x+Px(4), y-Px(4), nullptr); LineTo(dc, x-Px(5), y+Px(5));
        } else if (draw.CtlID == kOrientation) {
            for (int i = -1; i <= 1; ++i) {
                if (settings_.vertical) {
                    MoveToEx(dc, x+Px(i*5), y-Px(6), nullptr); LineTo(dc, x+Px(i*5), y+Px(7));
                } else {
                    MoveToEx(dc, x-Px(6), y+Px(i*5), nullptr); LineTo(dc, x+Px(7), y+Px(i*5));
                }
            }
        } else if (draw.CtlID == kTheme) {
            Gdiplus::Graphics graphics(dc);
            graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Gdiplus::Pen line(DrawingColor(colors_.secondary), static_cast<float>(Px(1)));
            graphics.DrawEllipse(&line, static_cast<float>(x-Px(6)), static_cast<float>(y-Px(6)),
                                 static_cast<float>(Px(12)), static_cast<float>(Px(12)));
            if (settings_.theme == ThemeMode::System) {
                Text(dc, smallFont_, colors_.text, L"A", draw.rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            } else {
                Gdiplus::SolidBrush ink(DrawingColor(colors_.secondary));
                graphics.FillPie(&ink, static_cast<float>(x-Px(5)), static_cast<float>(y-Px(5)),
                                 static_cast<float>(Px(10)), static_cast<float>(Px(10)),
                                 settings_.theme == ThemeMode::Dark ? 90.0f : 270.0f, 180.0f);
            }
        } else if (settingsView_) {
            MoveToEx(dc, x+Px(3), y-Px(5), nullptr); LineTo(dc, x-Px(3), y);
            LineTo(dc, x+Px(3), y+Px(5));
        } else {
            // A compact settings glyph, drawn with system GDI and no icon font dependency.
            for (int i = -1; i <= 1; ++i) {
                MoveToEx(dc, x-Px(6), y+Px(i*5), nullptr); LineTo(dc, x+Px(7), y+Px(i*5));
                const int knob = x + Px(i == 0 ? 3 : -2);
                Fill(dc, {knob-Px(1), y+Px(i*5)-Px(2), knob+Px(2), y+Px(i*5)+Px(3)}, colors_.secondary);
            }
        }
        SelectObject(dc, oldPen); DeleteObject(pen);
    }
    if (draw.itemState & ODS_FOCUS) DrawFocusRect(dc, &draw.rcItem);
}

void MixerWindow::ShowSettings(bool show) {
    settingsView_ = show;
    feedback_.clear();
    feedbackError_ = false;
    if (show) BuildSettings();
    else {
        for (const auto control : settingsControls_) DestroyWindow(control);
        settingsControls_.clear();
        hotkeyControl_ = startupControl_ = saveButton_ = nullptr;
        SyncRows();
    }
    SetWindowTextW(settingsButton_, show ? L"Voltar ao mixer" : L"Configurações");
    ShowWindow(viewport_, show ? SW_HIDE : SW_SHOW);
    LayoutPopup();
    SetFocus(show ? hotkeyControl_ : settingsButton_);
}

void MixerWindow::BuildSettings() {
    for (const auto control : settingsControls_) DestroyWindow(control);
    settingsControls_.clear();
    const auto create = [this](const wchar_t* cls, const wchar_t* text, DWORD style,
                               int x, int y, int width, int height, int id, HFONT font) {
        const auto control = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
            Px(x), Px(y), Px(width), Px(height), window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        settingsControls_.push_back(control);
        return control;
    };
    create(L"STATIC", L"Atalho para abrir o mixer", 0, 20, 68, 390, 22, 0, boldFont_);
    hotkeyControl_ = create(HOTKEY_CLASSW, L"", WS_TABSTOP | WS_BORDER, 20, 97, 270, 30, kHotkey, font_);
    WORD flags = 0;
    if (settings_.modifiers & MOD_CONTROL) flags |= HOTKEYF_CONTROL;
    if (settings_.modifiers & MOD_ALT) flags |= HOTKEYF_ALT;
    if (settings_.modifiers & MOD_SHIFT) flags |= HOTKEYF_SHIFT;
    if (settings_.key == VK_INSERT || settings_.key == VK_DELETE || settings_.key == VK_HOME ||
        settings_.key == VK_END || settings_.key == VK_PRIOR || settings_.key == VK_NEXT ||
        (settings_.key >= VK_LEFT && settings_.key <= VK_DOWN)) flags |= HOTKEYF_EXT;
    SendMessageW(hotkeyControl_, HKM_SETHOTKEY, MAKEWORD(settings_.key, flags), 0);
    create(L"STATIC", L"Pressione uma combinação com Ctrl, Alt ou Shift.", 0, 20, 132, 390, 22, 0, smallFont_);
    startupControl_ = create(L"BUTTON", L"Iniciar com o Windows (na bandeja)",
        WS_TABSTOP | BS_AUTOCHECKBOX, 20, 169, 390, 26, kStartup, font_);
    SetWindowTheme(startupControl_, L"", L"");
    SendMessageW(startupControl_, BM_SETCHECK, StartupEnabled() ? BST_CHECKED : BST_UNCHECKED, 0);
    saveButton_ = create(L"BUTTON", L"Salvar alterações", WS_TABSTOP | BS_OWNERDRAW,
                         20, 211, 160, 34, kSave, boldFont_);
    const auto label = create(L"STATIC", feedback_.empty() ? L"As preferências ficam salvas neste usuário." : feedback_.c_str(),
                              0, 20, 252, 398, 30, 108, smallFont_);
    (void)label;
}

void MixerWindow::ApplySettings() {
    const auto combination = static_cast<WORD>(SendMessageW(hotkeyControl_, HKM_GETHOTKEY, 0, 0));
    Settings next = settings_;
    next.key = LOBYTE(combination);
    const auto flags = HIBYTE(combination);
    next.modifiers = ((flags & HOTKEYF_CONTROL) ? MOD_CONTROL : 0) |
                     ((flags & HOTKEYF_ALT) ? MOD_ALT : 0) | ((flags & HOTKEYF_SHIFT) ? MOD_SHIFT : 0);
    if (next.key == 0 || next.modifiers == 0 || next.key == VK_F12) {
        Feedback(L"Use Ctrl, Alt ou Shift junto de uma tecla. F12 é reservada.", true); return;
    }
    const bool replace = !hotkeyRegistered_ || next.key != settings_.key || next.modifiers != settings_.modifiers;
    const int nextId = hotkeyId_ == 1 ? 2 : 1;
    if (replace && !RegisterHotKey(window_, nextId, next.modifiers | MOD_NOREPEAT, next.key)) {
        Feedback(L"Esse atalho já está em uso. Escolha outra combinação.", true); return;
    }
    const bool previousStartup = StartupEnabled();
    const bool startup = SendMessageW(startupControl_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    auto result = startup == previousStartup ? ERROR_SUCCESS : SetStartupEnabled(startup);
    if (result == ERROR_SUCCESS) result = SaveSettings(next);
    if (result != ERROR_SUCCESS) {
        if (replace) UnregisterHotKey(window_, nextId);
        SetStartupEnabled(previousStartup);
        SaveSettings(settings_);
        Feedback(L"Não foi possível salvar: " + WindowsError(result), true);
        return;
    }
    if (replace) {
        if (hotkeyRegistered_) UnregisterHotKey(window_, hotkeyId_);
        hotkeyId_ = nextId; hotkeyRegistered_ = true;
    }
    settings_ = next;
    Feedback(L"Preferências salvas. Atalho: " + HotkeyText(settings_), false);
}

void MixerWindow::ThemeMenu() {
    const auto menu = CreatePopupMenu();
    for (const auto mode : {ThemeMode::System, ThemeMode::Light, ThemeMode::Dark}) {
        AppendMenuW(menu, MF_STRING | (settings_.theme == mode ? MF_CHECKED : 0),
                    kSystemTheme + static_cast<UINT>(mode), ThemeName(mode));
    }
    RECT rect{}; GetWindowRect(themeButton_, &rect);
    menuOpen_ = true;
    const auto selected = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                        rect.left, rect.bottom, 0, window_, nullptr);
    menuOpen_ = false; DestroyMenu(menu);
    if (selected) SendMessageW(window_, WM_COMMAND, selected, 0);
    if (GetForegroundWindow() != window_) Hide();
}

void MixerWindow::UpdateTrayTheme() {
    const bool light = LightTaskbar();
    if (trayLight_ == static_cast<int>(light)) return;
    const auto size = std::max(16, GetSystemMetrics(SM_CXSMICON));
    Gdiplus::Bitmap image(size, size, PixelFormat32bppARGB);
    {
        Gdiplus::Graphics graphics(&image);
        graphics.Clear(Gdiplus::Color::Transparent);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::Pen pen(DrawingColor(light ? RGB(35,35,35) : RGB(245,245,245)),
                         static_cast<float>(size)/13.0f);
        pen.SetStartCap(Gdiplus::LineCapRound); pen.SetEndCap(Gdiplus::LineCapRound);
        const float knobs[]{0.38f,0.66f,0.45f};
        for (int i = 0; i < 3; ++i) {
            const float x = static_cast<float>(size)*(0.27f+0.23f*static_cast<float>(i));
            const float y = static_cast<float>(size)*knobs[i];
            graphics.DrawLine(&pen, x,static_cast<float>(size)*0.16f,x,static_cast<float>(size)*0.84f);
            graphics.DrawLine(&pen, x-static_cast<float>(size)*0.08f,y,x+static_cast<float>(size)*0.08f,y);
        }
    }
    HICON next = nullptr;
    if (image.GetHICON(&next) != Gdiplus::Ok) return;
    const auto old = trayIcon_; trayIcon_ = next; trayLight_ = static_cast<int>(light);
    if (trayAdded_) {
        NOTIFYICONDATAW data{}; data.cbSize = sizeof(data); data.hWnd = window_;
        data.uID = 1; data.uFlags = NIF_ICON; data.hIcon = trayIcon_;
        Shell_NotifyIconW(NIM_MODIFY, &data);
    }
    if (old) DestroyIcon(old);
}

void MixerWindow::ApplyTheme() {
    const auto next = ResolvePalette(settings_.theme);
    const bool changed = !themeInitialized_ || colors_ != next;
    colors_ = next;
    if (graphicsToken_) UpdateTrayTheme();
    if (window_ && changed) {
        const auto brush = CreateSolidBrush(colors_.background);
        if (brush) { const auto old = background_; background_ = brush; if (old) DeleteObject(old); }
        const BOOL dark = colors_.dark;
        const DWORD round = 2;
        DwmSetWindowAttribute(window_, 20, &dark, sizeof(dark));
        DwmSetWindowAttribute(window_, 33, &round, sizeof(round));
        DwmSetWindowAttribute(window_, 34, &colors_.border, sizeof(colors_.border));
        RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
    }
    themeInitialized_ = true;
    if (themeButton_) {
        const auto name = std::wstring(L"Tema: ") + ThemeName(settings_.theme);
        wchar_t previous[96]{}; GetWindowTextW(themeButton_, previous, 96);
        if (name != previous) SetWindowTextW(themeButton_, name.c_str());
        else if (changed) InvalidateRect(themeButton_, nullptr, FALSE);
    }
}

void MixerWindow::ToggleOrientation() {
    settings_.vertical = !settings_.vertical;
    scrollOffset_ = 0;
    const auto result = SaveSettings(settings_);
    SyncRows(true);
    LayoutPopup();
    if (result != ERROR_SUCCESS) Feedback(L"Orientação alterada, mas não foi possível salvá-la.", true);
}
void MixerWindow::Feedback(std::wstring text, bool error) {
    feedback_ = std::move(text); feedbackError_ = error;
    if (settingsView_) {
        if (const auto label = GetDlgItem(window_, 108)) {
            SetWindowTextW(label, feedback_.c_str()); InvalidateRect(label, nullptr, TRUE);
        }
    }
    if (window_) InvalidateRect(window_, nullptr, FALSE);
}

LRESULT CALLBACK MixerWindow::WindowProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    auto* app = reinterpret_cast<MixerWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        app = static_cast<MixerWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        app->window_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    return app ? app->HandleMessage(message, wp, lp) : DefWindowProcW(hwnd, message, wp, lp);
}

LRESULT MixerWindow::HandleMessage(UINT message, WPARAM wp, LPARAM lp) {
    if (showMessage_ != 0 && message == showMessage_) { Show(); return 0; }
    if (taskbarCreated_ != 0 && message == taskbarCreated_) { AddTrayIcon(); return 0; }
    switch (message) {
    case WM_NCCALCSIZE: if (wp) return 0; break;
    case WM_NCPAINT: return 0;
    case WM_NCACTIVATE: return DefWindowProcW(window_, message, wp, -1);
    case WM_NCHITTEST: return HTCLIENT;
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
    case WM_SYSCOLORCHANGE: ApplyTheme(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: Paint(); return 0;
    case WM_SIZE: LayoutChildren(); return 0;
    case WM_DPICHANGED: if (!layingOut_) LayoutPopup(); return 0;
    case WM_ACTIVATE: if (LOWORD(wp) == WA_INACTIVE && !menuOpen_) Hide(true); return 0;
    case WM_CLOSE: Hide(); return 0;
    case WM_HOTKEY:
        if (IsWindowVisible(window_)) Hide(); else Show();
        return 0;
    case kAudioChanged: UpdateAudio(); return 0;
    case kTray:
        if (LOWORD(lp) == NIN_SELECT || LOWORD(lp) == NIN_KEYSELECT) {
            if (IsWindowVisible(window_)) Hide();
            else if (GetTickCount64() - deactivatedAt_ > 250) Show();
        } else if (LOWORD(lp) == WM_CONTEXTMENU) TrayMenu();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case kOrientation: ToggleOrientation(); break;
        case kTheme: ThemeMenu(); break;
        case kSystemTheme: case kLightTheme: case kDarkTheme: {
            settings_.theme = static_cast<ThemeMode>(LOWORD(wp) - kSystemTheme);
            const auto result = SaveSettings(settings_); ApplyTheme();
            if (result != ERROR_SUCCESS) Feedback(L"Tema alterado, mas não foi possível salvá-lo.", true);
            break;
        }
        case kSettings: ShowSettings(!settingsView_); break;
        case kClose: Hide(); break;
        case kSave: ApplySettings(); break;
        case kRetry: audio_.Refresh(); break;
        }
        return 0;
    case WM_DRAWITEM: DrawButton(*reinterpret_cast<DRAWITEMSTRUCT*>(lp)); return TRUE;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORBTN: {
        const auto dc = reinterpret_cast<HDC>(wp);
        SetBkMode(dc, TRANSPARENT); SetBkColor(dc, colors_.background);
        SetTextColor(dc, GetDlgCtrlID(reinterpret_cast<HWND>(lp)) == 108 && feedbackError_ ? colors_.error : colors_.text);
        return reinterpret_cast<LRESULT>(background_);
    }
    }
    return DefWindowProcW(window_, message, wp, lp);
}

LRESULT CALLBACK MixerWindow::ViewportProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    auto* app = reinterpret_cast<MixerWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        app = static_cast<MixerWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (!app) return DefWindowProcW(hwnd, message, wp, lp);
    switch (message) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{}; const auto dc = BeginPaint(hwnd, &paint);
        RECT rect{}; GetClientRect(hwnd, &rect); Fill(dc, rect, app->colors_.background);
        if (app->rows_.empty()) {
            Text(dc, app->boldFont_, app->colors_.text, app->audioReady_ ? L"Nenhuma saída de áudio disponível" : L"Conectando ao áudio…",
                 {app->Px(8), app->Px(22), rect.right - app->Px(8), app->Px(52)}, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            Text(dc, app->font_, app->colors_.secondary, L"Conecte um fone ou alto-falante para começar.",
                 {app->Px(8), app->Px(55), rect.right - app->Px(8), app->Px(80)}, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else if (app->rows_.size() == 1 && !app->settings_.vertical) {
            Text(dc, app->font_, app->colors_.secondary, L"Reproduza áudio em um aplicativo para vê-lo aqui.",
                 {app->Px(8), app->Px(82), rect.right - app->Px(8), app->Px(120)}, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        EndPaint(hwnd, &paint); return 0;
    }
    case WM_COMMAND: return SendMessageW(app->window_, message, wp, lp);
    case WM_DRAWITEM: return SendMessageW(app->window_, message, wp, lp);
    case WM_MOUSEWHEEL: app->Scroll(-GET_WHEEL_DELTA_WPARAM(wp) * app->Px(66) / WHEEL_DELTA); return 0;
    case WM_MOUSEHWHEEL: app->Scroll(GET_WHEEL_DELTA_WPARAM(wp) * app->Px(104) / WHEEL_DELTA); return 0;
    case WM_HSCROLL:
    case WM_VSCROLL: {
        SCROLLINFO info{}; info.cbSize = sizeof(info); info.fMask = SIF_ALL;
        GetScrollInfo(hwnd, message == WM_HSCROLL ? SB_HORZ : SB_VERT, &info);
        int next = info.nPos;
        switch (LOWORD(wp)) {
        case SB_LINEUP: next -= app->Px(32); break;
        case SB_LINEDOWN: next += app->Px(32); break;
        case SB_PAGEUP: next -= static_cast<int>(info.nPage); break;
        case SB_PAGEDOWN: next += static_cast<int>(info.nPage); break;
        case SB_THUMBTRACK: next = info.nTrackPos; break;
        case SB_TOP: next = info.nMin; break;
        case SB_BOTTOM: next = info.nMax; break;
        }
        app->Scroll(next - app->scrollOffset_); return 0;
    }
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}
} // namespace tvm
