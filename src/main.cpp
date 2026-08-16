#include "NativeFileDialogHook.hpp"

#include <Geode/Geode.hpp>

using namespace geode::prelude;

$execute {
    auto result = gdlinux::installNativeFileDialogHook();
    if (!result) {
        log::error("Unable to install the native Linux file picker: {}", result.unwrapErr());
    }
}
