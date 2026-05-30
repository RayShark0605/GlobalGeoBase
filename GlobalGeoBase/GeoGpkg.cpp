#include "GeoGpkg.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <sstream>
#include <utility>

namespace
{
    const int GB_GpkgApplicationId = 1196444487; // 0x47504B47, ASCII "GPKG".
    const unsigned char GB_GpkgMagic0 = static_cast<unsigned char>('G');
    const unsigned char GB_GpkgMagic1 = static_cast<unsigned char>('P');
    const std::size_t GB_GpkgBaseHeaderSize = 8;
    const std::size_t GB_GpkgMaxQueryReserveCount = 4096;
    const double GB_GpkgRTreeQueryExpansionScale = 1.0e-6;

    static std::string ToString(int value)
    {
        std::ostringstream stream;
        stream << value;
        return stream.str();
    }

    static std::string ToString(long long value)
    {
        std::ostringstream stream;
        stream << value;
        return stream.str();
    }

    static std::string ToString(std::size_t value)
    {
        std::ostringstream stream;
        stream << value;
        return stream.str();
    }

    static char ToUpperAscii(char ch)
    {
        return ch >= 'a' && ch <= 'z' ? static_cast<char>(ch - 'a' + 'A') : ch;
    }

    static char ToLowerAscii(char ch)
    {
        return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch;
    }

    static std::string ToUpperAsciiString(const std::string& textUtf8)
    {
        std::string result = textUtf8;
        for (std::size_t index = 0; index < result.size(); index++)
        {
            result[index] = ToUpperAscii(result[index]);
        }
        return result;
    }

    static std::string ToLowerAsciiString(const std::string& textUtf8)
    {
        std::string result = textUtf8;
        for (std::size_t index = 0; index < result.size(); index++)
        {
            result[index] = ToLowerAscii(result[index]);
        }
        return result;
    }

    static bool IsValidIdentifier(const std::string& identifierUtf8)
    {
        if (identifierUtf8.empty())
        {
            return false;
        }

        return identifierUtf8.find('\0') == std::string::npos;
    }

    static bool IsValidSqlFragment(const std::string& sqlFragmentUtf8)
    {
        return sqlFragmentUtf8.find('\0') == std::string::npos;
    }

    static std::string QuoteIdentifier(const std::string& identifierUtf8)
    {
        std::string result;
        result.reserve(identifierUtf8.size() + 2);
        result.push_back('"');
        for (std::size_t index = 0; index < identifierUtf8.size(); index++)
        {
            const char ch = identifierUtf8[index];
            if (ch == '"')
            {
                result.push_back('"');
            }
            result.push_back(ch);
        }
        result.push_back('"');
        return result;
    }

    static std::string MakeSafeObjectName(const std::string& nameUtf8)
    {
        std::string result;
        result.reserve(nameUtf8.size());
        for (std::size_t index = 0; index < nameUtf8.size(); index++)
        {
            const unsigned char ch = static_cast<unsigned char>(nameUtf8[index]);
            if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_')
            {
                result.push_back(static_cast<char>(ch));
            }
            else
            {
                result.push_back('_');
            }
        }
        if (result.empty())
        {
            result = "object";
        }
        return result;
    }

    static std::string MakeRTreeName(const std::string& tableNameUtf8, const std::string& geometryColumnNameUtf8)
    {
        return "rtree_" + tableNameUtf8 + "_" + geometryColumnNameUtf8;
    }

    static std::string MakeTriggerName(const std::string& tableNameUtf8, const std::string& geometryColumnNameUtf8, const std::string& suffixUtf8)
    {
        return MakeSafeObjectName("rtree_" + tableNameUtf8 + "_" + geometryColumnNameUtf8 + "_" + suffixUtf8);
    }

    static int GetEnvelopeDoubleCount(int envelopeCode)
    {
        switch (envelopeCode)
        {
        case 0:
            return 0;
        case 1:
            return 4;
        case 2:
        case 3:
            return 6;
        case 4:
            return 8;
        default:
            return -1;
        }
    }

    static void AppendUInt32LittleEndian(GB_ByteBuffer& buffer, std::uint32_t value)
    {
        buffer.push_back(static_cast<unsigned char>(value & 0xff));
        buffer.push_back(static_cast<unsigned char>((value >> 8) & 0xff));
        buffer.push_back(static_cast<unsigned char>((value >> 16) & 0xff));
        buffer.push_back(static_cast<unsigned char>((value >> 24) & 0xff));
    }

    static void AppendInt32LittleEndian(GB_ByteBuffer& buffer, std::int32_t value)
    {
        std::uint32_t rawValue = 0;
        std::memcpy(&rawValue, &value, sizeof(rawValue));
        AppendUInt32LittleEndian(buffer, rawValue);
    }

    static void AppendDoubleLittleEndian(GB_ByteBuffer& buffer, double value)
    {
        unsigned char bytes[sizeof(double)] = { 0 };
        std::memcpy(bytes, &value, sizeof(double));

#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
        std::reverse(bytes, bytes + sizeof(double));
#endif
        buffer.insert(buffer.end(), bytes, bytes + sizeof(double));
    }

    static std::uint32_t ReadUInt32(const unsigned char* dataPtr, bool littleEndian)
    {
        if (littleEndian)
        {
            return static_cast<std::uint32_t>(dataPtr[0])
                | (static_cast<std::uint32_t>(dataPtr[1]) << 8)
                | (static_cast<std::uint32_t>(dataPtr[2]) << 16)
                | (static_cast<std::uint32_t>(dataPtr[3]) << 24);
        }

        return static_cast<std::uint32_t>(dataPtr[3])
            | (static_cast<std::uint32_t>(dataPtr[2]) << 8)
            | (static_cast<std::uint32_t>(dataPtr[1]) << 16)
            | (static_cast<std::uint32_t>(dataPtr[0]) << 24);
    }

    static std::int32_t ReadInt32(const unsigned char* dataPtr, bool littleEndian)
    {
        const std::uint32_t value = ReadUInt32(dataPtr, littleEndian);
        std::int32_t result = 0;
        std::memcpy(&result, &value, sizeof(result));
        return result;
    }

    static double ReadDouble(const unsigned char* dataPtr, bool littleEndian)
    {
        unsigned char bytes[sizeof(double)] = { 0 };
        if (littleEndian)
        {
            for (std::size_t index = 0; index < sizeof(double); index++)
            {
                bytes[index] = dataPtr[index];
            }
        }
        else
        {
            for (std::size_t index = 0; index < sizeof(double); index++)
            {
                bytes[index] = dataPtr[sizeof(double) - index - 1];
            }
        }

#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
        std::reverse(bytes, bytes + sizeof(double));
#endif
        double result = 0.0;
        std::memcpy(&result, bytes, sizeof(result));
        return result;
    }

    static bool IsFinite(double value)
    {
        return std::isfinite(value) != 0;
    }

    static bool AssignRectangle(GB_Rectangle& rectangle, double minX, double minY, double maxX, double maxY)
    {
        rectangle.minX = minX;
        rectangle.minY = minY;
        rectangle.maxX = maxX;
        rectangle.maxY = maxY;

        if (!rectangle.IsValid())
        {
            rectangle.Reset();
            return false;
        }

        return true;
    }

    static void ExpandRectangleToInclude(GB_Rectangle& rectangle, double x, double y)
    {
        if (!IsFinite(x) || !IsFinite(y))
        {
            return;
        }

        if (!rectangle.IsValid())
        {
            AssignRectangle(rectangle, x, y, x, y);
            return;
        }

        rectangle.minX = std::min(rectangle.minX, x);
        rectangle.minY = std::min(rectangle.minY, y);
        rectangle.maxX = std::max(rectangle.maxX, x);
        rectangle.maxY = std::max(rectangle.maxY, y);
    }

    static bool IsRectangleIntersects(const GB_Rectangle& rect1, const GB_Rectangle& rect2)
    {
        if (!rect1.IsValid() || !rect2.IsValid())
        {
            return false;
        }

        return rect1.minX <= rect2.maxX && rect1.maxX >= rect2.minX && rect1.minY <= rect2.maxY && rect1.maxY >= rect2.minY;
    }

    static GB_Rectangle ExpandRectangleForRTreeQuery(const GB_Rectangle& rectangle)
    {
        GB_Rectangle result = rectangle;
        if (!result.IsValid())
        {
            return result;
        }

        const double coordinateScale = std::max(std::max(std::fabs(result.minX), std::fabs(result.maxX)), std::max(std::fabs(result.minY), std::fabs(result.maxY)));
        const double extentScale = std::max(result.maxX - result.minX, result.maxY - result.minY);
        const double scale = std::max(1.0, std::max(coordinateScale, extentScale));
        const double delta = scale * GB_GpkgRTreeQueryExpansionScale + std::numeric_limits<double>::epsilon() * scale * 8.0;

        result.minX -= delta;
        result.minY -= delta;
        result.maxX += delta;
        result.maxY += delta;
        return result;
    }

    static double GetRectangleWidth(const GB_Rectangle& rectangle)
    {
        return rectangle.maxX - rectangle.minX;
    }

    static double GetRectangleHeight(const GB_Rectangle& rectangle)
    {
        return rectangle.maxY - rectangle.minY;
    }

    static bool NearlyEqualDouble(double value1, double value2)
    {
        if (!IsFinite(value1) || !IsFinite(value2))
        {
            return false;
        }

        const double scale = std::max(1.0, std::max(std::fabs(value1), std::fabs(value2)));
        return std::fabs(value1 - value2) <= scale * 1.0e-9;
    }

    static bool IsTileMatrixRangeCompatible(const GB_Rectangle& envelope, const GeoGpkgTileMatrixInfo& tileMatrixInfo)
    {
        if (!envelope.IsValid())
        {
            return false;
        }

        const double envelopeWidth = GetRectangleWidth(envelope);
        const double envelopeHeight = GetRectangleHeight(envelope);
        const double matrixWidth = static_cast<double>(tileMatrixInfo.matrixWidth) * static_cast<double>(tileMatrixInfo.tileWidth) * tileMatrixInfo.pixelXSize;
        const double matrixHeight = static_cast<double>(tileMatrixInfo.matrixHeight) * static_cast<double>(tileMatrixInfo.tileHeight) * tileMatrixInfo.pixelYSize;

        return envelopeWidth > 0.0 && envelopeHeight > 0.0 && NearlyEqualDouble(envelopeWidth, matrixWidth) && NearlyEqualDouble(envelopeHeight, matrixHeight);
    }

    static bool IsCoreGeometryType(const std::string& geometryTypeNameUtf8)
    {
        const std::string type = ToUpperAsciiString(geometryTypeNameUtf8);
        static const char* coreTypes[] =
        {
            "GEOMETRY",
            "POINT",
            "LINESTRING",
            "POLYGON",
            "MULTIPOINT",
            "MULTILINESTRING",
            "MULTIPOLYGON",
            "GEOMETRYCOLLECTION"
        };

        for (std::size_t index = 0; index < sizeof(coreTypes) / sizeof(coreTypes[0]); index++)
        {
            if (type == coreTypes[index])
            {
                return true;
            }
        }
        return false;
    }

    static bool IsExtendedGeometryType(const std::string& geometryTypeNameUtf8)
    {
        const std::string type = ToUpperAsciiString(geometryTypeNameUtf8);
        static const char* extendedTypes[] =
        {
            "CIRCULARSTRING",
            "COMPOUNDCURVE",
            "CURVEPOLYGON",
            "MULTICURVE",
            "MULTISURFACE",
            "CURVE",
            "SURFACE"
        };

        for (std::size_t index = 0; index < sizeof(extendedTypes) / sizeof(extendedTypes[0]); index++)
        {
            if (type == extendedTypes[index])
            {
                return true;
            }
        }
        return false;
    }

    static bool IsSupportedGeometryType(const std::string& geometryTypeNameUtf8)
    {
        return IsCoreGeometryType(geometryTypeNameUtf8) || IsExtendedGeometryType(geometryTypeNameUtf8);
    }

    static bool IsSupportedFieldType(const std::string& typeUtf8)
    {
        const std::string type = ToUpperAsciiString(typeUtf8);
        static const char* supportedTypes[] =
        {
            "BOOLEAN",
            "TINYINT",
            "SMALLINT",
            "MEDIUMINT",
            "INT",
            "INTEGER",
            "FLOAT",
            "DOUBLE",
            "REAL",
            "TEXT",
            "BLOB",
            "DATE",
            "DATETIME"
        };

        for (std::size_t index = 0; index < sizeof(supportedTypes) / sizeof(supportedTypes[0]); index++)
        {
            if (type == supportedTypes[index])
            {
                return true;
            }
        }
        return false;
    }

    static std::string ToContentTypeString(GeoGpkgContentType contentType)
    {
        switch (contentType)
        {
        case GeoGpkgContentType::Features:
            return "features";
        case GeoGpkgContentType::Tiles:
            return "tiles";
        case GeoGpkgContentType::Attributes:
            return "attributes";
        default:
            return std::string();
        }
    }

    static GeoGpkgContentType ParseContentType(const std::string& dataTypeUtf8)
    {
        const std::string type = ToLowerAsciiString(dataTypeUtf8);
        if (type == "features")
        {
            return GeoGpkgContentType::Features;
        }
        if (type == "tiles")
        {
            return GeoGpkgContentType::Tiles;
        }
        if (type == "attributes")
        {
            return GeoGpkgContentType::Attributes;
        }
        return GeoGpkgContentType::Unknown;
    }

    static GB_Variant MakeNullableDoubleVariant(bool valid, double value)
    {
        return valid ? GB_Variant(value) : GB_Variant();
    }

    class WkbReader
    {
    public:
        WkbReader(const unsigned char* dataPtr, std::size_t size) : dataPtr_(dataPtr), size_(size)
        {
        }

        bool ReadByte(unsigned char& outValue)
        {
            if (position_ + 1 > size_)
            {
                return false;
            }
            outValue = dataPtr_[position_];
            position_++;
            return true;
        }

        bool ReadUInt32(bool littleEndian, std::uint32_t& outValue)
        {
            if (position_ + 4 > size_)
            {
                return false;
            }
            outValue = ::ReadUInt32(dataPtr_ + position_, littleEndian);
            position_ += 4;
            return true;
        }

        bool ReadDouble(bool littleEndian, double& outValue)
        {
            if (position_ + 8 > size_)
            {
                return false;
            }
            outValue = ::ReadDouble(dataPtr_ + position_, littleEndian);
            position_ += 8;
            return true;
        }

    private:
        const unsigned char* dataPtr_ = nullptr;
        std::size_t size_ = 0;
        std::size_t position_ = 0;
    };

    static bool ReadWkbPointCoordinates(WkbReader& reader, bool littleEndian, int coordinateDimension, GB_Rectangle& envelope)
    {
        double x = 0.0;
        double y = 0.0;
        if (!reader.ReadDouble(littleEndian, x) || !reader.ReadDouble(littleEndian, y))
        {
            return false;
        }

        for (int dimension = 2; dimension < coordinateDimension; dimension++)
        {
            double unusedValue = 0.0;
            if (!reader.ReadDouble(littleEndian, unusedValue))
            {
                return false;
            }
        }

        if (IsFinite(x) && IsFinite(y))
        {
            ExpandRectangleToInclude(envelope, x, y);
        }
        return true;
    }

