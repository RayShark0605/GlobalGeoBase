#include "DelayLoadRuntime.h"

#ifdef max
#undef max
#endif
#include "GB_FileSystem.h"
#include "GB_Utf8String.h"

#include <windows.h>
#include <delayimp.h>

#include <algorithm>
#include <cctype>
#include <mutex>
#include <string>
#include <vector>

#include <cpl_conv.h>
#include <ogr_srs_api.h>

namespace
{
    enum class ProjDataDirectorySource
    {
        NotSet,
        Automatic,
        Manual
    };

    HMODULE globalBaseModuleHandle = nullptr;

    std::once_flag runtimeDependencyInitOnce;
    bool runtimeDependenciesSucceeded = false;
    std::wstring runtimeDependencyFailureDllList;

    std::once_flag runtimeInitOnce;

    std::mutex projDataDirectoryMutex;
    ProjDataDirectorySource projDataDirectorySource = ProjDataDirectorySource::NotSet;
    std::string projDataDirectoryUtf8;

    LONG messageBoxShownFlag = 0;

    std::wstring GetGlobalBaseDirectory();
    std::wstring GetExecutableDirectory();

    std::string TrimAscii(const std::string& text)
    {
        std::size_t beginIndex = 0;
        while (beginIndex < text.size() && std::isspace(static_cast<unsigned char>(text[beginIndex])) != 0)
        {
            beginIndex++;
        }

        std::size_t endIndex = text.size();
        while (endIndex > beginIndex && std::isspace(static_cast<unsigned char>(text[endIndex - 1])) != 0)
        {
            endIndex--;
        }

        return text.substr(beginIndex, endIndex - beginIndex);
    }

    bool EqualsIgnoreCaseAscii(const wchar_t* leftText, const wchar_t* rightText)
    {
        if (leftText == nullptr || rightText == nullptr)
        {
            return false;
        }

        while (*leftText != L'\0' && *rightText != L'\0')
        {
            wchar_t leftChar = *leftText;
            wchar_t rightChar = *rightText;

            if (leftChar >= L'A' && leftChar <= L'Z')
            {
                leftChar = static_cast<wchar_t>(leftChar - L'A' + L'a');
            }

            if (rightChar >= L'A' && rightChar <= L'Z')
            {
                rightChar = static_cast<wchar_t>(rightChar - L'A' + L'a');
            }

            if (leftChar != rightChar)
            {
                return false;
            }

            leftText++;
            rightText++;
        }

        return *leftText == L'\0' && *rightText == L'\0';
    }

    std::wstring Utf8OrAnsiToWide(const char* text)
    {
        if (text == nullptr || *text == '\0')
        {
            return L"";
        }

        const int utf8Length = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
        if (utf8Length > 0)
        {
            std::wstring wideText;
            wideText.resize(static_cast<size_t>(utf8Length - 1));
            MultiByteToWideChar(CP_UTF8, 0, text, -1, &wideText[0], utf8Length);
            return wideText;
        }

        const int ansiLength = MultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
        if (ansiLength > 0)
        {
            std::wstring wideText;
            wideText.resize(static_cast<size_t>(ansiLength - 1));
            MultiByteToWideChar(CP_ACP, 0, text, -1, &wideText[0], ansiLength);
            return wideText;
        }

        return L"";
    }

    std::wstring JoinPath(const std::wstring& leftPath, const std::wstring& rightPath)
    {
        if (leftPath.empty())
        {
            return rightPath;
        }

        if (rightPath.empty())
        {
            return leftPath;
        }

        if (leftPath.back() == L'\\' || leftPath.back() == L'/')
        {
            return leftPath + rightPath;
        }

        return leftPath + L"\\" + rightPath;
    }

    std::wstring GetDirectoryFromFilePath(const std::wstring& filePath)
    {
        const std::wstring::size_type pos = filePath.find_last_of(L"\\/");
        if (pos == std::wstring::npos)
        {
            return L"";
        }

        if (pos == 2 && filePath.size() >= 3 && filePath[1] == L':' && (filePath[2] == L'\\' || filePath[2] == L'/'))
        {
            return filePath.substr(0, 3);
        }

        return filePath.substr(0, pos);
    }

