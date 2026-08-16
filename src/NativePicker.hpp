#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace gdlinux {
    enum class PickerStatus {
        Selected,
        Cancelled,
        Unavailable,
        Failed,
    };

    struct PickerFilter {
        std::wstring name;
        std::wstring patterns;
    };

    struct PickerRequest {
        bool save = false;
        bool directory = false;
        bool multiple = false;
        std::wstring title;
        std::wstring initialPath;
        std::vector<PickerFilter> filters;
    };

    struct PickerResponse {
        PickerStatus status = PickerStatus::Unavailable;
        std::vector<std::wstring> paths;
        std::wstring error;
    };

    bool isRunningUnderWine();
    bool hasNativePicker();
    PickerResponse showNativePicker(PickerRequest const& request);
}