    static bool ReadWkbGeometryEnvelope(WkbReader& reader, GB_Rectangle& envelope, int depth)
    {
        if (depth > 64)
        {
            return false;
        }

        unsigned char byteOrder = 0;
        std::uint32_t rawType = 0;
        if (!reader.ReadByte(byteOrder))
        {
            return false;
        }

        const bool littleEndian = byteOrder == 1;
        if (byteOrder != 0 && byteOrder != 1)
        {
            return false;
        }

        if (!reader.ReadUInt32(littleEndian, rawType))
        {
            return false;
        }

        const bool ewkbHasZ = (rawType & 0x80000000u) != 0;
        const bool ewkbHasM = (rawType & 0x40000000u) != 0;
        std::uint32_t type = rawType & 0x1fffffffu;

        int coordinateDimension = 2;
        if (ewkbHasZ)
        {
            coordinateDimension++;
        }
        if (ewkbHasM)
        {
            coordinateDimension++;
        }

        if (type >= 3000)
        {
            coordinateDimension = std::max(coordinateDimension, 4);
            type -= 3000;
        }
        else if (type >= 2000)
        {
            coordinateDimension = std::max(coordinateDimension, 3);
            type -= 2000;
        }
        else if (type >= 1000)
        {
            coordinateDimension = std::max(coordinateDimension, 3);
            type -= 1000;
        }

        switch (type)
        {
        case 1:
            return ReadWkbPointCoordinates(reader, littleEndian, coordinateDimension, envelope);
        case 2:
        {
            std::uint32_t pointCount = 0;
            if (!reader.ReadUInt32(littleEndian, pointCount))
            {
                return false;
            }
            for (std::uint32_t pointIndex = 0; pointIndex < pointCount; pointIndex++)
            {
                if (!ReadWkbPointCoordinates(reader, littleEndian, coordinateDimension, envelope))
                {
                    return false;
                }
            }
            return true;
        }
        case 3:
        {
            std::uint32_t ringCount = 0;
            if (!reader.ReadUInt32(littleEndian, ringCount))
            {
                return false;
            }
            for (std::uint32_t ringIndex = 0; ringIndex < ringCount; ringIndex++)
            {
                std::uint32_t pointCount = 0;
                if (!reader.ReadUInt32(littleEndian, pointCount))
                {
                    return false;
                }
                for (std::uint32_t pointIndex = 0; pointIndex < pointCount; pointIndex++)
                {
                    if (!ReadWkbPointCoordinates(reader, littleEndian, coordinateDimension, envelope))
                    {
                        return false;
                    }
                }
            }
            return true;
        }
        case 4:
        case 5:
        case 6:
        case 7:
        {
            std::uint32_t geometryCount = 0;
            if (!reader.ReadUInt32(littleEndian, geometryCount))
            {
                return false;
            }
            for (std::uint32_t geometryIndex = 0; geometryIndex < geometryCount; geometryIndex++)
            {
                if (!ReadWkbGeometryEnvelope(reader, envelope, depth + 1))
                {
                    return false;
                }
            }
            return true;
        }
        default:
            return false;
        }
    }

    static bool GetBoolScalar(const GB_SqliteResult& result)
    {
        if (result.rows.empty() || result.rows[0].empty())
        {
            return false;
        }
        bool ok = false;
        const long long value = result.rows[0][0].ToInt64(&ok);
        return ok && value != 0;
    }

    static bool TableExists(const GB_Sqlite& database, const std::string& tableNameUtf8, bool& outExists)
    {
        outExists = false;
        return database.TableExists(tableNameUtf8, outExists, true);
    }

    static bool QueryStringColumn(const GB_SqliteResult& result, std::size_t rowIndex, const std::string& columnNameUtf8, std::string& outValue)
    {
        GB_Variant value;
        if (!result.TryGetValue(rowIndex, columnNameUtf8, value))
        {
            outValue.clear();
            return false;
        }

        bool ok = false;
        outValue = value.ToString(&ok);
        return ok || value.IsEmpty();
    }

    static bool QueryIntColumn(const GB_SqliteResult& result, std::size_t rowIndex, const std::string& columnNameUtf8, int& outValue)
    {
        GB_Variant value;
        if (!result.TryGetValue(rowIndex, columnNameUtf8, value))
        {
            outValue = 0;
            return false;
        }

        bool ok = false;
        outValue = value.ToInt(&ok);
        return ok || value.IsEmpty();
    }

    static bool QueryDoubleColumn(const GB_SqliteResult& result, std::size_t rowIndex, const std::string& columnNameUtf8, double& outValue)
    {
        GB_Variant value;
        if (!result.TryGetValue(rowIndex, columnNameUtf8, value))
        {
            outValue = 0.0;
            return false;
        }

        bool ok = false;
        outValue = value.ToDouble(&ok);
        return ok || value.IsEmpty();
    }

    static bool FindTablePrimaryKeyColumn(const GB_Sqlite& database, const std::string& tableNameUtf8, std::string& outPrimaryKeyColumnNameUtf8)
    {
        outPrimaryKeyColumnNameUtf8.clear();
        std::vector<GB_SqliteTableFieldInfo> fields;
        if (!database.GetTableFieldInfos(tableNameUtf8, fields, false))
        {
            return false;
        }

        for (std::size_t index = 0; index < fields.size(); index++)
        {
            const GB_SqliteTableFieldInfo& field = fields[index];
            if (field.primaryKeyIndex > 0 && ToUpperAsciiString(field.typeUtf8).find("INT") != std::string::npos)
            {
                outPrimaryKeyColumnNameUtf8 = field.nameUtf8;
                return true;
            }
        }

        return false;
    }

    static bool BuildFeatureFromValues(const std::vector<GB_SqliteColumnInfo>& columns, const std::vector<GB_Variant>& values, const GeoGpkgFeatureLayerInfo& layerInfo, GeoGpkgFeature& outFeature)
    {
        outFeature = GeoGpkgFeature();
        if (columns.size() != values.size())
        {
            return false;
        }

        for (std::size_t columnIndex = 0; columnIndex < columns.size(); columnIndex++)
        {
            const std::string columnNameUtf8 = columns[columnIndex].nameUtf8;
            const GB_Variant& value = values[columnIndex];

            if (columnNameUtf8 == layerInfo.primaryKeyColumnNameUtf8)
            {
                bool ok = false;
                outFeature.rowId = value.ToInt64(&ok);
                if (!ok && !value.IsEmpty())
                {
                    return false;
                }
            }
            else if (columnNameUtf8 == layerInfo.geometryColumnNameUtf8)
            {
                bool ok = false;
                outFeature.geometry = value.ToBinary(&ok);
                if (!ok && !value.IsEmpty())
                {
                    return false;
                }
            }
            else
            {
                outFeature.attributes[columnNameUtf8] = value;
            }
        }

        return true;
    }

    static bool BuildFeatureFromResultRow(const GB_SqliteResult& result, std::size_t rowIndex, const GeoGpkgFeatureLayerInfo& layerInfo, GeoGpkgFeature& outFeature)
    {
        if (rowIndex >= result.rows.size())
        {
            outFeature = GeoGpkgFeature();
            return false;
        }

        return BuildFeatureFromValues(result.columns, result.rows[rowIndex], layerInfo, outFeature);
    }

    static std::set<std::string> BuildWritableAttributeNameSet(const GeoGpkgFeatureLayerInfo& layerInfo)
    {
        std::set<std::string> result;
        const std::string primaryKeyName = ToLowerAsciiString(layerInfo.primaryKeyColumnNameUtf8);
        const std::string geometryName = ToLowerAsciiString(layerInfo.geometryColumnNameUtf8);
        for (std::size_t index = 0; index < layerInfo.fields.size(); index++)
        {
            const GB_SqliteTableFieldInfo& field = layerInfo.fields[index];
            const std::string lowerName = ToLowerAsciiString(field.nameUtf8);
            if (lowerName != primaryKeyName && lowerName != geometryName)
            {
                result.insert(lowerName);
            }
        }
        return result;
    }

    static bool AppendFeatureInsertSql(const GeoGpkgFeatureLayerInfo& layerInfo, const std::map<std::string, GB_Variant>& attributes, const std::set<std::string>& writableAttributeNames, std::string& outSqlUtf8)
    {
        outSqlUtf8.clear();
        outSqlUtf8 += "INSERT INTO ";
        outSqlUtf8 += QuoteIdentifier(layerInfo.tableNameUtf8);
        outSqlUtf8 += " (";
        outSqlUtf8 += QuoteIdentifier(layerInfo.geometryColumnNameUtf8);
        for (std::map<std::string, GB_Variant>::const_iterator iter = attributes.begin(); iter != attributes.end(); ++iter)
        {
            if (!IsValidIdentifier(iter->first) || writableAttributeNames.find(ToLowerAsciiString(iter->first)) == writableAttributeNames.end())
            {
                return false;
            }
            outSqlUtf8 += ", ";
            outSqlUtf8 += QuoteIdentifier(iter->first);
        }
        outSqlUtf8 += ") VALUES (?";
        for (std::size_t index = 0; index < attributes.size(); index++)
        {
            outSqlUtf8 += ", ?";
        }
        outSqlUtf8 += ")";
        return true;
    }

    static GB_SqliteParameterList BuildFeatureInsertParameters(const GB_ByteBuffer& geometry, const std::map<std::string, GB_Variant>& attributes)
    {
        GB_SqliteParameterList parameters;
        parameters.reserve(attributes.size() + 1);
        parameters.push_back(geometry);
        for (std::map<std::string, GB_Variant>::const_iterator iter = attributes.begin(); iter != attributes.end(); ++iter)
        {
            parameters.push_back(iter->second);
        }
        return parameters;
    }

    static unsigned int NormalizeWkbGeometryType(unsigned int rawType)
    {
        unsigned int type = rawType & 0x1fffffffU;
        if (type >= 3000U && type < 4000U)
        {
            type -= 3000U;
        }
        else if (type >= 2000U && type < 3000U)
        {
            type -= 2000U;
        }
        else if (type >= 1000U && type < 2000U)
        {
            type -= 1000U;
        }
        return type;
    }

    static std::string WkbGeometryTypeToName(unsigned int type)
    {
        switch (type)
        {
        case 1U:
            return "POINT";
        case 2U:
            return "LINESTRING";
        case 3U:
            return "POLYGON";
        case 4U:
            return "MULTIPOINT";
        case 5U:
            return "MULTILINESTRING";
        case 6U:
            return "MULTIPOLYGON";
        case 7U:
            return "GEOMETRYCOLLECTION";
        case 8U:
            return "CIRCULARSTRING";
        case 9U:
            return "COMPOUNDCURVE";
        case 10U:
            return "CURVEPOLYGON";
        case 11U:
            return "MULTICURVE";
        case 12U:
            return "MULTISURFACE";
        case 13U:
            return "CURVE";
        case 14U:
            return "SURFACE";
        default:
            return std::string();
        }
    }

    static bool ReadWkbGeometryTypeName(const GB_ByteBuffer& wkb, std::string& outTypeNameUtf8)
    {
        outTypeNameUtf8.clear();
        if (wkb.size() < 5)
        {
            return false;
        }

        const unsigned char byteOrder = wkb[0];
        if (byteOrder != 0 && byteOrder != 1)
        {
            return false;
        }

        const unsigned int type = NormalizeWkbGeometryType(ReadUInt32(wkb.data() + 1, byteOrder == 1));
        outTypeNameUtf8 = WkbGeometryTypeToName(type);
        return !outTypeNameUtf8.empty();
    }

    static bool IsWkbCompatibleWithLayerGeometry(const GeoGpkgFeatureLayerInfo& layerInfo, const GB_ByteBuffer& gpkgGeometry)
    {
        const std::string layerGeometryType = ToUpperAsciiString(layerInfo.geometryTypeNameUtf8);
        if (layerGeometryType == "GEOMETRY")
        {
            return true;
        }

        GB_ByteBuffer wkb;
        std::string wkbGeometryType;
        if (!GeoGpkg::ExtractWkbFromGpkgGeometry(gpkgGeometry, wkb) || !ReadWkbGeometryTypeName(wkb, wkbGeometryType))
        {
            return false;
        }

        return layerGeometryType == wkbGeometryType;
    }


    enum class GeoGpkgSqlEnvelopeField
    {
        MinX = 0,
        MaxX,
        MinY,
        MaxY
    };

    static bool ExtractSqlGeometryArgument(const std::vector<GB_Variant>& arguments, GB_ByteBuffer& outGeometry, bool& outIsNull, std::string& outErrorMessageUtf8)
    {
        outGeometry.clear();
        outIsNull = false;
        outErrorMessageUtf8.clear();

        if (arguments.size() != 1)
        {
            outErrorMessageUtf8 = "GeoPackage geometry SQL function requires exactly one argument.";
            return false;
        }

        if (arguments[0].IsEmpty())
        {
            outIsNull = true;
            return true;
        }

        bool ok = false;
        outGeometry = arguments[0].ToBinary(&ok);
        if (!ok)
        {
            outErrorMessageUtf8 = "GeoPackage geometry SQL function argument must be a BLOB.";
            return false;
        }

        if (outGeometry.empty())
        {
            outErrorMessageUtf8 = "GeoPackage geometry BLOB is empty.";
            return false;
        }

        return true;
    }

    static bool GetSqlGeometryHeader(const std::vector<GB_Variant>& arguments, GeoGpkgGeometryHeaderInfo& outHeaderInfo, bool& outIsNull, std::string& outErrorMessageUtf8)
    {
        GB_ByteBuffer geometry;
        if (!ExtractSqlGeometryArgument(arguments, geometry, outIsNull, outErrorMessageUtf8))
        {
            return false;
        }

        outHeaderInfo = GeoGpkgGeometryHeaderInfo();
        if (outIsNull)
        {
            return true;
        }

        if (!GeoGpkg::ParseGeometryHeader(geometry, outHeaderInfo) || !outHeaderInfo.valid)
        {
            outErrorMessageUtf8 = "Invalid GeoPackage geometry header.";
            return false;
        }

        return true;
    }

    static bool GetGeometryEnvelopeFromGpkgGeometry(const GB_ByteBuffer& geometry, GB_Rectangle& outEnvelope, bool& outIsEmpty, std::string& outErrorMessageUtf8)
    {
        outEnvelope.Reset();
        outIsEmpty = false;
        outErrorMessageUtf8.clear();

        GeoGpkgGeometryHeaderInfo headerInfo;
        if (!GeoGpkg::ParseGeometryHeader(geometry, headerInfo) || !headerInfo.valid)
        {
            outErrorMessageUtf8 = "Invalid GeoPackage geometry header.";
            return false;
        }

        outIsEmpty = headerInfo.empty;
        if (outIsEmpty)
        {
            return true;
        }

        if (headerInfo.envelope.IsValid())
        {
            outEnvelope = headerInfo.envelope;
            return true;
        }

        GB_ByteBuffer wkb;
        if (!GeoGpkg::ExtractWkbFromGpkgGeometry(geometry, wkb) || !GeoGpkg::CalculateWkbEnvelope(wkb, outEnvelope))
        {
            outErrorMessageUtf8 = "Failed to calculate GeoPackage geometry envelope.";
            return false;
        }

        return true;
    }

    static bool GetSqlGeometryEnvelope(const std::vector<GB_Variant>& arguments, GB_Rectangle& outEnvelope, bool& outIsNull, bool& outIsEmpty, std::string& outErrorMessageUtf8)
    {
        GB_ByteBuffer geometry;
        outEnvelope.Reset();
        outIsNull = false;
        outIsEmpty = false;

        if (!ExtractSqlGeometryArgument(arguments, geometry, outIsNull, outErrorMessageUtf8))
        {
            return false;
        }

        if (outIsNull)
        {
            return true;
        }

        return GetGeometryEnvelopeFromGpkgGeometry(geometry, outEnvelope, outIsEmpty, outErrorMessageUtf8);
    }

    static bool GeoGpkgSqlIsEmptyFunction(const std::vector<GB_Variant>& arguments, GB_Variant& outResult, std::string& outErrorMessageUtf8)
    {
        outResult = GB_Variant();

        bool isNull = false;
        GeoGpkgGeometryHeaderInfo headerInfo;
        if (!GetSqlGeometryHeader(arguments, headerInfo, isNull, outErrorMessageUtf8))
        {
            return false;
        }

        if (isNull)
        {
            return true;
        }

        outResult = headerInfo.empty ? GB_Variant(1) : GB_Variant(0);
        return true;
    }

