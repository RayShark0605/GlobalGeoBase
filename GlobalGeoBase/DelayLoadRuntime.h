#pragma once
#include "GlobalGeoBasePort.h"

#include <string>
#include <windows.h>

/**
 * @brief 初始化延迟加载运行时环境。
 *
 * 该函数会主动尝试加载当前模块依赖的延迟加载三方 DLL，并初始化 GDAL/PROJ 资源目录。
 *
 * DLL 加载顺序为：
 * 1. 当前模块所在目录下的 GlobalBaseDependencies 子目录；
 * 2. 当前模块所在目录；
 * 3. 可执行程序所在目录下的同名相对目录。
 *
 * PROJ 数据目录会自动在当前模块目录、可执行程序目录及其 GlobalBaseDependencies/share/proj、
 * GlobalBaseDependencies/proj、share/proj、proj 等子目录中查找。找到包含 proj.db 的目录后，
 * 会通过 OSRSetPROJSearchPaths() 和 GDAL 配置项设置给 GDAL/PROJ。
 *
 * 如果 GeoUtility::SetGdalProjDataDirectory() 已经手动设置成功，InitializeRuntime() 不再自动查找和覆盖 PROJ 数据目录；
 * 如果 InitializeRuntime() 已经自动设置成功，后续仍允许 GeoUtility::SetGdalProjDataDirectory() 手动覆盖。
 *
 * 如果任意关键依赖或 PROJ 数据目录初始化失败，会弹出一次 MessageBox 进行告警。
 *
 * @return true  所有关键依赖均已成功准备就绪，并且 PROJ 数据目录已成功设置。
 * @return false 至少存在一个关键依赖未成功准备就绪，或 PROJ 数据目录未成功设置。
 */
GLOBALGEOBASE_PORT bool InitializeRuntime();

/**
 * @brief 供 DllMain 在 DLL_PROCESS_ATTACH 时记录当前模块句柄。
 *
 * @param moduleHandle 当前模块的模块句柄。
 */
GLOBALGEOBASE_PORT void SetSelfModuleHandle(HMODULE moduleHandle);

/**
 * @brief 设置运行时 PROJ 数据目录。
 *
 * 该接口仅供 GeoUtility.cpp 转发调用。业务代码应优先调用 GeoUtility::SetGdalProjDataDirectory()。
 *
 * @param projDataDirectoryUtf8 包含 proj.db 的 PROJ 数据目录，UTF-8 编码；也允许直接传入 proj.db 文件路径。
 * @param errorMessageUtf8 可选错误信息输出，UTF-8 编码。
 * @return 设置成功返回 true；失败返回 false。
 */
GLOBALGEOBASE_PORT bool SetRuntimeProjDataDirectoryFromUser(const std::string& projDataDirectoryUtf8, std::string* errorMessageUtf8 = nullptr);
