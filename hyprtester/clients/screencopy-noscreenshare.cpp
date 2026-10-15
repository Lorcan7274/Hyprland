#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <print>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/poll.h>
#include <unistd.h>
#include <vector>

#include <wayland-client.h>
#include <wayland.hpp>
#include <wlr-screencopy-unstable-v1.hpp>
#include <xdg-shell.hpp>

#include <hyprutils/memory/Casts.hpp>
#include <hyprutils/memory/SharedPtr.hpp>
#include <hyprutils/os/FileDescriptor.hpp>

using namespace Hyprutils::Memory;
using namespace Hyprutils::OS;

// Maps a solid red toplevel, captures the output it is shown on and checks whether the window
// is in the capture ("visible") or replaced by the desktop behind it ("hidden").

constexpr int      MAX_WIDTH  = 1920;
constexpr int      MAX_HEIGHT = 1080;
constexpr uint32_t RED        = 0xFFFF0000; // wl_shm XRGB8888: B, G, R, X in memory

struct SState {
    wl_display*                               display = nullptr;
    CSharedPointer<CCWlRegistry>              registry;
    CSharedPointer<CCWlCompositor>            compositor;
    CSharedPointer<CCWlShm>                   shm;
    CSharedPointer<CCXdgWmBase>               wm;
    CSharedPointer<CCZwlrScreencopyManagerV1> screencopy;
    std::vector<CSharedPointer<CCWlOutput>>   outputs;

    CSharedPointer<CCWlSurface>               surface;
    CSharedPointer<CCXdgSurface>              xdgSurface;
    CSharedPointer<CCXdgToplevel>             toplevel;
    CSharedPointer<CCWlShmPool>               pool;
    CSharedPointer<CCWlBuffer>                buffer;
    CSharedPointer<CCWlCallback>              frameCallback;
    uint32_t*                                 pixels = nullptr;
    int                                       width = 0, height = 0;
    int                                       pendingWidth = 0, pendingHeight = 0;
    wl_proxy*                                 output    = nullptr;
    bool                                      presented = false;
    bool                                      closed    = false;

    CSharedPointer<CCZwlrScreencopyFrameV1>   frame;
    CSharedPointer<CCWlShmPool>               capturePool;
    CSharedPointer<CCWlBuffer>                captureBuffer;
    uint8_t*                                  captureData   = nullptr;
    size_t                                    captureSize   = 0;
    uint32_t                                  captureFormat = 0, captureWidth = 0, captureHeight = 0, captureStride = 0;
    bool                                      captureDone = false, captureFailed = false;
};

static bool bindGlobals(SState& state) {
    state.registry = makeShared<CCWlRegistry>(rc<wl_proxy*>(wl_display_get_registry(state.display)));
    state.registry->setGlobal([&state](CCWlRegistry* r, uint32_t name, const char* iface, uint32_t ver) {
        const std::string_view IFACE    = iface;
        auto* const            REGISTRY = rc<wl_registry*>(r->resource());

        if (IFACE == "wl_compositor")
            state.compositor = makeShared<CCWlCompositor>(rc<wl_proxy*>(wl_registry_bind(REGISTRY, name, &wl_compositor_interface, std::min(ver, 6U))));
        else if (IFACE == "wl_shm")
            state.shm = makeShared<CCWlShm>(rc<wl_proxy*>(wl_registry_bind(REGISTRY, name, &wl_shm_interface, 1)));
        else if (IFACE == "xdg_wm_base")
            state.wm = makeShared<CCXdgWmBase>(rc<wl_proxy*>(wl_registry_bind(REGISTRY, name, &xdg_wm_base_interface, 1)));
        else if (IFACE == "wl_output")
            state.outputs.emplace_back(makeShared<CCWlOutput>(rc<wl_proxy*>(wl_registry_bind(REGISTRY, name, &wl_output_interface, std::min(ver, 4U)))));
        else if (IFACE == "zwlr_screencopy_manager_v1" && ver >= 3)
            state.screencopy = makeShared<CCZwlrScreencopyManagerV1>(rc<wl_proxy*>(wl_registry_bind(REGISTRY, name, &zwlr_screencopy_manager_v1_interface, 3)));
    });
    state.registry->setGlobalRemove([](CCWlRegistry*, uint32_t) {});

    if (wl_display_roundtrip(state.display) < 0)
        return false;

    return state.compositor && state.shm && state.wm && state.screencopy && !state.outputs.empty();
}