    std::string WideToUtf8(const std::wstring& wideText)
    {
        if (wideText.empty())
        {
            return std::string();
        }

        const int utf8Length = WideCharToMultiByte(CP_UTF8, 0, wideText.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (utf8Length <= 0)
        {
            return std::string();
        }

        std::string utf8Text;
        utf8Text.resize(static_cast<size_t>(utf8Length - 1));
        WideCharToMultiByte(CP_UTF8, 0, wideText.c_str(), -1, &utf8Text[0], utf8Length, nullptr, nullptr);
        return utf8Text;
    }

    bool FileExists(const std::wstring& filePath)
    {
        const DWORD attributes = GetFileAttributesW(filePath.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    }

    bool DirectoryExists(const std::wstring& directoryPath)
    {
        const DWORD attributes = GetFileAttributesW(directoryPath.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }

    bool DirectoryContainsFile(const std::wstring& directoryPath, const std::wstring& fileName)
    {
        if (!DirectoryExists(directoryPath))
        {
            return false;
        }
        return FileExists(JoinPath(directoryPath, fileName));
    }

    void AppendUniqueDirectory(std::vector<std::wstring>& directoryPaths, const std::wstring& directoryPath)
    {
        if (directoryPath.empty())
        {
            return;
        }

        for (size_t i = 0; i < directoryPaths.size(); i++)
        {
            if (EqualsIgnoreCaseAscii(directoryPaths[i].c_str(), directoryPath.c_str()))
            {
                return;
            }
        }

        directoryPaths.push_back(directoryPath);
    }

    const std::wstring& GetRuntimeDisplayName()
    {
#ifdef _DEBUG
        static const std::wstring runtimeDisplayName = L"GlobalGeoBased";
#else
        static const std::wstring runtimeDisplayName = L"GlobalGeoBase";
#endif
        return runtimeDisplayName;
    }

    std::wstring GetRuntimeFailureMessageBoxCaption()
    {
        return GetRuntimeDisplayName() + L" 运行时依赖加载失败";
    }

    const std::vector<std::wstring>& GetManagedDelayLoadDllNames()
    {
        // 这里只能维护 GlobalGeoBase 自己在链接器 /DELAYLOAD 中声明的 DLL。
        // 否则会把其它模块依赖也提前加载，这些 DLL 的初始化逻辑和下级依赖通常只应由它们自己的宿主模块按需触发。
#ifdef _DEBUG
        static const std::vector<std::wstring> dllNames = { L"gdald.dll" };
#else
        static const std::vector<std::wstring> dllNames = { L"gdal.dll" };
#endif
        return dllNames;
    }

    std::wstring GetGlobalBaseDirectory()
    {
        if (globalBaseModuleHandle == nullptr)
        {
            return L"";
        }

        std::vector<wchar_t> filePathBuffer(MAX_PATH, L'\0');

        while (true)
        {
            const DWORD copiedLength = GetModuleFileNameW(globalBaseModuleHandle, filePathBuffer.data(), static_cast<DWORD>(filePathBuffer.size()));

            if (copiedLength == 0)
            {
                return L"";
            }

            if (copiedLength < filePathBuffer.size() - 1)
            {
                const std::wstring filePath(filePathBuffer.data(), copiedLength);
                return GetDirectoryFromFilePath(filePath);
            }

            filePathBuffer.resize(filePathBuffer.size() * 2, L'\0');
        }
    }

    std::wstring GetExecutableDirectory()
    {
        const std::string executableDirectoryUtf8 = GB_GetExeDirectory();
        if (executableDirectoryUtf8.empty())
        {
            return L"";
        }
        return GB_Utf8ToWString(executableDirectoryUtf8);
    }

    void AppendProjSearchPathCandidates(std::vector<std::wstring>& candidatePaths, const std::wstring& rootDirectory)
    {
        if (rootDirectory.empty())
        {
            return;
        }

        const std::wstring dependencyDirectory = JoinPath(rootDirectory, L"GlobalBaseDependencies");
        AppendUniqueDirectory(candidatePaths, JoinPath(dependencyDirectory, L"share\\proj"));
        AppendUniqueDirectory(candidatePaths, JoinPath(dependencyDirectory, L"share\\proj9"));
        AppendUniqueDirectory(candidatePaths, JoinPath(dependencyDirectory, L"proj"));
        AppendUniqueDirectory(candidatePaths, dependencyDirectory);
        AppendUniqueDirectory(candidatePaths, JoinPath(rootDirectory, L"share\\proj"));
        AppendUniqueDirectory(candidatePaths, JoinPath(rootDirectory, L"share\\proj9"));
        AppendUniqueDirectory(candidatePaths, JoinPath(rootDirectory, L"proj"));
        AppendUniqueDirectory(candidatePaths, rootDirectory);
    }

    std::wstring FindProjDataDirectory()
    {
        std::vector<std::wstring> candidatePaths;
        AppendProjSearchPathCandidates(candidatePaths, GetGlobalBaseDirectory());
        AppendProjSearchPathCandidates(candidatePaths, GetExecutableDirectory());

        for (size_t i = 0; i < candidatePaths.size(); i++)
        {
            if (DirectoryContainsFile(candidatePaths[i], L"proj.db"))
            {
                return candidatePaths[i];
            }
        }

        return L"";
    }

    bool IsProjDataDirectoryConfigured()
    {
        std::lock_guard<std::mutex> lockGuard(projDataDirectoryMutex);
        return projDataDirectorySource != ProjDataDirectorySource::NotSet && !projDataDirectoryUtf8.empty();
    }

    void ApplyProjDataDirectoryUtf8(const std::string& directoryPathUtf8)
    {
        const char* projSearchPaths[] = { directoryPathUtf8.c_str(), nullptr };
        OSRSetPROJSearchPaths(projSearchPaths);

        CPLSetConfigOption("PROJ_DATA", directoryPathUtf8.c_str());
        CPLSetConfigOption("PROJ_LIB", directoryPathUtf8.c_str());
    }

    bool InitializeProjDataDirectory(std::wstring* failureReason)
    {
        {
            std::lock_guard<std::mutex> lockGuard(projDataDirectoryMutex);
            if (projDataDirectorySource == ProjDataDirectorySource::Manual)
            {
                return true;
            }
            if (projDataDirectorySource == ProjDataDirectorySource::Automatic && !projDataDirectoryUtf8.empty())
            {
                return true;
            }
        }

        const std::wstring foundProjDataDirectory = FindProjDataDirectory();
        if (foundProjDataDirectory.empty())
        {
            if (failureReason != nullptr)
            {
                *failureReason = L"未能在当前模块目录、可执行程序目录及其 GlobalBaseDependencies/share/proj、GlobalBaseDependencies/proj 等子目录中找到 proj.db。";
            }
            return false;
        }

        const std::string foundProjDataDirectoryUtf8 = WideToUtf8(foundProjDataDirectory);
        if (foundProjDataDirectoryUtf8.empty())
        {
            if (failureReason != nullptr)
            {
                *failureReason = L"proj.db 所在目录路径无法转换为 UTF-8。";
            }
            return false;
        }

        std::lock_guard<std::mutex> lockGuard(projDataDirectoryMutex);
        if (projDataDirectorySource == ProjDataDirectorySource::Manual)
        {
            return true;
        }

        ApplyProjDataDirectoryUtf8(foundProjDataDirectoryUtf8);
        projDataDirectoryUtf8 = foundProjDataDirectoryUtf8;
        projDataDirectorySource = ProjDataDirectorySource::Automatic;
        return true;
    }

    bool IsManagedDelayLoadDll(const char* dllName)
    {
        if (dllName == nullptr || *dllName == '\0')
        {
            return false;
        }

        const std::wstring dllNameWide = Utf8OrAnsiToWide(dllName);
        if (dllNameWide.empty())
        {
            return false;
        }

        const std::vector<std::wstring>& managedDelayLoadDllNames = GetManagedDelayLoadDllNames();
        for (size_t i = 0; i < managedDelayLoadDllNames.size(); i++)
        {
            if (EqualsIgnoreCaseAscii(dllNameWide.c_str(), managedDelayLoadDllNames[i].c_str()))
            {
                return true;
            }
        }

        return false;
    }

    HMODULE LoadDependencyByPreferredRule(const wchar_t* dllName)
    {
        if (dllName == nullptr || *dllName == L'\0')
        {
            return nullptr;
        }

        std::vector<std::wstring> candidatePaths;

        const std::wstring globalBaseDirectory = GetGlobalBaseDirectory();
        AppendUniqueDirectory(candidatePaths, JoinPath(JoinPath(globalBaseDirectory, L"GlobalBaseDependencies"), dllName));
        AppendUniqueDirectory(candidatePaths, JoinPath(globalBaseDirectory, dllName));

        const std::wstring executableDirectory = GetExecutableDirectory();
        AppendUniqueDirectory(candidatePaths, JoinPath(JoinPath(executableDirectory, L"GlobalBaseDependencies"), dllName));
        AppendUniqueDirectory(candidatePaths, JoinPath(executableDirectory, dllName));

        for (size_t i = 0; i < candidatePaths.size(); i++)
        {
            HMODULE moduleHandle = LoadLibraryExW(candidatePaths[i].c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            if (moduleHandle != nullptr)
            {
                return moduleHandle;
            }
        }

        return nullptr;
    }

    HMODULE LoadDependencyByPreferredRule(const char* dllName)
    {
        return LoadDependencyByPreferredRule(Utf8OrAnsiToWide(dllName).c_str());
    }

    void ShowMessageBoxOnce(const std::wstring& messageText)
    {
        if (InterlockedCompareExchange(&messageBoxShownFlag, 1, 0) != 0)
        {
            return;
        }

        MessageBoxW(nullptr, messageText.c_str(), GetRuntimeFailureMessageBoxCaption().c_str(), MB_OK | MB_ICONERROR | MB_TOPMOST);
    }

    bool PreloadManagedDependencies(std::wstring* failedDllList)
    {
        std::vector<std::wstring> failedNames;

        const std::vector<std::wstring>& dllNames = GetManagedDelayLoadDllNames();
        for (size_t i = 0; i < dllNames.size(); i++)
        {
            const std::wstring& dllName = dllNames[i];

            const HMODULE moduleHandle = LoadDependencyByPreferredRule(dllName.c_str());
            if (moduleHandle == nullptr)
            {
                failedNames.push_back(dllName);
            }
        }

        if (!failedNames.empty())
        {
            if (failedDllList != nullptr)
            {
                failedDllList->clear();

                for (size_t i = 0; i < failedNames.size(); i++)
                {
                    if (i > 0)
                    {
                        *failedDllList += L"\r\n";
                    }

                    *failedDllList += failedNames[i];
                }
            }

            return false;
        }

        return true;
    }

    void EnsureRuntimeDependenciesLoadedInternal()
    {
        std::call_once(runtimeDependencyInitOnce, []() {
            runtimeDependenciesSucceeded = PreloadManagedDependencies(&runtimeDependencyFailureDllList);
            });
    }

    void EnsureRuntimeInitializedInternal()
    {
        std::call_once(runtimeInitOnce, []() {
            EnsureRuntimeDependenciesLoadedInternal();

            if (!runtimeDependenciesSucceeded)
            {
                const std::wstring& runtimeDisplayName = GetRuntimeDisplayName();

                std::wstring messageText =
                    runtimeDisplayName +
                    L" 无法加载以下关键运行时依赖：\r\n\r\n" +
                    runtimeDependencyFailureDllList +
                    L"\r\n\r\n"
                    L"加载顺序已按以下规则尝试：\r\n"
                    L"1. GlobalBaseDependencies 子目录\r\n"
                    L"2. 当前模块所在目录\r\n"
                    L"3. 可执行程序所在目录下的同名相对目录\r\n\r\n"
                    L"请检查部署目录、位数是否一致，以及依赖 DLL 自身的下级依赖是否齐全。";

                ShowMessageBoxOnce(messageText);
                return;
            }

            std::wstring projFailureReason;
            if (!InitializeProjDataDirectory(&projFailureReason))
            {
                const std::wstring& runtimeDisplayName = GetRuntimeDisplayName();

                std::wstring messageText =
                    runtimeDisplayName +
                    L" 无法初始化 PROJ 数据目录：\r\n\r\n" +
                    projFailureReason +
                    L"\r\n\r\n"
                    L"请把包含 proj.db 的 PROJ 数据目录部署到以下任一位置，或调用 GeoUtility::SetGdalProjDataDirectory() 手动指定：\r\n"
                    L"1. 当前模块所在目录\\GlobalBaseDependencies\\share\\proj\r\n"
                    L"2. 当前模块所在目录\\GlobalBaseDependencies\\proj\r\n"
                    L"3. 当前模块所在目录\\share\\proj\r\n"
                    L"4. 可执行程序所在目录下的同名相对目录";

                ShowMessageBoxOnce(messageText);
            }
            });
    }

    FARPROC WINAPI DelayLoadNotifyHook(unsigned notification, PDelayLoadInfo delayLoadInfo)
    {
        if (delayLoadInfo == nullptr)
        {
            return nullptr;
        }

        if (notification == dliNotePreLoadLibrary)
        {
            if (!IsManagedDelayLoadDll(delayLoadInfo->szDll))
            {
                return nullptr;
            }

            const HMODULE moduleHandle = LoadDependencyByPreferredRule(delayLoadInfo->szDll);
            if (moduleHandle != nullptr)
            {
                return reinterpret_cast<FARPROC>(moduleHandle);
            }
        }

        return nullptr;
    }

    FARPROC WINAPI DelayLoadFailureHook(unsigned notification, PDelayLoadInfo delayLoadInfo)
    {
        if (delayLoadInfo == nullptr)
        {
            return nullptr;
        }

        if (!IsManagedDelayLoadDll(delayLoadInfo->szDll))
        {
            return nullptr;
        }

        if (notification == dliFailLoadLib)
        {
            const std::wstring& runtimeDisplayName = GetRuntimeDisplayName();
            std::wstring dllName = Utf8OrAnsiToWide(delayLoadInfo->szDll);

            std::wstring messageText =
                runtimeDisplayName +
                L" 在延迟加载阶段仍然无法加载依赖 DLL：\r\n\r\n" +
                dllName +
                L"\r\n\r\n"
                L"已尝试以下位置：\r\n"
                L"1. GlobalBaseDependencies 子目录\r\n"
                L"2. 当前模块所在目录\r\n"
                L"3. 可执行程序所在目录下的同名相对目录";

            ShowMessageBoxOnce(messageText);
            return nullptr;
        }

        if (notification == dliFailGetProc)
        {
            const std::wstring& runtimeDisplayName = GetRuntimeDisplayName();
            std::wstring dllName = Utf8OrAnsiToWide(delayLoadInfo->szDll);
            std::wstring functionName;

            if (delayLoadInfo->dlp.fImportByName && delayLoadInfo->dlp.szProcName != nullptr)
            {
                functionName = Utf8OrAnsiToWide(delayLoadInfo->dlp.szProcName);
            }
            else
            {
                functionName = L"(ordinal import)";
            }

            std::wstring messageText =
                runtimeDisplayName +
                L" 已加载 DLL，但无法解析导入符号：\r\n\r\nDLL: " +
                dllName +
                L"\r\n符号: " +
                functionName +
                L"\r\n\r\n这通常表示 DLL 版本不匹配。";

            ShowMessageBoxOnce(messageText);
            return nullptr;
        }

        return nullptr;
    }
}

ExternC const PfnDliHook __pfnDliNotifyHook2 = DelayLoadNotifyHook;
ExternC const PfnDliHook __pfnDliFailureHook2 = DelayLoadFailureHook;

void SetSelfModuleHandle(HMODULE moduleHandle)
{
    globalBaseModuleHandle = moduleHandle;
}

bool InitializeRuntime()
{
    EnsureRuntimeInitializedInternal();
    return runtimeDependenciesSucceeded && IsProjDataDirectoryConfigured();
}

bool SetRuntimeProjDataDirectoryFromUser(const std::string& projDataDirectoryPathUtf8, std::string* errorMessageUtf8)
{
    if (errorMessageUtf8 != nullptr)
    {
        errorMessageUtf8->clear();
    }

    const std::string trimmedProjDataDirectoryPathUtf8 = TrimAscii(projDataDirectoryPathUtf8);
    if (trimmedProjDataDirectoryPathUtf8.empty())
    {
        if (errorMessageUtf8 != nullptr)
        {
            *errorMessageUtf8 = "PROJ 数据目录不能为空。";
        }
        return false;
    }

    std::wstring projDataDirectoryPath = GB_Utf8ToWString(trimmedProjDataDirectoryPathUtf8);
    if (projDataDirectoryPath.empty())
    {
        if (errorMessageUtf8 != nullptr)
        {
            *errorMessageUtf8 = "PROJ 数据目录路径无法按 UTF-8 转换。";
        }
        return false;
    }

    if (FileExists(projDataDirectoryPath))
    {
        const std::wstring::size_type fileNameBeginIndex = projDataDirectoryPath.find_last_of(L"\\/");
        const std::wstring fileName = projDataDirectoryPath.substr(fileNameBeginIndex == std::wstring::npos ? 0 : fileNameBeginIndex + 1);
        if (EqualsIgnoreCaseAscii(fileName.c_str(), L"proj.db"))
        {
            projDataDirectoryPath = GetDirectoryFromFilePath(projDataDirectoryPath);
        }
    }

    if (!DirectoryExists(projDataDirectoryPath))
    {
        if (errorMessageUtf8 != nullptr)
        {
            *errorMessageUtf8 = "PROJ 数据目录不存在或不是目录：" + trimmedProjDataDirectoryPathUtf8;
        }
        return false;
    }

    if (!DirectoryContainsFile(projDataDirectoryPath, L"proj.db"))
    {
        if (errorMessageUtf8 != nullptr)
        {
            *errorMessageUtf8 = "PROJ 数据目录中没有找到 proj.db：" + WideToUtf8(projDataDirectoryPath);
        }
        return false;
    }

    const std::string normalizedProjDataDirectoryUtf8 = WideToUtf8(projDataDirectoryPath);
    if (normalizedProjDataDirectoryUtf8.empty())
    {
        if (errorMessageUtf8 != nullptr)
        {
            *errorMessageUtf8 = "PROJ 数据目录路径无法转换为 UTF-8：" + trimmedProjDataDirectoryPathUtf8;
        }
        return false;
    }

    EnsureRuntimeDependenciesLoadedInternal();
    if (!runtimeDependenciesSucceeded)
    {
        if (errorMessageUtf8 != nullptr)
        {
            *errorMessageUtf8 = "无法加载关键运行时依赖，不能设置 PROJ 数据目录：" + WideToUtf8(runtimeDependencyFailureDllList);
        }
        return false;
    }

    {
        std::lock_guard<std::mutex> lockGuard(projDataDirectoryMutex);
        ApplyProjDataDirectoryUtf8(normalizedProjDataDirectoryUtf8);
        projDataDirectoryUtf8 = normalizedProjDataDirectoryUtf8;
        projDataDirectorySource = ProjDataDirectorySource::Manual;
    }

    return true;
}
