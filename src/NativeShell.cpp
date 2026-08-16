#include "NativeShell.hpp"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <iterator>
#include <string_view>

namespace {
    constexpr wchar_t kShellPath[] = L"Z:\\bin\\sh";
    constexpr wchar_t kXdgOpenPath[] = L"Z:\\usr\\bin\\xdg-open";
    constexpr wchar_t kGioPath[] = L"Z:\\usr\\bin\\gio";

    bool fileExists(std::wstring const& path) {
        auto const attributes = GetFileAttributesW(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES &&
            !(attributes & FILE_ATTRIBUTE_DIRECTORY);
    }

    std::wstring shellQuote(std::wstring_view value) {
        std::wstring quoted = L"'";
        for (auto const character : value) {
            if (character == L'\'') {
                quoted += L"'\\''";
            } else {
                quoted += character;
            }
        }
        quoted += L"'";
        return quoted;
    }

    std::wstring windowsArgumentQuote(std::wstring_view value) {
        std::wstring quoted = L"\"";
        std::size_t backslashes = 0;

        for (auto const character : value) {
            if (character == L'\\') {
                ++backslashes;
                continue;
            }

            if (character == L'\"') {
                quoted.append(backslashes * 2 + 1, L'\\');
            } else {
                quoted.append(backslashes, L'\\');
            }
            quoted += character;
            backslashes = 0;
        }

        quoted.append(backslashes * 2, L'\\');
        quoted += L'\"';
        return quoted;
    }

    bool launchShellDetached(std::wstring const& command) {
        auto commandLine = L"sh -c " + windowsArgumentQuote(command);

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};

        if (!CreateProcessW(
                kShellPath,
                commandLine.data(),
                nullptr,
                nullptr,
                FALSE,
                CREATE_NO_WINDOW,
                nullptr,
                nullptr,
                &startup,
                &process
            )) {
            return false;
        }

        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return true;
    }
}

namespace gdlinux {
    std::wstring windowsPathToUnix(std::wstring path) {
        if (path.size() >= 2 && (path[0] == L'Z' || path[0] == L'z') && path[1] == L':') {
            path.erase(0, 2);
            std::ranges::replace(path, L'\\', L'/');
            return path.empty() ? L"/" : path;
        }

        if (path.size() < 2 || path[1] != L':') {
            return {};
        }

        wchar_t prefix[32768]{};
        auto const prefixLength = GetEnvironmentVariableW(
            L"WINEPREFIX",
            prefix,
            static_cast<DWORD>(std::size(prefix))
        );
        if (prefixLength == 0 || prefixLength >= std::size(prefix)) {
            return {};
        }

        std::wstring unixPath(prefix, prefixLength);
        std::ranges::replace(unixPath, L'\\', L'/');
        if (!unixPath.empty() && unixPath.back() == L'/') {
            unixPath.pop_back();
        }

        unixPath += L"/dosdevices/";
        unixPath += static_cast<wchar_t>(std::towlower(path[0]));
        unixPath += L":";

        auto tail = path.substr(2);
        std::ranges::replace(tail, L'\\', L'/');
        unixPath += tail;
        return unixPath;
    }

    std::wstring unixPathToWindows(std::wstring path) {
        if (path.empty() || path.front() != L'/') {
            return {};
        }

        std::ranges::replace(path, L'/', L'\\');
        return L"Z:" + path;
    }

    bool hasNativeFolderOpener() {
        return fileExists(kShellPath) &&
            (fileExists(kXdgOpenPath) || fileExists(kGioPath));
    }

    bool openNativeFolder(std::wstring const& windowsPath) {
        auto const unixPath = windowsPathToUnix(windowsPath);
        if (unixPath.empty() || !fileExists(kShellPath)) {
            return false;
        }

        std::wstring command = L"/usr/bin/env -u LD_LIBRARY_PATH -u LD_PRELOAD ";
        if (fileExists(kXdgOpenPath)) {
            command += L"/usr/bin/xdg-open ";
        } else if (fileExists(kGioPath)) {
            command += L"/usr/bin/gio open ";
        } else {
            return false;
        }

        command += shellQuote(unixPath);
        command += L" >/dev/null 2>&1";
        return launchShellDetached(command);
    }
}
