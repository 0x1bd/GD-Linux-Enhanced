#include "NativeFileDialogHook.hpp"

#include "NativePicker.hpp"

#include <Geode/Geode.hpp>

#include <shlobj.h>
#include <shobjidl.h>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <vector>

using namespace geode::prelude;

namespace {
    thread_local bool g_creatingWineFallback = false;

    std::wstring shellItemPath(IShellItem *item) {
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

    HRESULT shellItemForPath(std::wstring const &path, IShellItem **result) {
        if (!result) {
            return E_POINTER;
        }
        *result = nullptr;
        return SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(result));
    }

    HRESULT shellItemArrayForPaths(
        std::vector<std::wstring> const &paths,
        IShellItemArray **result
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
        for (auto const &path: paths) {
            PIDLIST_ABSOLUTE itemId = nullptr;
            auto status = SHParseDisplayName(path.c_str(), nullptr, &itemId, 0, nullptr);
            if (FAILED(status)) {
                for (auto id: itemIds) {
                    CoTaskMemFree(const_cast<PIDLIST_ABSOLUTE>(id));
                }
                return status;
            }
            itemIds.push_back(itemId);
        }

        auto status = SHCreateShellItemArrayFromIDLists(
            static_cast<UINT>(itemIds.size()), itemIds.data(), result
        );
        for (auto id: itemIds) {
            CoTaskMemFree(const_cast<PIDLIST_ABSOLUTE>(id));
        }
        return status;
    }

