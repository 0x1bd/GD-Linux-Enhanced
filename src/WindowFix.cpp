#include "WindowFix.hpp"

#include "Wine.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/CCEGLView.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/loader/SettingV3.hpp>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <mutex>

using namespace geode::prelude;

namespace {
    constexpr std::uintptr_t kGlfwWindowHwndOffset = 0x370;
    constexpr char kMakeBorderlessTopSymbol[] =
        "?makeBorderlessTop@CCEGLView@cocos2d@@QEAAXXZ";
    constexpr char kResizeWindowSymbol[] =
        "?resizeWindow@CCEGLView@cocos2d@@QEAAXHH@Z";
    constexpr std::uintptr_t kGlfwPlatformCenterWindowRva = 0xD6300;
    constexpr std::uintptr_t kGlfwPlatformSetWindowMonitorRva = 0xD6EF0;
    constexpr std::uintptr_t kGlfwPlatformSetWindowPosRva = 0xD7300;
    constexpr std::uintptr_t kGlfwPlatformSetWindowSizeRva = 0xD7430;

    std::optional<RECT> g_windowedRect;
    std::mutex g_windowedRectMutex;
    std::atomic_uint g_nativeDialogDepth = 0;
    std::atomic_bool g_minimizeNativeDialogs = false;

    bool nativeDialogActive() {
        return g_nativeDialogDepth.load(std::memory_order_acquire) != 0;
    }

    template <class... Args>
    void suppress(Args...) {}

    HWND readGlfwWindowHandle(GLFWwindow* glfwWindow) {
        if (!glfwWindow) {
            return nullptr;
        }

        auto const address = reinterpret_cast<std::uintptr_t>(glfwWindow) + kGlfwWindowHwndOffset;
        HWND window = nullptr;
        SIZE_T bytesRead = 0;
        if (!ReadProcessMemory(
                GetCurrentProcess(),
                reinterpret_cast<void const*>(address),
                &window,
                sizeof(window),
                &bytesRead
            ) || bytesRead != sizeof(window)) {
            return nullptr;
        }

        return IsWindow(window) ? window : nullptr;
    }

    BOOL CALLBACK findProcessWindowCallback(HWND window, LPARAM data) {
        DWORD processId = 0;
        GetWindowThreadProcessId(window, &processId);
        if (
            processId != GetCurrentProcessId() ||
            !IsWindowVisible(window) ||
            GetWindow(window, GW_OWNER) != nullptr
        ) {
            return TRUE;
        }

        auto* result = reinterpret_cast<HWND*>(data);
        *result = window;
        return FALSE;
    }

    HWND findProcessWindow() {
        HWND window = nullptr;
        EnumWindows(&findProcessWindowCallback, reinterpret_cast<LPARAM>(&window));
        return window;
    }

    HWND getNativeWindow(cocos2d::CCEGLView* view) {
        if (view) {
            if (auto const window = readGlfwWindowHandle(view->m_pMainWindow)) {
                return window;
            }
        }
        return findProcessWindow();
    }

    std::optional<MONITORINFO> monitorInfoForWindow(HWND window) {
        auto const monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
        if (!monitor) {
            return std::nullopt;
        }

        MONITORINFO info{};
        info.cbSize = sizeof(info);
        if (!GetMonitorInfoW(monitor, &info)) {
            return std::nullopt;
        }
        return info;
    }

    bool hasArea(RECT const& rect) {
        return rect.right > rect.left && rect.bottom > rect.top;
    }

    void rememberWindowedRect(HWND window) {
        RECT rect{};
        if (!GetWindowRect(window, &rect) || !hasArea(rect)) {
            return;
        }

        std::lock_guard lock(g_windowedRectMutex);
        g_windowedRect = rect;
    }

    std::optional<RECT> savedWindowedRect() {
        std::lock_guard lock(g_windowedRectMutex);
        return g_windowedRect;
    }

    void updateRenderSize(cocos2d::CCEGLView* view, HWND window) {
        RECT client{};
        if (GetClientRect(window, &client) && client.right > 0 && client.bottom > 0) {
            view->updateWindow(client.right, client.bottom);
        }
    }

    void setModeState(
        cocos2d::CCEGLView* view,
        bool fullscreen,
        bool borderless,
        bool fix
    ) {
        view->m_bIsFullscreen = fullscreen;
        view->m_bIsBorderless = borderless;
        view->m_bIsFix = fix;

        if (auto* application = cocos2d::CCApplication::get()) {
            application->m_bFullscreen = fullscreen;
        }
    }

