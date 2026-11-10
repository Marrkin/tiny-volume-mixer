#include "model.hpp"
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
int failures = 0;
void Check(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
tvm::SessionInfo Session(std::wstring id, std::wstring path, std::wstring name,
                          unsigned pid, float volume, bool muted = false) {
    return {std::move(id), std::move(path), std::move(name), pid, volume, muted, true, false};
}
}

int main() {
    using namespace tvm;
    Check(GroupSessions({}).empty(), "an empty device has no application rows");
    auto browser1 = Session(L"a", L"C:\\Apps\\Browser.exe", L"Browser", 10, 0.45f);
    auto browser2 = Session(L"b", L"c:\\apps\\browser.EXE", L"Browser", 20, 0.80f, true);
    auto music = Session(L"c", L"C:\\Music.exe", L"Music", 30, 0.32f);
    auto groups = GroupSessions({music, browser1, browser2});
    Check(groups.size() == 2, "multiple processes of one executable share a control");
    Check(groups[0].name == L"Browser" && groups[1].name == L"Music", "rows have stable alphabetical order");
    Check(groups[0].sessionCount == 2, "all audio sessions are retained");
    Check(VolumePercent(groups[0].volume) == 80, "a mixed group displays its highest level");
    Check(groups[0].mixed && !groups[0].muted, "a partly muted group remains audible");
    browser1.muted = true;
    groups = GroupSessions({browser1, browser2});
    Check(groups[0].muted, "mute is true only when every session is muted");
    browser1.volume = browser2.volume;
    groups = GroupSessions({browser1, browser2});
    Check(!groups[0].mixed, "equal volumes and mute flags are not mixed");
    browser1.active = false; browser2.active = true;
    groups = GroupSessions({browser1, browser2});
    Check(groups[0].active, "one active session makes the group active");
    auto protected1 = Session(L"p1", L"", L"Protected", 50, 0.5f);
    auto protected2 = Session(L"p2", L"", L"Protected", 60, 0.5f);
    Check(GroupSessions({protected1, protected2}).size() == 2, "missing paths do not merge unrelated processes");
    protected2.processId = 50;
    Check(GroupSessions({protected1, protected2}).size() == 1, "missing paths still group the same process");
    protected1.processId = protected2.processId = 0;
    Check(GroupSessions({protected1, protected2}).size() == 2, "anonymous sessions remain independent");
    auto system = Session(L"sys", L"", L"Sons do sistema", 0, 1.0f);
    system.systemSounds = true;
    groups = GroupSessions({browser1, system});
    Check(groups[0].id == L"@system", "system sounds are identified separately and sorted first");
    auto otherInstall = browser1;
    otherInstall.executable = L"D:\\Other\\Browser.exe";
    Check(GroupId(otherInstall) != GroupId(browser1), "different installations are not merged by filename");
    auto packaged = browser1;
    packaged.appId = L"Example.Package!Application";
    otherInstall.appId = L"example.package!application";
    Check(GroupId(packaged) == GroupId(otherInstall), "packaged runtime and host share their Windows application identity");
    Check(GroupSessions({packaged,otherInstall})[0].appId == packaged.appId, "application identity is retained for the shell icon");
    Check(VolumePercent(AppSliderVolume(1.0f, 0.5f)) == 50,
          "a full-volume app follows the output slider down to 50");
    Check(VolumePercent(AppSliderVolume(0.4f, 0.5f)) == 20,
          "lowering the output preserves the balance between apps");
    Check(VolumePercent(SessionVolumeForSlider(0.2f, 0.5f)) == 40,
          "an app slider at 20 with output at 50 writes session volume 40");
    const auto relative = SessionVolumeForSlider(0.23f, 0.5f);
    Check(VolumePercent(AppSliderVolume(relative, 0.5f)) == 23,
          "displayed levels round trip through the session control");
    Check(AppSliderVolume(0.7f, 0.0f) == 0.0f &&
          VolumePercent(AppSliderVolume(0.7f, 1.0f)) == 70,
          "taking the output to zero and back retains session ratios");
    Check(SessionVolumeForSlider(0.0f, 0.0f) == 0.0f,
          "a silent output never causes division by zero");
    const auto held = SessionVolumeForSlider(AppSliderVolume(0.4f, 0.5f), 0.8f);
    Check(VolumePercent(AppSliderVolume(held, 0.8f)) == 20,
          "raising one app past the output leaves another app at its old level");
    Check(SessionVolumeForSlider(0.8f, 0.8f) == 1.0f,
          "the app that raises the output reaches that output's new level");
    browser1.volume = 0.4f; browser1.outputVolume = 0.5f;
    browser2.volume = 0.25f; browser2.outputVolume = 0.8f;
    browser1.muted = browser2.muted = false;
    groups = GroupSessions({browser1, browser2});
    Check(VolumePercent(groups[0].volume) == 20 && !groups[0].mixed,
          "sessions on different outputs group by their displayed level");
    browser2.outputVolume = 0.4f;
    groups = GroupSessions({browser1, browser2});
    Check(VolumePercent(groups[0].volume) == 20 && groups[0].mixed,
          "a mixed group displays the highest output-adjusted session level");
    Check(AppSliderVolume(2.0f, 0.5f) == 0.5f && SessionVolumeForSlider(2.0f, 0.5f) == 1.0f,
          "linked slider conversions stay in range");
    Check(AppSliderVolume(std::numeric_limits<float>::quiet_NaN(), 1.0f) == 0.0f &&
          SessionVolumeForSlider(1.0f, std::numeric_limits<float>::infinity()) == 0.0f,
          "linked slider conversions reject non-finite values");
    Check(VolumePercent(0.725f) == 73, "percentages round consistently");
    Check(VolumePercent(-1.0f) == 0 && VolumePercent(2.0f) == 100, "percentages are bounded");
    Check(VolumePercent(std::numeric_limits<float>::quiet_NaN()) == 0, "invalid volume data is safe");
    Check(VolumePercent(std::numeric_limits<float>::infinity()) == 0, "infinite volume data is safe");
    if (failures) return EXIT_FAILURE;
    std::cout << "All model checks passed.\n";
    return EXIT_SUCCESS;
}
