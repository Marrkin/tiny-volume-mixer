#include "painting.hpp"
#include "theme.hpp"
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {
int failures = 0;
void Check(bool value, const char* label) {
    if (!value) { ++failures; std::cerr << "FAIL: " << label << '\n'; }
}
double Luminance(COLORREF color) {
    const auto channel = [](BYTE c) { const auto x = c/255.0; return x <= 0.04045 ? x/12.92 : std::pow((x+0.055)/1.055,2.4); };
    return 0.2126*channel(GetRValue(color))+0.7152*channel(GetGValue(color))+0.0722*channel(GetBValue(color));
}
double Contrast(COLORREF a, COLORREF b) {
    const auto x = Luminance(a), y = Luminance(b);
    return (std::max(x,y)+0.05)/(std::min(x,y)+0.05);
}
std::vector<DWORD> Circle() {
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = 40; info.bmiHeader.biHeight = -40;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    const auto dc = CreateCompatibleDC(nullptr);
    const auto bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    const auto old = SelectObject(dc, bitmap);
    RECT bounds{0,0,40,40}; const auto background = CreateSolidBrush(RGB(24,24,24));
    FillRect(dc, &bounds, background); DeleteObject(background);
    tvm::SmoothRounded(dc, {5,5,35,35}, 30, RGB(248,248,248));
    GdiFlush();
    const auto* data = static_cast<DWORD*>(pixels);
    std::vector<DWORD> result(data,data+1600);
    SelectObject(dc, old); DeleteObject(bitmap); DeleteDC(dc);
    return result;
}
}
int main() {
    Gdiplus::GdiplusStartupInput input; ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token,&input,nullptr) != Gdiplus::Ok) return EXIT_FAILURE;
    const auto pixels = Circle();
    const auto intermediate = std::count_if(pixels.begin(),pixels.end(),[](DWORD color) {
        const auto value = color & 0xFF; return value > 24 && value < 248;
    });
    Check(intermediate > 30, "curved edges have intermediate coverage pixels, not a binary polygon outline");
    Check((pixels[20*40+20] & 0xFFFFFF) == 0xF8F8F8, "circle center is opaque");
    Check((pixels[0] & 0xFFFFFF) == 0x181818, "paint preserves surrounding background");
    Check(pixels == Circle(), "repeated frames produce identical pixels");
    for (const auto mode : {tvm::ThemeMode::Light,tvm::ThemeMode::Dark}) {
        const auto colors = tvm::ResolvePalette(mode);
        Check(Contrast(colors.text,colors.background) >= 4.5, "theme text has readable contrast");
        Check(Contrast(colors.secondary,colors.background) >= 4.5, "secondary text has readable contrast");
        Check(Contrast(colors.thumb,colors.track) >= 3.0, "slider thumb remains distinct from its track");
    }
    Gdiplus::GdiplusShutdown(token);
    if (failures) return EXIT_FAILURE;
    std::cout << "Antialiasing, deterministic painting, and theme contrast checks passed.\n";
    return EXIT_SUCCESS;
}