    static bool GeoGpkgSqlEnvelopeFunction(const std::vector<GB_Variant>& arguments, GB_Variant& outResult, std::string& outErrorMessageUtf8, GeoGpkgSqlEnvelopeField field)
    {
        outResult = GB_Variant();

        bool isNull = false;
        bool isEmpty = false;
        GB_Rectangle envelope;
        if (!GetSqlGeometryEnvelope(arguments, envelope, isNull, isEmpty, outErrorMessageUtf8))
        {
            return false;
        }

        if (isNull || isEmpty)
        {
            return true;
        }

        if (!envelope.IsValid())
        {
            outErrorMessageUtf8 = "GeoPackage geometry envelope is invalid.";
            return false;
        }

        switch (field)
        {
        case GeoGpkgSqlEnvelopeField::MinX:
            outResult = GB_Variant(envelope.minX);
            return true;
        case GeoGpkgSqlEnvelopeField::MaxX:
            outResult = GB_Variant(envelope.maxX);
            return true;
        case GeoGpkgSqlEnvelopeField::MinY:
            outResult = GB_Variant(envelope.minY);
            return true;
        case GeoGpkgSqlEnvelopeField::MaxY:
            outResult = GB_Variant(envelope.maxY);
            return true;
        default:
            outErrorMessageUtf8 = "Unknown GeoPackage envelope field.";
            return false;
        }
    }

    static bool GeoGpkgSqlMinXFunction(const std::vector<GB_Variant>& arguments, GB_Variant& outResult, std::string& outErrorMessageUtf8)
    {
        return GeoGpkgSqlEnvelopeFunction(arguments, outResult, outErrorMessageUtf8, GeoGpkgSqlEnvelopeField::MinX);
    }

    static bool GeoGpkgSqlMaxXFunction(const std::vector<GB_Variant>& arguments, GB_Variant& outResult, std::string& outErrorMessageUtf8)
    {
        return GeoGpkgSqlEnvelopeFunction(arguments, outResult, outErrorMessageUtf8, GeoGpkgSqlEnvelopeField::MaxX);
    }

    static bool GeoGpkgSqlMinYFunction(const std::vector<GB_Variant>& arguments, GB_Variant& outResult, std::string& outErrorMessageUtf8)
    {
        return GeoGpkgSqlEnvelopeFunction(arguments, outResult, outErrorMessageUtf8, GeoGpkgSqlEnvelopeField::MinY);
    }

    static bool GeoGpkgSqlMaxYFunction(const std::vector<GB_Variant>& arguments, GB_Variant& outResult, std::string& outErrorMessageUtf8)
    {
        return GeoGpkgSqlEnvelopeFunction(arguments, outResult, outErrorMessageUtf8, GeoGpkgSqlEnvelopeField::MaxY);
    }

}


GeoGpkg::GeoGpkg()
{
}

GeoGpkg::GeoGpkg(const std::string& filePathUtf8, const GeoGpkgOpenOptions& options)
{
    Open(filePathUtf8, options);
}

GeoGpkg::~GeoGpkg()
{
    Close();
}

bool GeoGpkg::Open(const std::string& filePathUtf8, const GeoGpkgOpenOptions& options)
{
    GB_WriteLockGuard guard(lock_);
    ClearLastError();

    if (filePathUtf8.empty())
    {
        return SetLastError(u8"GeoPackage 文件路径为空。 ");
    }

    database_.Close();
    if (!database_.Open(filePathUtf8, options.sqliteOptions))
    {
        return SetLastSqliteError(u8"打开 GeoPackage 失败");
    }

    if (!RegisterGeoPackageSqlFunctionsNoLock())
    {
        database_.Close();
        return false;
    }

    if (options.initializeIfNeeded && !InitializeCoreTablesNoLock(options.userVersion))
    {
        database_.Close();
        return false;
    }

    return true;
}

void GeoGpkg::Close()
{
    GB_WriteLockGuard guard(lock_);
    database_.Close();
    ClearLastError();
}

bool GeoGpkg::IsOpen() const
{
    GB_ReadLockGuard guard(lock_);
    return database_.IsOpen();
}

std::string GeoGpkg::GetFilePathUtf8() const
{
    GB_ReadLockGuard guard(lock_);
    return database_.GetDatabasePathUtf8();
}

std::string GeoGpkg::GetLastErrorUtf8() const
{
    GB_ReadLockGuard guard(lock_);
    return lastErrorUtf8_;
}

GB_Sqlite& GeoGpkg::GetSqlite()
{
    return database_;
}

const GB_Sqlite& GeoGpkg::GetSqlite() const
{
    return database_;
}

bool GeoGpkg::RegisterGeoPackageSqlFunctionsNoLock()
{
    GB_SqliteFunctionOptions options;
    options.argumentCount = 1;
    options.deterministic = true;
    options.directOnly = false;
    options.innocuous = false;

    if (!database_.RegisterScalarFunction("ST_IsEmpty", GeoGpkgSqlIsEmptyFunction, options))
    {
        return SetLastSqliteError(u8"注册 GeoPackage SQL 函数 ST_IsEmpty 失败");
    }
    if (!database_.RegisterScalarFunction("ST_MinX", GeoGpkgSqlMinXFunction, options))
    {
        return SetLastSqliteError(u8"注册 GeoPackage SQL 函数 ST_MinX 失败");
    }
    if (!database_.RegisterScalarFunction("ST_MaxX", GeoGpkgSqlMaxXFunction, options))
    {
        return SetLastSqliteError(u8"注册 GeoPackage SQL 函数 ST_MaxX 失败");
    }
    if (!database_.RegisterScalarFunction("ST_MinY", GeoGpkgSqlMinYFunction, options))
    {
        return SetLastSqliteError(u8"注册 GeoPackage SQL 函数 ST_MinY 失败");
    }
    if (!database_.RegisterScalarFunction("ST_MaxY", GeoGpkgSqlMaxYFunction, options))
    {
        return SetLastSqliteError(u8"注册 GeoPackage SQL 函数 ST_MaxY 失败");
    }

    return true;
}

bool GeoGpkg::InitializeCoreTables(int userVersion)
{
    GB_WriteLockGuard guard(lock_);
    return InitializeCoreTablesNoLock(userVersion);
}

bool GeoGpkg::InitializeCoreTablesNoLock(int userVersion)
{
    ClearLastError();
    if (!database_.IsOpen())
    {
        return SetLastError(u8"GeoPackage 尚未打开。 ");
    }

    std::ostringstream stream;
    stream << "PRAGMA application_id = " << GB_GpkgApplicationId << ";\n";
    stream << "PRAGMA user_version = " << userVersion << ";\n";
    stream << "CREATE TABLE IF NOT EXISTS gpkg_spatial_ref_sys (\n"
        << "  srs_name TEXT NOT NULL,\n"
        << "  srs_id INTEGER PRIMARY KEY,\n"
        << "  organization TEXT NOT NULL,\n"
        << "  organization_coordsys_id INTEGER NOT NULL,\n"
        << "  definition TEXT NOT NULL,\n"
        << "  description TEXT\n"
        << ");\n";
    stream << "CREATE TABLE IF NOT EXISTS gpkg_contents (\n"
        << "  table_name TEXT NOT NULL PRIMARY KEY,\n"
        << "  data_type TEXT NOT NULL,\n"
        << "  identifier TEXT UNIQUE,\n"
        << "  description TEXT DEFAULT '',\n"
        << "  last_change DATETIME NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),\n"
        << "  min_x DOUBLE,\n"
        << "  min_y DOUBLE,\n"
        << "  max_x DOUBLE,\n"
        << "  max_y DOUBLE,\n"
        << "  srs_id INTEGER,\n"
        << "  CONSTRAINT fk_gc_r_srs_id FOREIGN KEY (srs_id) REFERENCES gpkg_spatial_ref_sys(srs_id)\n"
        << ");\n";
    stream << "CREATE TABLE IF NOT EXISTS gpkg_geometry_columns (\n"
        << "  table_name TEXT NOT NULL,\n"
        << "  column_name TEXT NOT NULL,\n"
        << "  geometry_type_name TEXT NOT NULL,\n"
        << "  srs_id INTEGER NOT NULL,\n"
        << "  z TINYINT NOT NULL,\n"
        << "  m TINYINT NOT NULL,\n"
        << "  CONSTRAINT pk_geom_cols PRIMARY KEY (table_name, column_name),\n"
        << "  CONSTRAINT uk_gc_table_name UNIQUE (table_name),\n"
        << "  CONSTRAINT fk_gc_tn FOREIGN KEY (table_name) REFERENCES gpkg_contents(table_name),\n"
        << "  CONSTRAINT fk_gc_srs FOREIGN KEY (srs_id) REFERENCES gpkg_spatial_ref_sys(srs_id)\n"
        << ");\n";
    stream << "CREATE TABLE IF NOT EXISTS gpkg_tile_matrix_set (\n"
        << "  table_name TEXT NOT NULL PRIMARY KEY,\n"
        << "  srs_id INTEGER NOT NULL,\n"
        << "  min_x DOUBLE NOT NULL,\n"
        << "  min_y DOUBLE NOT NULL,\n"
        << "  max_x DOUBLE NOT NULL,\n"
        << "  max_y DOUBLE NOT NULL,\n"
        << "  CONSTRAINT fk_gtms_table_name FOREIGN KEY (table_name) REFERENCES gpkg_contents(table_name),\n"
        << "  CONSTRAINT fk_gtms_srs FOREIGN KEY (srs_id) REFERENCES gpkg_spatial_ref_sys(srs_id)\n"
        << ");\n";
    stream << "CREATE TABLE IF NOT EXISTS gpkg_tile_matrix (\n"
        << "  table_name TEXT NOT NULL,\n"
        << "  zoom_level INTEGER NOT NULL,\n"
        << "  matrix_width INTEGER NOT NULL,\n"
        << "  matrix_height INTEGER NOT NULL,\n"
        << "  tile_width INTEGER NOT NULL,\n"
        << "  tile_height INTEGER NOT NULL,\n"
        << "  pixel_x_size DOUBLE NOT NULL,\n"
        << "  pixel_y_size DOUBLE NOT NULL,\n"
        << "  CONSTRAINT pk_ttm PRIMARY KEY (table_name, zoom_level),\n"
        << "  CONSTRAINT fk_tmm_table_name FOREIGN KEY (table_name) REFERENCES gpkg_contents(table_name)\n"
        << ");\n";
    stream << "CREATE TABLE IF NOT EXISTS gpkg_extensions (\n"
        << "  table_name TEXT,\n"
        << "  column_name TEXT,\n"
        << "  extension_name TEXT NOT NULL,\n"
        << "  definition TEXT NOT NULL,\n"
        << "  scope TEXT NOT NULL,\n"
        << "  CONSTRAINT ge_tce UNIQUE (table_name, column_name, extension_name)\n"
        << ");\n";

    if (!database_.ExecuteBatch(stream.str()))
    {
        return SetLastSqliteError(u8"初始化 GeoPackage 核心表失败");
    }

    return EnsureDefaultSpatialRefSysNoLock();
}

bool GeoGpkg::EnsureDefaultSpatialRefSys()
{
    GB_WriteLockGuard guard(lock_);
    return EnsureDefaultSpatialRefSysNoLock();
}

bool GeoGpkg::EnsureDefaultSpatialRefSysNoLock()
{
    ClearLastError();

    const std::string wgs84Definition = "GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\",SPHEROID[\"WGS 84\",6378137,298.257223563]],PRIMEM[\"Greenwich\",0],UNIT[\"degree\",0.0174532925199433]]";

    return database_.ExecuteInTransaction([&](GB_SqliteTransaction& transaction) -> bool
        {
            GB_SqliteParameterList row4326;
            row4326.push_back("WGS 84 geodetic");
            row4326.push_back(4326);
            row4326.push_back("EPSG");
            row4326.push_back(4326);
            row4326.push_back(wgs84Definition);
            row4326.push_back("longitude/latitude coordinates in decimal degrees on the WGS 84 spheroid");

            GB_SqliteParameterList rowMinus1;
            rowMinus1.push_back("Undefined Cartesian SRS");
            rowMinus1.push_back(-1);
            rowMinus1.push_back("NONE");
            rowMinus1.push_back(-1);
            rowMinus1.push_back("undefined");
            rowMinus1.push_back("undefined Cartesian coordinate reference system");

            GB_SqliteParameterList row0;
            row0.push_back("Undefined geographic SRS");
            row0.push_back(0);
            row0.push_back("NONE");
            row0.push_back(0);
            row0.push_back("undefined");
            row0.push_back("undefined geographic coordinate reference system");

            const std::string sql = "INSERT OR IGNORE INTO gpkg_spatial_ref_sys(srs_name, srs_id, organization, organization_coordsys_id, definition, description) VALUES(?, ?, ?, ?, ?, ?)";
            return transaction.Execute(sql, row4326) && transaction.Execute(sql, rowMinus1) && transaction.Execute(sql, row0);
        }, GB_SqliteTransactionMode::Immediate) ? true : SetLastSqliteError("写入默认空间参考失败");
}

bool GeoGpkg::ValidateBasicSchema(std::vector<std::string>* outMessages) const
{
    GB_ReadLockGuard guard(lock_);
    ClearLastError();
    if (outMessages != nullptr)
    {
        outMessages->clear();
    }

    static const char* requiredTables[] =
    {
        "gpkg_spatial_ref_sys",
        "gpkg_contents",
        "gpkg_geometry_columns",
        "gpkg_tile_matrix_set",
        "gpkg_tile_matrix"
    };

    bool hasError = false;
    for (std::size_t index = 0; index < sizeof(requiredTables) / sizeof(requiredTables[0]); index++)
    {
        bool exists = false;
        if (!TableExists(database_, requiredTables[index], exists))
        {
            return SetLastSqliteError(u8"检查 GeoPackage 核心表失败");
        }
        if (!exists)
        {
            hasError = true;
            if (outMessages != nullptr)
            {
                outMessages->push_back(u8"缺少核心表：" + std::string(requiredTables[index]));
            }
        }
    }
    if (hasError)
    {
        return false;
    }

    for (int srsIdIndex = 0; srsIdIndex < 3; srsIdIndex++)
    {
        const int srsId = srsIdIndex == 0 ? 4326 : (srsIdIndex == 1 ? -1 : 0);
        GB_Variant value;
        if (!database_.ExecuteScalar("SELECT COUNT(*) FROM gpkg_spatial_ref_sys WHERE srs_id = ?", GB_SqliteParameterList(1, srsId), value))
        {
            return SetLastSqliteError(u8"检查默认空间参考失败");
        }

        bool ok = false;
        const long long count = value.ToInt64(&ok);
        if (!ok || count <= 0)
        {
            hasError = true;
            if (outMessages != nullptr)
            {
                outMessages->push_back(u8"缺少默认空间参考：" + ToString(srsId));
            }
        }
    }

    GB_SqliteResult result;
    if (!database_.Query("PRAGMA foreign_key_check", result))
    {
        return SetLastSqliteError(u8"执行 foreign_key_check 失败");
    }
    if (!result.rows.empty())
    {
        hasError = true;
        if (outMessages != nullptr)
        {
            outMessages->push_back(u8"foreign_key_check 返回 " + ToString(result.rows.size()) + u8" 条异常记录。 ");
        }
    }

    return !hasError;
}

