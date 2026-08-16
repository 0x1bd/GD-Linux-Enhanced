#include "NativeFolderOpenHook.hpp"

#include "NativeShell.hpp"
#include "Wine.hpp"

#include <Geode/Geode.hpp>

#include <shlobj.h>
#include <shobjidl.h>

#include <memory>
#include <string>

using namespace geode::prelude;

namespace {
    struct CoTaskMemDeleter {
        void operator()(wchar_t* pointer) const {
            CoTaskMemFree(pointer);
        }
    };

    std::wstring folderPath(PCIDLIST_ABSOLUTE folder) {
        if (!folder) {
            return {};
        }

        PWSTR rawPath = nullptr;
        if (FAILED(SHGetNameFromIDList(folder, SIGDN_FILESYSPATH, &rawPath)) || !rawPath) {
            return {};
        }

        std::unique_ptr<wchar_t, CoTaskMemDeleter> path(rawPath);
        return std::wstring(path.get());
    }

    HRESULT WINAPI openFolderHook(
        PCIDLIST_ABSOLUTE folder,
        UINT itemCount,
        PCUITEMID_CHILD_ARRAY items,
        DWORD flags
    ) {
        auto const path = folderPath(folder);
        if (!path.empty() && gdlinux::openNativeFolder(path)) {
            return S_OK;
        }

        return SHOpenFolderAndSelectItems(folder, itemCount, items, flags);
    }
}

namespace gdlinux {
    Result<> installNativeFolderOpenHook() {
        if (!isRunningUnderWine()) {
            return Ok();
        }
        if (!hasNativeFolderOpener()) {
            log::warn("Native Linux folder opener unavailable: install xdg-utils or gio");
            return Ok();
        }

        auto shell32 = GetModuleHandleW(L"shell32.dll");
        if (!shell32) {
            shell32 = LoadLibraryW(L"shell32.dll");
        }
        if (!shell32) {
            return Err("Unable to load shell32.dll");
        }

        auto* address = reinterpret_cast<void*>(
            GetProcAddress(shell32, "SHOpenFolderAndSelectItems")
        );
        if (!address) {
            return Err("Unable to resolve SHOpenFolderAndSelectItems");
        }

        GEODE_UNWRAP(Mod::get()->hook(
            address,
            &openFolderHook,
            "Native Linux folder opener"
        ));
        log::info("Native Linux folder opener enabled");
        return Ok();
    }
}
