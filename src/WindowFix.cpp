#include "WindowFix.hpp"

#include "NativePicker.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/CCEGLView.hpp>

#include <windows.h>

#include <algorithm>
#include <cstdint>

using namespace geode::prelude;

namespace {
    constexpr std::uintptr_t GLFW_WINDOW_HWND_OFFSET = 0x370;
    constexpr char MAKE_BORDERLESS_TOP_SYMBOL[] =
            "?makeBorderlessTop@CCEGLView@cocos2d@@QEAAXXZ";
    constexpr char RESIZE_WINDOW_SYMBOL[] =
            "?resizeWindow@CCEGLView@cocos2d@@QEAAXHH@Z";
    constexpr std::uintptr_t GLFW_PLATFORM_CENTER_WINDOW_RVA = 0xD6300;
    constexpr std::uintptr_t GLFW_PLATFORM_SET_WINDOW_MONITOR_RVA = 0xD6EF0;
    constexpr std::uintptr_t GLFW_PLATFORM_SET_WINDOW_POS_RVA = 0xD7300;
    constexpr std::uintptr_t GLFW_PLATFORM_SET_WINDOW_SIZE_RVA = 0xD7430;

    RECT g_windowedRect{};
    bool g_hasWindowedRect = false;

    template <class... Args>
    void suppress(Args...) {}

    HWND getNativeWindow(cocos2d::CCEGLView *view) {
        if (view && view->m_pMainWindow) {
            auto const address = reinterpret_cast<std::uintptr_t>(view->m_pMainWindow);
            auto const window = *reinterpret_cast<HWND const *>(
                address + GLFW_WINDOW_HWND_OFFSET
            );
            if (IsWindow(window)) {
                return window;
            }
        }

        return FindWindowW(nullptr, L"Geometry Dash");
    }

    bool getMonitorInfo(HWND window, MONITORINFO &info) {
        info.cbSize = sizeof(info);
        auto const monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
        return monitor && GetMonitorInfoW(monitor, &info);
    }

    bool hasArea(RECT const &rect) {
        return rect.right > rect.left && rect.bottom > rect.top;
    }

    void rememberWindowedRect(HWND window) {
        RECT rect{};
        if (GetWindowRect(window, &rect) && hasArea(rect)) {
            g_windowedRect = rect;
            g_hasWindowedRect = true;
        }
    }

    void updateRenderSize(cocos2d::CCEGLView *view, HWND window) {
        RECT client{};
        if (GetClientRect(window, &client) && client.right > 0 && client.bottom > 0) {
            view->updateWindow(client.right, client.bottom);
        }
    }

    void setModeState(
        cocos2d::CCEGLView *view,
        bool fullscreen,
        bool borderless,
        bool fix
    ) {
        view->m_bIsFullscreen = fullscreen;
        view->m_bIsBorderless = borderless;
        view->m_bIsFix = fix;

        if (auto *application = cocos2d::CCApplication::get()) {
            application->m_bFullscreen = fullscreen;
        }
    }

    void releaseWindowToManager(cocos2d::CCEGLView *view) {
        auto const window = getNativeWindow(view);
        if (!window) {
            log::warn("Unable to release the GD window to the window manager");
            return;
        }

        auto style = GetWindowLongPtrW(window, GWL_STYLE);
        style |= WS_THICKFRAME | WS_MAXIMIZEBOX | WS_MINIMIZEBOX;
        SetWindowLongPtrW(window, GWL_STYLE, style);

        auto extendedStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
        extendedStyle &= ~WS_EX_TOPMOST;
        SetWindowLongPtrW(window, GWL_EXSTYLE, extendedStyle);

        SetWindowPos(
            window,
            HWND_NOTOPMOST,
            0,
            0,
            0,
            0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER
        );

        if (!view->getIsFullscreen()) {
            rememberWindowedRect(window);
        }
    }

