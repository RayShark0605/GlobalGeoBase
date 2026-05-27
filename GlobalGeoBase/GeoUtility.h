#ifndef GLOBALGEOBASE_GEO_UTILITY_H_H
#define GLOBALGEOBASE_GEO_UTILITY_H_H

#include "GlobalGeoBasePort.h"

#include <string>

/**
 * @brief GIS/空间参考相关通用工具接口。
 */
class GLOBALGEOBASE_PORT GeoUtility
{
public:
    /**
     * @brief 手动设置 GDAL/PROJ 使用的 PROJ 数据目录。
     * @param projDataDirectoryUtf8 包含 proj.db 的 PROJ 数据目录，UTF-8 编码；也允许直接传入 proj.db 文件路径。
     * @param errorMessageUtf8 可选错误信息输出，UTF-8 编码。
     * @return 设置成功返回 true；失败返回 false。
     *
     * 该接口会同时调用 OSRSetPROJSearchPaths()，并设置 GDAL 配置项 PROJ_DATA / PROJ_LIB。
     *
     * 调用关系约定：
     * - 如果本接口先设置成功，InitializeRuntime() 不再自动查找并覆盖 PROJ 数据目录；
     * - 如果 InitializeRuntime() 已经自动设置成功，本接口仍可以重新设置并覆盖 PROJ 数据目录；
     * - 设置失败时不会清除已有的自动或手动 PROJ 数据目录配置。
     */
    static bool SetGdalProjDataDirectory(const std::string& projDataDirectoryUtf8, std::string* errorMessageUtf8 = nullptr);
};

#endif
