#pragma once

#if defined(GLOBALGEOBASE_EXPORTS)
#define GLOBALGEOBASE_PORT __declspec(dllexport)
#else
#define GLOBALGEOBASE_PORT __declspec(dllimport)
#endif