#include "NativeFileDialogHook.hpp"
#include "WindowFix.hpp"

#include <Geode/Geode.hpp>

using namespace geode::prelude;

$execute {
    auto result = gdlinux::installNativeFileDialogHook();
    if (!result) {
        log::error("Unable to install the native Linux file picker: {}", result.unwrapErr());
    }

    auto windowResult = gdlinux::initializeWindowFix();
    if (!windowResult) {
        log::error("Unable to install window fixes: {}", windowResult.unwrapErr());
    }
}