    RECT makeWindowedRect(
        HWND window,
        LONG_PTR style,
        LONG_PTR extendedStyle,
        int requestedWidth,
        int requestedHeight
    ) {
        MONITORINFO monitor{};
        if (!getMonitorInfo(window, monitor)) {
            return g_hasWindowedRect ? g_windowedRect : RECT{100, 100, 1380, 820};
        }

        auto const workWidth = static_cast<int>(
            monitor.rcWork.right - monitor.rcWork.left
        );
        auto const workHeight = static_cast<int>(
            monitor.rcWork.bottom - monitor.rcWork.top
        );
        auto width = requestedWidth > 0 ? requestedWidth : 1280;
        auto height = requestedHeight > 0 ? requestedHeight : 720;

        auto const maxWidth = std::max(640, workWidth * 9 / 10);
        auto const maxHeight = std::max(360, workHeight * 9 / 10);
        if (width > maxWidth || height > maxHeight) {
            auto const widthScale = static_cast<double>(maxWidth) / width;
            auto const heightScale = static_cast<double>(maxHeight) / height;
            auto const scale = std::min(widthScale, heightScale);
            width = std::max(640, static_cast<int>(width * scale));
            height = std::max(360, static_cast<int>(height * scale));
        }

        RECT rect{0, 0, width, height};
        AdjustWindowRectEx(
            &rect,
            static_cast<DWORD>(style),
            FALSE,
            static_cast<DWORD>(extendedStyle)
        );

        auto const outerWidth = rect.right - rect.left;
        auto const outerHeight = rect.bottom - rect.top;
        auto const left = monitor.rcWork.left + (workWidth - outerWidth) / 2;
        auto const top = monitor.rcWork.top + (workHeight - outerHeight) / 2;
        return RECT{left, top, left + outerWidth, top + outerHeight};
    }

    void applyMode(
        cocos2d::CCEGLView *view,
        bool fullscreen,
        int requestedWidth = 0,
        int requestedHeight = 0
    ) {
        auto const window = getNativeWindow(view);
        if (!window) {
            log::warn("Unable to apply the GD window mode: window not found");
            return;
        }

        ReleaseCapture();

        RECT target{};
        auto style = GetWindowLongPtrW(window, GWL_STYLE);
        auto extendedStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
        extendedStyle &= ~(WS_EX_TOPMOST | WS_EX_TOOLWINDOW);
        extendedStyle |= WS_EX_APPWINDOW;

        if (fullscreen) {
            MONITORINFO monitor{};
            if (!getMonitorInfo(window, monitor)) {
                log::warn("Unable to apply fullscreen: monitor not found");
                return;
            }
            target = monitor.rcMonitor;
            style &= ~(WS_CAPTION | WS_MAXIMIZE);
            style |= WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS |
                    WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
            extendedStyle &= ~(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME);
        } else {
            style &= ~(WS_POPUP | WS_MAXIMIZE);
            style |= WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
            extendedStyle |= WS_EX_WINDOWEDGE;

            if (requestedWidth <= 0 && requestedHeight <= 0 && g_hasWindowedRect) {
                target = g_windowedRect;
            } else {
                target = makeWindowedRect(
                    window,
                    style,
                    extendedStyle,
                    requestedWidth,
                    requestedHeight
                );
            }
        }

        SetWindowLongPtrW(window, GWL_STYLE, style);
        SetWindowLongPtrW(window, GWL_EXSTYLE, extendedStyle);

        SetWindowPos(
            window,
            HWND_NOTOPMOST,
            target.left,
            target.top,
            target.right - target.left,
            target.bottom - target.top,
            SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOACTIVATE | SWP_NOOWNERZORDER
        );

        updateRenderSize(view, window);
        if (!fullscreen) {
            rememberWindowedRect(window);
        }
    }

    void resizeWindowHook(cocos2d::CCEGLView *view, int width, int height) {
        if (view->getIsFullscreen()) {
            auto const window = getNativeWindow(view);
            if (window) {
                updateRenderSize(view, window);
            }
            return;
        }

        applyMode(view, false, width, height);
    }

