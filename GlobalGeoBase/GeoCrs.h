#ifndef GLOBALGEOBASE_GEO_CRS_H_H
#define GLOBALGEOBASE_GEO_CRS_H_H

#include "GlobalGeoBasePort.h"
#include "Geometry/GB_Rectangle.h"

#include <memory>
#include <string>
#include <vector>

class OGRSpatialReference;

#ifdef _MSC_VER
# pragma warning(push)
# pragma warning(disable: 4251)
#endif

/**
 * @brief GIS 坐标参考系统（Coordinate Reference System / CRS）封装。
 *
 * GeoCrs 是对 GDAL OGRSpatialReference 的工程化封装：
 * - 头文件不包含 GDAL / PROJ 等三方库头文件，降低外部编译耦合；
 * - 所有字符串输入/输出均约定为 UTF-8 编码；
 * - 所有对外坐标顺序均约定为传统 GIS 顺序：X/Y，即经度/纬度或东向/北向；
 * - 内部对常用导出结果和判定结果进行惰性缓存，适合读多写少场景；
 * - 同一个 GeoCrs 对象支持多线程并行只读访问，不支持读写并发修改。
 */
class GLOBALGEOBASE_PORT GeoCrs
{
public:
    /**
     * @brief 坐标系单位信息。
     *
     * 对线性单位，conversionFactor 表示“一个该单位等于多少米”。
     * 对角度单位，conversionFactor 表示“一个该单位等于多少弧度”。
     */
    struct UnitInfo
    {
        /** @brief 单位名称，使用 UTF-8 编码；无法获取时为空字符串。 */
        std::string name;

        /** @brief 单位换算系数；线性单位为到米的系数，角度单位为到弧度的系数。 */
        double conversionFactor = 0.0;

        /** @brief 单位信息是否有效；为 false 时 name 和 conversionFactor 不应被业务逻辑直接使用。 */
        bool isValid = false;
    };

public:
    /**
     * @brief 构造一个空坐标系。
     *
     * 空坐标系 IsValid() 返回 false，可通过 SetFromUserInput() 等接口赋值。
     */
    GeoCrs();

    /**
     * @brief 根据用户输入构造坐标系。
     * @param userInput 用户输入字符串，UTF-8 编码；语义与 SetFromUserInput() 完全相同。
     * @param allowFileAccess 是否允许 GDAL 把输入解析为 .prj 等文件路径。
     * @param allowNetworkAccess 是否允许 GDAL/PROJ 在解析过程中访问网络资源。
     *
     * 解析失败时构造为空坐标系，IsValid() 返回 false。
     */
    explicit GeoCrs(const std::string& userInput, bool allowFileAccess = true, bool allowNetworkAccess = true);

    /**
     * @brief 根据 GDAL OGRSpatialReference 构造坐标系。
     * @param spatialReference 外部传入的 GDAL 坐标系对象，函数内部会复制一份并强制设置为传统 GIS 坐标顺序。
     *
     * 头文件仅前向声明 OGRSpatialReference，调用此构造函数的源文件需要自行包含 GDAL 头文件。
     */
    explicit GeoCrs(const OGRSpatialReference& spatialReference);

    /**
     * @brief 拷贝构造函数。
     * @param other 待复制的坐标系对象。
     *
     * 仅复制 GDAL 坐标系定义，不复制缓存内容；新对象会在首次查询时重新生成缓存。
     */
    GeoCrs(const GeoCrs& other);

    /**
     * @brief 拷贝赋值运算符。
     * @param other 待复制的坐标系对象。
     * @return 当前对象引用。
     *
     * 赋值后会清空当前对象已有缓存，后续只读查询会重新计算缓存。
     */
    GeoCrs& operator=(const GeoCrs& other);

    /**
     * @brief 移动构造函数。
     * @param other 待移动的坐标系对象。
     *
     * 移动后源对象会被重置为空坐标系。
     */
    GeoCrs(GeoCrs&& other);

    /**
     * @brief 移动赋值运算符。
     * @param other 待移动的坐标系对象。
     * @return 当前对象引用。
     *
     * 移动后源对象会被重置为空坐标系。
     */
    GeoCrs& operator=(GeoCrs&& other);