bool GeoGpkg::AddOrUpdateSpatialRefSys(const GeoGpkgSpatialRefSys& srs)
{
    GB_WriteLockGuard guard(lock_);
    ClearLastError();

    if (srs.srsNameUtf8.empty() || srs.organizationUtf8.empty() || srs.definitionUtf8.empty())
    {
        return SetLastError(u8"空间参考记录的 srs_name、organization、definition 不能为空。 ");
    }

    GB_SqliteParameterList updateParameters;
    updateParameters.push_back(srs.srsNameUtf8);
    updateParameters.push_back(srs.organizationUtf8);
    updateParameters.push_back(srs.organizationCoordsysId);
    updateParameters.push_back(srs.definitionUtf8);
    updateParameters.push_back(srs.descriptionUtf8);
    updateParameters.push_back(srs.srsId);

    GB_SqliteParameterList insertParameters;
    insertParameters.push_back(srs.srsNameUtf8);
    insertParameters.push_back(srs.srsId);
    insertParameters.push_back(srs.organizationUtf8);
    insertParameters.push_back(srs.organizationCoordsysId);
    insertParameters.push_back(srs.definitionUtf8);
    insertParameters.push_back(srs.descriptionUtf8);

    if (!database_.ExecuteInTransaction([&](GB_SqliteTransaction& transaction) -> bool
        {
            if (!transaction.Execute("UPDATE gpkg_spatial_ref_sys SET srs_name = ?, organization = ?, organization_coordsys_id = ?, definition = ?, description = ? WHERE srs_id = ?", updateParameters))
            {
                return false;
            }

            GB_Variant changedValue;
            if (!transaction.ExecuteScalar("SELECT changes()", changedValue))
            {
                return false;
            }

            bool ok = false;
            const long long changedCount = changedValue.ToInt64(&ok);
            if (!ok)
            {
                return false;
            }

            if (changedCount > 0)
            {
                return true;
            }

            return transaction.Execute("INSERT INTO gpkg_spatial_ref_sys(srs_name, srs_id, organization, organization_coordsys_id, definition, description) VALUES(?, ?, ?, ?, ?, ?)", insertParameters);
        }, GB_SqliteTransactionMode::Immediate))
    {
        return SetLastSqliteError(u8"写入空间参考失败");
    }
    return true;
}

bool GeoGpkg::SpatialRefSysExists(int srsId, bool& outExists) const
{
    GB_ReadLockGuard guard(lock_);
    ClearLastError();
    outExists = false;

    GB_Variant value;
    if (!database_.ExecuteScalar("SELECT COUNT(*) FROM gpkg_spatial_ref_sys WHERE srs_id = ?", GB_SqliteParameterList(1, srsId), value))
    {
        return SetLastSqliteError(u8"查询空间参考是否存在失败");
    }

    bool ok = false;
    outExists = value.ToInt64(&ok) > 0;
    if (!ok)
    {
        outExists = false;
        return SetLastError(u8"空间参考数量字段类型异常。 ");
    }
    return true;
}

bool GeoGpkg::GetSpatialRefSys(int srsId, GeoGpkgSpatialRefSys& outSrs) const
{
    GB_ReadLockGuard guard(lock_);
    ClearLastError();
    outSrs = GeoGpkgSpatialRefSys();

    GB_SqliteResult result;
    if (!database_.Query("SELECT srs_name, srs_id, organization, organization_coordsys_id, definition, description FROM gpkg_spatial_ref_sys WHERE srs_id = ?", GB_SqliteParameterList(1, srsId), result, 1))
    {
        return SetLastSqliteError(u8"查询空间参考失败");
    }

    if (result.rows.empty())
    {
        return SetLastError(u8"指定空间参考不存在：" + ToString(srsId));
    }

    QueryStringColumn(result, 0, "srs_name", outSrs.srsNameUtf8);
    QueryIntColumn(result, 0, "srs_id", outSrs.srsId);
    QueryStringColumn(result, 0, "organization", outSrs.organizationUtf8);
    QueryIntColumn(result, 0, "organization_coordsys_id", outSrs.organizationCoordsysId);
    QueryStringColumn(result, 0, "definition", outSrs.definitionUtf8);
    QueryStringColumn(result, 0, "description", outSrs.descriptionUtf8);
    return true;
}

bool GeoGpkg::ListSpatialRefSys(std::vector<GeoGpkgSpatialRefSys>& outSrsList) const
{
    GB_ReadLockGuard guard(lock_);
    ClearLastError();
    outSrsList.clear();

    GB_SqliteResult result;
    if (!database_.Query("SELECT srs_name, srs_id, organization, organization_coordsys_id, definition, description FROM gpkg_spatial_ref_sys ORDER BY srs_id", result))
    {
        return SetLastSqliteError(u8"查询空间参考列表失败");
    }

    outSrsList.reserve(result.rows.size());
    for (std::size_t rowIndex = 0; rowIndex < result.rows.size(); rowIndex++)
    {
        GeoGpkgSpatialRefSys srs;
        QueryStringColumn(result, rowIndex, "srs_name", srs.srsNameUtf8);
        QueryIntColumn(result, rowIndex, "srs_id", srs.srsId);
        QueryStringColumn(result, rowIndex, "organization", srs.organizationUtf8);
        QueryIntColumn(result, rowIndex, "organization_coordsys_id", srs.organizationCoordsysId);
        QueryStringColumn(result, rowIndex, "definition", srs.definitionUtf8);
        QueryStringColumn(result, rowIndex, "description", srs.descriptionUtf8);
        outSrsList.push_back(srs);
    }
    return true;
}

bool GeoGpkg::ListContents(std::vector<GeoGpkgContentInfo>& outContents) const
{
    GB_ReadLockGuard guard(lock_);
    ClearLastError();
    outContents.clear();

    GB_SqliteResult result;
    if (!database_.Query("SELECT table_name, data_type, identifier, description, last_change, min_x, min_y, max_x, max_y, srs_id FROM gpkg_contents ORDER BY table_name", result))
    {
        return SetLastSqliteError(u8"查询 gpkg_contents 失败");
    }

    outContents.reserve(result.rows.size());
    for (std::size_t rowIndex = 0; rowIndex < result.rows.size(); rowIndex++)
    {
        GeoGpkgContentInfo content;
        QueryStringColumn(result, rowIndex, "table_name", content.tableNameUtf8);
        QueryStringColumn(result, rowIndex, "data_type", content.dataTypeUtf8);
        QueryStringColumn(result, rowIndex, "identifier", content.identifierUtf8);
        QueryStringColumn(result, rowIndex, "description", content.descriptionUtf8);
        QueryStringColumn(result, rowIndex, "last_change", content.lastChangeUtf8);
        content.contentType = ParseContentType(content.dataTypeUtf8);
        QueryIntColumn(result, rowIndex, "srs_id", content.srsId);

        GB_Variant minXValue = result.GetValue(rowIndex, "min_x");
        GB_Variant minYValue = result.GetValue(rowIndex, "min_y");
        GB_Variant maxXValue = result.GetValue(rowIndex, "max_x");
        GB_Variant maxYValue = result.GetValue(rowIndex, "max_y");
        if (!minXValue.IsEmpty() && !minYValue.IsEmpty() && !maxXValue.IsEmpty() && !maxYValue.IsEmpty())
        {
            bool okMinX = false;
            bool okMinY = false;
            bool okMaxX = false;
            bool okMaxY = false;
            const double minX = minXValue.ToDouble(&okMinX);
            const double minY = minYValue.ToDouble(&okMinY);
            const double maxX = maxXValue.ToDouble(&okMaxX);
            const double maxY = maxYValue.ToDouble(&okMaxY);
            if (okMinX && okMinY && okMaxX && okMaxY)
            {
                AssignRectangle(content.envelope, minX, minY, maxX, maxY);
            }
        }
        outContents.push_back(content);
    }
    return true;
}

bool GeoGpkg::CreateFeatureLayer(const std::string& tableNameUtf8, const std::string& geometryTypeNameUtf8, int srsId, const std::vector<GeoGpkgFieldDef>& fields, const std::string& geometryColumnNameUtf8, const std::string& primaryKeyColumnNameUtf8, const GB_Rectangle* envelope, const std::string& identifierUtf8, const std::string& descriptionUtf8, int z, int m, bool overwrite)
{
    GB_WriteLockGuard guard(lock_);
    ClearLastError();

    if (!IsValidIdentifier(tableNameUtf8) || !IsValidIdentifier(geometryColumnNameUtf8) || !IsValidIdentifier(primaryKeyColumnNameUtf8))
    {
        return SetLastError(u8"图层名、几何字段名或主键字段名非法。 ");
    }

    if (geometryColumnNameUtf8 == primaryKeyColumnNameUtf8)
    {
        return SetLastError(u8"几何字段名不能与主键字段名相同。 ");
    }

    const std::string geometryType = ToUpperAsciiString(geometryTypeNameUtf8);
    if (!IsSupportedGeometryType(geometryType))
    {
        return SetLastError(u8"不支持的 GeoPackage 几何类型：" + geometryTypeNameUtf8);
    }

    if (z < 0 || z > 2 || m < 0 || m > 2)
    {
        return SetLastError(u8"gpkg_geometry_columns.z 和 m 只能为 0、1 或 2。 ");
    }

    bool srsExists = false;
    GB_Variant srsCount;
    if (!database_.ExecuteScalar("SELECT COUNT(*) FROM gpkg_spatial_ref_sys WHERE srs_id = ?", GB_SqliteParameterList(1, srsId), srsCount))
    {
        return SetLastSqliteError(u8"检查空间参考失败");
    }
    bool ok = false;
    srsExists = srsCount.ToInt64(&ok) > 0;
    if (!ok || !srsExists)
    {
        return SetLastError(u8"SRID 必须先存在于 gpkg_spatial_ref_sys：" + ToString(srsId));
    }

    std::set<std::string> usedFieldNames;
    usedFieldNames.insert(ToLowerAsciiString(primaryKeyColumnNameUtf8));
    usedFieldNames.insert(ToLowerAsciiString(geometryColumnNameUtf8));
    for (std::size_t index = 0; index < fields.size(); index++)
    {
        const GeoGpkgFieldDef& field = fields[index];
        if (!IsValidIdentifier(field.nameUtf8))
        {
            return SetLastError(u8"字段名非法：" + field.nameUtf8);
        }
        const std::string lowerName = ToLowerAsciiString(field.nameUtf8);
        if (usedFieldNames.find(lowerName) != usedFieldNames.end())
        {
            return SetLastError(u8"字段名重复：" + field.nameUtf8);
        }
        usedFieldNames.insert(lowerName);
        if (!IsSupportedFieldType(field.typeUtf8))
        {
            return SetLastError(u8"不支持的 GeoPackage 字段类型：" + field.typeUtf8);
        }
        if (!IsValidSqlFragment(field.defaultSqlUtf8))
        {
            return SetLastError(u8"字段默认值 SQL 片段非法：" + field.nameUtf8);
        }
    }

    const GB_Rectangle realEnvelope = envelope == nullptr ? GB_Rectangle() : *envelope;
    if (envelope != nullptr && !realEnvelope.IsValid())
    {
        return SetLastError(u8"图层范围 envelope 无效。 ");
    }

    const std::string tableSql = [&]() -> std::string
        {
            std::ostringstream stream;
            stream << "CREATE TABLE " << QuoteIdentifier(tableNameUtf8) << " (";
            stream << QuoteIdentifier(primaryKeyColumnNameUtf8) << " INTEGER PRIMARY KEY AUTOINCREMENT";
            stream << ", " << QuoteIdentifier(geometryColumnNameUtf8) << " " << geometryType;
            for (std::size_t index = 0; index < fields.size(); index++)
            {
                const GeoGpkgFieldDef& field = fields[index];
                stream << ", " << QuoteIdentifier(field.nameUtf8) << " " << ToUpperAsciiString(field.typeUtf8);
                if (field.notNull)
                {
                    stream << " NOT NULL";
                }
                if (field.unique)
                {
                    stream << " UNIQUE";
                }
                if (!field.defaultSqlUtf8.empty())
                {
                    stream << " DEFAULT " << field.defaultSqlUtf8;
                }
            }
            stream << ")";
            return stream.str();
        }();

    const std::string realIdentifier = identifierUtf8.empty() ? tableNameUtf8 : identifierUtf8;

    if (overwrite)
    {
        GeoGpkgFeatureLayerInfo oldLayerInfo;
        if (GetFeatureLayerInfoNoLock(tableNameUtf8, oldLayerInfo))
        {
            if (!DropSpatialIndexNoLock(tableNameUtf8, oldLayerInfo.geometryColumnNameUtf8))
            {
                return false;
            }
        }
        ClearLastError();
    }

    if (!database_.ExecuteInTransaction([&](GB_SqliteTransaction& transaction) -> bool
        {
            if (overwrite)
            {
                if (!transaction.Execute("DELETE FROM gpkg_extensions WHERE table_name = ?", GB_SqliteParameterList(1, tableNameUtf8)))
                {
                    return false;
                }
                if (!transaction.Execute("DELETE FROM gpkg_geometry_columns WHERE table_name = ?", GB_SqliteParameterList(1, tableNameUtf8)))
                {
                    return false;
                }
                if (!transaction.Execute("DELETE FROM gpkg_tile_matrix WHERE table_name = ?", GB_SqliteParameterList(1, tableNameUtf8)))
                {
                    return false;
                }
                if (!transaction.Execute("DELETE FROM gpkg_tile_matrix_set WHERE table_name = ?", GB_SqliteParameterList(1, tableNameUtf8)))
                {
                    return false;
                }
                if (!transaction.Execute("DELETE FROM gpkg_contents WHERE table_name = ?", GB_SqliteParameterList(1, tableNameUtf8)))
                {
                    return false;
                }
                if (!transaction.Execute("DROP TABLE IF EXISTS " + QuoteIdentifier(tableNameUtf8)))
                {
                    return false;
                }
            }

            if (!transaction.Execute(tableSql))
            {
                return false;
            }

            GB_SqliteParameterList contentParameters;
            contentParameters.push_back(tableNameUtf8);
            contentParameters.push_back("features");
            contentParameters.push_back(realIdentifier);
            contentParameters.push_back(descriptionUtf8);
            contentParameters.push_back(MakeNullableDoubleVariant(realEnvelope.IsValid(), realEnvelope.minX));
            contentParameters.push_back(MakeNullableDoubleVariant(realEnvelope.IsValid(), realEnvelope.minY));
            contentParameters.push_back(MakeNullableDoubleVariant(realEnvelope.IsValid(), realEnvelope.maxX));
            contentParameters.push_back(MakeNullableDoubleVariant(realEnvelope.IsValid(), realEnvelope.maxY));
            contentParameters.push_back(srsId);

            if (!transaction.Execute("INSERT INTO gpkg_contents(table_name, data_type, identifier, description, min_x, min_y, max_x, max_y, srs_id) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?)", contentParameters))
            {
                return false;
            }

            GB_SqliteParameterList geometryParameters;
            geometryParameters.push_back(tableNameUtf8);
            geometryParameters.push_back(geometryColumnNameUtf8);
            geometryParameters.push_back(geometryType);
            geometryParameters.push_back(srsId);
            geometryParameters.push_back(z);
            geometryParameters.push_back(m);
            if (!transaction.Execute("INSERT INTO gpkg_geometry_columns(table_name, column_name, geometry_type_name, srs_id, z, m) VALUES(?, ?, ?, ?, ?, ?)", geometryParameters))
            {
                return false;
            }

            if (IsExtendedGeometryType(geometryType))
            {
                GB_SqliteParameterList extensionParameters;
                extensionParameters.push_back(tableNameUtf8);
                extensionParameters.push_back(geometryColumnNameUtf8);
                extensionParameters.push_back("gpkg_geom_" + ToLowerAsciiString(geometryType));
                extensionParameters.push_back("http://www.geopackage.org/spec/#extension_geometry_types");
                extensionParameters.push_back("read-write");
                if (!transaction.Execute("INSERT OR REPLACE INTO gpkg_extensions(table_name, column_name, extension_name, definition, scope) VALUES(?, ?, ?, ?, ?)", extensionParameters))
                {
                    return false;
                }
            }

            return true;
        }, GB_SqliteTransactionMode::Immediate))
    {
        return SetLastSqliteError(u8"创建矢量图层失败");
    }

    return true;
}

