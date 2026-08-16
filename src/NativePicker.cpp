#include "NativePicker.hpp"

#include <Geode/Geode.hpp>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cwctype>
#include <fstream>
#include <optional>
#include <string_view>

using namespace geode::prelude;

namespace {
    enum class Backend {
        Zenity,
        KDialog,
    };

    struct ProcessResult {
        bool launched = false;
        DWORD exitCode = ERROR_GEN_FAILURE;
    };

    std::wstring unixPathToWindows(std::wstring path);

    std::optional<std::string> readHandoffFile(std::wstring const& unixPath) {
        auto windowsPath = unixPathToWindows(unixPath);
        std::ifstream input(std::filesystem::path(windowsPath), std::ios::binary);
        if (!input) {
            return std::nullopt;
        }
        return std::string {
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()
        };
    }

    bool fileExists(std::wstring const& path) {
        auto attributes = GetFileAttributesW(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
    }

    std::optional<Backend> findBackend() {
        wchar_t desktop[128] {};
        auto desktopLength = GetEnvironmentVariableW(L"XDG_CURRENT_DESKTOP", desktop, 128);
        std::wstring_view desktopName(desktop, std::min<DWORD>(desktopLength, 127));
        auto kde = desktopName.find(L"KDE") != std::wstring_view::npos;

        if (kde && fileExists(L"Z:\\usr\\bin\\kdialog")) {
            return Backend::KDialog;
        }
        if (fileExists(L"Z:\\usr\\bin\\zenity")) {
            return Backend::Zenity;
        }
        if (fileExists(L"Z:\\usr\\bin\\kdialog")) {
            return Backend::KDialog;
        }
        return std::nullopt;
    }

    std::wstring shellQuote(std::wstring_view value) {
        std::wstring result = L"'";
        for (auto character : value) {
            if (character == L'\'') {
                result += L"'\\''";
            }
            else {
                result += character;
            }
        }
        result += L"'";
        return result;
    }

    std::wstring windowsArgumentQuote(std::wstring_view value) {
        std::wstring result = L"\"";
        std::size_t backslashes = 0;
        for (auto character : value) {
            if (character == L'\\') {
                ++backslashes;
                continue;
            }
            if (character == L'\"') {
                result.append(backslashes * 2 + 1, L'\\');
                result += character;
            }
            else {
                result.append(backslashes, L'\\');
                result += character;
            }
            backslashes = 0;
        }
        result.append(backslashes * 2, L'\\');
        result += L'\"';
        return result;
    }

    std::wstring trimLineEndings(std::wstring value) {
        while (!value.empty() && (value.back() == L'\n' || value.back() == L'\r')) {
            value.pop_back();
        }
        return value;
    }

    std::wstring windowsPathToUnix(std::wstring path) {
        if (path.size() >= 3 && (path[0] == L'Z' || path[0] == L'z') && path[1] == L':') {
            path.erase(0, 2);
            std::ranges::replace(path, L'\\', L'/');
            return path;
        }

        if (path.size() >= 3 && path[1] == L':') {
            wchar_t prefix[32768] {};
            auto prefixLength = GetEnvironmentVariableW(L"WINEPREFIX", prefix, 32768);
            if (prefixLength > 0 && prefixLength < 32768) {
                auto drive = static_cast<wchar_t>(std::towlower(path[0]));
                std::wstring result(prefix, prefixLength);
                result += L"/dosdevices/";
                result += drive;
                result += L":";
                auto tail = path.substr(2);
                std::ranges::replace(tail, L'\\', L'/');
                result += tail;
                return result;
            }
        }
        return {};
    }

    std::wstring unixPathToWindows(std::wstring path) {
        if (path.empty() || path.front() != L'/') {
            return {};
        }
        std::ranges::replace(path, L'/', L'\\');
        return L"Z:" + path;
    }

    std::wstring makeOutputPath() {
        static std::atomic_uint counter = 0;
        return L"/tmp/gd-linux-picker-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64()) + L"-" +
            std::to_wstring(counter.fetch_add(1, std::memory_order_relaxed)) + L".result";
    }

    ProcessResult runShellCommand(
        std::wstring const& command,
        std::wstring const& statusPath
    ) {
        auto commandLine = L"sh -c " + windowsArgumentQuote(command);
        STARTUPINFOW startup {};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process {};

        auto launched = CreateProcessW(
            L"Z:\\bin\\sh", commandLine.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process
        );
        if (!launched) {
            return {};
        }

        DWORD exitCode = ERROR_GEN_FAILURE;
        for (;;) {
            auto status = readHandoffFile(statusPath);
            if (status && !status->empty()) {
                char* end = nullptr;
                auto parsed = std::strtoul(status->c_str(), &end, 10);
                if (end != status->c_str()) {
                    exitCode = static_cast<DWORD>(parsed);
                }
                break;
            }
            Sleep(50);
        }
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return { true, exitCode };
    }

    std::wstring zenityCommand(gdlinux::PickerRequest const& request) {
        std::wstring command =
            L"/usr/bin/env -u LD_LIBRARY_PATH -u LD_PRELOAD /usr/bin/zenity --file-selection";
        if (request.save) {
            command += L" --save --confirm-overwrite";
        }
        if (request.directory) {
            command += L" --directory";
        }
        if (request.multiple) {
            command += L" --multiple --separator='\\n'";
        }
        if (!request.title.empty()) {
            command += L" --title=" + shellQuote(request.title);
        }
        if (!request.initialPath.empty()) {
            command += L" --filename=" + shellQuote(request.initialPath);
        }
        for (auto const& filter : request.filters) {
            auto patterns = filter.patterns;
            std::ranges::replace(patterns, L';', L' ');
            command += L" --file-filter=" + shellQuote(filter.name + L" | " + patterns);
        }
        return command;
    }

    std::wstring kdialogFilter(std::vector<gdlinux::PickerFilter> const& filters) {
        std::wstring result;
        for (auto const& filter : filters) {
            if (!result.empty()) {
                result += L"\n";
            }
            auto patterns = filter.patterns;
            std::ranges::replace(patterns, L';', L' ');
            result += patterns + L"|" + filter.name;
        }
        return result;
    }

    std::wstring kdialogCommand(gdlinux::PickerRequest const& request) {
        std::wstring command =
            L"/usr/bin/env -u LD_LIBRARY_PATH -u LD_PRELOAD /usr/bin/kdialog ";
        if (request.directory) {
            command += L"--getexistingdirectory";
        }
        else if (request.save) {
            command += L"--getsavefilename";
        }
        else {
            command += L"--getopenfilename";
        }
        if (!request.initialPath.empty()) {
            command += L" " + shellQuote(request.initialPath);
        }
        if (!request.directory && !request.filters.empty()) {
            command += L" " + shellQuote(kdialogFilter(request.filters));
        }
        if (!request.title.empty()) {
            command += L" --title " + shellQuote(request.title);
        }
        if (request.multiple) {
            command += L" --multiple --separate-output";
        }
        return command;
    }

    std::vector<std::wstring> readSelectedPaths(std::wstring const& unixOutputPath) {
        auto contents = readHandoffFile(unixOutputPath);
        if (!contents) {
            return {};
        }
        auto wide = string::utf8ToWide(*contents);
        std::vector<std::wstring> paths;
        std::size_t start = 0;
        while (start <= wide.size()) {
            auto end = wide.find(L'\n', start);
            auto line = trimLineEndings(wide.substr(start, end - start));
            if (!line.empty()) {
                auto windowsPath = unixPathToWindows(line);
                if (!windowsPath.empty()) {
                    paths.push_back(std::move(windowsPath));
                }
            }
            if (end == std::wstring::npos) {
                break;
            }
            start = end + 1;
        }
        return paths;
    }
}

bool gdlinux::isRunningUnderWine() {
    auto ntdll = GetModuleHandleW(L"ntdll.dll");
    return ntdll && GetProcAddress(ntdll, "wine_get_version");
}

bool gdlinux::hasNativePicker() {
    return fileExists(L"Z:\\bin\\sh") && findBackend().has_value();
}

gdlinux::PickerResponse gdlinux::showNativePicker(PickerRequest const& originalRequest) {
    auto backend = findBackend();
    if (!backend || !fileExists(L"Z:\\bin\\sh")) {
        return {};
    }

    auto request = originalRequest;
    request.initialPath = windowsPathToUnix(request.initialPath);
    if (request.directory && !request.initialPath.empty() && request.initialPath.back() != L'/') {
        request.initialPath += L'/';
    }
    auto outputPath = makeOutputPath();
    auto statusPath = makeOutputPath();
    auto windowsOutputPath = unixPathToWindows(outputPath);
    auto windowsStatusPath = unixPathToWindows(statusPath);
    auto handle = CreateFileW(
        windowsOutputPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_TEMPORARY, nullptr
    );
    if (handle == INVALID_HANDLE_VALUE) {
        return { PickerStatus::Failed, {}, L"Unable to create picker result file" };
    }
    CloseHandle(handle);
    handle = CreateFileW(
        windowsStatusPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_TEMPORARY, nullptr
    );
    if (handle == INVALID_HANDLE_VALUE) {
        DeleteFileW(windowsOutputPath.c_str());
        return { PickerStatus::Failed, {}, L"Unable to create picker status file" };
    }
    CloseHandle(handle);

    auto command = *backend == Backend::Zenity
        ? zenityCommand(request)
        : kdialogCommand(request);
    command += L" > " + shellQuote(outputPath);
    command += L"; echo $? > " + shellQuote(statusPath);

    auto process = runShellCommand(command, statusPath);
    std::vector<std::wstring> paths;
    if (process.launched && process.exitCode == 0) {
        paths = readSelectedPaths(outputPath);
    }
    DeleteFileW(windowsOutputPath.c_str());
    DeleteFileW(windowsStatusPath.c_str());

    if (!process.launched) {
        return { PickerStatus::Unavailable, {}, L"Wine could not launch /bin/sh" };
    }
    if (process.exitCode == 1) {
        return { PickerStatus::Cancelled, {}, {} };
    }
    if (process.exitCode != 0) {
        return {
            PickerStatus::Failed,
            {},
            L"Native picker exited with status " + std::to_wstring(process.exitCode),
        };
    }
    if (paths.empty()) {
        return { PickerStatus::Failed, {}, L"Native picker returned no usable path" };
    }
    return { PickerStatus::Selected, std::move(paths), {} };
}
