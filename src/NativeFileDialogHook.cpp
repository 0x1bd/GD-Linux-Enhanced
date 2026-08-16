#include "NativeFileDialogHook.hpp"

#include "NativePicker.hpp"
#include "Wine.hpp"

#include <Geode/Geode.hpp>

#include <shlobj.h>
#include <shobjidl.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <new>
#include <utility>
#include <vector>

using namespace geode::prelude;

namespace {
    thread_local bool g_creatingWineFallback = false;

    class ScopedFlag {
    public:
        explicit ScopedFlag(bool& flag)
          : m_flag(flag), m_previous(std::exchange(flag, true)) {}

        ~ScopedFlag() {
            m_flag = m_previous;
        }

    private:
        bool& m_flag;
        bool m_previous;
    };

    struct CoTaskMemDeleter {
        template <class T>
        void operator()(T* pointer) const {
            CoTaskMemFree(pointer);
        }
    };

    std::wstring shellItemPath(IShellItem* item) {
        if (!item) {
            return {};
        }

        PWSTR rawPath = nullptr;
        if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath)) || !rawPath) {
            return {};
        }

        std::unique_ptr<wchar_t, CoTaskMemDeleter> path(rawPath);
        return std::wstring(path.get());
    }

    HRESULT shellItemForPath(std::wstring const& path, IShellItem** result) {
        if (!result) {
            return E_POINTER;
        }
        *result = nullptr;
        if (path.empty()) {
            return E_FAIL;
        }
        return SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(result));
    }

    HRESULT shellItemArrayForPaths(
        std::vector<std::wstring> const& paths,
        IShellItemArray** result
    ) {
        if (!result) {
            return E_POINTER;
        }
        *result = nullptr;
        if (paths.empty()) {
            return E_FAIL;
        }

        using ItemId = std::unique_ptr<ITEMIDLIST, CoTaskMemDeleter>;
        std::vector<ItemId> ownedIds;
        std::vector<PCIDLIST_ABSOLUTE> itemIds;
        ownedIds.reserve(paths.size());
        itemIds.reserve(paths.size());

        for (auto const& path : paths) {
            PIDLIST_ABSOLUTE rawId = nullptr;
            auto const status = SHParseDisplayName(path.c_str(), nullptr, &rawId, 0, nullptr);
            if (FAILED(status)) {
                return status;
            }
            ownedIds.emplace_back(rawId);
            itemIds.push_back(rawId);
        }

        return SHCreateShellItemArrayFromIDLists(
            static_cast<UINT>(itemIds.size()),
            itemIds.data(),
            result
        );
    }

    HRESULT copyCoTaskMemString(std::wstring const& value, LPWSTR* result) {
        if (!result) {
            return E_POINTER;
        }
        *result = nullptr;

        auto const bytes = (value.size() + 1) * sizeof(wchar_t);
        auto* buffer = static_cast<LPWSTR>(CoTaskMemAlloc(bytes));
        if (!buffer) {
            return E_OUTOFMEMORY;
        }

        std::memcpy(buffer, value.c_str(), bytes);
        *result = buffer;
        return S_OK;
    }

    template <class Interface>
    class FileDialogProxy : public Interface {
    public:
        explicit FileDialogProxy(bool save)
          : m_save(save),
            m_options(
                FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR |
                (save ? FOS_OVERWRITEPROMPT : FOS_FILEMUSTEXIST)
            ) {}

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
            if (!object) {
                return E_POINTER;
            }
            *object = nullptr;

            if (iid == IID_IUnknown) {
                *object = static_cast<IUnknown*>(static_cast<Interface*>(this));
            } else if (iid == IID_IModalWindow) {
                *object = static_cast<IModalWindow*>(static_cast<Interface*>(this));
            } else if (iid == IID_IFileDialog) {
                *object = static_cast<IFileDialog*>(static_cast<Interface*>(this));
            } else if (iid == interfaceId()) {
                *object = static_cast<Interface*>(this);
            } else {
                return E_NOINTERFACE;
            }

            AddRef();
            return S_OK;
        }

        ULONG STDMETHODCALLTYPE AddRef() override {
            return ++m_references;
        }

        ULONG STDMETHODCALLTYPE Release() override {
            auto const references = --m_references;
            if (references == 0) {
                delete this;
            }
            return references;
        }

        HRESULT STDMETHODCALLTYPE Show(HWND owner) override {
            m_usedNativePicker = false;
            m_paths.clear();

            auto request = makePickerRequest();
            request.ownerWindow = reinterpret_cast<std::uintptr_t>(owner);
            auto response = gdlinux::showNativePicker(request);
            switch (response.status) {
                case gdlinux::PickerStatus::Selected:
                    m_usedNativePicker = true;
                    m_paths = std::move(response.paths);
                    applyDefaultExtension();
                    rememberSelectedFileName();
                    return S_OK;

                case gdlinux::PickerStatus::Cancelled:
                    m_usedNativePicker = true;
                    m_paths.clear();
                    return HRESULT_FROM_WIN32(ERROR_CANCELLED);

                case gdlinux::PickerStatus::Failed:
                    log::warn(
                        "Native Linux file picker failed: {}",
                        string::wideToUtf8(response.error)
                    );
                    break;

                case gdlinux::PickerStatus::Unavailable:
                    break;
            }

            m_usedNativePicker = false;
            return showWineFallback(owner);
        }

        HRESULT STDMETHODCALLTYPE SetFileTypes(
            UINT count,
            COMDLG_FILTERSPEC const* filters
        ) override {
            if (count != 0 && !filters) {
                return E_INVALIDARG;
            }

            m_filters.clear();
            m_filters.reserve(count);
            for (UINT index = 0; index < count; ++index) {
                m_filters.push_back({
                    filters[index].pszName ? filters[index].pszName : L"Files",
                    filters[index].pszSpec ? filters[index].pszSpec : L"*",
                });
            }
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE SetFileTypeIndex(UINT index) override {
            m_fileTypeIndex = index;
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE GetFileTypeIndex(UINT* index) override {
            if (!index) {
                return E_POINTER;
            }
            if (!m_usedNativePicker && m_inner) {
                return m_inner->GetFileTypeIndex(index);
            }
            *index = m_fileTypeIndex;
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE Advise(IFileDialogEvents*, DWORD*) override {
            return E_NOTIMPL;
        }

        HRESULT STDMETHODCALLTYPE Unadvise(DWORD) override {
            return E_NOTIMPL;
        }

        HRESULT STDMETHODCALLTYPE SetOptions(FILEOPENDIALOGOPTIONS options) override {
            m_options = options;
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE GetOptions(FILEOPENDIALOGOPTIONS* options) override {
            if (!options) {
                return E_POINTER;
            }
            if (!m_usedNativePicker && m_inner) {
                return m_inner->GetOptions(options);
            }
            *options = m_options;
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE SetDefaultFolder(IShellItem* folder) override {
            auto path = shellItemPath(folder);
            if (path.empty()) {
                return E_NOTIMPL;
            }
            m_defaultFolder = std::move(path);
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE SetFolder(IShellItem* folder) override {
            auto path = shellItemPath(folder);
            if (path.empty()) {
                return E_NOTIMPL;
            }
            m_folder = std::move(path);
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE GetFolder(IShellItem** folder) override {
            if (!m_usedNativePicker && m_inner) {
                return m_inner->GetFolder(folder);
            }

            auto const path = currentFolderPath();
            return shellItemForPath(path, folder);
        }

        HRESULT STDMETHODCALLTYPE GetCurrentSelection(IShellItem** selection) override {
            if (m_usedNativePicker && !m_paths.empty()) {
                return shellItemForPath(m_paths.front(), selection);
            }
            if (!m_usedNativePicker && m_inner) {
                return m_inner->GetCurrentSelection(selection);
            }
            return GetFolder(selection);
        }

        HRESULT STDMETHODCALLTYPE SetFileName(LPCWSTR name) override {
            m_fileName = name ? name : L"";
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE GetFileName(LPWSTR* name) override {
            if (!m_usedNativePicker && m_inner) {
                return m_inner->GetFileName(name);
            }
            return copyCoTaskMemString(m_fileName, name);
        }

        HRESULT STDMETHODCALLTYPE SetTitle(LPCWSTR title) override {
            m_title = title ? title : L"";
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE SetOkButtonLabel(LPCWSTR) override {
            return E_NOTIMPL;
        }

        HRESULT STDMETHODCALLTYPE SetFileNameLabel(LPCWSTR) override {
            return E_NOTIMPL;
        }

        HRESULT STDMETHODCALLTYPE GetResult(IShellItem** item) override {
            if (!m_usedNativePicker) {
                return m_inner ? m_inner->GetResult(item) : E_UNEXPECTED;
            }
            if (m_paths.empty()) {
                return E_FAIL;
            }
            return shellItemForPath(m_paths.front(), item);
        }

        HRESULT STDMETHODCALLTYPE AddPlace(IShellItem*, FDAP) override {
            return E_NOTIMPL;
        }

        HRESULT STDMETHODCALLTYPE SetDefaultExtension(LPCWSTR extension) override {
            m_defaultExtension = extension ? extension : L"";
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE Close(HRESULT) override {
            return E_NOTIMPL;
        }

        HRESULT STDMETHODCALLTYPE SetClientGuid(REFGUID) override {
            return E_NOTIMPL;
        }

        HRESULT STDMETHODCALLTYPE ClearClientData() override {
            return E_NOTIMPL;
        }

        HRESULT STDMETHODCALLTYPE SetFilter(IShellItemFilter*) override {
            return E_NOTIMPL;
        }

    protected:
        virtual ~FileDialogProxy() {
            if (m_inner) {
                m_inner->Release();
            }
        }

        virtual IID const& interfaceId() const = 0;

        Interface* m_inner = nullptr;
        bool m_usedNativePicker = false;
        std::vector<std::wstring> m_paths;

    private:
        gdlinux::PickerRequest makePickerRequest() const {
            gdlinux::PickerRequest request;
            request.save = m_save;
            request.directory = (m_options & FOS_PICKFOLDERS) != 0;
            request.multiple = (m_options & FOS_ALLOWMULTISELECT) != 0;
            request.title = m_title;
            request.initialPath = initialPath();
            request.filters = m_filters;
            return request;
        }

        std::wstring initialPath() const {
            auto const& folder = !m_folder.empty() ? m_folder : m_defaultFolder;
            if (folder.empty()) {
                return m_fileName;
            }
            if (m_fileName.empty()) {
                return folder;
            }
            return (std::filesystem::path(folder) / m_fileName).wstring();
        }

        std::wstring currentFolderPath() const {
            if (m_usedNativePicker && !m_paths.empty()) {
                auto path = std::filesystem::path(m_paths.front());
                if ((m_options & FOS_PICKFOLDERS) == 0) {
                    path = path.parent_path();
                }
                return path.wstring();
            }
            if (!m_folder.empty()) {
                return m_folder;
            }
            return m_defaultFolder;
        }

        HRESULT showWineFallback(HWND owner) {
            auto const status = createWineFallback();
            if (FAILED(status)) {
                return status;
            }
            return m_inner->Show(owner);
        }

        HRESULT createWineFallback() {
            if (m_inner) {
                return S_OK;
            }

            ScopedFlag creating(g_creatingWineFallback);
            auto const classId = m_save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog;
            auto const status = CoCreateInstance(
                classId,
                nullptr,
                CLSCTX_ALL,
                interfaceId(),
                reinterpret_cast<void**>(&m_inner)
            );
            if (FAILED(status)) {
                return status;
            }

            syncWineFallback();
            return S_OK;
        }

        void syncWineFallback() {
            m_inner->SetOptions(m_options);

            if (!m_filters.empty()) {
                std::vector<COMDLG_FILTERSPEC> filters;
                filters.reserve(m_filters.size());
                for (auto const& filter : m_filters) {
                    filters.push_back({filter.name.c_str(), filter.patterns.c_str()});
                }
                m_inner->SetFileTypes(static_cast<UINT>(filters.size()), filters.data());
            }
            m_inner->SetFileTypeIndex(m_fileTypeIndex);

            applyFolder(m_defaultFolder, true);
            applyFolder(m_folder, false);

            if (!m_fileName.empty()) {
                m_inner->SetFileName(m_fileName.c_str());
            }
            if (!m_title.empty()) {
                m_inner->SetTitle(m_title.c_str());
            }
            if (!m_defaultExtension.empty()) {
                m_inner->SetDefaultExtension(m_defaultExtension.c_str());
            }
        }

        void applyFolder(std::wstring const& path, bool defaultFolder) {
            if (path.empty()) {
                return;
            }

            IShellItem* item = nullptr;
            if (FAILED(shellItemForPath(path, &item))) {
                return;
            }

            if (defaultFolder) {
                m_inner->SetDefaultFolder(item);
            } else {
                m_inner->SetFolder(item);
            }
            item->Release();
        }

        void rememberSelectedFileName() {
            if (!m_paths.empty()) {
                m_fileName = std::filesystem::path(m_paths.front()).filename().wstring();
            }
        }

        void applyDefaultExtension() {
            if (!m_save || m_paths.empty()) {
                return;
            }

            std::filesystem::path path(m_paths.front());
            if (path.has_extension()) {
                return;
            }

            auto extension = m_defaultExtension;
            if (extension.empty() && m_fileTypeIndex > 0 && m_fileTypeIndex <= m_filters.size()) {
                auto const& pattern = m_filters[m_fileTypeIndex - 1].patterns;
                if (pattern.starts_with(L"*.") &&
                    pattern.find_first_of(L";*?", 2) == std::wstring::npos) {
                    extension = pattern.substr(2);
                }
            }

            if (extension.empty()) {
                return;
            }
            if (extension.front() != L'.') {
                extension.insert(extension.begin(), L'.');
            }
            m_paths.front() += extension;
        }

        std::atomic_ulong m_references{1};
        bool m_save;
        FILEOPENDIALOGOPTIONS m_options;
        UINT m_fileTypeIndex = 1;
        std::wstring m_title;
        std::wstring m_defaultFolder;
        std::wstring m_folder;
        std::wstring m_fileName;
        std::wstring m_defaultExtension;
        std::vector<gdlinux::PickerFilter> m_filters;
    };

    class OpenFileDialogProxy final : public FileDialogProxy<IFileOpenDialog> {
    public:
        OpenFileDialogProxy()
          : FileDialogProxy(false) {}

        HRESULT STDMETHODCALLTYPE GetResults(IShellItemArray** items) override {
            if (!m_usedNativePicker) {
                return m_inner ? m_inner->GetResults(items) : E_UNEXPECTED;
            }
            return shellItemArrayForPaths(m_paths, items);
        }

        HRESULT STDMETHODCALLTYPE GetSelectedItems(IShellItemArray** items) override {
            if (!m_usedNativePicker) {
                return m_inner ? m_inner->GetSelectedItems(items) : E_UNEXPECTED;
            }
            return shellItemArrayForPaths(m_paths, items);
        }

    private:
        IID const& interfaceId() const override {
            return IID_IFileOpenDialog;
        }
    };

    class SaveFileDialogProxy final : public FileDialogProxy<IFileSaveDialog> {
    public:
        SaveFileDialogProxy()
          : FileDialogProxy(true) {}

        HRESULT STDMETHODCALLTYPE SetSaveAsItem(IShellItem*) override {
            return E_NOTIMPL;
        }

        HRESULT STDMETHODCALLTYPE SetProperties(IPropertyStore*) override {
            return E_NOTIMPL;
        }

        HRESULT STDMETHODCALLTYPE SetCollectedProperties(
            IPropertyDescriptionList*,
            BOOL
        ) override {
            return E_NOTIMPL;
        }

        HRESULT STDMETHODCALLTYPE GetProperties(IPropertyStore**) override {
            return E_NOTIMPL;
        }

        HRESULT STDMETHODCALLTYPE ApplyProperties(
            IShellItem*,
            IPropertyStore*,
            HWND,
            IFileOperationProgressSink*
        ) override {
            return E_NOTIMPL;
        }

    private:
        IID const& interfaceId() const override {
            return IID_IFileSaveDialog;
        }
    };

    HRESULT WINAPI coCreateInstanceHook(
        REFCLSID classId,
        LPUNKNOWN outer,
        DWORD context,
        REFIID interfaceId,
        LPVOID* object
    ) {
        if (g_creatingWineFallback) {
            return CoCreateInstance(classId, outer, context, interfaceId, object);
        }
        if (!object) {
            return E_POINTER;
        }
        *object = nullptr;

        if (!outer && classId == CLSID_FileOpenDialog && interfaceId == IID_IFileOpenDialog) {
            auto* proxy = new (std::nothrow) OpenFileDialogProxy();
            if (!proxy) {
                return E_OUTOFMEMORY;
            }
            *object = static_cast<IFileOpenDialog*>(proxy);
            return S_OK;
        }
        if (!outer && classId == CLSID_FileSaveDialog && interfaceId == IID_IFileSaveDialog) {
            auto* proxy = new (std::nothrow) SaveFileDialogProxy();
            if (!proxy) {
                return E_OUTOFMEMORY;
            }
            *object = static_cast<IFileSaveDialog*>(proxy);
            return S_OK;
        }
        return CoCreateInstance(classId, outer, context, interfaceId, object);
    }
}

namespace gdlinux {
    geode::Result<> installNativeFileDialogHook() {
        if (!isRunningUnderWine()) {
            return Ok();
        }
        if (!hasNativePicker()) {
            log::warn("Native Linux file picker unavailable: install zenity or kdialog");
            return Ok();
        }

        auto ole32 = GetModuleHandleW(L"ole32.dll");
        if (!ole32) {
            ole32 = LoadLibraryW(L"ole32.dll");
        }
        if (!ole32) {
            return Err("Unable to load ole32.dll");
        }

        auto* address = reinterpret_cast<void*>(GetProcAddress(ole32, "CoCreateInstance"));
        if (!address) {
            return Err("Unable to resolve CoCreateInstance");
        }

        GEODE_UNWRAP(Mod::get()->hook(
            address,
            &coCreateInstanceHook,
            "Native Linux file picker"
        ));
        log::info("Native Linux file picker enabled");
        return Ok();
    }
}