bool GeoGpkg::ListFeatureLayers(std::vector<GeoGpkgFeatureLayerInfo>& outLayers) const
{
    GB_ReadLockGuard guard(lock_);
    ClearLastError();
    outLayers.clear();

    GB_SqliteResult result;
    const std::string sql =
        "SELECT c.table_name, c.identifier, c.description, c.min_x, c.min_y, c.max_x, c.max_y, g.column_name, g.geometry_type_name, g.srs_id, g.z, g.m "
        "FROM gpkg_contents c JOIN gpkg_geometry_columns g ON c.table_name = g.table_name "
        "WHERE c.data_type = 'features' ORDER BY c.table_name";
    if (!database_.Query(sql, result))
    {
        return SetLastSqliteError(u8"查询矢量图层列表失败");
    }

    outLayers.reserve(result.rows.size());
    for (std::size_t rowIndex = 0; rowIndex < result.rows.size(); rowIndex++)
    {
        GeoGpkgFeatureLayerInfo layer;
        QueryStringColumn(result, rowIndex, "table_name", layer.tableNameUtf8);
        QueryStringColumn(result, rowIndex, "identifier", layer.identifierUtf8);
        QueryStringColumn(result, rowIndex, "description", layer.descriptionUtf8);
        QueryStringColumn(result, rowIndex, "column_name", layer.geometryColumnNameUtf8);
        QueryStringColumn(result, rowIndex, "geometry_type_name", layer.geometryTypeNameUtf8);
        QueryIntColumn(result, rowIndex, "srs_id", layer.srsId);
        QueryIntColumn(result, rowIndex, "z", layer.z);
        QueryIntColumn(result, rowIndex, "m", layer.m);

        GB_Variant minXValue = result.GetValue(rowIndex, "min_x");
        GB_Variant minYValue = result.GetValue(rowIndex, "min_y");
        GB_Variant maxXValue = result.GetValue(rowIndex, "max_x");
        GB_Variant maxYValue = result.GetValue(rowIndex, "max_y");
        if (!minXValue.IsEmpty() && !minYValue.IsEmpty() && !maxXValue.IsEmpty() && !maxYValue.IsEmpty())
        {
            bool okMinX = false;
            bool okMinY = false;
            bool okMaxX = false;
            bool okMaxY = false;
            const double minX = minXValue.ToDouble(&okMinX);
            const double minY = minYValue.ToDouble(&okMinY);
            const double maxX = maxXValue.ToDouble(&okMaxX);
            const double maxY = maxYValue.ToDouble(&okMaxY);
            if (okMinX && okMinY && okMaxX && okMaxY)
            {
                AssignRectangle(layer.envelope, minX, minY, maxX, maxY);
            }
        }

        FindTablePrimaryKeyColumn(database_, layer.tableNameUtf8, layer.primaryKeyColumnNameUtf8);
        database_.GetTableFieldInfos(layer.tableNameUtf8, layer.fields, false);
        outLayers.push_back(layer);
    }
    return true;
}

bool GeoGpkg::GetFeatureLayerInfo(const std::string& tableNameUtf8, GeoGpkgFeatureLayerInfo& outLayer) const
{
    GB_ReadLockGuard guard(lock_);
    return GetFeatureLayerInfoNoLock(tableNameUtf8, outLayer);
}

bool GeoGpkg::GetFeatureLayerInfoNoLock(const std::string& tableNameUtf8, GeoGpkgFeatureLayerInfo& outLayer) const
{
    ClearLastError();
    outLayer = GeoGpkgFeatureLayerInfo();

    GB_SqliteResult result;
    const std::string sql =
        "SELECT c.table_name, c.identifier, c.description, c.min_x, c.min_y, c.max_x, c.max_y, g.column_name, g.geometry_type_name, g.srs_id, g.z, g.m "
        "FROM gpkg_contents c JOIN gpkg_geometry_columns g ON c.table_name = g.table_name "
        "WHERE c.data_type = 'features' AND c.table_name = ?";
    if (!database_.Query(sql, GB_SqliteParameterList(1, tableNameUtf8), result, 1))
    {
        return SetLastSqliteError(u8"查询矢量图层信息失败");
    }
    if (result.rows.empty())
    {
        return SetLastError(u8"矢量图层不存在：" + tableNameUtf8);
    }

    QueryStringColumn(result, 0, "table_name", outLayer.tableNameUtf8);
    QueryStringColumn(result, 0, "identifier", outLayer.identifierUtf8);
    QueryStringColumn(result, 0, "description", outLayer.descriptionUtf8);
    QueryStringColumn(result, 0, "column_name", outLayer.geometryColumnNameUtf8);
    QueryStringColumn(result, 0, "geometry_type_name", outLayer.geometryTypeNameUtf8);
    QueryIntColumn(result, 0, "srs_id", outLayer.srsId);
    QueryIntColumn(result, 0, "z", outLayer.z);
    QueryIntColumn(result, 0, "m", outLayer.m);

    GB_Variant minXValue = result.GetValue(0, "min_x");
    GB_Variant minYValue = result.GetValue(0, "min_y");
    GB_Variant maxXValue = result.GetValue(0, "max_x");
    GB_Variant maxYValue = result.GetValue(0, "max_y");
    if (!minXValue.IsEmpty() && !minYValue.IsEmpty() && !maxXValue.IsEmpty() && !maxYValue.IsEmpty())
    {
        bool okMinX = false;
        bool okMinY = false;
        bool okMaxX = false;
        bool okMaxY = false;
        const double minX = minXValue.ToDouble(&okMinX);
        const double minY = minYValue.ToDouble(&okMinY);
        const double maxX = maxXValue.ToDouble(&okMaxX);
        const double maxY = maxYValue.ToDouble(&okMaxY);
        if (okMinX && okMinY && okMaxX && okMaxY)
        {
            AssignRectangle(outLayer.envelope, minX, minY, maxX, maxY);
        }
    }

    if (!FindTablePrimaryKeyColumn(database_, outLayer.tableNameUtf8, outLayer.primaryKeyColumnNameUtf8))
    {
        return SetLastError(u8"矢量图层缺少 INTEGER PRIMARY KEY 字段：" + tableNameUtf8);
    }

    if (!database_.GetTableFieldInfos(outLayer.tableNameUtf8, outLayer.fields, false))
    {
        return SetLastSqliteError(u8"查询图层字段失败");
    }
    return true;
}

bool GeoGpkg::InsertFeature(const std::string& tableNameUtf8, const GB_ByteBuffer& geometry, const std::map<std::string, GB_Variant>& attributes, long long* outRowId)
{
    if (outRowId != nullptr)
    {
        *outRowId = 0;
    }

    GeoGpkgFeature feature;
    feature.geometry = geometry;
    feature.attributes = attributes;
    std::vector<GeoGpkgFeature> features(1, feature);
    std::vector<long long> rowIds;
    if (!InsertFeatures(tableNameUtf8, features, outRowId == nullptr ? nullptr : &rowIds))
    {
        return false;
    }

    if (outRowId != nullptr && !rowIds.empty())
    {
        *outRowId = rowIds[0];
    }
    return true;
}

bool GeoGpkg::InsertFeatures(const std::string& tableNameUtf8, const std::vector<GeoGpkgFeature>& features, std::vector<long long>* outRowIds)
{
    GB_WriteLockGuard guard(lock_);
    ClearLastError();
    if (outRowIds != nullptr)
    {
        outRowIds->clear();
    }
    if (features.empty())
    {
        return true;
    }

    GeoGpkgFeatureLayerInfo layerInfo;
    if (!GetFeatureLayerInfoNoLock(tableNameUtf8, layerInfo))
    {
        return false;
    }

    const std::set<std::string> writableAttributeNames = BuildWritableAttributeNameSet(layerInfo);
    for (std::size_t featureIndex = 0; featureIndex < features.size(); featureIndex++)
    {
        for (std::map<std::string, GB_Variant>::const_iterator iter = features[featureIndex].attributes.begin(); iter != features[featureIndex].attributes.end(); ++iter)
        {
            if (!IsValidIdentifier(iter->first) || writableAttributeNames.find(ToLowerAsciiString(iter->first)) == writableAttributeNames.end())
            {
                return SetLastError(u8"要素属性字段不存在或不可写：" + iter->first);
            }
        }
    }

    std::vector<long long> insertedRowIds;
    if (outRowIds != nullptr)
    {
        insertedRowIds.reserve(features.size());
    }

    std::string validationErrorUtf8;
    GB_Rectangle insertedEnvelope;
    bool hasInsertedEnvelope = false;
    if (!database_.ExecuteInTransaction([&](GB_SqliteTransaction& transaction) -> bool
        {
            for (std::size_t featureIndex = 0; featureIndex < features.size(); featureIndex++)
            {
                const GeoGpkgFeature& feature = features[featureIndex];
                GeoGpkgGeometryHeaderInfo headerInfo;
                if (!ParseGeometryHeader(feature.geometry, headerInfo) || !headerInfo.valid)
                {
                    validationErrorUtf8 = u8"要素 GeoPackageBinary 几何头无效。";
                    return false;
                }
                if (headerInfo.srsId != layerInfo.srsId)
                {
                    validationErrorUtf8 = u8"要素 SRID 与图层登记 SRID 不一致。";
                    return false;
                }
                if (!IsWkbCompatibleWithLayerGeometry(layerInfo, feature.geometry))
                {
                    validationErrorUtf8 = u8"要素 WKB 几何类型与图层登记几何类型不一致。";
                    return false;
                }

                GB_Rectangle featureEnvelope;
                bool isEmptyGeometry = false;
                std::string geometryErrorMessageUtf8;
                if (!GetGeometryEnvelopeFromGpkgGeometry(feature.geometry, featureEnvelope, isEmptyGeometry, geometryErrorMessageUtf8))
                {
                    validationErrorUtf8 = u8"计算要素几何范围失败：" + geometryErrorMessageUtf8;
                    return false;
                }
                if (!isEmptyGeometry && featureEnvelope.IsValid())
                {
                    if (!hasInsertedEnvelope)
                    {
                        insertedEnvelope = featureEnvelope;
                        hasInsertedEnvelope = true;
                    }
                    else
                    {
                        ExpandRectangleToInclude(insertedEnvelope, featureEnvelope.minX, featureEnvelope.minY);
                        ExpandRectangleToInclude(insertedEnvelope, featureEnvelope.maxX, featureEnvelope.maxY);
                    }
                }

                std::string insertSqlUtf8;
                if (!AppendFeatureInsertSql(layerInfo, feature.attributes, writableAttributeNames, insertSqlUtf8))
                {
                    validationErrorUtf8 = u8"构造要素插入 SQL 失败。";
                    return false;
                }
                if (!transaction.Execute(insertSqlUtf8, BuildFeatureInsertParameters(feature.geometry, feature.attributes)))
                {
                    return false;
                }

                GB_Variant rowIdValue;
                if (!transaction.ExecuteScalar("SELECT last_insert_rowid()", rowIdValue))
                {
                    return false;
                }
                bool ok = false;
                const long long rowId = rowIdValue.ToInt64(&ok);
                if (!ok)
                {
                    return false;
                }
                if (outRowIds != nullptr)
                {
                    insertedRowIds.push_back(rowId);
                }

            }

            if (hasInsertedEnvelope)
            {
                GB_SqliteResult contentResult;
                if (!transaction.Query("SELECT min_x, min_y, max_x, max_y FROM gpkg_contents WHERE table_name = ?", GB_SqliteParameterList(1, layerInfo.tableNameUtf8), contentResult))
                {
                    return false;
                }
                if (contentResult.rows.size() != 1)
                {
                    validationErrorUtf8 = u8"gpkg_contents 中缺少当前图层记录。";
                    return false;
                }

                GB_Rectangle mergedEnvelope = insertedEnvelope;
                GB_Variant minXValue = contentResult.GetValue(0, "min_x");
                GB_Variant minYValue = contentResult.GetValue(0, "min_y");
                GB_Variant maxXValue = contentResult.GetValue(0, "max_x");
                GB_Variant maxYValue = contentResult.GetValue(0, "max_y");
                if (!minXValue.IsEmpty() && !minYValue.IsEmpty() && !maxXValue.IsEmpty() && !maxYValue.IsEmpty())
                {
                    bool okMinX = false;
                    bool okMinY = false;
                    bool okMaxX = false;
                    bool okMaxY = false;
                    const double minX = minXValue.ToDouble(&okMinX);
                    const double minY = minYValue.ToDouble(&okMinY);
                    const double maxX = maxXValue.ToDouble(&okMaxX);
                    const double maxY = maxYValue.ToDouble(&okMaxY);
                    if (okMinX && okMinY && okMaxX && okMaxY)
                    {
                        ExpandRectangleToInclude(mergedEnvelope, minX, minY);
                        ExpandRectangleToInclude(mergedEnvelope, maxX, maxY);
                    }
                }

                GB_SqliteParameterList envelopeParameters;
                envelopeParameters.push_back(mergedEnvelope.minX);
                envelopeParameters.push_back(mergedEnvelope.minY);
                envelopeParameters.push_back(mergedEnvelope.maxX);
                envelopeParameters.push_back(mergedEnvelope.maxY);
                envelopeParameters.push_back(layerInfo.tableNameUtf8);
                if (!transaction.Execute("UPDATE gpkg_contents SET min_x = ?, min_y = ?, max_x = ?, max_y = ?, last_change = strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE table_name = ?", envelopeParameters))
                {
                    return false;
                }
            }
            else
            {
                if (!transaction.Execute("UPDATE gpkg_contents SET last_change = strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE table_name = ?", GB_SqliteParameterList(1, layerInfo.tableNameUtf8)))
                {
                    return false;
                }
            }
            return true;
        }, GB_SqliteTransactionMode::Immediate))
    {
        if (!validationErrorUtf8.empty())
        {
            return SetLastError(validationErrorUtf8);
        }
        return SetLastSqliteError(u8"插入要素失败");
    }

    if (outRowIds != nullptr)
    {
        *outRowIds = insertedRowIds;
    }
    return true;
}

