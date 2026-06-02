#ifndef GLOBALBASE_GPKG_H_H
#define GLOBALBASE_GPKG_H_H

#include "GB_BaseTypes.h"
#include "CV/GB_Image.h"
#include "GB_ReadWriteLock.h"
#include "GB_Sqlite.h"
#include "GB_Variant.h"
#include "GlobalGeoBasePort.h"
#include "Geometry/GB_Rectangle.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#ifdef _MSC_VER
#  pragma warning(push)
#  pragma warning(disable: 4251)
#endif

/**
 * @brief GeoPackage 内容类型。
 *
 * @details
 * GeoPackage 以 SQLite 为容器，核心内容主要包括矢量要素表、瓦片金字塔表以及普通属性表。
 * 本枚举用于描述 gpkg_contents.data_type 的常见取值。
 */
enum class GeoGpkgContentType
{
    Unknown = 0,
    Features,
    Tiles,
    Attributes
};

/**
 * @brief GeoPackage 打开配置。
 */
struct GeoGpkgOpenOptions
{
    /** @brief 底层 SQLite 打开配置。默认使用 GB_Sqlite 的 WAL、多读一写连接池和外键约束。 */
    GB_SqliteOptions sqliteOptions;

    /** @brief 打开后是否自动初始化 GeoPackage 核心表和默认空间参考记录。 */
    bool initializeIfNeeded = true;

    /** @brief 初始化时写入 SQLite header user_version。默认 10400，表示 GeoPackage 1.4.0；若需要严格生成旧版本文件，可按需改为 10300/10200。 */
    int userVersion = 10400;
};

/**
 * @brief GeoPackage 空间参考记录，对应 gpkg_spatial_ref_sys。
 */
struct GeoGpkgSpatialRefSys
{
    std::string srsNameUtf8 = "";
    int srsId = 0;
    std::string organizationUtf8 = "";
    int organizationCoordsysId = 0;
    std::string definitionUtf8 = "";
    std::string descriptionUtf8 = "";
};

/**
 * @brief GeoPackage 内容记录，对应 gpkg_contents。
 */
struct GeoGpkgContentInfo
{
    std::string tableNameUtf8 = "";
    GeoGpkgContentType contentType = GeoGpkgContentType::Unknown;
    std::string dataTypeUtf8 = "";
    std::string identifierUtf8 = "";
    std::string descriptionUtf8 = "";
    std::string lastChangeUtf8 = "";
    GB_Rectangle envelope;
    int srsId = 0;
};

/**
 * @brief 用户字段定义。
 *
 * @remarks
 * typeUtf8 应使用 GeoPackage / SQLite 可互操作的数据类型，例如 TEXT、INTEGER、REAL、DOUBLE、BOOLEAN、BLOB、DATE、DATETIME 等。
 * defaultSqlUtf8 是 SQL 表达式文本，不会被额外加引号，例如可传入 "0"、"''" 或 "CURRENT_TIMESTAMP"。
 */
struct GeoGpkgFieldDef
{
    std::string nameUtf8 = "";
    std::string typeUtf8 = "TEXT";
    bool notNull = false;
    bool unique = false;
    std::string defaultSqlUtf8 = "";
};

/**
 * @brief 矢量图层信息。
 */
struct GeoGpkgFeatureLayerInfo
{
    std::string tableNameUtf8 = "";
    std::string identifierUtf8 = "";
    std::string descriptionUtf8 = "";
    std::string primaryKeyColumnNameUtf8 = "";
    std::string geometryColumnNameUtf8 = "";
    std::string geometryTypeNameUtf8 = "";
    int srsId = 0;
    int z = 0;
    int m = 0;
    GB_Rectangle envelope;
    std::vector<GB_SqliteTableFieldInfo> fields;
};

/**
 * @brief 矢量要素记录。
 *
 * @details
 * geometry 非空时按 GeoPackageBinary 存储，即 GPKG 几何头 + 标准 WKB；为空时表示 SQL NULL 几何。
 */