    Result<> installCocosWindowHooks() {
        auto const cocosBase = base::getCocos();
        auto const cocos = reinterpret_cast<HMODULE>(cocosBase);
        if (!cocos) {
            return Err("Unable to locate libcocos2d.dll");
        }

        auto const borderlessTop = reinterpret_cast<void *>(
            GetProcAddress(cocos, MAKE_BORDERLESS_TOP_SYMBOL)
        );
        auto const resizeWindow = reinterpret_cast<void *>(
            GetProcAddress(cocos, RESIZE_WINDOW_SYMBOL)
        );
        if (!borderlessTop || !resizeWindow) {
            return Err("Unable to resolve Cocos window-management functions");
        }

        GEODE_UNWRAP(Mod::get()->hook(
            borderlessTop,
            &suppress<cocos2d::CCEGLView *>,
            "Suppress Cocos topmost enforcement"
        ));
        GEODE_UNWRAP(Mod::get()->hook(
            resizeWindow,
            &resizeWindowHook,
            "Replace Cocos window resizing"
        ));
        GEODE_UNWRAP(Mod::get()->hook(
            reinterpret_cast<void *>(cocosBase + GLFW_PLATFORM_CENTER_WINDOW_RVA),
            &suppress<GLFWwindow *>,
            "Suppress GLFW native window centering"
        ));
        GEODE_UNWRAP(Mod::get()->hook(
            reinterpret_cast<void *>(cocosBase + GLFW_PLATFORM_SET_WINDOW_MONITOR_RVA),
            &suppress<GLFWwindow *, GLFWmonitor *, int, int, int, int>,
            "Suppress GLFW monitor ownership"
        ));
        GEODE_UNWRAP(Mod::get()->hook(
            reinterpret_cast<void *>(cocosBase + GLFW_PLATFORM_SET_WINDOW_POS_RVA),
            &suppress<GLFWwindow *, int, int>,
            "Suppress GLFW native window positioning"
        ));
        GEODE_UNWRAP(Mod::get()->hook(
            reinterpret_cast<void *>(cocosBase + GLFW_PLATFORM_SET_WINDOW_SIZE_RVA),
            &suppress<GLFWwindow *, int, int>,
            "Suppress GLFW native window sizing"
        ));
        return Ok();
    }
}

class $modify(GDLinuxWindowFix, cocos2d::CCEGLView) {
    void onGLFWWindowFocus(GLFWwindow *window, int focused) {
        if (!gdlinux::isRunningUnderWine()) {
            cocos2d::CCEGLView::onGLFWWindowFocus(window, focused);
            return;
        }

        auto const fix = this->m_bIsFix;
        this->m_bIsFix = false;
        cocos2d::CCEGLView::onGLFWWindowFocus(window, focused);
        this->m_bIsFix = fix;
    }

    void toggleFullScreen(bool fullscreen, bool borderless, bool fix) {
        if (!gdlinux::isRunningUnderWine()) {
            cocos2d::CCEGLView::toggleFullScreen(fullscreen, borderless, fix);
            return;
        }

        if (fullscreen && !this->getIsFullscreen()) {
            if (auto const window = getNativeWindow(this)) {
                rememberWindowedRect(window);
            }
        }
        setModeState(this, fullscreen, borderless, fix);
        applyMode(this, fullscreen);
    }
};

Result<> gdlinux::initializeWindowFix() {
    if (!isRunningUnderWine()) {
        return Ok();
    }

    GEODE_UNWRAP(installCocosWindowHooks());

    queueInMainThread([] {
        auto *view = cocos2d::CCEGLView::get();
        if (!view) {
            log::warn("Unable to initialize Wine window integration: CCEGLView not found");
            return;
        }

        releaseWindowToManager(view);
    });
    return Ok();
}