    /**
     * @brief 析构函数。
     *
     * 负责释放内部 GDAL 对象和缓存数据。
     */
    ~GeoCrs();

    /**
     * @brief 从用户输入构造坐标系。
     * @param userInput 用户输入字符串，UTF-8 编码。
     * @param allowFileAccess 是否允许 GDAL 把输入解析为 .prj 等文件路径。
     * @param allowNetworkAccess 是否允许 GDAL/PROJ 在解析过程中访问网络资源。
     * @return 构造得到的坐标系；解析失败时返回空坐标系。
     *
     * 支持 GDAL SetFromUserInput() 可识别的输入形式，例如：
     * - EPSG:4326、EPSG:3857；
     * - ESRI:xxxx、IGNF:xxxx 等 PROJ 数据库中的 authority:code；
     * - urn:ogc:def:crs:EPSG::4490；
     * - WKT / PROJ.4 / PROJJSON；
     * - 当 allowFileAccess 为 true 时，也允许输入 .prj 等文件路径；
     * - 当 allowNetworkAccess 为 true 时，也允许输入 http:// 或 https:// CRS 定义地址。
     */
    static GeoCrs FromUserInput(const std::string& userInput, bool allowFileAccess = true, bool allowNetworkAccess = true);

    /**
     * @brief 尝试从用户输入构造坐标系。
     * @param userInput 用户输入字符串，UTF-8 编码。
     * @param crs 输出坐标系对象指针，不允许为 nullptr。
     * @param allowFileAccess 是否允许 GDAL 把输入解析为 .prj 等文件路径。
     * @param allowNetworkAccess 是否允许 GDAL/PROJ 在解析过程中访问网络资源。
     * @return 构造成功返回 true；失败返回 false，并把 crs 重置为空坐标系。
     */
    static bool TryFromUserInput(const std::string& userInput, GeoCrs* crs, bool allowFileAccess = true, bool allowNetworkAccess = true);

    /**
     * @brief 使用用户输入重设当前坐标系。
     * @param userInput 用户输入字符串，UTF-8 编码。
     * @param allowFileAccess 是否允许 GDAL 把输入解析为 .prj 等文件路径。
     * @param allowNetworkAccess 是否允许 GDAL/PROJ 在解析过程中访问网络资源。
     * @return 设置成功返回 true；失败返回 false，并把当前对象重置为空坐标系。
     */
    bool SetFromUserInput(const std::string& userInput, bool allowFileAccess = true, bool allowNetworkAccess = true);

    /**
     * @brief 使用 GDAL OGRSpatialReference 重设当前坐标系。
     * @param spatialReference 外部传入的 GDAL 坐标系对象，函数内部会复制一份并强制设置为传统 GIS 坐标顺序。
     * @return 设置成功返回 true；传入空坐标系时返回 false，并把当前对象重置为空坐标系。
     */
    bool SetFromOgrSpatialReference(const OGRSpatialReference& spatialReference);

    /**
     * @brief 重置为空坐标系。
     *
     * 重置后 IsValid() 返回 false，同时清空全部内部缓存。
     */
    void Reset();

    /**
     * @brief 判断当前对象是否持有有效坐标系定义。
     * @return 有效返回 true；为空坐标系返回 false。
     */
    bool IsValid() const;

    /**
     * @brief 布尔转换运算符。
     * @return 等价于 IsValid()。
     */
    explicit operator bool() const;

    /**
     * @brief 获取规范化标识符。
     * @return 标识符字符串，UTF-8 编码；空坐标系返回空字符串。
     *
     * 优先返回 authority 标识（如 AUTHORITY:EPSG:4326），无法识别 authority 时退化为 OGC URN、WKT2_2019、PROJJSON 或 PROJ.4。
     *
     * @note 该字符串适合做缓存键或日志输出；CRS 等价判断请使用 IsSame()，不要直接比较该字符串。
     */
    std::string GetUniqueId() const;

    /**
     * @brief 判断两个坐标系是否等价。
     * @param other 待比较的另一个坐标系对象。
     * @return 二者均有效且 GDAL 判定为等价 CRS 时返回 true，否则返回 false。
     *
     * 比较时忽略 data axis mapping 的差异，并允许地理 CRS 的轴顺序差异；GeoCrs 对外统一使用传统 GIS 坐标顺序。
     */
    bool IsSame(const GeoCrs& other) const;

