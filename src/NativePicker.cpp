#include "NativePicker.hpp"

#include "NativeShell.hpp"
#include "WindowFix.hpp"
#include "Wine.hpp"

#include <Geode/Geode.hpp>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string_view>

using namespace geode::prelude;

namespace {
    constexpr wchar_t kShellPath[] = L"Z:\\bin\\sh";
    constexpr wchar_t kZenityPath[] = L"Z:\\usr\\bin\\zenity";
    constexpr wchar_t kKDialogPath[] = L"Z:\\usr\\bin\\kdialog";

    enum class Backend {
        Zenity,
        KDialog,
    };

    bool fileExists(std::wstring const& path) {
        auto const attributes = GetFileAttributesW(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES &&
            !(attributes & FILE_ATTRIBUTE_DIRECTORY);
    }

    std::optional<Backend> findBackend() {
        wchar_t desktop[128]{};
        auto const length = GetEnvironmentVariableW(
            L"XDG_CURRENT_DESKTOP",
            desktop,
            static_cast<DWORD>(std::size(desktop))
        );
        auto const desktopName = length > 0 && length < std::size(desktop)
            ? std::wstring_view(desktop, length)
            : std::wstring_view{};

        if (desktopName.find(L"KDE") != std::wstring_view::npos && fileExists(kKDialogPath)) {
            return Backend::KDialog;
        }
        if (fileExists(kZenityPath)) {
            return Backend::Zenity;
        }
        if (fileExists(kKDialogPath)) {
            return Backend::KDialog;
        }
        return std::nullopt;
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

    std::wstring pickerInitialPath(std::wstring path) {
        if (path.empty()) {
            return {};
        }

        auto unixPath = gdlinux::windowsPathToUnix(path);
        if (!unixPath.empty()) {
            return unixPath;
        }

        if (path.size() >= 2 && path[1] == L':') {
            return {};
        }
        std::ranges::replace(path, L'\\', L'/');
        return path;
    }

    std::optional<std::string> readFile(std::wstring const& unixPath) {
        auto const windowsPath = gdlinux::unixPathToWindows(unixPath);
        if (windowsPath.empty()) {
            return std::nullopt;
        }

        std::ifstream input(std::filesystem::path(windowsPath), std::ios::binary);
        if (!input) {
            return std::nullopt;
        }

        return std::string{
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>(),
        };
    }

    std::wstring makeOutputPath() {
        static std::atomic_uint counter = 0;
        return L"/tmp/gd-linux-picker-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64()) + L"-" +
            std::to_wstring(counter.fetch_add(1, std::memory_order_relaxed)) + L".result";
    }

    class TemporaryOutputFile {
    public:
        TemporaryOutputFile()
          : m_unixPath(makeOutputPath()),
            m_windowsPath(gdlinux::unixPathToWindows(m_unixPath)) {}

        ~TemporaryOutputFile() {
            if (!m_windowsPath.empty()) {
                DeleteFileW(m_windowsPath.c_str());
            }
        }

        std::wstring const& unixPath() const {
            return m_unixPath;
        }

    private:
        std::wstring m_unixPath;
        std::wstring m_windowsPath;
    };

    std::optional<DWORD> readExitCode(std::wstring const& statusPath) {
        auto const contents = readFile(statusPath);
        if (!contents || contents->empty()) {
            return std::nullopt;
        }

        char* end = nullptr;
        auto const value = std::strtoul(contents->c_str(), &end, 10);
        if (end == contents->c_str()) {
            return std::nullopt;
        }
        return static_cast<DWORD>(value);
    }

    std::optional<DWORD> runShellCommand(
        std::wstring const& command,
        std::wstring const& statusPath
    ) {
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
            return std::nullopt;
        }

        CloseHandle(process.hThread);

        std::optional<DWORD> exitCode;
        while (!(exitCode = readExitCode(statusPath))) {
            Sleep(25);
        }

        CloseHandle(process.hProcess);
        return exitCode;
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
                result += L'\n';
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
        } else if (request.save) {
            command += L"--getsavefilename";
        } else {
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

    std::wstring trimLineEndings(std::wstring value) {
        while (!value.empty() && (value.back() == L'\n' || value.back() == L'\r')) {
            value.pop_back();
        }
        return value;
    }

    std::vector<std::wstring> readSelectedPaths(std::wstring const& outputPath) {
        auto const contents = readFile(outputPath);
        if (!contents) {
            return {};
        }

        auto const wide = string::utf8ToWide(*contents);
        std::vector<std::wstring> paths;

        std::size_t start = 0;
        while (start <= wide.size()) {
            auto const end = wide.find(L'\n', start);
            auto line = trimLineEndings(wide.substr(start, end - start));
            if (!line.empty()) {
                auto windowsPath = gdlinux::unixPathToWindows(line);
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

namespace gdlinux {
    bool hasNativePicker() {
        return fileExists(kShellPath) && findBackend().has_value();
    }

    PickerResponse showNativePicker(PickerRequest const& originalRequest) {
        auto const backend = findBackend();
        if (!backend || !fileExists(kShellPath)) {
            return {};
        }

        auto request = originalRequest;
        request.initialPath = pickerInitialPath(request.initialPath);
        if (
            request.directory &&
            !request.initialPath.empty() &&
            request.initialPath.back() != L'/'
        ) {
            request.initialPath += L'/';
        }

        TemporaryOutputFile output;
        TemporaryOutputFile status;
        auto command = *backend == Backend::Zenity
            ? zenityCommand(request)
            : kdialogCommand(request);
        command += L" > " + shellQuote(output.unixPath());
        command += L"; printf '%d\n' $? > " + shellQuote(status.unixPath());

        NativeDialogWindowGuard windowGuard(request.ownerWindow);
        auto const exitCode = runShellCommand(command, status.unixPath());
        if (!exitCode) {
            return {PickerStatus::Unavailable, {}, L"Wine could not launch /bin/sh"};
        }
        if (*exitCode == 1) {
            return {PickerStatus::Cancelled, {}, {}};
        }
        if (*exitCode != 0) {
            return {
                PickerStatus::Failed,
                {},
                L"Native picker exited with status " + std::to_wstring(*exitCode),
            };
        }

        auto paths = readSelectedPaths(output.unixPath());
        if (paths.empty()) {
            return {PickerStatus::Failed, {}, L"Native picker returned no usable path"};
        }
        return {PickerStatus::Selected, std::move(paths), {}};
    }
}