struct GeoGpkgFeature
{
    long long rowId = 0;
    GB_ByteBuffer geometry;
    std::map<std::string, GB_Variant> attributes;
};

/**
 * @brief GeoPackageBinary 几何头信息。
 */
struct GeoGpkgGeometryHeaderInfo
{
    bool valid = false;
    bool littleEndian = true;
    bool empty = false;
    bool extended = false;
    int version = 0;
    int srsId = 0;
    int envelopeCode = 0;
    std::size_t headerSize = 0;
    GB_Rectangle envelope;
};

/**
 * @brief 瓦片矩阵集信息，对应 gpkg_tile_matrix_set。
 */
struct GeoGpkgTileMatrixSetInfo
{
    std::string tableNameUtf8 = "";
    int srsId = 0;
    GB_Rectangle envelope;
};

/**
 * @brief 瓦片矩阵信息，对应 gpkg_tile_matrix。
 */
struct GeoGpkgTileMatrixInfo
{
    std::string tableNameUtf8 = "";
    int zoomLevel = 0;
    int matrixWidth = 0;
    int matrixHeight = 0;
    int tileWidth = 256;
    int tileHeight = 256;
    double pixelXSize = 0.0;
    double pixelYSize = 0.0;
};

/**
 * @brief 瓦片记录。
 */
struct GeoGpkgTile
{
    int zoomLevel = 0;
    int tileColumn = 0;
    int tileRow = 0;
    GB_ByteBuffer tileData;
};

/**
 * @brief GeoPackage 读写器。
 *
 * @details
 * GeoGpkg 是基于 GB_Sqlite 的 GeoPackage 轻量级读写封装，负责：
 * - 初始化 GeoPackage 核心表、默认空间参考记录、application_id 和 user_version。
 * - 维护 gpkg_contents / gpkg_spatial_ref_sys / gpkg_geometry_columns / gpkg_tile_matrix_set / gpkg_tile_matrix 等规范表。
 * - 对矢量图层进行创建、插入、批量插入、范围查询和 RTree 空间索引维护。
 * - 对瓦片矩阵集进行创建、瓦片矩阵注册、瓦片读写。
 * - 提供 GeoPackageBinary 几何头的封装与解析工具。
 *
 * @remarks
 * 本类内部使用 GB_ReadWriteLock 保护多步骤 GeoPackage 元数据一致性；底层 GB_Sqlite 仍负责 SQLite 连接池、WAL、busy timeout、事务和参数绑定。
 * 同一个 GeoGpkg 对象允许多线程并发读，写操作独占。跨对象或跨进程访问同一 .gpkg 文件时，最终仍由 SQLite 锁和 busy timeout 协调。
 */
class GLOBALGEOBASE_PORT GeoGpkg
{
public:
    GeoGpkg();
    explicit GeoGpkg(const std::string& filePathUtf8, const GeoGpkgOpenOptions& options = GeoGpkgOpenOptions());
    ~GeoGpkg();

    GeoGpkg(const GeoGpkg& other) = delete;
    GeoGpkg& operator=(const GeoGpkg& other) = delete;

    /** @brief 打开或创建 GeoPackage 文件。 */
    bool Open(const std::string& filePathUtf8, const GeoGpkgOpenOptions& options = GeoGpkgOpenOptions());

    /** @brief 关闭当前 GeoPackage。 */
    void Close();

    /** @brief 判断当前文件是否已经打开。 */
    bool IsOpen() const;

    /** @brief 获取当前文件路径。 */
    std::string GetFilePathUtf8() const;

    /** @brief 获取最近一次错误信息。 */
    std::string GetLastErrorUtf8() const;

    /** @brief 获取底层 SQLite 对象，便于执行高级自定义 SQL。 */
    GB_Sqlite& GetSqlite();
    const GB_Sqlite& GetSqlite() const;

    /** @brief 初始化 GeoPackage 核心表、默认 CRS 以及 SQLite header 标识。 */
    bool InitializeCoreTables(int userVersion = 10400);