    /**
     * @brief 判断两个坐标系是否相等。
     * @param other 待比较的另一个坐标系对象。
     * @return 等价于 IsSame(other)。
     */
    bool operator==(const GeoCrs& other) const;

    /**
     * @brief 判断两个坐标系是否不相等。
     * @param other 待比较的另一个坐标系对象。
     * @return 等价于 !IsSame(other)。
     */
    bool operator!=(const GeoCrs& other) const;

    /**
     * @brief 获取坐标系名称。
     * @return 坐标系名称，UTF-8 编码；无法获取时返回空字符串。
     */
    std::string GetName() const;

    /**
     * @brief 获取坐标系参考椭球名称。
     * @return 参考椭球名称，UTF-8 编码；无法获取时返回空字符串。
     */
    std::string GetReferenceEllipsoidName() const;

    /**
     * @brief 判断是否为地理坐标系。
     * @return 是地理坐标系返回 true，否则返回 false。
     */
    bool IsGeographic() const;

    /**
     * @brief 判断是否为投影坐标系。
     * @return 是投影坐标系返回 true，否则返回 false。
     */
    bool IsProjected() const;

    /**
     * @brief 判断是否为局部坐标系。
     * @return 是局部坐标系返回 true，否则返回 false。
     */
    bool IsLocal() const;

    /**
     * @brief 判断是否为复合坐标系。
     * @return 是复合坐标系返回 true，否则返回 false。
     */
    bool IsCompound() const;

    /**
     * @brief 判断是否为动态坐标系。
     * @return 是动态坐标系返回 true，否则返回 false。
     */
    bool IsDynamic() const;

    /**
     * @brief 判断是否为自定义坐标系。
     * @return 无可识别 authority 且无法匹配到标准 authority 时返回 true，否则返回 false。
     */
    bool IsCustom() const;

    /**
     * @brief 是否按“经度、纬度”作为对外地理坐标输入/输出顺序。
     * @return 地理坐标系且使用传统 GIS 顺序时返回 true；投影坐标系或空坐标系返回 false。
     */
    bool IsLongitudeLatitudeOrder() const;

    /**
     * @brief 是否使用传统 GIS 坐标顺序。
     * @return 当前坐标系有效且 GDAL 轴映射策略为 OAMS_TRADITIONAL_GIS_ORDER 时返回 true。
     */
    bool UsesTraditionalGisOrder() const;

    /**
     * @brief 导出为 WKT 字符串。
     * @return WKT2_2019 格式字符串，UTF-8 编码；失败或空坐标系返回空字符串。
     */
    std::string ExportToWkt() const;

    /**
     * @brief 导出为格式化 WKT 字符串。
     * @param simplify 是否使用 GDAL 的简化 pretty WKT 输出。
     * @return 格式化 WKT 字符串，UTF-8 编码；失败或空坐标系返回空字符串。
     */
    std::string ExportToPrettyWkt(bool simplify = false) const;

    /**
     * @brief 导出为 PROJ.4 字符串。
     * @return PROJ.4 字符串，UTF-8 编码；失败、空坐标系或无法无损表达时可能返回空字符串。
     *
     * PROJ.4 无法完整表达现代 CRS，业务上更建议优先使用 WKT2 或 PROJJSON。
     */
    std::string ExportToProj4() const;

    /**
     * @brief 导出为 PROJJSON 字符串。
     * @param multiline 是否输出多行格式化 JSON；false 时输出紧凑 JSON。
     * @return PROJJSON 字符串，UTF-8 编码；失败、不支持或空坐标系返回空字符串。
     */
    std::string ExportToProjJson(bool multiline = false) const;

    /**
     * @brief 尝试获取 EPSG 或 ESRI authority 字符串。
     * @return 例如 EPSG:3857、ESRI:102100；失败返回空字符串。
     */
    std::string GetEpsgOrEsriCode() const;

    /**
     * @brief 尝试获取 OGC URN 字符串。
     * @return 例如 urn:ogc:def:crs:EPSG::4326；失败或 GDAL 版本不支持时返回空字符串。
     */
    std::string GetOgcUrn() const;

