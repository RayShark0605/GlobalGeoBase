#ifndef GLOBALGEOBASE_GEO_EXTENT_H_H
#define GLOBALGEOBASE_GEO_EXTENT_H_H

#include "GlobalGeoBasePort.h"
#include "GeoCrs.h"
#include "Geometry/GB_Point2d.h"
#include "Geometry/GB_Rectangle.h"

#include <string>
#include <vector>

/**
 * @brief 带坐标参考系统的二维轴对齐空间范围。
 *
 * GeoExtent 只表达“范围包围盒”（Axis-Aligned Bounding Box / AABB），不表达精确裁剪后的真实几何区域：
 * - 对投影坐标系、工程坐标系、局部坐标系等平面坐标系，rectangle 遵循 GB_Rectangle 的常规语义，
 *   即 minX/minY/maxX/maxY 必须是数值意义上的最小/最大坐标，且 minX <= maxX、minY <= maxY；
 * - 对地理坐标系，rectangle 使用传统 GIS 坐标顺序 X/Y，即经度/纬度。
 *   此时 rectangle.minX 表示西边界经度，rectangle.maxX 表示东边界经度，rectangle.minY 表示南边界纬度，rectangle.maxY 表示北边界纬度；
 * - 对跨 ±180° 经线的地理范围，允许 rectangle.minX > rectangle.maxX。
 *   例如 [170, -20, -170, -16] 表示从 170°E 向东跨过 180° 到 170°W 的小范围，而不是横跨 340° 的大范围。
 *
 * @note GB_Rectangle 的构造函数和 Set() 会自动把 min/max 归一化，因此不要用 GB_Rectangle(170, -20, -170, -16)
 *       来构造跨反经线范围；推荐使用 FromGeographicBounds()，或直接设置 rectangle 的 minX/minY/maxX/maxY 四个字段。
 */
struct GLOBALGEOBASE_PORT GeoExtent
{
    /** @brief 范围所属坐标参考系统。 */
    GeoCrs crs;

    /** @brief 二维轴对齐范围。地理坐标系跨反经线时允许 minX > maxX，详见类注释。 */
    GB_Rectangle rectangle;

    /** @brief 构造一个空范围。 */
    GeoExtent();

    /**
     * @brief 构造一个仅带 CRS 的范围。
     * @param crs 坐标参考系统。
     *
     * 该构造函数不会自动填充 rectangle，因此 IsValid() 通常返回 false。
     */
    explicit GeoExtent(const GeoCrs& crs);

    /**
     * @brief 根据 CRS 和矩形范围构造 GeoExtent。
     * @param crs 坐标参考系统。
     * @param rectangle 二维范围。
     *
     * 对地理坐标系，若要表达跨反经线范围，请不要使用会自动归一化的 GB_Rectangle 构造函数构造 rectangle。
     */
    GeoExtent(const GeoCrs& crs, const GB_Rectangle& rectangle);

    /**
     * @brief 根据 CRS 用户输入构造一个仅带 CRS 的范围。
     * @param crsUserInput CRS 输入字符串，UTF-8 编码，语义同 GeoCrs::SetFromUserInput()。
     * @param allowFileAccess 是否允许 GDAL 把输入解析为 .prj 等文件路径。
     * @param allowNetworkAccess 是否允许 GDAL/PROJ 在解析过程中访问网络资源。
     */
    explicit GeoExtent(const std::string& crsUserInput, bool allowFileAccess = true, bool allowNetworkAccess = true);

    /**
     * @brief 根据 CRS 用户输入和矩形范围构造 GeoExtent。
     * @param crsUserInput CRS 输入字符串，UTF-8 编码，语义同 GeoCrs::SetFromUserInput()。
     * @param rectangle 二维范围。
     * @param allowFileAccess 是否允许 GDAL 把输入解析为 .prj 等文件路径。
     * @param allowNetworkAccess 是否允许 GDAL/PROJ 在解析过程中访问网络资源。
     */
    GeoExtent(const std::string& crsUserInput, const GB_Rectangle& rectangle, bool allowFileAccess = true, bool allowNetworkAccess = true);

    /**
     * @brief 使用经纬度边界构造地理坐标范围。
     * @param crs 地理坐标参考系统。
     * @param westLongitude 西边界经度，单位为度，范围 [-180, 180]。
     * @param southLatitude 南边界纬度，单位为度，范围 [-90, 90]。
     * @param eastLongitude 东边界经度，单位为度，范围 [-180, 180]。
     * @param northLatitude 北边界纬度，单位为度，范围 [-90, 90]。
     * @return 构造得到的范围；参数不合法或 CRS 不是地理坐标系时返回空范围。
     *
     * 当 westLongitude > eastLongitude 时，表示范围跨越 ±180° 经线。
     */
    static GeoExtent FromGeographicBounds(const GeoCrs& crs, double westLongitude, double southLatitude, double eastLongitude, double northLatitude);