    /** @brief 写入 GeoPackage 规范要求的默认 CRS：4326、-1、0。 */
    bool EnsureDefaultSpatialRefSys();

    /** @brief 基础一致性检查：核心表存在性、默认 CRS、外键关系等。 */
    bool ValidateBasicSchema(std::vector<std::string>* outMessages = nullptr) const;

    /** @brief 添加或更新空间参考记录。 */
    bool AddOrUpdateSpatialRefSys(const GeoGpkgSpatialRefSys& srs);

    /** @brief 判断空间参考是否存在。 */
    bool SpatialRefSysExists(int srsId, bool& outExists) const;

    /** @brief 获取指定空间参考记录。 */
    bool GetSpatialRefSys(int srsId, GeoGpkgSpatialRefSys& outSrs) const;

    /** @brief 获取全部空间参考记录。 */
    bool ListSpatialRefSys(std::vector<GeoGpkgSpatialRefSys>& outSrsList) const;

    /** @brief 获取 gpkg_contents 中的全部内容记录。 */
    bool ListContents(std::vector<GeoGpkgContentInfo>& outContents) const;

    /** @brief 创建矢量图层，同时写入 gpkg_contents、gpkg_geometry_columns，并在使用扩展几何类型时写入 gpkg_extensions。 */
    bool CreateFeatureLayer(const std::string& tableNameUtf8, const std::string& geometryTypeNameUtf8, int srsId, const std::vector<GeoGpkgFieldDef>& fields = std::vector<GeoGpkgFieldDef>(), const std::string& geometryColumnNameUtf8 = "geom", const std::string& primaryKeyColumnNameUtf8 = "id", const GB_Rectangle* envelope = nullptr, const std::string& identifierUtf8 = std::string(), const std::string& descriptionUtf8 = std::string(), int z = 0, int m = 0, bool overwrite = false);

    /** @brief 获取所有矢量图层。 */
    bool ListFeatureLayers(std::vector<GeoGpkgFeatureLayerInfo>& outLayers) const;

    /** @brief 获取单个矢量图层信息。 */
    bool GetFeatureLayerInfo(const std::string& tableNameUtf8, GeoGpkgFeatureLayerInfo& outLayer) const;

    /** @brief 插入单个要素。geometry 非空时必须是 GeoPackageBinary；geometry 为空时写入 SQL NULL。 */
    bool InsertFeature(const std::string& tableNameUtf8, const GB_ByteBuffer& geometry, const std::map<std::string, GB_Variant>& attributes = std::map<std::string, GB_Variant>(), long long* outRowId = nullptr);

    /** @brief 批量插入要素，内部使用显式事务，并校验属性字段、SRID 和 WKB 几何类型；空 geometry 会按 SQL NULL 写入。 */
    bool InsertFeatures(const std::string& tableNameUtf8, const std::vector<GeoGpkgFeature>& features, std::vector<long long>* outRowIds = nullptr);

    /** @brief 按外包矩形查询要素。若存在 rtree_<table>_<geometry>，优先使用 RTree，并对候选结果进行精确外包矩形二次过滤。 */
    bool QueryFeaturesByEnvelope(const std::string& tableNameUtf8, const GB_Rectangle& envelope, std::vector<GeoGpkgFeature>& outFeatures, std::size_t maxFeatureCount = 0) const;

    /** @brief 判断矢量图层是否已经存在 RTree 空间索引虚表。 */
    bool HasSpatialIndex(const std::string& tableNameUtf8, const std::string& geometryColumnNameUtf8, bool& outHasSpatialIndex) const;

    /** @brief 创建标准 GeoPackage RTree 空间索引虚表、GeoPackage 1.4 兼容维护触发器和 gpkg_extensions 记录。 */
    bool CreateSpatialIndex(const std::string& tableNameUtf8, const std::string& geometryColumnNameUtf8 = std::string());

