#include "../../hyprctlCompat.hpp"
#include "../shared.hpp"
#include "build.hpp"
#include "tests.hpp"

#include <format>
#include <string>

#include <hyprutils/os/Process.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>

using namespace Hyprutils::OS;
using namespace Hyprutils::Utils;

static int runScreencopyClient(const std::string& appID, const std::string& mode) {
    CProcess process(std::format("{}/screencopy-noscreenshare", binaryDir), {appID, mode});
    process.addEnv("WAYLAND_DISPLAY", WLDISPLAY);

    if (!process.runSync()) {
        NLog::log("{}Failed to run screencopy-noscreenshare helper", Colors::RED);
        return -1;
    }

    return process.exitCode();
}

TEST_CASE(noScreenShareWindowIsExcludedFromOutputCapture) {
    CScopeGuard guard([] {
        Tests::killAllWindows();
        getFromSocket("/eval hl.config({ misc = { disable_hyprland_logo = false, background_color = 'rgb(111111)' } })");
    });

    // a flat background makes the desktop behind the window recognizable in the capture
    OK(getFromSocket("/eval hl.config({ misc = { disable_hyprland_logo = true, background_color = 'rgb(112233)' } })"));
    OK(getFromSocket("/eval hl.window_rule({ match = { class = 'screencopy-noscreenshare-hidden' }, no_screen_share = true })"));

    Tests::killAllWindows();

    // control: without the rule the window's pixels are captured
    ASSERT(runScreencopyClient("screencopy-noscreenshare-visible", "visible"), 0);
    Tests::waitUntilWindowsN(0);

    // with the rule the capture shows the desktop behind the window, not its pixels and not a black box
    ASSERT(runScreencopyClient("screencopy-noscreenshare-hidden", "hidden"), 0);
}