bool GeoGpkg::QueryFeaturesByEnvelope(const std::string& tableNameUtf8, const GB_Rectangle& envelope, std::vector<GeoGpkgFeature>& outFeatures, std::size_t maxFeatureCount) const
{
    GB_ReadLockGuard guard(lock_);
    ClearLastError();
    outFeatures.clear();

    if (!envelope.IsValid())
    {
        return SetLastError(u8"空间查询范围无效。 ");
    }

    GeoGpkgFeatureLayerInfo layerInfo;
    if (!GetFeatureLayerInfoNoLock(tableNameUtf8, layerInfo))
    {
        return false;
    }

    bool hasSpatialIndex = false;
    if (!HasSpatialIndexNoLock(layerInfo.tableNameUtf8, layerInfo.geometryColumnNameUtf8, hasSpatialIndex))
    {
        return false;
    }

    if (maxFeatureCount > 0)
    {
        outFeatures.reserve(std::min<std::size_t>(maxFeatureCount, GB_GpkgMaxQueryReserveCount));
    }

    if (hasSpatialIndex)
    {
        const std::string rtreeNameUtf8 = MakeRTreeName(layerInfo.tableNameUtf8, layerInfo.geometryColumnNameUtf8);
        std::ostringstream stream;
        stream << "SELECT f.* FROM " << QuoteIdentifier(layerInfo.tableNameUtf8) << " f "
            << "JOIN " << QuoteIdentifier(rtreeNameUtf8) << " r ON f." << QuoteIdentifier(layerInfo.primaryKeyColumnNameUtf8) << " = r.id "
            << "WHERE r.minx <= ? AND r.maxx >= ? AND r.miny <= ? AND r.maxy >= ?";
        const GB_Rectangle queryEnvelope = ExpandRectangleForRTreeQuery(envelope);
        GB_SqliteParameterList parameters;
        parameters.push_back(queryEnvelope.maxX);
        parameters.push_back(queryEnvelope.minX);
        parameters.push_back(queryEnvelope.maxY);
        parameters.push_back(queryEnvelope.minY);

        bool buildFeatureFailed = false;
        bool geometryEnvelopeFailed = false;
        std::string geometryEnvelopeErrorMessageUtf8;
        if (!database_.QueryEach(stream.str(), parameters, [&](const std::vector<GB_SqliteColumnInfo>& columns, const std::vector<GB_Variant>& values) -> bool
            {
                GeoGpkgFeature feature;
                if (!BuildFeatureFromValues(columns, values, layerInfo, feature))
                {
                    buildFeatureFailed = true;
                    return false;
                }

                GB_Rectangle featureEnvelope;
                bool isEmpty = false;
                std::string errorMessageUtf8;
                if (!GetGeometryEnvelopeFromGpkgGeometry(feature.geometry, featureEnvelope, isEmpty, errorMessageUtf8))
                {
                    geometryEnvelopeFailed = true;
                    geometryEnvelopeErrorMessageUtf8 = errorMessageUtf8;
                    return false;
                }

                if (!isEmpty && IsRectangleIntersects(featureEnvelope, envelope))
                {
                    outFeatures.push_back(feature);
                    if (maxFeatureCount > 0 && outFeatures.size() >= maxFeatureCount)
                    {
                        return false;
                    }
                }
                return true;
            }))
        {
            return SetLastSqliteError(u8"使用 RTree 查询要素失败");
        }
        if (buildFeatureFailed)
        {
            return SetLastError(u8"构造要素结果失败。 ");
        }
        if (geometryEnvelopeFailed)
        {
            return SetLastError(u8"计算要素几何范围失败：" + geometryEnvelopeErrorMessageUtf8);
        }
        return true;
    }

    bool buildFeatureFailed = false;
    bool geometryEnvelopeFailed = false;
    std::string geometryEnvelopeErrorMessageUtf8;
    const std::string sql = "SELECT * FROM " + QuoteIdentifier(layerInfo.tableNameUtf8);
    if (!database_.QueryEach(sql, [&](const std::vector<GB_SqliteColumnInfo>& columns, const std::vector<GB_Variant>& values) -> bool
        {
            GeoGpkgFeature feature;
            if (!BuildFeatureFromValues(columns, values, layerInfo, feature))
            {
                buildFeatureFailed = true;
                return false;
            }

            GB_Rectangle featureEnvelope;
            bool isEmpty = false;
            std::string errorMessageUtf8;
            if (!GetGeometryEnvelopeFromGpkgGeometry(feature.geometry, featureEnvelope, isEmpty, errorMessageUtf8))
            {
                geometryEnvelopeFailed = true;
                geometryEnvelopeErrorMessageUtf8 = errorMessageUtf8;
                return false;
            }
            if (isEmpty || !IsRectangleIntersects(featureEnvelope, envelope))
            {
                return true;
            }

            outFeatures.push_back(feature);
            return maxFeatureCount == 0 || outFeatures.size() < maxFeatureCount;
        }))
    {
        return SetLastSqliteError(u8"查询要素失败");
    }
    if (buildFeatureFailed)
    {
        return SetLastError(u8"构造要素结果失败。 ");
    }
    if (geometryEnvelopeFailed)
    {
        return SetLastError(u8"计算要素几何范围失败：" + geometryEnvelopeErrorMessageUtf8);
    }
    return true;
}

bool GeoGpkg::HasSpatialIndex(const std::string& tableNameUtf8, const std::string& geometryColumnNameUtf8, bool& outHasSpatialIndex) const
{
    GB_ReadLockGuard guard(lock_);
    return HasSpatialIndexNoLock(tableNameUtf8, geometryColumnNameUtf8, outHasSpatialIndex);
}

bool GeoGpkg::HasSpatialIndexNoLock(const std::string& tableNameUtf8, const std::string& geometryColumnNameUtf8, bool& outHasSpatialIndex) const
{
    ClearLastError();
    outHasSpatialIndex = false;
    GeoGpkgFeatureLayerInfo layerInfo;
    if (geometryColumnNameUtf8.empty())
    {
        if (!GetFeatureLayerInfoNoLock(tableNameUtf8, layerInfo))
        {
            return false;
        }
    }
    const std::string geometryColumnName = geometryColumnNameUtf8.empty() ? layerInfo.geometryColumnNameUtf8 : geometryColumnNameUtf8;
    const std::string rtreeNameUtf8 = MakeRTreeName(tableNameUtf8, geometryColumnName);
    if (!database_.TableExists(rtreeNameUtf8, outHasSpatialIndex, true))
    {
        return SetLastSqliteError(u8"检查空间索引失败");
    }
    return true;
}

bool GeoGpkg::CreateSpatialIndex(const std::string& tableNameUtf8, const std::string& geometryColumnNameUtf8)
{
    GB_WriteLockGuard guard(lock_);
    ClearLastError();

    GeoGpkgFeatureLayerInfo layerInfo;
    if (!GetFeatureLayerInfoNoLock(tableNameUtf8, layerInfo))
    {
        return false;
    }
    const std::string geometryColumnName = geometryColumnNameUtf8.empty() ? layerInfo.geometryColumnNameUtf8 : geometryColumnNameUtf8;
    if (geometryColumnName != layerInfo.geometryColumnNameUtf8)
    {
        return SetLastError(u8"指定几何字段不是 gpkg_geometry_columns 中登记的几何字段。 ");
    }

    const std::string rtreeNameUtf8 = MakeRTreeName(layerInfo.tableNameUtf8, geometryColumnName);
    const std::string quotedRtreeName = QuoteIdentifier(rtreeNameUtf8);
    const std::string quotedTableName = QuoteIdentifier(layerInfo.tableNameUtf8);
    const std::string quotedGeometryColumnName = QuoteIdentifier(geometryColumnName);
    const std::string quotedPrimaryKeyColumnName = QuoteIdentifier(layerInfo.primaryKeyColumnNameUtf8);
    const std::string triggerInsertName = QuoteIdentifier(MakeTriggerName(layerInfo.tableNameUtf8, geometryColumnName, "insert"));
    const std::string triggerUpdate1Name = QuoteIdentifier(MakeTriggerName(layerInfo.tableNameUtf8, geometryColumnName, "update1"));
    const std::string triggerUpdate2Name = QuoteIdentifier(MakeTriggerName(layerInfo.tableNameUtf8, geometryColumnName, "update2"));
    const std::string triggerUpdate3Name = QuoteIdentifier(MakeTriggerName(layerInfo.tableNameUtf8, geometryColumnName, "update3"));
    const std::string triggerUpdate4Name = QuoteIdentifier(MakeTriggerName(layerInfo.tableNameUtf8, geometryColumnName, "update4"));
    const std::string triggerUpdate5Name = QuoteIdentifier(MakeTriggerName(layerInfo.tableNameUtf8, geometryColumnName, "update5"));
    const std::string triggerUpdate6Name = QuoteIdentifier(MakeTriggerName(layerInfo.tableNameUtf8, geometryColumnName, "update6"));
    const std::string triggerUpdate7Name = QuoteIdentifier(MakeTriggerName(layerInfo.tableNameUtf8, geometryColumnName, "update7"));
    const std::string triggerDeleteName = QuoteIdentifier(MakeTriggerName(layerInfo.tableNameUtf8, geometryColumnName, "delete"));

    std::ostringstream ddl;
    ddl << "CREATE VIRTUAL TABLE IF NOT EXISTS " << quotedRtreeName << " USING rtree(id, minx, maxx, miny, maxy);\n";
    ddl << "DROP TRIGGER IF EXISTS " << triggerInsertName << ";\n";
    ddl << "DROP TRIGGER IF EXISTS " << triggerUpdate1Name << ";\n";
    ddl << "DROP TRIGGER IF EXISTS " << triggerUpdate2Name << ";\n";
    ddl << "DROP TRIGGER IF EXISTS " << triggerUpdate3Name << ";\n";
    ddl << "DROP TRIGGER IF EXISTS " << triggerUpdate4Name << ";\n";
    ddl << "DROP TRIGGER IF EXISTS " << triggerUpdate5Name << ";\n";
    ddl << "DROP TRIGGER IF EXISTS " << triggerUpdate6Name << ";\n";
    ddl << "DROP TRIGGER IF EXISTS " << triggerUpdate7Name << ";\n";
    ddl << "DROP TRIGGER IF EXISTS " << triggerDeleteName << ";\n";

    ddl << "CREATE TRIGGER " << triggerInsertName << " AFTER INSERT ON " << quotedTableName << "\n"
        << "WHEN (NEW." << quotedGeometryColumnName << " IS NOT NULL AND NOT ST_IsEmpty(NEW." << quotedGeometryColumnName << "))\n"
        << "BEGIN\n"
        << "  INSERT OR REPLACE INTO " << quotedRtreeName << " VALUES (NEW." << quotedPrimaryKeyColumnName << ", ST_MinX(NEW." << quotedGeometryColumnName << "), ST_MaxX(NEW." << quotedGeometryColumnName << "), ST_MinY(NEW." << quotedGeometryColumnName << "), ST_MaxY(NEW." << quotedGeometryColumnName << "));\n"
        << "END;\n";
    ddl << "CREATE TRIGGER " << triggerUpdate2Name << " AFTER UPDATE OF " << quotedGeometryColumnName << " ON " << quotedTableName << "\n"
        << "WHEN OLD." << quotedPrimaryKeyColumnName << " = NEW." << quotedPrimaryKeyColumnName << " AND (NEW." << quotedGeometryColumnName << " IS NULL OR ST_IsEmpty(NEW." << quotedGeometryColumnName << "))\n"
        << "BEGIN\n"
        << "  DELETE FROM " << quotedRtreeName << " WHERE id = OLD." << quotedPrimaryKeyColumnName << ";\n"
        << "END;\n";
    ddl << "CREATE TRIGGER " << triggerUpdate4Name << " AFTER UPDATE ON " << quotedTableName << "\n"
        << "WHEN OLD." << quotedPrimaryKeyColumnName << " != NEW." << quotedPrimaryKeyColumnName << " AND (NEW." << quotedGeometryColumnName << " IS NULL OR ST_IsEmpty(NEW." << quotedGeometryColumnName << "))\n"
        << "BEGIN\n"
        << "  DELETE FROM " << quotedRtreeName << " WHERE id IN (OLD." << quotedPrimaryKeyColumnName << ", NEW." << quotedPrimaryKeyColumnName << ");\n"
        << "END;\n";
    ddl << "CREATE TRIGGER " << triggerUpdate5Name << " AFTER UPDATE ON " << quotedTableName << "\n"
        << "WHEN OLD." << quotedPrimaryKeyColumnName << " != NEW." << quotedPrimaryKeyColumnName << " AND (NEW." << quotedGeometryColumnName << " IS NOT NULL AND NOT ST_IsEmpty(NEW." << quotedGeometryColumnName << "))\n"
        << "BEGIN\n"
        << "  DELETE FROM " << quotedRtreeName << " WHERE id = OLD." << quotedPrimaryKeyColumnName << ";\n"
        << "  INSERT OR REPLACE INTO " << quotedRtreeName << " VALUES (NEW." << quotedPrimaryKeyColumnName << ", ST_MinX(NEW." << quotedGeometryColumnName << "), ST_MaxX(NEW." << quotedGeometryColumnName << "), ST_MinY(NEW." << quotedGeometryColumnName << "), ST_MaxY(NEW." << quotedGeometryColumnName << "));\n"
        << "END;\n";
    ddl << "CREATE TRIGGER " << triggerUpdate6Name << " AFTER UPDATE OF " << quotedGeometryColumnName << " ON " << quotedTableName << "\n"
        << "WHEN OLD." << quotedPrimaryKeyColumnName << " = NEW." << quotedPrimaryKeyColumnName << " AND (OLD." << quotedGeometryColumnName << " IS NOT NULL AND NOT ST_IsEmpty(OLD." << quotedGeometryColumnName << ")) AND (NEW." << quotedGeometryColumnName << " IS NOT NULL AND NOT ST_IsEmpty(NEW." << quotedGeometryColumnName << "))\n"
        << "BEGIN\n"
        << "  UPDATE " << quotedRtreeName << " SET minx = ST_MinX(NEW." << quotedGeometryColumnName << "), maxx = ST_MaxX(NEW." << quotedGeometryColumnName << "), miny = ST_MinY(NEW." << quotedGeometryColumnName << "), maxy = ST_MaxY(NEW." << quotedGeometryColumnName << ") WHERE id = NEW." << quotedPrimaryKeyColumnName << ";\n"
        << "END;\n";
    ddl << "CREATE TRIGGER " << triggerUpdate7Name << " AFTER UPDATE OF " << quotedGeometryColumnName << " ON " << quotedTableName << "\n"
        << "WHEN OLD." << quotedPrimaryKeyColumnName << " = NEW." << quotedPrimaryKeyColumnName << " AND (OLD." << quotedGeometryColumnName << " IS NULL OR ST_IsEmpty(OLD." << quotedGeometryColumnName << ")) AND (NEW." << quotedGeometryColumnName << " IS NOT NULL AND NOT ST_IsEmpty(NEW." << quotedGeometryColumnName << "))\n"
        << "BEGIN\n"
        << "  INSERT OR REPLACE INTO " << quotedRtreeName << " VALUES (NEW." << quotedPrimaryKeyColumnName << ", ST_MinX(NEW." << quotedGeometryColumnName << "), ST_MaxX(NEW." << quotedGeometryColumnName << "), ST_MinY(NEW." << quotedGeometryColumnName << "), ST_MaxY(NEW." << quotedGeometryColumnName << "));\n"
        << "END;\n";
    ddl << "CREATE TRIGGER " << triggerDeleteName << " AFTER DELETE ON " << quotedTableName << "\n"
        << "WHEN OLD." << quotedGeometryColumnName << " IS NOT NULL\n"
        << "BEGIN\n"
        << "  DELETE FROM " << quotedRtreeName << " WHERE id = OLD." << quotedPrimaryKeyColumnName << ";\n"
        << "END;\n";

    if (!database_.ExecuteBatch(ddl.str()))
    {
        return SetLastSqliteError(u8"创建 RTree 空间索引结构失败");
    }

    GB_SqliteParameterList extensionParameters;
    extensionParameters.push_back(layerInfo.tableNameUtf8);
    extensionParameters.push_back(geometryColumnName);
    extensionParameters.push_back("gpkg_rtree_index");
    extensionParameters.push_back("http://www.geopackage.org/spec/#extension_rtree");
    extensionParameters.push_back("write-only");
    if (!database_.Execute("INSERT OR REPLACE INTO gpkg_extensions(table_name, column_name, extension_name, definition, scope) VALUES(?, ?, ?, ?, ?)", extensionParameters))
    {
        return SetLastSqliteError(u8"写入 gpkg_extensions 空间索引记录失败");
    }

    return RebuildSpatialIndexFromGeometryHeaderNoLock(layerInfo.tableNameUtf8, geometryColumnName);
}

bool GeoGpkg::DropSpatialIndex(const std::string& tableNameUtf8, const std::string& geometryColumnNameUtf8)
{
    GB_WriteLockGuard guard(lock_);
    return DropSpatialIndexNoLock(tableNameUtf8, geometryColumnNameUtf8);
}