static bool attachWindowBuffer(SState& state, int width, int height) {
    if (width <= 0 || height <= 0 || width > MAX_WIDTH || height > MAX_HEIGHT)
        return false;

    state.width  = width;
    state.height = height;
    std::fill_n(state.pixels, sc<size_t>(width) * height, RED);

    state.buffer = makeShared<CCWlBuffer>(state.pool->sendCreateBuffer(0, width, height, width * 4, WL_SHM_FORMAT_XRGB8888));
    state.xdgSurface->sendSetWindowGeometry(0, 0, width, height);
    state.surface->sendAttach(state.buffer.get(), 0, 0);
    state.surface->sendDamage(0, 0, width, height);

    state.presented     = false;
    state.frameCallback = makeShared<CCWlCallback>(state.surface->sendFrame());
    state.frameCallback->setDone([&state](CCWlCallback*, uint32_t) { state.presented = true; });
    state.surface->sendCommit();
    return true;
}

static bool createWindow(SState& state, const std::string& appID) {
    constexpr size_t      BYTES = sc<size_t>(MAX_WIDTH) * MAX_HEIGHT * 4;
    const CFileDescriptor FD{memfd_create("screencopy-noscreenshare", MFD_CLOEXEC)};
    if (FD.get() < 0 || ftruncate(FD.get(), sc<off_t>(BYTES)) < 0)
        return false;

    state.pixels = sc<uint32_t*>(mmap(nullptr, BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, FD.get(), 0));
    if (state.pixels == MAP_FAILED)
        return false;

    state.pool       = makeShared<CCWlShmPool>(state.shm->sendCreatePool(FD.get(), sc<int32_t>(BYTES)));
    state.surface    = makeShared<CCWlSurface>(state.compositor->sendCreateSurface());
    state.xdgSurface = makeShared<CCXdgSurface>(state.wm->sendGetXdgSurface(state.surface->resource()));
    state.toplevel   = makeShared<CCXdgToplevel>(state.xdgSurface->sendGetToplevel());

    state.wm->setPing([&state](CCXdgWmBase*, uint32_t serial) { state.wm->sendPong(serial); });
    state.surface->setEnter([&state](CCWlSurface*, wl_proxy* output) { state.output = output; });
    state.surface->setLeave([&state](CCWlSurface*, wl_proxy* output) {
        if (state.output == output)
            state.output = nullptr;
    });
    state.toplevel->setConfigure([&state](CCXdgToplevel*, int32_t w, int32_t h, wl_array*) {
        state.pendingWidth  = w > 0 ? w : 400;
        state.pendingHeight = h > 0 ? h : 300;
    });
    state.toplevel->setClose([&state](CCXdgToplevel*) { state.closed = true; });
    state.xdgSurface->setConfigure([&state](CCXdgSurface*, uint32_t serial) {
        state.xdgSurface->sendAckConfigure(serial);
        if (state.pendingWidth == state.width && state.pendingHeight == state.height) {
            state.surface->sendCommit();
            return;
        }
        if (!attachWindowBuffer(state, state.pendingWidth, state.pendingHeight))
            state.closed = true;
    });

    state.toplevel->sendSetAppId(appID.c_str());
    state.toplevel->sendSetTitle("Screencopy noscreenshare test client");
    state.surface->sendCommit();
    return true;
}

