#include "DelayLoadRuntime.h"

#ifdef max
#undef max
#endif
#include "GB_FileSystem.h"
#include "GB_Utf8String.h"

#include <windows.h>
#include <delayimp.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

#include <cpl_conv.h>
#include <ogr_srs_api.h>

namespace
{
    HMODULE globalBaseModuleHandle = nullptr;

    std::once_flag runtimeInitOnce;
    bool runtimeInitialized = false;
    bool runtimeInitializeSucceeded = false;

    LONG messageBoxShownFlag = 0;

    std::wstring GetGlobalBaseDirectory();
    std::wstring GetExecutableDirectory();

    bool EqualsIgnoreCaseAscii(const char* leftText, const char* rightText)
    {
        if (leftText == nullptr || rightText == nullptr)
        {
            return false;
        }

        while (*leftText != '\0' && *rightText != '\0')
        {
            char leftChar = *leftText;
            char rightChar = *rightText;

            if (leftChar >= 'A' && leftChar <= 'Z')
            {
                leftChar = static_cast<char>(leftChar - 'A' + 'a');
            }

            if (rightChar >= 'A' && rightChar <= 'Z')
            {
                rightChar = static_cast<char>(rightChar - 'A' + 'a');
            }

            if (leftChar != rightChar)
            {
                return false;
            }

            leftText++;
            rightText++;
        }

        return *leftText == '\0' && *rightText == '\0';
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


    void AppendDllNamesFromDirectory(std::vector<std::wstring>& dllNames, const std::wstring& directoryPath)
    {
        if (!DirectoryExists(directoryPath))
        {
            return;
        }

        WIN32_FIND_DATAW findData = {};
        const std::wstring searchPattern = JoinPath(directoryPath, L"*.dll");
        HANDLE findHandle = FindFirstFileW(searchPattern.c_str(), &findData);
        if (findHandle == INVALID_HANDLE_VALUE)
        {
            return;
        }

        do
        {
            if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
            {
                AppendUniqueDirectory(dllNames, findData.cFileName);
            }
        } while (FindNextFileW(findHandle, &findData) != FALSE);

        FindClose(findHandle);
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
        // 否则会把 本模块依赖也提前加载，这些 DLL 的初始化逻辑和下级依赖通常只应由它们自己的宿主模块按需触发。
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

    bool InitializeProjDataDirectory(std::wstring* failureReason)
    {
        const std::wstring projDataDirectory = FindProjDataDirectory();
        if (projDataDirectory.empty())
        {
            if (failureReason != nullptr)
            {
                *failureReason = L"未能在当前模块目录、可执行程序目录及其 GlobalBaseDependencies/share/proj、GlobalBaseDependencies/proj 等子目录中找到 proj.db。";
            }
            return false;
        }

        const std::string projDataDirectoryUtf8 = WideToUtf8(projDataDirectory);
        if (projDataDirectoryUtf8.empty())
        {
            if (failureReason != nullptr)
            {
                *failureReason = L"proj.db 所在目录路径无法转换为 UTF-8。";
            }
            return false;
        }

        const char* projSearchPaths[] = { projDataDirectoryUtf8.c_str(), nullptr };
        OSRSetPROJSearchPaths(projSearchPaths);

        CPLSetConfigOption("PROJ_DATA", projDataDirectoryUtf8.c_str());
        CPLSetConfigOption("PROJ_LIB", projDataDirectoryUtf8.c_str());

        return true;
    }

    static std::wstring GetLastErrorMessage(DWORD errorCode)
    {
        LPWSTR buffer = nullptr;
        const DWORD length = ::FormatMessageW(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr,
            errorCode,
            0,
            reinterpret_cast<LPWSTR>(&buffer),
            0,
            nullptr);

        std::wstring message;
        if (length > 0 && buffer != nullptr)
        {
            message.assign(buffer, length);
            ::LocalFree(buffer);
        }
        return message;
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

    void EnsureRuntimeInitializedInternal()
    {
        std::call_once(runtimeInitOnce, []() {
            runtimeInitialized = true;

            std::wstring failedDllList;
            runtimeInitializeSucceeded = PreloadManagedDependencies(&failedDllList);

            if (!runtimeInitializeSucceeded)
            {
                const std::wstring& runtimeDisplayName = GetRuntimeDisplayName();

                std::wstring messageText =
                    runtimeDisplayName +
                    L" 无法加载以下关键运行时依赖：\r\n\r\n" +
                    failedDllList +
                    L"\r\n\r\n"
                    L"加载顺序已按以下规则尝试：\r\n"
                    L"1. GlobalBaseDependencies 子目录\r\n"
                    L"2. 当前模块所在目录\r\n\r\n"
                    L"请检查部署目录、位数是否一致，以及依赖 DLL 自身的下级依赖是否齐全。";

                ShowMessageBoxOnce(messageText);
                return;
            }

            std::wstring projFailureReason;
            runtimeInitializeSucceeded = InitializeProjDataDirectory(&projFailureReason);
            if (!runtimeInitializeSucceeded)
            {
                const std::wstring& runtimeDisplayName = GetRuntimeDisplayName();

                std::wstring messageText =
                    runtimeDisplayName +
                    L" 无法初始化 PROJ 数据目录：\r\n\r\n" +
                    projFailureReason +
                    L"\r\n\r\n"
                    L"请把包含 proj.db 的 PROJ 数据目录部署到以下任一位置：\r\n"
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
                L"2. 当前模块所在目录";

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
    return runtimeInitializeSucceeded;
}