    /**
     * @brief 获取线性单位信息。
     * @return 线性单位信息；无法获取时返回 isValid 为 false 的 UnitInfo。
     */
    UnitInfo GetLinearUnits() const;

    /**
     * @brief 获取角度单位信息。
     * @return 角度单位信息；无法获取时返回 isValid 为 false 的 UnitInfo。
     */
    UnitInfo GetAngularUnits() const;

    /**
     * @brief 获取每一个坐标单位对应的米数。
     * @return 可计算时返回正数；无法计算时返回 0。
     *
     * 对投影坐标系，返回线性单位到米的换算系数。
     * 对地理坐标系，返回“一个角度单位在参考椭球赤道半径上的近似弧长”，因此只能用于尺度估算。
     * 无法计算时返回 0。
     */
    double GetMetersPerUnit() const;

    /**
     * @brief 尝试获取每一个坐标单位对应的米数。
     * @param metersPerUnit 输出米数指针，不允许为 nullptr。
     * @return 成功获取正数返回 true；失败返回 false，并把输出值置为 0。
     */
    bool TryGetMetersPerUnit(double* metersPerUnit) const;

    /**
     * @brief 获取参考椭球长半轴。
     * @return 长半轴，单位为米；无法获取时返回 0。
     */
    double GetSemiMajor() const;

    /**
     * @brief 获取参考椭球短半轴。
     * @return 短半轴，单位为米；无法获取时返回 0。
     */
    double GetSemiMinor() const;

    /**
     * @brief 获取参考椭球反扁率。
     * @return 反扁率；无法获取时返回 0。
     */
    double GetInvFlattening() const;

    /**
     * @brief 获取坐标历元。
     * @return 坐标历元；未定义、不支持或无法获取时返回 0。
     */
    double GetCoordinateEpoch() const;

    /**
     * @brief 获取 CRS 定义的经纬度适用范围。
     * @return 经纬度范围矩形数组；无法获取或范围未知时返回空数组。
     *
     * 返回矩形坐标序恒为 minX/minY/maxX/maxY = 西经度/南纬度/东经度/北纬度。
     * 若范围跨越 ±180° 经线，会拆成两个矩形返回。
     */
    std::vector<GB_Rectangle> GetGeographicAreaOfUse() const;

    /**
     * @brief 获取 CRS 适用范围名称。
     * @return 适用范围名称，UTF-8 编码；无法获取时返回空字符串。
     */
    std::string GetAreaOfUseName() const;

    /**
     * @brief 尝试获取 CRS 定义的经纬度适用范围和范围名称。
     * @param rectangles 输出经纬度范围矩形数组指针，不允许为 nullptr。
     * @param areaName 可选输出范围名称指针，允许为 nullptr。
     * @return 成功获取至少一个范围矩形返回 true；失败返回 false。
     */
    bool TryGetGeographicAreaOfUse(std::vector<GB_Rectangle>* rectangles, std::string* areaName = nullptr) const;

    /**
     * @brief 获取内部 GDAL 坐标系对象的只读引用。
     * @return 内部 OGRSpatialReference 只读引用。
     *
     * 返回对象归 GeoCrs 持有，调用方不得保存超过 GeoCrs 生命周期的引用，也不得通过 const_cast 修改。
     * 该接口只适合立即读取；若需要长期保存、跨线程传递或跨模块修改，请使用 CopyToOgrSpatialReference()。
     */
    const OGRSpatialReference& GetOgrSpatialReference() const;

    /**
     * @brief 复制内部 GDAL 坐标系对象。
     * @param spatialReference 输出 GDAL 坐标系对象引用。
     * @return 当前坐标系有效时返回 true；为空坐标系时返回 false，并清空输出对象。
     *
     * 输出对象会被设置为传统 GIS 坐标顺序。
     */
    bool CopyToOgrSpatialReference(OGRSpatialReference& spatialReference) const;

private:
    /** @brief 内部实现对象，隐藏 GDAL 头文件依赖和缓存字段。 */
    struct Impl;

    /** @brief 内部实现对象指针。 */
    std::unique_ptr<Impl> impl_;
};

#ifdef _MSC_VER
# pragma warning(pop)
#endif

#endif