bool GeoGpkg::DropSpatialIndexNoLock(const std::string& tableNameUtf8, const std::string& geometryColumnNameUtf8)
{
    ClearLastError();

    GeoGpkgFeatureLayerInfo layerInfo;
    if (!GetFeatureLayerInfoNoLock(tableNameUtf8, layerInfo))
    {
        return false;
    }
    const std::string geometryColumnName = geometryColumnNameUtf8.empty() ? layerInfo.geometryColumnNameUtf8 : geometryColumnNameUtf8;
    const std::string rtreeNameUtf8 = MakeRTreeName(layerInfo.tableNameUtf8, geometryColumnName);

    std::ostringstream ddl;
    ddl << "DROP TRIGGER IF EXISTS " << QuoteIdentifier(MakeTriggerName(layerInfo.tableNameUtf8, geometryColumnName, "insert")) << ";\n";
    ddl << "DROP TRIGGER IF EXISTS " << QuoteIdentifier(MakeTriggerName(layerInfo.tableNameUtf8, geometryColumnName, "update1")) << ";\n";
    ddl << "DROP TRIGGER IF EXISTS " << QuoteIdentifier(MakeTriggerName(layerInfo.tableNameUtf8, geometryColumnName, "update2")) << ";\n";
    ddl << "DROP TRIGGER IF EXISTS " << QuoteIdentifier(MakeTriggerName(layerInfo.tableNameUtf8, geometryColumnName, "update3")) << ";\n";
    ddl << "DROP TRIGGER IF EXISTS " << QuoteIdentifier(MakeTriggerName(layerInfo.tableNameUtf8, geometryColumnName, "update4")) << ";\n";
    ddl << "DROP TRIGGER IF EXISTS " << QuoteIdentifier(MakeTriggerName(layerInfo.tableNameUtf8, geometryColumnName, "update5")) << ";\n";
    ddl << "DROP TRIGGER IF EXISTS " << QuoteIdentifier(MakeTriggerName(layerInfo.tableNameUtf8, geometryColumnName, "update6")) << ";\n";
    ddl << "DROP TRIGGER IF EXISTS " << QuoteIdentifier(MakeTriggerName(layerInfo.tableNameUtf8, geometryColumnName, "update7")) << ";\n";
    ddl << "DROP TRIGGER IF EXISTS " << QuoteIdentifier(MakeTriggerName(layerInfo.tableNameUtf8, geometryColumnName, "delete")) << ";\n";
    ddl << "DROP TABLE IF EXISTS " << QuoteIdentifier(rtreeNameUtf8) << ";\n";

    if (!database_.ExecuteBatch(ddl.str()))
    {
        return SetLastSqliteError(u8"删除空间索引结构失败");
    }

    GB_SqliteParameterList parameters;
    parameters.push_back(layerInfo.tableNameUtf8);
    parameters.push_back(geometryColumnName);
    parameters.push_back("gpkg_rtree_index");
    if (!database_.Execute("DELETE FROM gpkg_extensions WHERE table_name = ? AND column_name = ? AND extension_name = ?", parameters))
    {
        return SetLastSqliteError(u8"删除 gpkg_extensions 空间索引记录失败");
    }
    return true;
}

bool GeoGpkg::RebuildSpatialIndexFromGeometryHeader(const std::string& tableNameUtf8, const std::string& geometryColumnNameUtf8)
{
    GB_WriteLockGuard guard(lock_);
    return RebuildSpatialIndexFromGeometryHeaderNoLock(tableNameUtf8, geometryColumnNameUtf8);
}

bool GeoGpkg::RebuildSpatialIndexFromGeometryHeaderNoLock(const std::string& tableNameUtf8, const std::string& geometryColumnNameUtf8)
{
    ClearLastError();

    GeoGpkgFeatureLayerInfo layerInfo;
    if (!GetFeatureLayerInfoNoLock(tableNameUtf8, layerInfo))
    {
        return false;
    }
    const std::string geometryColumnName = geometryColumnNameUtf8.empty() ? layerInfo.geometryColumnNameUtf8 : geometryColumnNameUtf8;
    if (geometryColumnName != layerInfo.geometryColumnNameUtf8)
    {
        return SetLastError(u8"指定几何字段不是 gpkg_geometry_columns 中登记的几何字段。 ");
    }

    const std::string rtreeNameUtf8 = MakeRTreeName(layerInfo.tableNameUtf8, geometryColumnName);
    bool hasSpatialIndex = false;
    if (!database_.TableExists(rtreeNameUtf8, hasSpatialIndex, true))
    {
        return SetLastSqliteError(u8"检查 RTree 表失败");
    }
    if (!hasSpatialIndex)
    {
        return SetLastError(u8"RTree 空间索引表不存在：" + rtreeNameUtf8);
    }

    const std::string quotedRtreeName = QuoteIdentifier(rtreeNameUtf8);
    const std::string quotedTableName = QuoteIdentifier(layerInfo.tableNameUtf8);
    const std::string quotedPrimaryKeyColumnName = QuoteIdentifier(layerInfo.primaryKeyColumnNameUtf8);
    const std::string quotedGeometryColumnName = QuoteIdentifier(geometryColumnName);
    const std::string clearSql = "DELETE FROM " + quotedRtreeName;
    const std::string insertSql = "INSERT OR REPLACE INTO " + quotedRtreeName + "(id, minx, maxx, miny, maxy) SELECT " + quotedPrimaryKeyColumnName + ", ST_MinX(" + quotedGeometryColumnName + "), ST_MaxX(" + quotedGeometryColumnName + "), ST_MinY(" + quotedGeometryColumnName + "), ST_MaxY(" + quotedGeometryColumnName + ") FROM " + quotedTableName + " WHERE " + quotedGeometryColumnName + " IS NOT NULL AND NOT ST_IsEmpty(" + quotedGeometryColumnName + ")";

    if (!database_.ExecuteInTransaction([&](GB_SqliteTransaction& transaction) -> bool
        {
            if (!transaction.Execute(clearSql))
            {
                return false;
            }
            return transaction.Execute(insertSql);
        }, GB_SqliteTransactionMode::Immediate))
    {
        return SetLastSqliteError(u8"重建 RTree 空间索引失败");
    }

    return true;
}

bool GeoGpkg::CreateTileMatrixSet(const GeoGpkgTileMatrixSetInfo& tileMatrixSetInfo, const std::string& identifierUtf8, const std::string& descriptionUtf8, bool overwrite)
{
    return CreateTileMatrixSet(tileMatrixSetInfo.tableNameUtf8, tileMatrixSetInfo.srsId, tileMatrixSetInfo.envelope, identifierUtf8, descriptionUtf8, overwrite);
}

bool GeoGpkg::CreateTileMatrixSet(const std::string& tableNameUtf8, int srsId, const GB_Rectangle& envelope, const std::string& identifierUtf8, const std::string& descriptionUtf8, bool overwrite)
{
    GB_WriteLockGuard guard(lock_);
    ClearLastError();

    if (!IsValidIdentifier(tableNameUtf8))
    {
        return SetLastError(u8"瓦片矩阵集表名非法。 ");
    }
    if (!envelope.IsValid() || GetRectangleWidth(envelope) <= 0.0 || GetRectangleHeight(envelope) <= 0.0)
    {
        return SetLastError(u8"瓦片矩阵集范围无效。 ");
    }

    GB_Variant srsCount;
    if (!database_.ExecuteScalar("SELECT COUNT(*) FROM gpkg_spatial_ref_sys WHERE srs_id = ?", GB_SqliteParameterList(1, srsId), srsCount))
    {
        return SetLastSqliteError(u8"检查瓦片矩阵集空间参考失败");
    }
    bool ok = false;
    if (srsCount.ToInt64(&ok) <= 0 || !ok)
    {
        return SetLastError(u8"SRID 必须先存在于 gpkg_spatial_ref_sys：" + ToString(srsId));
    }

    const std::string realIdentifier = identifierUtf8.empty() ? tableNameUtf8 : identifierUtf8;
    const std::string createSql = "CREATE TABLE " + QuoteIdentifier(tableNameUtf8) + " (id INTEGER PRIMARY KEY AUTOINCREMENT, zoom_level INTEGER NOT NULL, tile_column INTEGER NOT NULL, tile_row INTEGER NOT NULL, tile_data BLOB NOT NULL, UNIQUE (zoom_level, tile_column, tile_row))";

    if (overwrite)
    {
        GeoGpkgFeatureLayerInfo oldLayerInfo;
        if (GetFeatureLayerInfoNoLock(tableNameUtf8, oldLayerInfo))
        {
            if (!DropSpatialIndexNoLock(tableNameUtf8, oldLayerInfo.geometryColumnNameUtf8))
            {
                return false;
            }
        }
        ClearLastError();
    }

    if (!database_.ExecuteInTransaction([&](GB_SqliteTransaction& transaction) -> bool
        {
            if (overwrite)
            {
                if (!transaction.Execute("DELETE FROM gpkg_extensions WHERE table_name = ?", GB_SqliteParameterList(1, tableNameUtf8)))
                {
                    return false;
                }
                if (!transaction.Execute("DELETE FROM gpkg_geometry_columns WHERE table_name = ?", GB_SqliteParameterList(1, tableNameUtf8)))
                {
                    return false;
                }
                if (!transaction.Execute("DELETE FROM gpkg_tile_matrix WHERE table_name = ?", GB_SqliteParameterList(1, tableNameUtf8)))
                {
                    return false;
                }
                if (!transaction.Execute("DELETE FROM gpkg_tile_matrix_set WHERE table_name = ?", GB_SqliteParameterList(1, tableNameUtf8)))
                {
                    return false;
                }
                if (!transaction.Execute("DELETE FROM gpkg_contents WHERE table_name = ?", GB_SqliteParameterList(1, tableNameUtf8)))
                {
                    return false;
                }
                if (!transaction.Execute("DROP TABLE IF EXISTS " + QuoteIdentifier(tableNameUtf8)))
                {
                    return false;
                }
            }
            if (!transaction.Execute(createSql))
            {
                return false;
            }

            GB_SqliteParameterList contentParameters;
            contentParameters.push_back(tableNameUtf8);
            contentParameters.push_back("tiles");
            contentParameters.push_back(realIdentifier);
            contentParameters.push_back(descriptionUtf8);
            contentParameters.push_back(envelope.minX);
            contentParameters.push_back(envelope.minY);
            contentParameters.push_back(envelope.maxX);
            contentParameters.push_back(envelope.maxY);
            contentParameters.push_back(srsId);
            if (!transaction.Execute("INSERT INTO gpkg_contents(table_name, data_type, identifier, description, min_x, min_y, max_x, max_y, srs_id) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?)", contentParameters))
            {
                return false;
            }

            GB_SqliteParameterList matrixSetParameters;
            matrixSetParameters.push_back(tableNameUtf8);
            matrixSetParameters.push_back(srsId);
            matrixSetParameters.push_back(envelope.minX);
            matrixSetParameters.push_back(envelope.minY);
            matrixSetParameters.push_back(envelope.maxX);
            matrixSetParameters.push_back(envelope.maxY);
            return transaction.Execute("INSERT INTO gpkg_tile_matrix_set(table_name, srs_id, min_x, min_y, max_x, max_y) VALUES(?, ?, ?, ?, ?, ?)", matrixSetParameters);
        }, GB_SqliteTransactionMode::Immediate))
    {
        return SetLastSqliteError(u8"创建瓦片矩阵集失败");
    }
    return true;
}

bool GeoGpkg::ListTileMatrixSets(std::vector<GeoGpkgTileMatrixSetInfo>& outTileMatrixSets) const
{
    GB_ReadLockGuard guard(lock_);
    ClearLastError();
    outTileMatrixSets.clear();

    GB_SqliteResult result;
    const std::string sql =
        "SELECT t.table_name, t.srs_id, t.min_x, t.min_y, t.max_x, t.max_y "
        "FROM gpkg_tile_matrix_set t JOIN gpkg_contents c ON t.table_name = c.table_name "
        "WHERE c.data_type = 'tiles' ORDER BY t.table_name";
    if (!database_.Query(sql, result))
    {
        return SetLastSqliteError(u8"查询瓦片矩阵集列表失败");
    }

    outTileMatrixSets.reserve(result.rows.size());
    for (std::size_t rowIndex = 0; rowIndex < result.rows.size(); rowIndex++)
    {
        GeoGpkgTileMatrixSetInfo tileMatrixSetInfo;
        QueryStringColumn(result, rowIndex, "table_name", tileMatrixSetInfo.tableNameUtf8);
        QueryIntColumn(result, rowIndex, "srs_id", tileMatrixSetInfo.srsId);
        QueryDoubleColumn(result, rowIndex, "min_x", tileMatrixSetInfo.envelope.minX);
        QueryDoubleColumn(result, rowIndex, "min_y", tileMatrixSetInfo.envelope.minY);
        QueryDoubleColumn(result, rowIndex, "max_x", tileMatrixSetInfo.envelope.maxX);
        QueryDoubleColumn(result, rowIndex, "max_y", tileMatrixSetInfo.envelope.maxY);
        if (!tileMatrixSetInfo.envelope.IsValid())
        {
            tileMatrixSetInfo.envelope.Reset();
        }
        outTileMatrixSets.push_back(tileMatrixSetInfo);
    }
    return true;
}

bool GeoGpkg::GetTileMatrixSetInfo(const std::string& tableNameUtf8, GeoGpkgTileMatrixSetInfo& outTileMatrixSetInfo) const
{
    GB_ReadLockGuard guard(lock_);
    ClearLastError();
    outTileMatrixSetInfo = GeoGpkgTileMatrixSetInfo();

    GB_SqliteResult result;
    if (!database_.Query("SELECT table_name, srs_id, min_x, min_y, max_x, max_y FROM gpkg_tile_matrix_set WHERE table_name = ?", GB_SqliteParameterList(1, tableNameUtf8), result, 1))
    {
        return SetLastSqliteError(u8"查询瓦片矩阵集失败");
    }
    if (result.rows.empty())
    {
        return SetLastError(u8"瓦片矩阵集不存在：" + tableNameUtf8);
    }

    QueryStringColumn(result, 0, "table_name", outTileMatrixSetInfo.tableNameUtf8);
    QueryIntColumn(result, 0, "srs_id", outTileMatrixSetInfo.srsId);
    QueryDoubleColumn(result, 0, "min_x", outTileMatrixSetInfo.envelope.minX);
    QueryDoubleColumn(result, 0, "min_y", outTileMatrixSetInfo.envelope.minY);
    QueryDoubleColumn(result, 0, "max_x", outTileMatrixSetInfo.envelope.maxX);
    QueryDoubleColumn(result, 0, "max_y", outTileMatrixSetInfo.envelope.maxY);
    if (!outTileMatrixSetInfo.envelope.IsValid())
    {
        outTileMatrixSetInfo.envelope.Reset();
    }
    return true;
}

bool GeoGpkg::AddOrUpdateTileMatrix(const GeoGpkgTileMatrixInfo& tileMatrixInfo)
{
    GB_WriteLockGuard guard(lock_);
    ClearLastError();

    if (tileMatrixInfo.tableNameUtf8.empty() || tileMatrixInfo.zoomLevel < 0 || tileMatrixInfo.matrixWidth < 1 || tileMatrixInfo.matrixHeight < 1 || tileMatrixInfo.tileWidth < 1 || tileMatrixInfo.tileHeight < 1 || tileMatrixInfo.pixelXSize <= 0.0 || tileMatrixInfo.pixelYSize <= 0.0)
    {
        return SetLastError(u8"瓦片矩阵参数无效。 ");
    }

    GB_SqliteResult matrixSetResult;
    if (!database_.Query("SELECT min_x, min_y, max_x, max_y FROM gpkg_tile_matrix_set WHERE table_name = ?", GB_SqliteParameterList(1, tileMatrixInfo.tableNameUtf8), matrixSetResult, 1))
    {
        return SetLastSqliteError(u8"查询瓦片矩阵集失败");
    }
    if (matrixSetResult.rows.empty())
    {
        return SetLastError(u8"瓦片矩阵必须先存在对应的瓦片矩阵集：" + tileMatrixInfo.tableNameUtf8);
    }

    GB_Rectangle envelope;
    QueryDoubleColumn(matrixSetResult, 0, "min_x", envelope.minX);
    QueryDoubleColumn(matrixSetResult, 0, "min_y", envelope.minY);
    QueryDoubleColumn(matrixSetResult, 0, "max_x", envelope.maxX);
    QueryDoubleColumn(matrixSetResult, 0, "max_y", envelope.maxY);
    if (!IsTileMatrixRangeCompatible(envelope, tileMatrixInfo))
    {
        return SetLastError(u8"瓦片矩阵尺寸、瓦片像素大小与瓦片矩阵集范围不匹配。 ");
    }

    GB_SqliteParameterList parameters;
    parameters.push_back(tileMatrixInfo.tableNameUtf8);
    parameters.push_back(tileMatrixInfo.zoomLevel);
    parameters.push_back(tileMatrixInfo.matrixWidth);
    parameters.push_back(tileMatrixInfo.matrixHeight);
    parameters.push_back(tileMatrixInfo.tileWidth);
    parameters.push_back(tileMatrixInfo.tileHeight);
    parameters.push_back(tileMatrixInfo.pixelXSize);
    parameters.push_back(tileMatrixInfo.pixelYSize);

    const std::string sql = "INSERT OR REPLACE INTO gpkg_tile_matrix(table_name, zoom_level, matrix_width, matrix_height, tile_width, tile_height, pixel_x_size, pixel_y_size) VALUES(?, ?, ?, ?, ?, ?, ?, ?)";
    if (!database_.Execute(sql, parameters))
    {
        return SetLastSqliteError(u8"写入瓦片矩阵失败");
    }
    return true;
}