static bool createCaptureBuffer(SState& state) {
    state.captureSize = sc<size_t>(state.captureStride) * state.captureHeight;
    if (state.captureSize == 0)
        return false;

    const CFileDescriptor FD{memfd_create("screencopy-noscreenshare-capture", MFD_CLOEXEC)};
    if (FD.get() < 0 || ftruncate(FD.get(), sc<off_t>(state.captureSize)) < 0)
        return false;

    state.captureData = sc<uint8_t*>(mmap(nullptr, state.captureSize, PROT_READ | PROT_WRITE, MAP_SHARED, FD.get(), 0));
    if (state.captureData == MAP_FAILED)
        return false;

    state.capturePool   = makeShared<CCWlShmPool>(state.shm->sendCreatePool(FD.get(), sc<int32_t>(state.captureSize)));
    state.captureBuffer = makeShared<CCWlBuffer>(
        state.capturePool->sendCreateBuffer(0, sc<int32_t>(state.captureWidth), sc<int32_t>(state.captureHeight), sc<int32_t>(state.captureStride), state.captureFormat));
    return state.captureBuffer && state.captureBuffer->resource();
}

static bool requestCapture(SState& state) {
    state.frame = makeShared<CCZwlrScreencopyFrameV1>(state.screencopy->sendCaptureOutput(0 /* overlay_cursor */, state.output));
    if (!state.frame || !state.frame->resource())
        return false;

    state.frame->setBuffer([&state](CCZwlrScreencopyFrameV1*, uint32_t fmt, uint32_t w, uint32_t h, uint32_t stride) {
        state.captureFormat = fmt;
        state.captureWidth  = w;
        state.captureHeight = h;
        state.captureStride = stride;
    });
    state.frame->setFlags([](CCZwlrScreencopyFrameV1*, uint32_t) {});
    state.frame->setDamage([](CCZwlrScreencopyFrameV1*, uint32_t, uint32_t, uint32_t, uint32_t) {});
    state.frame->setLinuxDmabuf([](CCZwlrScreencopyFrameV1*, uint32_t, uint32_t, uint32_t) {});
    state.frame->setReady([&state](CCZwlrScreencopyFrameV1*, uint32_t, uint32_t, uint32_t) { state.captureDone = true; });
    state.frame->setFailed([&state](CCZwlrScreencopyFrameV1*) {
        std::println(stderr, "capture failed (server sent 'failed')");
        state.captureFailed = true;
    });
    state.frame->setBufferDone([&state](CCZwlrScreencopyFrameV1*) {
        if (!createCaptureBuffer(state)) {
            std::println(stderr, "failed to allocate capture buffer");
            state.captureFailed = true;
            return;
        }
        state.frame->sendCopy(rc<wl_proxy*>(state.captureBuffer->resource()));
    });

    return true;
}

template <typename F>
static bool dispatchUntil(SState& state, F&& done, std::chrono::seconds timeout) {
    const auto DEADLINE = std::chrono::steady_clock::now() + timeout;
    while (!done()) {
        if (state.closed)
            return false;

        while (wl_display_prepare_read(state.display) != 0) {
            if (wl_display_dispatch_pending(state.display) < 0)
                return false;
            if (done())
                return true;
        }

        if (wl_display_flush(state.display) < 0 && errno != EAGAIN) {
            wl_display_cancel_read(state.display);
            return false;
        }

        const auto NOW = std::chrono::steady_clock::now();
        if (NOW >= DEADLINE) {
            wl_display_cancel_read(state.display);
            return false;
        }

        pollfd    pfd      = {.fd = wl_display_get_fd(state.display), .events = POLLIN, .revents = 0};
        const int POLL_RET = poll(&pfd, 1, sc<int>(std::chrono::duration_cast<std::chrono::milliseconds>(DEADLINE - NOW).count()));
        if (POLL_RET <= 0) {
            wl_display_cancel_read(state.display);
            if (POLL_RET < 0 && errno == EINTR)
                continue;
            return false;
        }

        if (wl_display_read_events(state.display) < 0 || wl_display_dispatch_pending(state.display) < 0)
            return false;
    }
    return true;
}