    bool gameWantsFullscreen(cocos2d::CCEGLView* view) {
        if (!view) {
            return false;
        }

        auto fullscreen = view->getIsFullscreen();
        if (auto* application = cocos2d::CCApplication::get()) {
            fullscreen = fullscreen || application->m_bFullscreen;
        }
        return fullscreen;
    }

    void releaseWindowToManager(cocos2d::CCEGLView* view) {
        auto const window = getNativeWindow(view);
        if (!window) {
            log::warn("Unable to release the Geometry Dash window to the window manager");
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
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_FRAMECHANGED
        );

        if (!view->getIsFullscreen()) {
            rememberWindowedRect(window);
        }
    }

    RECT fallbackWindowedRect() {
        return savedWindowedRect().value_or(RECT{100, 100, 1380, 820});
    }

    RECT makeWindowedRect(
        HWND window,
        LONG_PTR style,
        LONG_PTR extendedStyle,
        int requestedWidth,
        int requestedHeight
    ) {
        auto const monitor = monitorInfoForWindow(window);
        if (!monitor) {
            return fallbackWindowedRect();
        }

        auto const workWidth = static_cast<int>(monitor->rcWork.right - monitor->rcWork.left);
        auto const workHeight = static_cast<int>(monitor->rcWork.bottom - monitor->rcWork.top);

        auto width = requestedWidth > 0 ? requestedWidth : 1280;
        auto height = requestedHeight > 0 ? requestedHeight : 720;

        auto const maxWidth = std::max(1, workWidth * 9 / 10);
        auto const maxHeight = std::max(1, workHeight * 9 / 10);
        if (width > maxWidth || height > maxHeight) {
            auto const widthScale = static_cast<double>(maxWidth) / width;
            auto const heightScale = static_cast<double>(maxHeight) / height;
            auto const scale = std::min(widthScale, heightScale);
            auto const minWidth = std::min(640, maxWidth);
            auto const minHeight = std::min(360, maxHeight);
            width = std::max(minWidth, static_cast<int>(width * scale));
            height = std::max(minHeight, static_cast<int>(height * scale));
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
        auto const left = monitor->rcWork.left + (workWidth - outerWidth) / 2;
        auto const top = monitor->rcWork.top + (workHeight - outerHeight) / 2;
        return RECT{left, top, left + outerWidth, top + outerHeight};
    }


    bool nativeWindowIsFullscreen(HWND window) {
        auto const style = GetWindowLongPtrW(window, GWL_STYLE);
        if ((style & WS_POPUP) != 0 && (style & WS_CAPTION) == 0) {
            return true;
        }

        RECT rect{};
        auto const monitor = monitorInfoForWindow(window);
        if (!monitor || !GetWindowRect(window, &rect)) {
            return false;
        }

        constexpr auto tolerance = 2;
        return
            std::abs(rect.left - monitor->rcMonitor.left) <= tolerance &&
            std::abs(rect.top - monitor->rcMonitor.top) <= tolerance &&
            std::abs(rect.right - monitor->rcMonitor.right) <= tolerance &&
            std::abs(rect.bottom - monitor->rcMonitor.bottom) <= tolerance;
    }

    void makeWindowedForNativeDialog(HWND window) {
        auto style = GetWindowLongPtrW(window, GWL_STYLE);
        auto extendedStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);

        style &= ~(WS_POPUP | WS_MAXIMIZE);
        style |= WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
        extendedStyle &= ~(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME);
        extendedStyle |= WS_EX_APPWINDOW | WS_EX_WINDOWEDGE;

        auto const target = savedWindowedRect().value_or(
            makeWindowedRect(window, style, extendedStyle, 1280, 720)
        );

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
    }

