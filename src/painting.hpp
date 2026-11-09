#pragma once
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>

namespace tvm {
// Offscreen opaque paint; the target's invalid-region/child clipping is retained
// by the final blit. No clear/text/shape intermediate frame reaches the screen.
class PaintBuffer {
public:
    PaintBuffer(HDC target, RECT bounds) : target_(target), bounds_(bounds) {
        dc_ = CreateCompatibleDC(target);
        bitmap_ = CreateCompatibleBitmap(target, std::max(1L, bounds.right), std::max(1L, bounds.bottom));
        if (dc_ && bitmap_) old_ = SelectObject(dc_, bitmap_);
    }
    ~PaintBuffer() {
        if (old_) {
            BitBlt(target_, bounds_.left, bounds_.top, bounds_.right - bounds_.left,
                   bounds_.bottom - bounds_.top, dc_, bounds_.left, bounds_.top, SRCCOPY);
            SelectObject(dc_, old_);
        }
        if (bitmap_) DeleteObject(bitmap_);
        if (dc_) DeleteDC(dc_);
    }
    HDC get() const { return old_ ? dc_ : target_; }
private:
    HDC target_ = nullptr, dc_ = nullptr;
    RECT bounds_{};
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ old_ = nullptr;
};

inline Gdiplus::Color DrawingColor(COLORREF color) {
    return Gdiplus::Color(255, GetRValue(color), GetGValue(color), GetBValue(color));
}
inline void SmoothRounded(HDC dc, RECT rect, int diameter, COLORREF color) {
    const auto x = static_cast<float>(rect.left), y = static_cast<float>(rect.top);
    const auto w = static_cast<float>(rect.right - rect.left), h = static_cast<float>(rect.bottom - rect.top);
    if (w <= 0 || h <= 0) return;
    const float d = std::min({static_cast<float>(diameter), w, h});
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    Gdiplus::SolidBrush brush(DrawingColor(color));
    if (w == h && d == w) { graphics.FillEllipse(&brush, x, y, w, h); return; }
    Gdiplus::GraphicsPath path;
    path.AddArc(x, y, d, d, 180.0f, 90.0f);
    path.AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
    path.AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
    path.AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
    graphics.FillPath(&brush, &path);
}
} // namespace tvm