    template<class Interface>
    class FileDialogProxy : public Interface {
    public:
        explicit FileDialogProxy(bool save)
            : m_save(save),
              m_options(
                  FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR |
                  (save ? FOS_OVERWRITEPROMPT : FOS_FILEMUSTEXIST)
              ) {
        }

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) override {
            if (!object) {
                return E_POINTER;
            }
            if (iid == IID_IUnknown || iid == IID_IFileDialog || iid == interfaceId()) {
                *object = static_cast<Interface *>(this);
                AddRef();
                return S_OK;
            }
            *object = nullptr;
            return E_NOINTERFACE;
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

            request.directory = m_options & FOS_PICKFOLDERS;
            request.multiple = m_options & FOS_ALLOWMULTISELECT;

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
                    return showWineFallback(owner);
                case gdlinux::PickerStatus::Unavailable:
                    m_nativeResult = false;
                    return showWineFallback(owner);
            }
            return E_UNEXPECTED;
        }

        HRESULT STDMETHODCALLTYPE SetFileTypes(
            UINT count,
            COMDLG_FILTERSPEC const *filters
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
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE SetFileTypeIndex(UINT index) override {
            m_fileTypeIndex = index;
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE GetFileTypeIndex(UINT *index) override {
            if (!index) {
                return E_POINTER;
            }
            *index = m_fileTypeIndex;
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE Advise(IFileDialogEvents *events, DWORD *cookie) override {
            return E_NOTIMPL;
        }

        HRESULT STDMETHODCALLTYPE Unadvise(DWORD cookie) override {
            return E_NOTIMPL;
        }

        HRESULT STDMETHODCALLTYPE SetOptions(FILEOPENDIALOGOPTIONS options) override {
            m_options = options;
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE GetOptions(FILEOPENDIALOGOPTIONS *options) override {
            if (!options) {
                return E_POINTER;
            }
            *options = m_options;
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE SetDefaultFolder(IShellItem *folder) override {
            rememberFolder(folder);
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE SetFolder(IShellItem *folder) override {
            rememberFolder(folder);
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE GetFolder(IShellItem **folder) override {
            if (!folder) {
                return E_POINTER;
            }
            if (m_folder.empty()) {
                *folder = nullptr;
                return E_FAIL;
            }
            return shellItemForPath(m_folder, folder);
        }

        HRESULT STDMETHODCALLTYPE GetCurrentSelection(IShellItem **selection) override {
            if (m_nativeResult && !m_paths.empty()) {
                return shellItemForPath(m_paths.front(), selection);
            }
            return GetFolder(selection);
        }

        HRESULT STDMETHODCALLTYPE SetFileName(LPCWSTR name) override {
            m_fileName = name ? name : L"";
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE GetFileName(LPWSTR *name) override {
            if (!name) {
                return E_POINTER;
            }
            auto bytes = (m_fileName.size() + 1) * sizeof(wchar_t);
            *name = static_cast<LPWSTR>(CoTaskMemAlloc(bytes));
            if (!*name) {
                return E_OUTOFMEMORY;
            }
            std::memcpy(*name, m_fileName.c_str(), bytes);
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE SetTitle(LPCWSTR title) override {
            m_title = title ? title : L"";
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE SetOkButtonLabel(LPCWSTR text) override {
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE SetFileNameLabel(LPCWSTR label) override {
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE GetResult(IShellItem **item) override {
            if (!m_nativeResult) {
                return m_inner ? m_inner->GetResult(item) : E_UNEXPECTED;
            }
            if (m_paths.empty()) {
                return E_FAIL;
            }
            return shellItemForPath(m_paths.front(), item);
        }

        HRESULT STDMETHODCALLTYPE AddPlace(IShellItem *item, FDAP placement) override {
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE SetDefaultExtension(LPCWSTR extension) override {
            m_defaultExtension = extension ? extension : L"";
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE Close(HRESULT status) override {
            return E_NOTIMPL;
        }

        HRESULT STDMETHODCALLTYPE SetClientGuid(REFGUID guid) override {
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE ClearClientData() override {
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE SetFilter(IShellItemFilter *filter) override {
            return E_NOTIMPL;
        }

    protected:
        virtual ~FileDialogProxy() {
            if (m_inner) {
                m_inner->Release();
            }
        }

        virtual IID const &interfaceId() const = 0;

        Interface *m_inner = nullptr;
        bool m_nativeResult = false;
        std::vector<std::wstring> m_paths;

    private:
        HRESULT showWineFallback(HWND owner) {
            auto status = createWineFallback();
            if (FAILED(status)) {
                return status;
            }
            return m_inner->Show(owner);
        }

        HRESULT createWineFallback() {
            if (m_inner) {
                return S_OK;
            }

            g_creatingWineFallback = true;
            auto classId = m_save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog;
            auto status = CoCreateInstance(
                classId, nullptr, CLSCTX_ALL, interfaceId(),
                reinterpret_cast<void **>(&m_inner)
            );
            g_creatingWineFallback = false;
            if (FAILED(status)) {
                return status;
            }

            m_inner->SetOptions(m_options);
            if (!m_filters.empty()) {
                std::vector<COMDLG_FILTERSPEC> filters;
                filters.reserve(m_filters.size());
                for (auto const &filter: m_filters) {
                    filters.push_back({filter.name.c_str(), filter.patterns.c_str()});
                }
                m_inner->SetFileTypes(static_cast<UINT>(filters.size()), filters.data());
            }
            m_inner->SetFileTypeIndex(m_fileTypeIndex);
            if (!m_folder.empty()) {
                IShellItem *folder = nullptr;
                if (SUCCEEDED(shellItemForPath(m_folder, &folder))) {
                    m_inner->SetFolder(folder);
                    folder->Release();
                }
            }
            if (!m_fileName.empty()) {
                m_inner->SetFileName(m_fileName.c_str());
            }
            if (!m_title.empty()) {
                m_inner->SetTitle(m_title.c_str());
            }
            if (!m_defaultExtension.empty()) {
                m_inner->SetDefaultExtension(m_defaultExtension.c_str());
            }
            return S_OK;
        }

        void rememberFolder(IShellItem *folder) {
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
                auto selectedFilter = m_fileTypeIndex;
                if (selectedFilter > 0 && selectedFilter <= m_filters.size()) {
                    auto const &pattern = m_filters[selectedFilter - 1].patterns;
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

        std::atomic_ulong m_references{1};
        bool m_save;
        FILEOPENDIALOGOPTIONS m_options;
        UINT m_fileTypeIndex = 1;
        std::wstring m_title;
        std::wstring m_folder;
        std::wstring m_fileName;
        std::wstring m_defaultExtension;
        std::vector<gdlinux::PickerFilter> m_filters;
    };

    class OpenFileDialogProxy final : public FileDialogProxy<IFileOpenDialog> {
    public:
        OpenFileDialogProxy() : FileDialogProxy(false) {
        }

        HRESULT STDMETHODCALLTYPE GetResults(IShellItemArray **items) override {
            if (!m_nativeResult) {
                return m_inner ? m_inner->GetResults(items) : E_UNEXPECTED;
            }
            return shellItemArrayForPaths(m_paths, items);
        }

        HRESULT STDMETHODCALLTYPE GetSelectedItems(IShellItemArray **items) override {
            if (!m_nativeResult) {
                return m_inner ? m_inner->GetSelectedItems(items) : E_UNEXPECTED;
            }
            return shellItemArrayForPaths(m_paths, items);
        }

    private:
        IID const &interfaceId() const override {
            return IID_IFileOpenDialog;
        }
    };

    class SaveFileDialogProxy final : public FileDialogProxy<IFileSaveDialog> {
    public:
        SaveFileDialogProxy() : FileDialogProxy(true) {
        }

        HRESULT STDMETHODCALLTYPE SetSaveAsItem(IShellItem *item) override {
            return E_NOTIMPL;
        }

        HRESULT STDMETHODCALLTYPE SetProperties(IPropertyStore *properties) override {
            return E_NOTIMPL;
        }

        HRESULT STDMETHODCALLTYPE SetCollectedProperties(
            IPropertyDescriptionList *properties,
            BOOL appendDefault
        ) override {
            return E_NOTIMPL;
        }

        HRESULT STDMETHODCALLTYPE GetProperties(IPropertyStore **properties) override {
            return E_NOTIMPL;
        }

        HRESULT STDMETHODCALLTYPE ApplyProperties(
            IShellItem *item,
            IPropertyStore *properties,
            HWND owner,
            IFileOperationProgressSink *sink
        ) override {
            return E_NOTIMPL;
        }

    private:
        IID const &interfaceId() const override {
            return IID_IFileSaveDialog;
        }
    };

    HRESULT WINAPI coCreateInstanceHook(
        REFCLSID classId,
        LPUNKNOWN outer,
        DWORD context,
        REFIID interfaceId,
        LPVOID *object
    ) {
        if (g_creatingWineFallback) {
            return CoCreateInstance(classId, outer, context, interfaceId, object);
        }
        if (!object) {
            return E_POINTER;
        }

        if (!outer && classId == CLSID_FileOpenDialog && interfaceId == IID_IFileOpenDialog) {
            *object = static_cast<IFileOpenDialog *>(new OpenFileDialogProxy());
            return S_OK;
        }
        if (!outer && classId == CLSID_FileSaveDialog && interfaceId == IID_IFileSaveDialog) {
            *object = static_cast<IFileSaveDialog *>(new SaveFileDialogProxy());
            return S_OK;
        }
        return CoCreateInstance(classId, outer, context, interfaceId, object);
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
    auto address = reinterpret_cast<void *>(GetProcAddress(ole32, "CoCreateInstance"));
    if (!address) {
        return Err("Unable to resolve CoCreateInstance");
    }

    GEODE_UNWRAP(Mod::get()->hook(address, &coCreateInstanceHook, "Native Linux file picker"));
    log::info("Native Linux file picker enabled");
    return Ok();
}