    /** @brief 删除空间索引虚表、触发器和 gpkg_extensions 记录。 */
    bool DropSpatialIndex(const std::string& tableNameUtf8, const std::string& geometryColumnNameUtf8 = std::string());

    /** @brief 重新扫描 GeoPackageBinary 几何头或 WKB 几何数据并重建 RTree 内容。 */
    bool RebuildSpatialIndexFromGeometryHeader(const std::string& tableNameUtf8, const std::string& geometryColumnNameUtf8 = std::string());

    /** @brief 创建瓦片矩阵集，同时创建瓦片用户数据表，并写入 gpkg_contents 和 gpkg_tile_matrix_set。 */
    bool CreateTileMatrixSet(const std::string& tableNameUtf8, int srsId, const GB_Rectangle& envelope, const std::string& identifierUtf8 = std::string(), const std::string& descriptionUtf8 = std::string(), bool overwrite = false);

    /** @brief 根据瓦片矩阵集信息创建瓦片矩阵集，同时创建瓦片用户数据表，并写入 gpkg_contents 和 gpkg_tile_matrix_set。 */
    bool CreateTileMatrixSet(const GeoGpkgTileMatrixSetInfo& tileMatrixSetInfo, const std::string& identifierUtf8 = std::string(), const std::string& descriptionUtf8 = std::string(), bool overwrite = false);

    /** @brief 获取所有瓦片矩阵集信息。 */
    bool ListTileMatrixSets(std::vector<GeoGpkgTileMatrixSetInfo>& outTileMatrixSets) const;

    /** @brief 获取单个瓦片矩阵集信息。 */
    bool GetTileMatrixSetInfo(const std::string& tableNameUtf8, GeoGpkgTileMatrixSetInfo& outTileMatrixSetInfo) const;

    /** @brief 添加或更新瓦片矩阵定义。 */
    bool AddOrUpdateTileMatrix(const GeoGpkgTileMatrixInfo& tileMatrixInfo);

    /** @brief 获取瓦片矩阵定义。 */
    bool ListTileMatrices(const std::string& tableNameUtf8, std::vector<GeoGpkgTileMatrixInfo>& outTileMatrices) const;

    /** @brief 写入或覆盖单张瓦片。tileData 通常为 PNG、JPEG 或 WebP 字节。 */
    bool PutTile(const std::string& tableNameUtf8, int zoomLevel, int tileColumn, int tileRow, const GB_ByteBuffer& tileData);

    /** @brief 批量写入或覆盖瓦片。内部只开启一次事务，并且只更新一次 gpkg_contents.last_change。 */
    bool PutTiles(const std::string& tableNameUtf8, const std::vector<GeoGpkgTile>& tiles);

    /** @brief 读取单张瓦片。若不存在则返回 true 且 outExists=false。 */
    bool GetTile(const std::string& tableNameUtf8, int zoomLevel, int tileColumn, int tileRow, GB_ByteBuffer& outTileData, bool& outExists) const;

    /** @brief 按 zoom_level 读取一批瓦片。 */
    bool GetTiles(const std::string& tableNameUtf8, int zoomLevel, std::vector<GeoGpkgTile>& outTiles, std::size_t maxTileCount = 0) const;

    /** @brief 删除单张瓦片。若瓦片不存在则返回 true 且 outDeleted=false。 */
    bool DeleteTile(const std::string& tableNameUtf8, int zoomLevel, int tileColumn, int tileRow, bool* outDeleted = nullptr);

    /** @brief 删除内容表及其 GeoPackage 元数据。 */
    bool DropContentTable(const std::string& tableNameUtf8);

    /** @brief 把标准 WKB 包装为 GeoPackageBinary。若 envelope 为空，会尝试从 WKB 计算二维范围；empty=true 时要求 WKB 本身确实没有有效二维坐标；WKB 体不允许包含 EWKB 内嵌 SRID。 */
    static bool CreateGpkgGeometryFromWkb(const GB_ByteBuffer& wkb, int srsId, GB_ByteBuffer& outGpkgGeometry, const GB_Rectangle* envelope = nullptr, bool empty = false);

