#include "NativeFileDialogHook.hpp"

#include "NativePicker.hpp"

#include <Geode/Geode.hpp>

#include <shlobj.h>
#include <shobjidl.h>

#include <atomic>
#include <filesystem>
#include <vector>

using namespace geode::prelude;

namespace {
    std::wstring shellItemPath(IShellItem* item) {
        if (!item) {
            return {};
        }
        LPWSTR path = nullptr;
        if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) || !path) {
            return {};
        }
        std::wstring result(path);
        CoTaskMemFree(path);
        return result;
    }

    HRESULT shellItemForPath(std::wstring const& path, IShellItem** result) {
        if (!result) {
            return E_POINTER;
        }
        *result = nullptr;
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

        std::vector<PCIDLIST_ABSOLUTE> itemIds;
        itemIds.reserve(paths.size());
        for (auto const& path : paths) {
            PIDLIST_ABSOLUTE itemId = nullptr;
            auto status = SHParseDisplayName(path.c_str(), nullptr, &itemId, 0, nullptr);
            if (FAILED(status)) {
                for (auto id : itemIds) {
                    CoTaskMemFree(const_cast<PIDLIST_ABSOLUTE>(id));
                }
                return status;
            }
            itemIds.push_back(itemId);
        }

        auto status = SHCreateShellItemArrayFromIDLists(
            static_cast<UINT>(itemIds.size()), itemIds.data(), result
        );
        for (auto id : itemIds) {
            CoTaskMemFree(const_cast<PIDLIST_ABSOLUTE>(id));
        }
        return status;
    }

    template <class Interface>
    class FileDialogProxy : public Interface {
    public:
        explicit FileDialogProxy(Interface* inner, bool save)
          : m_inner(inner), m_save(save) {}

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
            if (!object) {
                return E_POINTER;
            }
            if (iid == IID_IUnknown || iid == IID_IFileDialog || iid == interfaceId()) {
                *object = static_cast<Interface*>(this);
                AddRef();
                return S_OK;
            }
            return m_inner->QueryInterface(iid, object);
        }

        ULONG STDMETHODCALLTYPE AddRef() override {
            return ++m_references;
        }

        ULONG STDMETHODCALLTYPE Release() override {
            auto references = --m_references;
            if (!references) {
                delete this;
            }
            return references;
        }

        HRESULT STDMETHODCALLTYPE Show(HWND owner) override {
            gdlinux::PickerRequest request;
            request.save = m_save;
            request.title = m_title;
            request.initialPath = m_folder;
            if (!m_fileName.empty()) {
                if (!request.initialPath.empty() && request.initialPath.back() != L'\\') {
                    request.initialPath += L"\\";
                }
                request.initialPath += m_fileName;
            }
            request.filters = m_filters;

            FILEOPENDIALOGOPTIONS options = 0;
            if (SUCCEEDED(m_inner->GetOptions(&options))) {
                request.directory = options & FOS_PICKFOLDERS;
                request.multiple = options & FOS_ALLOWMULTISELECT;
            }

            auto response = gdlinux::showNativePicker(request);
            m_nativeResult = response.status != gdlinux::PickerStatus::Unavailable;
            switch (response.status) {
                case gdlinux::PickerStatus::Selected:
                    m_paths = std::move(response.paths);
                    applyDefaultExtension();
                    return S_OK;
                case gdlinux::PickerStatus::Cancelled:
                    m_paths.clear();
                    return HRESULT_FROM_WIN32(ERROR_CANCELLED);
                case gdlinux::PickerStatus::Failed:
                    log::warn(
                        "Native Linux file picker failed ({})",
                        string::wideToUtf8(response.error)
                    );
                    m_nativeResult = false;
                    return m_inner->Show(owner);
                case gdlinux::PickerStatus::Unavailable:
                    m_nativeResult = false;
                    return m_inner->Show(owner);
            }
            return E_UNEXPECTED;
        }

        HRESULT STDMETHODCALLTYPE SetFileTypes(
            UINT count,
            COMDLG_FILTERSPEC const* filters
        ) override {
            m_filters.clear();
            if (filters) {
                for (UINT index = 0; index < count; ++index) {
                    m_filters.push_back({
                        filters[index].pszName ? filters[index].pszName : L"Files",
                        filters[index].pszSpec ? filters[index].pszSpec : L"*",
                    });
                }
            }
            return m_inner->SetFileTypes(count, filters);
        }

        HRESULT STDMETHODCALLTYPE SetFileTypeIndex(UINT index) override {
            return m_inner->SetFileTypeIndex(index);
        }

        HRESULT STDMETHODCALLTYPE GetFileTypeIndex(UINT* index) override {
            return m_inner->GetFileTypeIndex(index);
        }

        HRESULT STDMETHODCALLTYPE Advise(IFileDialogEvents* events, DWORD* cookie) override {
            return m_inner->Advise(events, cookie);
        }

        HRESULT STDMETHODCALLTYPE Unadvise(DWORD cookie) override {
            return m_inner->Unadvise(cookie);
        }

        HRESULT STDMETHODCALLTYPE SetOptions(FILEOPENDIALOGOPTIONS options) override {
            return m_inner->SetOptions(options);
        }

        HRESULT STDMETHODCALLTYPE GetOptions(FILEOPENDIALOGOPTIONS* options) override {
            return m_inner->GetOptions(options);
        }

        HRESULT STDMETHODCALLTYPE SetDefaultFolder(IShellItem* folder) override {
            rememberFolder(folder);
            return m_inner->SetDefaultFolder(folder);
        }

        HRESULT STDMETHODCALLTYPE SetFolder(IShellItem* folder) override {
            rememberFolder(folder);
            return m_inner->SetFolder(folder);
        }

        HRESULT STDMETHODCALLTYPE GetFolder(IShellItem** folder) override {
            return m_inner->GetFolder(folder);
        }

        HRESULT STDMETHODCALLTYPE GetCurrentSelection(IShellItem** selection) override {
            return m_inner->GetCurrentSelection(selection);
        }

        HRESULT STDMETHODCALLTYPE SetFileName(LPCWSTR name) override {
            m_fileName = name ? name : L"";
            return m_inner->SetFileName(name);
        }

        HRESULT STDMETHODCALLTYPE GetFileName(LPWSTR* name) override {
            return m_inner->GetFileName(name);
        }

        HRESULT STDMETHODCALLTYPE SetTitle(LPCWSTR title) override {
            m_title = title ? title : L"";
            return m_inner->SetTitle(title);
        }

        HRESULT STDMETHODCALLTYPE SetOkButtonLabel(LPCWSTR text) override {
            return m_inner->SetOkButtonLabel(text);
        }

        HRESULT STDMETHODCALLTYPE SetFileNameLabel(LPCWSTR label) override {
            return m_inner->SetFileNameLabel(label);
        }

        HRESULT STDMETHODCALLTYPE GetResult(IShellItem** item) override {
            if (!m_nativeResult) {
                return m_inner->GetResult(item);
            }
            if (m_paths.empty()) {
                return E_FAIL;
            }
            return shellItemForPath(m_paths.front(), item);
        }

        HRESULT STDMETHODCALLTYPE AddPlace(IShellItem* item, FDAP placement) override {
            return m_inner->AddPlace(item, placement);
        }

        HRESULT STDMETHODCALLTYPE SetDefaultExtension(LPCWSTR extension) override {
            m_defaultExtension = extension ? extension : L"";
            return m_inner->SetDefaultExtension(extension);
        }

        HRESULT STDMETHODCALLTYPE Close(HRESULT status) override {
            return m_inner->Close(status);
        }

        HRESULT STDMETHODCALLTYPE SetClientGuid(REFGUID guid) override {
            return m_inner->SetClientGuid(guid);
        }

        HRESULT STDMETHODCALLTYPE ClearClientData() override {
            return m_inner->ClearClientData();
        }

        HRESULT STDMETHODCALLTYPE SetFilter(IShellItemFilter* filter) override {
            return m_inner->SetFilter(filter);
        }

    protected:
        virtual ~FileDialogProxy() {
            m_inner->Release();
        }

        virtual IID const& interfaceId() const = 0;

        Interface* m_inner;
        bool m_nativeResult = false;
        std::vector<std::wstring> m_paths;

    private:
        void rememberFolder(IShellItem* folder) {
            auto path = shellItemPath(folder);
            if (!path.empty()) {
                m_folder = std::move(path);
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
            if (extension.empty()) {
                UINT selectedFilter = 1;
                m_inner->GetFileTypeIndex(&selectedFilter);
                if (selectedFilter > 0 && selectedFilter <= m_filters.size()) {
                    auto const& pattern = m_filters[selectedFilter - 1].patterns;
                    if (pattern.starts_with(L"*.") && pattern.find_first_of(L";*?", 2) == std::wstring::npos) {
                        extension = pattern.substr(2);
                    }
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

        std::atomic_ulong m_references { 1 };
        bool m_save;
        std::wstring m_title;
        std::wstring m_folder;
        std::wstring m_fileName;
        std::wstring m_defaultExtension;
        std::vector<gdlinux::PickerFilter> m_filters;
    };

    class OpenFileDialogProxy final : public FileDialogProxy<IFileOpenDialog> {
    public:
        explicit OpenFileDialogProxy(IFileOpenDialog* inner)
          : FileDialogProxy(inner, false) {}

        HRESULT STDMETHODCALLTYPE GetResults(IShellItemArray** items) override {
            if (!m_nativeResult) {
                return m_inner->GetResults(items);
            }
            return shellItemArrayForPaths(m_paths, items);
        }

        HRESULT STDMETHODCALLTYPE GetSelectedItems(IShellItemArray** items) override {
            if (!m_nativeResult) {
                return m_inner->GetSelectedItems(items);
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
        explicit SaveFileDialogProxy(IFileSaveDialog* inner)
          : FileDialogProxy(inner, true) {}

        HRESULT STDMETHODCALLTYPE SetSaveAsItem(IShellItem* item) override {
            return m_inner->SetSaveAsItem(item);
        }

        HRESULT STDMETHODCALLTYPE SetProperties(IPropertyStore* properties) override {
            return m_inner->SetProperties(properties);
        }

        HRESULT STDMETHODCALLTYPE SetCollectedProperties(
            IPropertyDescriptionList* properties,
            BOOL appendDefault
        ) override {
            return m_inner->SetCollectedProperties(properties, appendDefault);
        }

        HRESULT STDMETHODCALLTYPE GetProperties(IPropertyStore** properties) override {
            return m_inner->GetProperties(properties);
        }

        HRESULT STDMETHODCALLTYPE ApplyProperties(
            IShellItem* item,
            IPropertyStore* properties,
            HWND owner,
            IFileOperationProgressSink* sink
        ) override {
            return m_inner->ApplyProperties(item, properties, owner, sink);
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
        auto result = CoCreateInstance(classId, outer, context, interfaceId, object);
        if (FAILED(result) || !object || !*object || outer) {
            return result;
        }

        if (classId == CLSID_FileOpenDialog && interfaceId == IID_IFileOpenDialog) {
            auto inner = static_cast<IFileOpenDialog*>(*object);
            *object = static_cast<IFileOpenDialog*>(new OpenFileDialogProxy(inner));
        }
        else if (classId == CLSID_FileSaveDialog && interfaceId == IID_IFileSaveDialog) {
            auto inner = static_cast<IFileSaveDialog*>(*object);
            *object = static_cast<IFileSaveDialog*>(new SaveFileDialogProxy(inner));
        }
        return result;
    }
}

geode::Result<> gdlinux::installNativeFileDialogHook() {
    if (!isRunningUnderWine()) {
        log::info("Native Linux file picker inactive. Geometry Dash is not running under Wine (what??)");
        return Ok();
    }
    if (!hasNativePicker()) {
        log::warn("Native Linux file picker inactive: install zenity or kdialog");
        return Ok();
    }

    auto ole32 = GetModuleHandleW(L"ole32.dll");
    if (!ole32) {
        ole32 = LoadLibraryW(L"ole32.dll");
    }
    if (!ole32) {
        return Err("Unable to load ole32.dll");
    }
    auto address = reinterpret_cast<void*>(GetProcAddress(ole32, "CoCreateInstance"));
    if (!address) {
        return Err("Unable to resolve CoCreateInstance");
    }

    GEODE_UNWRAP(Mod::get()->hook(address, &coCreateInstanceHook, "Native Linux file picker"));
    log::info("Native Linux file picker enabled");
    return Ok();
}
