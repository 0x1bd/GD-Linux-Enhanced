#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace gdlinux {
    [[nodiscard]] inline bool isRunningUnderWine() {
        auto const ntdll = GetModuleHandleW(L"ntdll.dll");
        return ntdll && GetProcAddress(ntdll, "wine_get_version");
    }
}