    /** @brief 重置为空范围。 */
    void Reset();

    /**
     * @brief 判断当前范围是否有效。
     * @return CRS 有效且 rectangle 在对应 CRS 语义下有效时返回 true。
     *
     * 地理坐标系下允许 rectangle.minX > rectangle.maxX，用于表达跨 ±180° 经线的范围。
     */
    bool IsValid() const;

    /**
     * @brief 判断当前范围是否跨越 ±180° 经线。
     * @return 当前范围有效、CRS 为地理坐标系且 rectangle.minX > rectangle.maxX 时返回 true。
     */
    bool IsCrossesAntimeridian() const;

    /**
     * @brief 判断两个范围是否使用等价 CRS。
     * @param other 待比较范围。
     * @return 两个 CRS 均有效且 GeoCrs::IsSame() 返回 true 时返回 true。
     */
    bool HasSameCrs(const GeoExtent& other) const;

    /**
     * @brief 获取范围宽度。
     * @return 无效范围返回 NaN；跨反经线地理范围返回向东跨越 ±180° 后的经度跨度。
     *
     * 地理坐标系下返回值单位为度；投影坐标系下返回值单位为 CRS 坐标单位。
     */
    double Width() const;

    /**
     * @brief 获取范围高度。
     * @return 无效范围返回 NaN。
     *
     * 地理坐标系下返回值单位为度；投影坐标系下返回值单位为 CRS 坐标单位。
     */
    double Height() const;

    /**
     * @brief 获取范围中心点。
     * @return 无效范围返回 NaN 点；跨反经线地理范围返回规范到 [-180, 180] 的中心经度。
     */
    GB_Point2d Center() const;

    /**
     * @brief 判断当前范围是否包含点。
     * @param point 待判断点，坐标必须使用当前 CRS。
     * @param tolerance 判定容差；负数会按绝对值处理，NaN/Inf 会按 0 处理。
     * @return 包含或落在边界上返回 true。
     */
    bool IsContains(const GB_Point2d& point, double tolerance = 1e-10) const;

    /**
     * @brief 判断当前范围是否完整包含另一个范围。
     * @param other 待判断范围。
     * @param tolerance 判定容差；负数会按绝对值处理，NaN/Inf 会按 0 处理。
     * @return CRS 等价且当前范围完整包含 other 时返回 true。
     */
    bool IsContains(const GeoExtent& other, double tolerance = 1e-10) const;

    /**
     * @brief 判断当前范围是否与另一个范围相交。
     * @param other 待判断范围。
     * @param tolerance 判定容差；负数会按绝对值处理，NaN/Inf 会按 0 处理。
     * @return CRS 等价且范围相交时返回 true。
     */
    bool IsIntersects(const GeoExtent& other, double tolerance = 1e-10) const;

    /**
     * @brief 计算当前范围与另一个范围的交集。
     * @param other 另一个范围。
     * @param tolerance 判定容差；负数会按绝对值处理，NaN/Inf 会按 0 处理。
     * @return CRS 不等价、输入无效或不相交时返回空数组；跨反经线时可能返回多个不跨反经线的范围。
     */
    std::vector<GeoExtent> Intersected(const GeoExtent& other, double tolerance = 1e-10) const;

    /**
     * @brief 获取不跨反经线的规范矩形数组。
     * @return 无效范围返回空数组；普通范围返回一个矩形；跨反经线地理范围返回两个矩形。
     *
     * 对跨反经线地理范围，例如 [170, -20, -170, -16]，返回：
     * - [170, -20, 180, -16]
     * - [-180, -20, -170, -16]
     *
     * 返回的每个 GB_Rectangle 都满足 minX <= maxX，适合常规渲染、索引和相交判定。
     */
    std::vector<GB_Rectangle> GetNormalizedRectangles() const;

    /**
     * @brief 获取不跨反经线的规范空间范围数组。
     * @return 无效范围返回空数组；普通范围返回一个 GeoExtent；跨反经线地理范围返回两个 GeoExtent。
     *
     * 该接口与 GetNormalizedRectangles() 的拆分规则一致，但返回值会保留当前 CRS，便于上层流程继续以 GeoExtent 作为统一数据载体。
     * 返回的每个 GeoExtent 都不再跨反经线，且其 rectangle 均满足 minX <= maxX。
     */
    std::vector<GeoExtent> GetNormalizedExtents() const;
};

#endif