bool GeoGpkg::ListTileMatrices(const std::string& tableNameUtf8, std::vector<GeoGpkgTileMatrixInfo>& outTileMatrices) const
{
    GB_ReadLockGuard guard(lock_);
    ClearLastError();
    outTileMatrices.clear();

    GB_SqliteResult result;
    if (!database_.Query("SELECT table_name, zoom_level, matrix_width, matrix_height, tile_width, tile_height, pixel_x_size, pixel_y_size FROM gpkg_tile_matrix WHERE table_name = ? ORDER BY zoom_level", GB_SqliteParameterList(1, tableNameUtf8), result))
    {
        return SetLastSqliteError(u8"查询瓦片矩阵失败");
    }

    outTileMatrices.reserve(result.rows.size());
    for (std::size_t rowIndex = 0; rowIndex < result.rows.size(); rowIndex++)
    {
        GeoGpkgTileMatrixInfo tileMatrixInfo;
        QueryStringColumn(result, rowIndex, "table_name", tileMatrixInfo.tableNameUtf8);
        QueryIntColumn(result, rowIndex, "zoom_level", tileMatrixInfo.zoomLevel);
        QueryIntColumn(result, rowIndex, "matrix_width", tileMatrixInfo.matrixWidth);
        QueryIntColumn(result, rowIndex, "matrix_height", tileMatrixInfo.matrixHeight);
        QueryIntColumn(result, rowIndex, "tile_width", tileMatrixInfo.tileWidth);
        QueryIntColumn(result, rowIndex, "tile_height", tileMatrixInfo.tileHeight);
        QueryDoubleColumn(result, rowIndex, "pixel_x_size", tileMatrixInfo.pixelXSize);
        QueryDoubleColumn(result, rowIndex, "pixel_y_size", tileMatrixInfo.pixelYSize);
        outTileMatrices.push_back(tileMatrixInfo);
    }
    return true;
}

bool GeoGpkg::PutTile(const std::string& tableNameUtf8, int zoomLevel, int tileColumn, int tileRow, const GB_ByteBuffer& tileData)
{
    GB_WriteLockGuard guard(lock_);
    ClearLastError();

    if (!IsValidIdentifier(tableNameUtf8) || zoomLevel < 0 || tileColumn < 0 || tileRow < 0 || tileData.empty())
    {
        return SetLastError(u8"瓦片参数无效。 ");
    }

    GB_SqliteParameterList matrixParameters;
    matrixParameters.push_back(tableNameUtf8);
    matrixParameters.push_back(zoomLevel);
    GB_SqliteResult matrixResult;
    if (!database_.Query("SELECT matrix_width, matrix_height FROM gpkg_tile_matrix WHERE table_name = ? AND zoom_level = ?", matrixParameters, matrixResult, 1))
    {
        return SetLastSqliteError(u8"查询瓦片矩阵失败");
    }
    if (matrixResult.rows.empty())
    {
        return SetLastError(u8"写入瓦片前必须先写入对应的瓦片矩阵定义。 ");
    }

    int matrixWidth = 0;
    int matrixHeight = 0;
    QueryIntColumn(matrixResult, 0, "matrix_width", matrixWidth);
    QueryIntColumn(matrixResult, 0, "matrix_height", matrixHeight);
    if (tileColumn >= matrixWidth || tileRow >= matrixHeight)
    {
        return SetLastError(u8"瓦片行列号超出瓦片矩阵范围。 ");
    }

    GB_SqliteParameterList parameters;
    parameters.push_back(zoomLevel);
    parameters.push_back(tileColumn);
    parameters.push_back(tileRow);
    parameters.push_back(tileData);
    const std::string sql = "INSERT OR REPLACE INTO " + QuoteIdentifier(tableNameUtf8) + "(zoom_level, tile_column, tile_row, tile_data) VALUES(?, ?, ?, ?)";
    if (!database_.Execute(sql, parameters))
    {
        return SetLastSqliteError(u8"写入瓦片失败");
    }
    return true;
}

bool GeoGpkg::GetTile(const std::string& tableNameUtf8, int zoomLevel, int tileColumn, int tileRow, GB_ByteBuffer& outTileData, bool& outExists) const
{
    GB_ReadLockGuard guard(lock_);
    ClearLastError();
    outTileData.clear();
    outExists = false;

    if (!IsValidIdentifier(tableNameUtf8) || zoomLevel < 0 || tileColumn < 0 || tileRow < 0)
    {
        return SetLastError(u8"瓦片参数无效。 ");
    }

    GB_SqliteParameterList parameters;
    parameters.push_back(zoomLevel);
    parameters.push_back(tileColumn);
    parameters.push_back(tileRow);
    const std::string sql = "SELECT tile_data FROM " + QuoteIdentifier(tableNameUtf8) + " WHERE zoom_level = ? AND tile_column = ? AND tile_row = ?";
    GB_SqliteResult result;
    if (!database_.Query(sql, parameters, result, 1))
    {
        return SetLastSqliteError(u8"读取瓦片失败");
    }
    if (result.rows.empty())
    {
        return true;
    }

    bool ok = false;
    outTileData = result.GetValue(0, "tile_data").ToBinary(&ok);
    outExists = ok;
    return ok;
}

bool GeoGpkg::GetTiles(const std::string& tableNameUtf8, int zoomLevel, std::vector<GeoGpkgTile>& outTiles, std::size_t maxTileCount) const
{
    GB_ReadLockGuard guard(lock_);
    ClearLastError();
    outTiles.clear();

    if (!IsValidIdentifier(tableNameUtf8) || zoomLevel < 0)
    {
        return SetLastError(u8"瓦片参数无效。 ");
    }

    std::ostringstream stream;
    stream << "SELECT zoom_level, tile_column, tile_row, tile_data FROM " << QuoteIdentifier(tableNameUtf8) << " WHERE zoom_level = ? ORDER BY tile_row, tile_column";
    if (maxTileCount > 0)
    {
        stream << " LIMIT " << maxTileCount;
    }

    GB_SqliteResult result;
    if (!database_.Query(stream.str(), GB_SqliteParameterList(1, zoomLevel), result))
    {
        return SetLastSqliteError(u8"读取瓦片列表失败");
    }

    outTiles.reserve(result.rows.size());
    for (std::size_t rowIndex = 0; rowIndex < result.rows.size(); rowIndex++)
    {
        GeoGpkgTile tile;
        QueryIntColumn(result, rowIndex, "zoom_level", tile.zoomLevel);
        QueryIntColumn(result, rowIndex, "tile_column", tile.tileColumn);
        QueryIntColumn(result, rowIndex, "tile_row", tile.tileRow);
        bool ok = false;
        tile.tileData = result.GetValue(rowIndex, "tile_data").ToBinary(&ok);
        if (!ok)
        {
            return SetLastError(u8"读取瓦片 BLOB 失败。 ");
        }
        outTiles.push_back(tile);
    }
    return true;
}

bool GeoGpkg::DropContentTable(const std::string& tableNameUtf8)
{
    GB_WriteLockGuard guard(lock_);
    ClearLastError();

    if (!IsValidIdentifier(tableNameUtf8))
    {
        return SetLastError(u8"GeoPackage 内容表名非法。 ");
    }

    GeoGpkgFeatureLayerInfo layerInfo;
    const bool isFeatureLayer = GetFeatureLayerInfoNoLock(tableNameUtf8, layerInfo);
    if (isFeatureLayer)
    {
        if (!DropSpatialIndexNoLock(tableNameUtf8, layerInfo.geometryColumnNameUtf8))
        {
            return false;
        }
        ClearLastError();
    }
    else
    {
        ClearLastError();
    }

    if (!database_.ExecuteInTransaction([&](GB_SqliteTransaction& transaction) -> bool
        {
            if (!transaction.Execute("DELETE FROM gpkg_extensions WHERE table_name = ?", GB_SqliteParameterList(1, tableNameUtf8)))
            {
                return false;
            }
            if (!transaction.Execute("DELETE FROM gpkg_geometry_columns WHERE table_name = ?", GB_SqliteParameterList(1, tableNameUtf8)))
            {
                return false;
            }
            if (!transaction.Execute("DELETE FROM gpkg_tile_matrix WHERE table_name = ?", GB_SqliteParameterList(1, tableNameUtf8)))
            {
                return false;
            }
            if (!transaction.Execute("DELETE FROM gpkg_tile_matrix_set WHERE table_name = ?", GB_SqliteParameterList(1, tableNameUtf8)))
            {
                return false;
            }
            if (!transaction.Execute("DELETE FROM gpkg_contents WHERE table_name = ?", GB_SqliteParameterList(1, tableNameUtf8)))
            {
                return false;
            }
            return transaction.Execute("DROP TABLE IF EXISTS " + QuoteIdentifier(tableNameUtf8));
        }, GB_SqliteTransactionMode::Immediate))
    {
        return SetLastSqliteError(u8"删除 GeoPackage 内容表失败");
    }
    return true;
}

bool GeoGpkg::CreateGpkgGeometryFromWkb(const GB_ByteBuffer& wkb, int srsId, GB_ByteBuffer& outGpkgGeometry, const GB_Rectangle* envelope, bool empty)
{
    outGpkgGeometry.clear();
    if (wkb.empty())
    {
        return false;
    }

    std::string wkbGeometryType;
    if (!ReadWkbGeometryTypeName(wkb, wkbGeometryType))
    {
        return false;
    }

    GB_Rectangle realEnvelope;
    if (envelope != nullptr)
    {
        realEnvelope = *envelope;
        if (!empty && !realEnvelope.IsValid())
        {
            return false;
        }
    }
    else if (!empty)
    {
        if (!CalculateWkbEnvelope(wkb, realEnvelope))
        {
            return false;
        }
    }

    const bool useEnvelope = !empty && realEnvelope.IsValid();
    const int envelopeCode = useEnvelope ? 1 : 0;
    const unsigned char flags = static_cast<unsigned char>(0x01 | ((envelopeCode & 0x07) << 1) | (empty ? 0x10 : 0x00));

    outGpkgGeometry.reserve(GB_GpkgBaseHeaderSize + (useEnvelope ? 32 : 0) + wkb.size());
    outGpkgGeometry.push_back(GB_GpkgMagic0);
    outGpkgGeometry.push_back(GB_GpkgMagic1);
    outGpkgGeometry.push_back(0);
    outGpkgGeometry.push_back(flags);
    AppendInt32LittleEndian(outGpkgGeometry, static_cast<std::int32_t>(srsId));

    if (useEnvelope)
    {
        AppendDoubleLittleEndian(outGpkgGeometry, realEnvelope.minX);
        AppendDoubleLittleEndian(outGpkgGeometry, realEnvelope.maxX);
        AppendDoubleLittleEndian(outGpkgGeometry, realEnvelope.minY);
        AppendDoubleLittleEndian(outGpkgGeometry, realEnvelope.maxY);
    }

    outGpkgGeometry.insert(outGpkgGeometry.end(), wkb.begin(), wkb.end());
    return true;
}

bool GeoGpkg::ParseGeometryHeader(const GB_ByteBuffer& gpkgGeometry, GeoGpkgGeometryHeaderInfo& outHeaderInfo)
{
    outHeaderInfo = GeoGpkgGeometryHeaderInfo();
    if (gpkgGeometry.size() < GB_GpkgBaseHeaderSize)
    {
        return false;
    }
    if (gpkgGeometry[0] != GB_GpkgMagic0 || gpkgGeometry[1] != GB_GpkgMagic1)
    {
        return false;
    }

    outHeaderInfo.version = gpkgGeometry[2];
    const unsigned char flags = gpkgGeometry[3];
    if ((flags & 0xC0) != 0)
    {
        return false;
    }
    outHeaderInfo.littleEndian = (flags & 0x01) != 0;
    outHeaderInfo.envelopeCode = (flags >> 1) & 0x07;
    outHeaderInfo.empty = (flags & 0x10) != 0;
    outHeaderInfo.extended = (flags & 0x20) != 0;

    const int envelopeDoubleCount = GetEnvelopeDoubleCount(outHeaderInfo.envelopeCode);
    if (outHeaderInfo.version != 0 || envelopeDoubleCount < 0)
    {
        return false;
    }
    if (outHeaderInfo.empty && outHeaderInfo.envelopeCode != 0)
    {
        return false;
    }

    outHeaderInfo.srsId = ReadInt32(gpkgGeometry.data() + 4, outHeaderInfo.littleEndian);
    outHeaderInfo.headerSize = GB_GpkgBaseHeaderSize + static_cast<std::size_t>(envelopeDoubleCount) * sizeof(double);
    if (gpkgGeometry.size() < outHeaderInfo.headerSize)
    {
        return false;
    }

    if (outHeaderInfo.envelopeCode >= 1)
    {
        const unsigned char* envelopePtr = gpkgGeometry.data() + GB_GpkgBaseHeaderSize;
        const double minX = ReadDouble(envelopePtr + 0, outHeaderInfo.littleEndian);
        const double maxX = ReadDouble(envelopePtr + 8, outHeaderInfo.littleEndian);
        const double minY = ReadDouble(envelopePtr + 16, outHeaderInfo.littleEndian);
        const double maxY = ReadDouble(envelopePtr + 24, outHeaderInfo.littleEndian);
        if (!AssignRectangle(outHeaderInfo.envelope, minX, minY, maxX, maxY))
        {
            return false;
        }
    }

    outHeaderInfo.valid = true;
    return true;
}

bool GeoGpkg::ExtractWkbFromGpkgGeometry(const GB_ByteBuffer& gpkgGeometry, GB_ByteBuffer& outWkb)
{
    outWkb.clear();
    GeoGpkgGeometryHeaderInfo headerInfo;
    if (!ParseGeometryHeader(gpkgGeometry, headerInfo))
    {
        return false;
    }
    if (gpkgGeometry.size() <= headerInfo.headerSize)
    {
        return false;
    }
    outWkb.assign(gpkgGeometry.begin() + static_cast<std::ptrdiff_t>(headerInfo.headerSize), gpkgGeometry.end());
    return true;
}

bool GeoGpkg::CalculateWkbEnvelope(const GB_ByteBuffer& wkb, GB_Rectangle& outEnvelope)
{
    outEnvelope.Reset();
    if (wkb.empty())
    {
        return false;
    }

    WkbReader reader(wkb.data(), wkb.size());
    if (!ReadWkbGeometryEnvelope(reader, outEnvelope, 0))
    {
        outEnvelope.Reset();
        return false;
    }
    return outEnvelope.IsValid();
}

bool GeoGpkg::SetLastError(const std::string& messageUtf8) const
{
    lastErrorUtf8_ = messageUtf8;
    return false;
}

bool GeoGpkg::SetLastSqliteError(const std::string& prefixUtf8) const
{
    const GB_SqliteError sqliteError = database_.GetLastError();
    std::string message = prefixUtf8;
    if (!sqliteError.messageUtf8.empty())
    {
        message += "：";
        message += sqliteError.messageUtf8;
    }
    if (!sqliteError.sqlUtf8.empty())
    {
        message += " SQL=";
        message += sqliteError.sqlUtf8;
    }
    lastErrorUtf8_ = message;
    return false;
}

void GeoGpkg::ClearLastError() const
{
    lastErrorUtf8_.clear();
}
