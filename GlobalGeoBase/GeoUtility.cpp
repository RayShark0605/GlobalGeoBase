#include "GeoUtility.h"

#include "DelayLoadRuntime.h"

bool GeoUtility::SetGdalProjDataDirectory(const std::string& projDataDirectoryUtf8, std::string* errorMessageUtf8)
{
    return SetRuntimeProjDataDirectoryFromUser(projDataDirectoryUtf8, errorMessageUtf8);
}