static int analyzeCapture(SState& state, bool hidden) {
    const bool BGRX = state.captureFormat == WL_SHM_FORMAT_XRGB8888 || state.captureFormat == WL_SHM_FORMAT_ARGB8888;
    const bool RGBX = state.captureFormat == WL_SHM_FORMAT_XBGR8888 || state.captureFormat == WL_SHM_FORMAT_ABGR8888;
    if (!BGRX && !RGBX) {
        std::println(stderr, "unsupported capture format {:#x}", state.captureFormat);
        return 2;
    }

    const auto NEAR = [](uint8_t value, uint8_t target) { return value >= target - 4 && value <= target + 4; };

    size_t     red = 0, black = 0, background = 0;
    for (uint32_t y = 0; y < state.captureHeight; ++y) {
        const uint8_t* ROW = state.captureData + sc<size_t>(y) * state.captureStride;
        for (uint32_t x = 0; x < state.captureWidth; ++x) {
            const uint8_t* PX = ROW + sc<size_t>(x) * 4;
            const uint8_t  R = BGRX ? PX[2] : PX[0], G = PX[1], B = BGRX ? PX[0] : PX[2];

            if (R >= 250 && G <= 5 && B <= 5)
                ++red;
            else if (R <= 2 && G <= 2 && B <= 2)
                ++black;
            else if (NEAR(R, 0x11) && NEAR(G, 0x22) && NEAR(B, 0x33)) // misc:background_color set by the test
                ++background;
        }
    }

    const size_t WINDOW = sc<size_t>(state.width) * state.height;
    const size_t SCREEN = sc<size_t>(state.captureWidth) * state.captureHeight;
    std::println(stderr, "capture {}x{}: window {}x{}, red {}, black {}, background {}", state.captureWidth, state.captureHeight, state.width, state.height, red, black,
                 background);

    if (hidden) {
        if (red != 0) {
            std::println(stderr, "noscreenshare window pixels are in the capture");
            return 1;
        }
        if (black >= WINDOW / 10) {
            std::println(stderr, "noscreenshare window was replaced with a black box");
            return 1;
        }
        if (background + WINDOW / 10 < SCREEN) {
            std::println(stderr, "desktop behind the noscreenshare window is not in the capture");
            return 1;
        }
        return 0;
    }

    if (red < WINDOW * 8 / 10) {
        std::println(stderr, "window pixels are missing from the capture");
        return 1;
    }
    return 0;
}

static void disconnect(SState& state) {
    const auto DISPLAY = state.display;
    if (!DISPLAY)
        return;

    state.frame.reset();
    state.captureBuffer.reset();
    state.capturePool.reset();
    state.frameCallback.reset();
    state.buffer.reset();
    state.toplevel.reset();
    state.xdgSurface.reset();
    state.surface.reset();
    state.pool.reset();
    state.outputs.clear();
    state.screencopy.reset();
    state.wm.reset();
    state.shm.reset();
    state.compositor.reset();
    state.registry.reset();

    if (state.captureData && state.captureData != MAP_FAILED)
        munmap(state.captureData, state.captureSize);
    if (state.pixels && state.pixels != MAP_FAILED)
        munmap(state.pixels, sc<size_t>(MAX_WIDTH) * MAX_HEIGHT * 4);

    state.display = nullptr;
    wl_display_flush(DISPLAY);
    wl_display_disconnect(DISPLAY);
}

int main(int argc, char** argv) {
    const std::string_view MODE = argc == 3 ? argv[2] : "";
    if (MODE != "visible" && MODE != "hidden") {
        std::println(stderr, "usage: screencopy-noscreenshare <app-id> <visible|hidden>");
        return 2;
    }

    SState state;
    state.display = wl_display_connect(nullptr);
    if (!state.display) {
        std::println(stderr, "failed to connect to Wayland display");
        return 2;
    }

    int result = 2;
    if (!bindGlobals(state))
        std::println(stderr, "wl_compositor, wl_shm, wl_output, xdg_wm_base or zwlr_screencopy_manager_v1 (v3) unavailable");
    else if (!createWindow(state, argv[1]))
        std::println(stderr, "failed to create the window");
    else if (!dispatchUntil(state, [&state] { return state.presented && state.output; }, std::chrono::seconds(5)))
        std::println(stderr, "window was not presented on an output");
    else if (wl_display_roundtrip(state.display) < 0 || !requestCapture(state))
        std::println(stderr, "failed to request an output capture");
    else if (!dispatchUntil(state, [&state] { return state.captureDone || state.captureFailed; }, std::chrono::seconds(5)) || state.captureFailed)
        std::println(stderr, "did not receive a captured frame");
    else
        result = analyzeCapture(state, MODE == "hidden");

    disconnect(state);
    return result;
}
