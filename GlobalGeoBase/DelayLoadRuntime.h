#pragma once
#include "GlobalGeoBasePort.h"
#include <windows.h>

/**
 * @brief 初始化延迟加载运行时环境。
 *
 * 该函数会主动尝试加载当前模块依赖的延迟加载三方 DLL，并初始化 GDAL/PROJ 资源目录。
 *
 * DLL 加载顺序为：
 * 1. 当前模块所在目录下的 GlobalBaseDependencies 子目录；
 * 2. 当前模块所在目录。
 *
 * PROJ 数据目录会自动在当前模块目录、可执行程序目录及其 GlobalBaseDependencies/share/proj、
 * GlobalBaseDependencies/proj、share/proj、proj 等子目录中查找。找到包含 proj.db 的目录后，
 * 会通过 OSRSetPROJSearchPaths() 和 GDAL 配置项设置给 GDAL/PROJ。
 *
 * 如果任意关键依赖或 PROJ 数据目录初始化失败，会弹出一次 MessageBox 进行告警。
 *
 * @return true  所有关键依赖均已成功准备就绪。
 * @return false 至少存在一个关键依赖未成功准备就绪。
 */
GLOBALGEOBASE_PORT bool InitializeRuntime();

/**
 * @brief 供 DllMain 在 DLL_PROCESS_ATTACH 时记录当前模块句柄。
 *
 * @param moduleHandle 当前模块的模块句柄。
 */
GLOBALGEOBASE_PORT void SetSelfModuleHandle(HMODULE moduleHandle);
