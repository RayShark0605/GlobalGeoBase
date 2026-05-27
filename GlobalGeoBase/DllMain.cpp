#include <windows.h>
#include "DelayLoadRuntime.h"

BOOL APIENTRY DllMain(HMODULE moduleHandle, DWORD reason, LPVOID reserved)
{
    UNREFERENCED_PARAMETER(reserved);

    if (reason == DLL_PROCESS_ATTACH)
    {
        SetSelfModuleHandle(moduleHandle);
        DisableThreadLibraryCalls(moduleHandle);
    }

    return TRUE;
}