    /** @brief 解析 GeoPackageBinary 几何头，并校验魔数、版本、标志位和 envelope 合法性。 */
    static bool ParseGeometryHeader(const GB_ByteBuffer& gpkgGeometry, GeoGpkgGeometryHeaderInfo& outHeaderInfo);

    /** @brief 从 GeoPackageBinary 中取出标准 WKB。 */
    static bool ExtractWkbFromGpkgGeometry(const GB_ByteBuffer& gpkgGeometry, GB_ByteBuffer& outWkb);

    /** @brief 从标准 WKB 中计算二维外包矩形；会校验集合子类型、线/环点数、Polygon ring 闭合性，CircularString 会按圆弧真实极值计算范围。 */
    static bool CalculateWkbEnvelope(const GB_ByteBuffer& wkb, GB_Rectangle& outEnvelope);

private:
    bool InitializeCoreTablesNoLock(int userVersion);
    bool EnsureDefaultSpatialRefSysNoLock();
    bool GetFeatureLayerInfoNoLock(const std::string& tableNameUtf8, GeoGpkgFeatureLayerInfo& outLayer) const;
    bool HasSpatialIndexNoLock(const std::string& tableNameUtf8, const std::string& geometryColumnNameUtf8, bool& outHasSpatialIndex) const;
    bool DropSpatialIndexNoLock(const std::string& tableNameUtf8, const std::string& geometryColumnNameUtf8);
    bool RebuildSpatialIndexFromGeometryHeaderNoLock(const std::string& tableNameUtf8, const std::string& geometryColumnNameUtf8);
    bool RegisterGeoPackageSqlFunctionsNoLock();

    bool SetLastError(const std::string& messageUtf8) const;
    bool SetLastSqliteError(const std::string& prefixUtf8) const;
    void ClearLastError() const;

private:
    mutable GB_ReadWriteLock lock_;
    mutable std::mutex lastErrorLock_;
    mutable std::string lastErrorUtf8_;
    GB_Sqlite database_;
};

/**
 * @brief 从 GeoPackage 中读取单张瓦片并解码为 GB_Image。
 *
 * @param filePathUtf8 GeoPackage 文件路径，UTF-8 编码。
 * @param tableNameUtf8 瓦片矩阵集名称，也是瓦片用户数据表名。
 * @param zoomLevel 缩放层级。
 * @param row 瓦片行号，对应 GeoPackage tile_row。
 * @param col 瓦片列号，对应 GeoPackage tile_column。
 * @param outImage 输出图像；失败时会被清空。
 * @param loadOptions 图像解码选项。
 * @return true 读取并解码成功；false 文件不存在、无法打开、矩阵集不存在、瓦片不存在或图像无效。
 */
GLOBALGEOBASE_PORT bool GeoGpkgReadTileImage(const std::string& filePathUtf8, const std::string& tableNameUtf8, int zoomLevel, int row, int col, GB_Image& outImage, const GB_ImageLoadOptions& loadOptions = GB_ImageLoadOptions());

/**
 * @brief GeoPackage 瓦片图像写入选项。
 *
 * @details
 * 该结构只服务于 GeoGpkgWriteTileImage() 这类便捷接口：
 * - 当目标瓦片矩阵集不存在时，必须提供 envelope、srsWktUtf8、matrixWidth、matrixHeight。
 * - 当目标 zoom_level 不存在时，必须提供 matrixWidth、matrixHeight；tileWidth/tileHeight 为 0 时自动使用输入 GB_Image 的宽高。
 * - srsWktUtf8 会按 gpkg_spatial_ref_sys.definition 精确匹配；若不存在，会自动创建一条新的空间参考记录。
 */
struct GeoGpkgTileImageWriteOptions
{
    /** @brief 瓦片矩阵集范围。仅在目标瓦片矩阵集不存在时必填。 */
    GB_Rectangle envelope;