    void applyMode(
        cocos2d::CCEGLView* view,
        bool fullscreen,
        int requestedWidth = 0,
        int requestedHeight = 0
    ) {
        if (nativeDialogActive()) {
            return;
        }

        auto const window = getNativeWindow(view);
        if (!window) {
            log::warn("Unable to apply Geometry Dash window mode: native window not found");
            return;
        }

        ReleaseCapture();

        RECT target{};
        auto style = GetWindowLongPtrW(window, GWL_STYLE);
        auto extendedStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
        extendedStyle &= ~(WS_EX_TOPMOST | WS_EX_TOOLWINDOW);
        extendedStyle |= WS_EX_APPWINDOW;

        if (fullscreen) {
            auto const monitor = monitorInfoForWindow(window);
            if (!monitor) {
                log::warn("Unable to enter fullscreen: monitor not found");
                return;
            }

            target = monitor->rcMonitor;
            style &= ~(WS_CAPTION | WS_MAXIMIZE);
            style |= WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS |
                WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
            extendedStyle &= ~(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME);
        } else {
            style &= ~(WS_POPUP | WS_MAXIMIZE);
            style |= WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
            extendedStyle |= WS_EX_WINDOWEDGE;

            auto const savedRect = savedWindowedRect();
            if (requestedWidth <= 0 && requestedHeight <= 0 && savedRect) {
                target = *savedRect;
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

    void synchronizeWindowMode() {
        if (nativeDialogActive()) {
            return;
        }

        auto* view = cocos2d::CCEGLView::get();
        if (!view) {
            log::warn("Unable to synchronize window mode: CCEGLView not found");
            return;
        }

        if (gameWantsFullscreen(view)) {
            setModeState(view, true, view->m_bIsBorderless, view->m_bIsFix);
            applyMode(view, true);
        } else {
            releaseWindowToManager(view);
        }
    }

    void resizeWindowHook(cocos2d::CCEGLView* view, int width, int height) {
        if (nativeDialogActive()) {
            return;
        }

        if (view->getIsFullscreen()) {
            if (auto const window = getNativeWindow(view)) {
                updateRenderSize(view, window);
            }
            return;
        }
        applyMode(view, false, width, height);
    }

    bool rvaIsInsideModule(HMODULE module, std::uintptr_t rva) {
        auto const* base = reinterpret_cast<std::byte const*>(module);
        auto const* dosHeader = reinterpret_cast<IMAGE_DOS_HEADER const*>(base);
        if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE) {
            return false;
        }

        auto const* ntHeaders = reinterpret_cast<IMAGE_NT_HEADERS const*>(
            base + dosHeader->e_lfanew
        );
        if (ntHeaders->Signature != IMAGE_NT_SIGNATURE) {
            return false;
        }
        return rva < ntHeaders->OptionalHeader.SizeOfImage;
    }

    void* addressAtRva(HMODULE module, std::uintptr_t rva) {
        if (!rvaIsInsideModule(module, rva)) {
            return nullptr;
        }
        return reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(module) + rva);
    }

    Result<> installCocosWindowHooks() {
        auto const cocosBase = base::getCocos();
        auto const cocos = reinterpret_cast<HMODULE>(cocosBase);
        if (!cocos) {
            return Err("Unable to locate libcocos2d.dll");
        }

        auto* borderlessTop = reinterpret_cast<void*>(
            GetProcAddress(cocos, kMakeBorderlessTopSymbol)
        );
        auto* resizeWindow = reinterpret_cast<void*>(
            GetProcAddress(cocos, kResizeWindowSymbol)
        );
        auto* centerWindow = addressAtRva(cocos, kGlfwPlatformCenterWindowRva);
        auto* setWindowMonitor = addressAtRva(cocos, kGlfwPlatformSetWindowMonitorRva);
        auto* setWindowPos = addressAtRva(cocos, kGlfwPlatformSetWindowPosRva);
        auto* setWindowSize = addressAtRva(cocos, kGlfwPlatformSetWindowSizeRva);

        if (!borderlessTop || !resizeWindow) {
            return Err("Unable to resolve Cocos window-management functions");
        }
        if (!centerWindow || !setWindowMonitor || !setWindowPos || !setWindowSize) {
            return Err("Cocos window hook offsets do not match the loaded module");
        }

        GEODE_UNWRAP(Mod::get()->hook(
            borderlessTop,
            &suppress<cocos2d::CCEGLView*>,
            "Suppress Cocos topmost enforcement"
        ));
        GEODE_UNWRAP(Mod::get()->hook(
            resizeWindow,
            &resizeWindowHook,
            "Replace Cocos window resizing"
        ));
        GEODE_UNWRAP(Mod::get()->hook(
            centerWindow,
            &suppress<GLFWwindow*>,
            "Suppress GLFW native window centering"
        ));
        GEODE_UNWRAP(Mod::get()->hook(
            setWindowMonitor,
            &suppress<GLFWwindow*, GLFWmonitor*, int, int, int, int>,
            "Suppress GLFW monitor ownership"
        ));
        GEODE_UNWRAP(Mod::get()->hook(
            setWindowPos,
            &suppress<GLFWwindow*, int, int>,
            "Suppress GLFW native window positioning"
        ));
        GEODE_UNWRAP(Mod::get()->hook(
            setWindowSize,
            &suppress<GLFWwindow*, int, int>,
            "Suppress GLFW native window sizing"
        ));
        return Ok();
    }
}

class $modify(GDLinuxWindowFix, cocos2d::CCEGLView) {
    void onGLFWWindowFocus(GLFWwindow* window, int focused) {
        if (!gdlinux::isRunningUnderWine()) {
            cocos2d::CCEGLView::onGLFWWindowFocus(window, focused);
            return;
        }

        if (nativeDialogActive()) {
            return;
        }

        auto const fix = m_bIsFix;
        m_bIsFix = false;
        cocos2d::CCEGLView::onGLFWWindowFocus(window, focused);
        m_bIsFix = fix;
    }

