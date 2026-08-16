#pragma once

#include <cstdint>
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
        std::uintptr_t ownerWindow = 0;
        std::wstring title;
        std::wstring initialPath;
        std::vector<PickerFilter> filters;
    };

    struct PickerResponse {
        PickerStatus status = PickerStatus::Unavailable;
        std::vector<std::wstring> paths;
        std::wstring error;
    };

    [[nodiscard]] bool hasNativePicker();
    [[nodiscard]] PickerResponse showNativePicker(PickerRequest const& request);
}
