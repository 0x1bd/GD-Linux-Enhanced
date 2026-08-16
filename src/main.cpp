#include "NativeFileDialogHook.hpp"
#include "NativeFolderOpenHook.hpp"
#include "WindowFix.hpp"

#include <Geode/Geode.hpp>

using namespace geode::prelude;

$execute {
    auto pickerResult = gdlinux::installNativeFileDialogHook();
    if (!pickerResult) {
        log::error("Unable to install the native Linux file picker: {}", pickerResult.unwrapErr());
    }

    auto folderResult = gdlinux::installNativeFolderOpenHook();
    if (!folderResult) {
        log::error("Unable to install the native Linux folder opener: {}", folderResult.unwrapErr());
    }

    auto windowResult = gdlinux::initializeWindowFix();
    if (!windowResult) {
        log::error("Unable to install window fixes: {}", windowResult.unwrapErr());
    }
}
