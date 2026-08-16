#pragma once

#include <Geode/Result.hpp>

#include <cstdint>

namespace gdlinux {
    class NativeDialogWindowGuard final {
    public:
        explicit NativeDialogWindowGuard(std::uintptr_t preferredWindow = 0);
        ~NativeDialogWindowGuard();

        NativeDialogWindowGuard(NativeDialogWindowGuard const&) = delete;
        NativeDialogWindowGuard& operator=(NativeDialogWindowGuard const&) = delete;

    private:
        std::uintptr_t m_window = 0;
        bool m_active = false;
        bool m_restoreFullscreen = false;
        bool m_minimized = false;
    };

    geode::Result<> initializeWindowFix();
}
