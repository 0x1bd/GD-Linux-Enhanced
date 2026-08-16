#pragma once

#include <string>

namespace gdlinux {
    [[nodiscard]] std::wstring windowsPathToUnix(std::wstring path);
    [[nodiscard]] std::wstring unixPathToWindows(std::wstring path);

    [[nodiscard]] bool hasNativeFolderOpener();
    [[nodiscard]] bool openNativeFolder(std::wstring const& windowsPath);
}