    void toggleFullScreen(bool fullscreen, bool borderless, bool fix) {
        if (!gdlinux::isRunningUnderWine()) {
            cocos2d::CCEGLView::toggleFullScreen(fullscreen, borderless, fix);
            return;
        }

        if (nativeDialogActive()) {
            return;
        }

        if (fullscreen && !getIsFullscreen()) {
            if (auto const window = getNativeWindow(this)) {
                rememberWindowedRect(window);
            }
        }

        setModeState(this, fullscreen, borderless, fix);
        applyMode(this, fullscreen);
    }
};

class $modify(GDLinuxMenuWindowSync, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) {
            return false;
        }

        if (gdlinux::isRunningUnderWine()) {
            queueInMainThread([] {
                synchronizeWindowMode();
            });
        }
        return true;
    }
};

namespace gdlinux {
    NativeDialogWindowGuard::NativeDialogWindowGuard(std::uintptr_t preferredWindow) {
        if (!isRunningUnderWine()) {
            return;
        }

        auto window = reinterpret_cast<HWND>(preferredWindow);
        if (!window || !IsWindow(window)) {
            window = findProcessWindow();
        }
        if (!window || !IsWindowVisible(window)) {
            return;
        }

        m_window = reinterpret_cast<std::uintptr_t>(window);
        m_active = true;

        auto const previousDepth = g_nativeDialogDepth.fetch_add(1, std::memory_order_acq_rel);
        if (previousDepth != 0) {
            return;
        }

        m_restoreFullscreen = nativeWindowIsFullscreen(window);

        if (g_minimizeNativeDialogs.load(std::memory_order_acquire)) {
            if (m_restoreFullscreen) {
                makeWindowedForNativeDialog(window);
            }
            ShowWindow(window, SW_MINIMIZE);
            m_minimized = true;
            return;
        }

        if (m_restoreFullscreen) {
            makeWindowedForNativeDialog(window);
        }
    }

    NativeDialogWindowGuard::~NativeDialogWindowGuard() {
        if (!m_active) {
            return;
        }

        auto const previousDepth = g_nativeDialogDepth.fetch_sub(1, std::memory_order_acq_rel);
        if (previousDepth > 1) {
            return;
        }

        auto const windowHandle = m_window;
        auto const restoreFullscreen = m_restoreFullscreen;
        auto const minimized = m_minimized;
        queueInMainThread([windowHandle, restoreFullscreen, minimized] {
            auto const window = reinterpret_cast<HWND>(windowHandle);
            if (!window || !IsWindow(window)) {
                return;
            }

            if (minimized && IsIconic(window)) {
                ShowWindow(window, SW_RESTORE);
            }

            if (restoreFullscreen) {
                synchronizeWindowMode();
            } else if (auto* view = cocos2d::CCEGLView::get()) {
                if (!gameWantsFullscreen(view)) {
                    releaseWindowToManager(view);
                }
            }

            SetForegroundWindow(window);
        });
    }

    Result<> initializeWindowFix() {
        if (!isRunningUnderWine()) {
            return Ok();
        }

        g_minimizeNativeDialogs.store(
            Mod::get()->getSettingValue<bool>("minimize-native-dialogs"),
            std::memory_order_release
        );
        listenForSettingChanges<bool>("minimize-native-dialogs", [](bool value) {
            g_minimizeNativeDialogs.store(value, std::memory_order_release);
        });

        GEODE_UNWRAP(installCocosWindowHooks());

        queueInMainThread([] {
            synchronizeWindowMode();
        });

        return Ok();
    }
}