    /** @brief 坐标系 WKT。仅在目标瓦片矩阵集不存在且需要创建空间参考时必填。 */
    std::string srsWktUtf8 = "";

    /** @brief 新建空间参考时优先使用的 srs_id；若小于等于 0 或已被占用，则自动生成新的 srs_id。 */
    int preferredSrsId = 0;

    /** @brief 新建空间参考记录的名称；为空时自动生成。 */
    std::string srsNameUtf8 = "";

    /** @brief 新建空间参考记录的组织名称；为空时使用 "NONE"。 */
    std::string srsOrganizationUtf8 = "NONE";

    /** @brief 新建空间参考记录的组织坐标系编号；为 0 时使用实际写入的 srs_id。 */
    int srsOrganizationCoordsysId = 0;

    /** @brief 新建瓦片矩阵集时写入 gpkg_contents.identifier；为空时使用 tableNameUtf8。 */
    std::string identifierUtf8 = "";

    /** @brief 新建瓦片矩阵集时写入 gpkg_contents.description。 */
    std::string descriptionUtf8 = "";

    /** @brief 当前 zoom_level 的矩阵列数。目标 zoom_level 不存在时必填。 */
    int matrixWidth = 0;

    /** @brief 当前 zoom_level 的矩阵行数。目标 zoom_level 不存在时必填。 */
    int matrixHeight = 0;

    /** @brief 当前 zoom_level 的瓦片像素宽度；为 0 时使用输入图像宽度。 */
    int tileWidth = 0;

    /** @brief 当前 zoom_level 的瓦片像素高度；为 0 时使用输入图像高度。 */
    int tileHeight = 0;

    /** @brief 写入 tile_data 前对 GB_Image 进行内存编码的格式，例如 ".png"、".jpg"、".webp"。 */
    std::string imageFileExtUtf8 = ".png";

    /** @brief 图像编码参数。 */
    GB_ImageSaveOptions imageSaveOptions;
};

/**
 * @brief 向 GeoPackage 写入或覆盖单张 GB_Image 瓦片。
 *
 * @param filePathUtf8 GeoPackage 文件路径，UTF-8 编码；文件不存在时自动创建，父目录不存在时自动递归创建。
 * @param tableNameUtf8 瓦片矩阵集名称，也是瓦片用户数据表名。
 * @param zoomLevel 缩放层级。
 * @param row 瓦片行号，对应 GeoPackage tile_row。
 * @param col 瓦片列号，对应 GeoPackage tile_column。
 * @param image 输入图像，必须为非空有效图像。
 * @param options 写入选项；当矩阵集或当前层级不存在时用于自动创建元数据。
 * @return true 写入成功；false 路径无效、无法打开、图像无效、必要元数据缺失、行列越界或编码失败。
 */
GLOBALGEOBASE_PORT bool GeoGpkgWriteTileImage(const std::string& filePathUtf8, const std::string& tableNameUtf8, int zoomLevel, int row, int col, const GB_Image& image, const GeoGpkgTileImageWriteOptions& options = GeoGpkgTileImageWriteOptions());

/**
 * @brief 从 GeoPackage 中删除单张瓦片。
 *
 * @param filePathUtf8 GeoPackage 文件路径，UTF-8 编码。
 * @param tableNameUtf8 瓦片矩阵集名称，也是瓦片用户数据表名。
 * @param zoomLevel 缩放层级。
 * @param row 瓦片行号，对应 GeoPackage tile_row。
 * @param col 瓦片列号，对应 GeoPackage tile_column。
 * @return true 删除成功；false 文件不存在、无法打开、矩阵集不存在、瓦片不存在或删除失败。
 */
GLOBALGEOBASE_PORT bool GeoGpkgDeleteTile(const std::string& filePathUtf8, const std::string& tableNameUtf8, int zoomLevel, int row, int col);

#ifdef _MSC_VER
#  pragma warning(pop)
#endif

#endif
