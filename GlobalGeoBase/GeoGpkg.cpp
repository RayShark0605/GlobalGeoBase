#include "GeoGpkg.h"
#include "GB_FileSystem.h"

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
    const double GB_GpkgRTreeQueryExpansionScale = 1.2e-7;
    const double GB_GpkgAngleEpsilon = 1.0e-12;


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

        for (std::size_t index = 0; index < identifierUtf8.size(); index++)
        {
            const unsigned char ch = static_cast<unsigned char>(identifierUtf8[index]);
            if (ch == 0 || ch < 0x20 || ch == 0x7f)
            {
                return false;
            }
        }
        return true;
    }

    static bool StartsWithAsciiNoCase(const std::string& textUtf8, const std::string& prefixUtf8)
    {
        if (textUtf8.size() < prefixUtf8.size())
        {
            return false;
        }

        for (std::size_t index = 0; index < prefixUtf8.size(); index++)
        {
            if (ToLowerAscii(textUtf8[index]) != ToLowerAscii(prefixUtf8[index]))
            {
                return false;
            }
        }
        return true;
    }

    static bool IsReservedGeoPackageObjectName(const std::string& objectNameUtf8)
    {
        return StartsWithAsciiNoCase(objectNameUtf8, "sqlite_") || StartsWithAsciiNoCase(objectNameUtf8, "gpkg_") || StartsWithAsciiNoCase(objectNameUtf8, "rtree_");
    }

    static bool IsValidUserContentTableName(const std::string& tableNameUtf8)
    {
        return IsValidIdentifier(tableNameUtf8) && !IsReservedGeoPackageObjectName(tableNameUtf8);
    }

    static bool IsValidSqlDefaultExpression(const std::string& sqlExpressionUtf8)
    {
        bool inSingleQuote = false;
        bool inDoubleQuote = false;
        bool inBacktickQuote = false;
        bool inBracketQuote = false;

        for (std::size_t index = 0; index < sqlExpressionUtf8.size(); index++)
        {
            const char ch = sqlExpressionUtf8[index];
            if (ch == '\0')
            {
                return false;
            }

            if (inSingleQuote)
            {
                if (ch == '\'')
                {
                    if (index + 1 < sqlExpressionUtf8.size() && sqlExpressionUtf8[index + 1] == '\'')
                    {
                        index++;
                    }
                    else
                    {
                        inSingleQuote = false;
                    }
                }
                continue;
            }
            if (inDoubleQuote)
            {
                if (ch == '"')
                {
                    if (index + 1 < sqlExpressionUtf8.size() && sqlExpressionUtf8[index + 1] == '"')
                    {
                        index++;
                    }
                    else
                    {
                        inDoubleQuote = false;
                    }
                }
                continue;
            }
            if (inBacktickQuote)
            {
                if (ch == '`')
                {
                    inBacktickQuote = false;
                }
                continue;
            }
            if (inBracketQuote)
            {
                if (ch == ']')
                {
                    inBracketQuote = false;
                }
                continue;
            }

            if (ch == '\'')
            {
                inSingleQuote = true;
                continue;
            }
            if (ch == '"')
            {
                inDoubleQuote = true;
                continue;
            }
            if (ch == '`')
            {
                inBacktickQuote = true;
                continue;
            }
            if (ch == '[')
            {
                inBracketQuote = true;
                continue;
            }
            if (ch == ';')
            {
                return false;
            }
            if (ch == '-' && index + 1 < sqlExpressionUtf8.size() && sqlExpressionUtf8[index + 1] == '-')
            {
                return false;
            }
            if (ch == '/' && index + 1 < sqlExpressionUtf8.size() && sqlExpressionUtf8[index + 1] == '*')
            {
                return false;
            }
        }

        return !inSingleQuote && !inDoubleQuote && !inBacktickQuote && !inBracketQuote;
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

    static std::string QuoteSqlString(const std::string& textUtf8)
    {
        std::string result;
        result.reserve(textUtf8.size() + 2);
        result.push_back('\'');
        for (std::size_t index = 0; index < textUtf8.size(); index++)
        {
            const char ch = textUtf8[index];
            if (ch == '\'')
            {
                result.push_back('\'');
            }
            result.push_back(ch);
        }
        result.push_back('\'');
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

    static std::uint64_t CalculateFnv1a64Hash(const std::string& textUtf8)
    {
        std::uint64_t hashValue = 14695981039346656037ULL;
        for (std::size_t index = 0; index < textUtf8.size(); index++)
        {
            hashValue ^= static_cast<unsigned char>(textUtf8[index]);
            hashValue *= 1099511628211ULL;
        }
        return hashValue;
    }

    static std::string ToFixedHexString(std::uint64_t value)
    {
        static const char hexDigits[] = "0123456789abcdef";
        std::string result(16, '0');
        for (int index = 15; index >= 0; index--)
        {
            result[static_cast<std::size_t>(index)] = hexDigits[value & 0x0f];
            value >>= 4;
        }
        return result;
    }

    static std::string MakeTriggerName(const std::string& tableNameUtf8, const std::string& geometryColumnNameUtf8, const std::string& suffixUtf8)
    {
        return "rtree_" + tableNameUtf8 + "_" + geometryColumnNameUtf8 + "_" + suffixUtf8;
    }

    static std::string MakeLegacyTriggerName(const std::string& tableNameUtf8, const std::string& geometryColumnNameUtf8, const std::string& suffixUtf8)
    {
        return MakeSafeObjectName("rtree_" + tableNameUtf8 + "_" + geometryColumnNameUtf8 + "_" + suffixUtf8);
    }

    static std::string MakeHashedTriggerName(const std::string& tableNameUtf8, const std::string& geometryColumnNameUtf8, const std::string& suffixUtf8)
    {
        const std::string rawNameUtf8 = "rtree_" + tableNameUtf8 + "_" + geometryColumnNameUtf8 + "_" + suffixUtf8;
        std::string safeNameUtf8 = MakeSafeObjectName(rawNameUtf8);
        if (safeNameUtf8.size() > 96)
        {
            safeNameUtf8.resize(96);
        }
        return safeNameUtf8 + "_" + ToFixedHexString(CalculateFnv1a64Hash(rawNameUtf8));
    }

    static void AppendSpatialIndexTriggerName(std::vector<std::string>& triggerNames, std::set<std::string>& uniqueTriggerNames, const std::string& triggerNameUtf8)
    {
        if (uniqueTriggerNames.insert(triggerNameUtf8).second)
        {
            triggerNames.push_back(triggerNameUtf8);
        }
    }

    static void BuildSpatialIndexTriggerNames(const std::string& tableNameUtf8, const std::string& geometryColumnNameUtf8, std::vector<std::string>& outTriggerNames)
    {
        outTriggerNames.clear();
        static const char* triggerSuffixes[] = { "insert", "update1", "update2", "update3", "update4", "update5", "update6", "update7", "delete" };
        std::set<std::string> uniqueTriggerNames;
        for (std::size_t index = 0; index < sizeof(triggerSuffixes) / sizeof(triggerSuffixes[0]); index++)
        {
            const std::string suffixUtf8 = triggerSuffixes[index];
            AppendSpatialIndexTriggerName(outTriggerNames, uniqueTriggerNames, MakeTriggerName(tableNameUtf8, geometryColumnNameUtf8, suffixUtf8));
            AppendSpatialIndexTriggerName(outTriggerNames, uniqueTriggerNames, MakeLegacyTriggerName(tableNameUtf8, geometryColumnNameUtf8, suffixUtf8));
            AppendSpatialIndexTriggerName(outTriggerNames, uniqueTriggerNames, MakeHashedTriggerName(tableNameUtf8, geometryColumnNameUtf8, suffixUtf8));
        }
    }

    static std::string MakeTileConstraintTriggerName(const std::string& tableNameUtf8, const std::string& suffixUtf8)
    {
        return tableNameUtf8 + "_" + suffixUtf8;
    }

    static std::string MakeLegacyTileConstraintTriggerName(const std::string& tableNameUtf8, const std::string& suffixUtf8)
    {
        return MakeSafeObjectName(tableNameUtf8 + "_" + suffixUtf8);
    }

    static std::string MakeHashedTileConstraintTriggerName(const std::string& tableNameUtf8, const std::string& suffixUtf8)
    {
        const std::string rawNameUtf8 = tableNameUtf8 + "_" + suffixUtf8;
        std::string safeNameUtf8 = MakeSafeObjectName(rawNameUtf8);
        if (safeNameUtf8.size() > 96)
        {
            safeNameUtf8.resize(96);
        }
        return safeNameUtf8 + "_" + ToFixedHexString(CalculateFnv1a64Hash(rawNameUtf8));
    }

    static void AppendTileConstraintTriggerName(std::vector<std::string>& triggerNames, std::set<std::string>& uniqueTriggerNames, const std::string& triggerNameUtf8)
    {
        if (uniqueTriggerNames.insert(triggerNameUtf8).second)
        {
            triggerNames.push_back(triggerNameUtf8);
        }
    }

    static void BuildTileConstraintTriggerNames(const std::string& tableNameUtf8, std::vector<std::string>& outTriggerNames)
    {
        outTriggerNames.clear();
        static const char* triggerSuffixes[] = { "zoom_insert", "zoom_update", "tile_column_insert", "tile_column_update", "tile_row_insert", "tile_row_update" };
        std::set<std::string> uniqueTriggerNames;
        for (std::size_t index = 0; index < sizeof(triggerSuffixes) / sizeof(triggerSuffixes[0]); index++)
        {
            const std::string suffixUtf8 = triggerSuffixes[index];
            AppendTileConstraintTriggerName(outTriggerNames, uniqueTriggerNames, MakeTileConstraintTriggerName(tableNameUtf8, suffixUtf8));
            AppendTileConstraintTriggerName(outTriggerNames, uniqueTriggerNames, MakeLegacyTileConstraintTriggerName(tableNameUtf8, suffixUtf8));
            AppendTileConstraintTriggerName(outTriggerNames, uniqueTriggerNames, MakeHashedTileConstraintTriggerName(tableNameUtf8, suffixUtf8));
        }
    }

    static void BuildTileConstraintTriggerSqlList(const std::string& tableNameUtf8, std::vector<std::string>& outSqlList)
    {
        outSqlList.clear();
        const std::string quotedTableName = QuoteIdentifier(tableNameUtf8);
        const std::string quotedTableLiteral = QuoteSqlString(tableNameUtf8);

        std::ostringstream zoomInsertTrigger;
        zoomInsertTrigger << "CREATE TRIGGER " << QuoteIdentifier(MakeTileConstraintTriggerName(tableNameUtf8, "zoom_insert")) << " BEFORE INSERT ON " << quotedTableName << "\n"
            << "BEGIN\n"
            << "  SELECT RAISE(ABORT, 'insert violates GeoPackage tile constraint: zoom_level is not registered in gpkg_tile_matrix') WHERE NOT (NEW.zoom_level IN (SELECT zoom_level FROM gpkg_tile_matrix WHERE table_name = " << quotedTableLiteral << "));\n"
            << "  SELECT RAISE(ABORT, 'insert violates GeoPackage tile constraint: tile_column must be less than matrix_width') WHERE NOT (NEW.tile_column < (SELECT matrix_width FROM gpkg_tile_matrix WHERE table_name = " << quotedTableLiteral << " AND zoom_level = NEW.zoom_level));\n"
            << "  SELECT RAISE(ABORT, 'insert violates GeoPackage tile constraint: tile_row must be less than matrix_height') WHERE NOT (NEW.tile_row < (SELECT matrix_height FROM gpkg_tile_matrix WHERE table_name = " << quotedTableLiteral << " AND zoom_level = NEW.zoom_level));\n"
            << "END";
        outSqlList.push_back(zoomInsertTrigger.str());

        std::ostringstream zoomUpdateTrigger;
        zoomUpdateTrigger << "CREATE TRIGGER " << QuoteIdentifier(MakeTileConstraintTriggerName(tableNameUtf8, "zoom_update")) << " BEFORE UPDATE OF zoom_level ON " << quotedTableName << "\n"
            << "BEGIN\n"
            << "  SELECT RAISE(ABORT, 'update violates GeoPackage tile constraint: zoom_level is not registered in gpkg_tile_matrix') WHERE NOT (NEW.zoom_level IN (SELECT zoom_level FROM gpkg_tile_matrix WHERE table_name = " << quotedTableLiteral << "));\n"
            << "  SELECT RAISE(ABORT, 'update violates GeoPackage tile constraint: tile_column must be less than matrix_width') WHERE NOT (NEW.tile_column < (SELECT matrix_width FROM gpkg_tile_matrix WHERE table_name = " << quotedTableLiteral << " AND zoom_level = NEW.zoom_level));\n"
            << "  SELECT RAISE(ABORT, 'update violates GeoPackage tile constraint: tile_row must be less than matrix_height') WHERE NOT (NEW.tile_row < (SELECT matrix_height FROM gpkg_tile_matrix WHERE table_name = " << quotedTableLiteral << " AND zoom_level = NEW.zoom_level));\n"
            << "END";
        outSqlList.push_back(zoomUpdateTrigger.str());

        std::ostringstream tileColumnInsertTrigger;
        tileColumnInsertTrigger << "CREATE TRIGGER " << QuoteIdentifier(MakeTileConstraintTriggerName(tableNameUtf8, "tile_column_insert")) << " BEFORE INSERT ON " << quotedTableName << "\n"
            << "BEGIN\n"
            << "  SELECT RAISE(ABORT, 'insert violates GeoPackage tile constraint: tile_column cannot be negative') WHERE NEW.tile_column < 0;\n"
            << "  SELECT RAISE(ABORT, 'insert violates GeoPackage tile constraint: tile_column must be less than matrix_width') WHERE NOT (NEW.tile_column < (SELECT matrix_width FROM gpkg_tile_matrix WHERE table_name = " << quotedTableLiteral << " AND zoom_level = NEW.zoom_level));\n"
            << "END";
        outSqlList.push_back(tileColumnInsertTrigger.str());

        std::ostringstream tileColumnUpdateTrigger;
        tileColumnUpdateTrigger << "CREATE TRIGGER " << QuoteIdentifier(MakeTileConstraintTriggerName(tableNameUtf8, "tile_column_update")) << " BEFORE UPDATE OF tile_column ON " << quotedTableName << "\n"
            << "BEGIN\n"
            << "  SELECT RAISE(ABORT, 'update violates GeoPackage tile constraint: tile_column cannot be negative') WHERE NEW.tile_column < 0;\n"
            << "  SELECT RAISE(ABORT, 'update violates GeoPackage tile constraint: tile_column must be less than matrix_width') WHERE NOT (NEW.tile_column < (SELECT matrix_width FROM gpkg_tile_matrix WHERE table_name = " << quotedTableLiteral << " AND zoom_level = NEW.zoom_level));\n"
            << "END";
        outSqlList.push_back(tileColumnUpdateTrigger.str());

        std::ostringstream tileRowInsertTrigger;
        tileRowInsertTrigger << "CREATE TRIGGER " << QuoteIdentifier(MakeTileConstraintTriggerName(tableNameUtf8, "tile_row_insert")) << " BEFORE INSERT ON " << quotedTableName << "\n"
            << "BEGIN\n"
            << "  SELECT RAISE(ABORT, 'insert violates GeoPackage tile constraint: tile_row cannot be negative') WHERE NEW.tile_row < 0;\n"
            << "  SELECT RAISE(ABORT, 'insert violates GeoPackage tile constraint: tile_row must be less than matrix_height') WHERE NOT (NEW.tile_row < (SELECT matrix_height FROM gpkg_tile_matrix WHERE table_name = " << quotedTableLiteral << " AND zoom_level = NEW.zoom_level));\n"
            << "END";
        outSqlList.push_back(tileRowInsertTrigger.str());

        std::ostringstream tileRowUpdateTrigger;
        tileRowUpdateTrigger << "CREATE TRIGGER " << QuoteIdentifier(MakeTileConstraintTriggerName(tableNameUtf8, "tile_row_update")) << " BEFORE UPDATE OF tile_row ON " << quotedTableName << "\n"
            << "BEGIN\n"
            << "  SELECT RAISE(ABORT, 'update violates GeoPackage tile constraint: tile_row cannot be negative') WHERE NEW.tile_row < 0;\n"
            << "  SELECT RAISE(ABORT, 'update violates GeoPackage tile constraint: tile_row must be less than matrix_height') WHERE NOT (NEW.tile_row < (SELECT matrix_height FROM gpkg_tile_matrix WHERE table_name = " << quotedTableLiteral << " AND zoom_level = NEW.zoom_level));\n"
            << "END";
        outSqlList.push_back(tileRowUpdateTrigger.str());
    }


    static void BuildCoreTileMatrixConstraintTriggerSqlList(std::vector<std::string>& outSqlList)
    {
        outSqlList.clear();
        struct TriggerRule
        {
            const char* name;
            const char* eventSql;
            const char* conditionSql;
            const char* message;
        };

        static const TriggerRule rules[] =
        {
            { "gpkg_tile_matrix_zoom_level_insert", "BEFORE INSERT ON gpkg_tile_matrix", "NEW.zoom_level < 0", "insert on gpkg_tile_matrix violates constraint: zoom_level cannot be less than 0" },
            { "gpkg_tile_matrix_zoom_level_update", "BEFORE UPDATE OF zoom_level ON gpkg_tile_matrix", "NEW.zoom_level < 0", "update on gpkg_tile_matrix violates constraint: zoom_level cannot be less than 0" },
            { "gpkg_tile_matrix_matrix_width_insert", "BEFORE INSERT ON gpkg_tile_matrix", "NEW.matrix_width < 1", "insert on gpkg_tile_matrix violates constraint: matrix_width cannot be less than 1" },
            { "gpkg_tile_matrix_matrix_width_update", "BEFORE UPDATE OF matrix_width ON gpkg_tile_matrix", "NEW.matrix_width < 1", "update on gpkg_tile_matrix violates constraint: matrix_width cannot be less than 1" },
            { "gpkg_tile_matrix_matrix_height_insert", "BEFORE INSERT ON gpkg_tile_matrix", "NEW.matrix_height < 1", "insert on gpkg_tile_matrix violates constraint: matrix_height cannot be less than 1" },
            { "gpkg_tile_matrix_matrix_height_update", "BEFORE UPDATE OF matrix_height ON gpkg_tile_matrix", "NEW.matrix_height < 1", "update on gpkg_tile_matrix violates constraint: matrix_height cannot be less than 1" },
            { "gpkg_tile_matrix_tile_width_insert", "BEFORE INSERT ON gpkg_tile_matrix", "NEW.tile_width < 1", "insert on gpkg_tile_matrix violates constraint: tile_width cannot be less than 1" },
            { "gpkg_tile_matrix_tile_width_update", "BEFORE UPDATE OF tile_width ON gpkg_tile_matrix", "NEW.tile_width < 1", "update on gpkg_tile_matrix violates constraint: tile_width cannot be less than 1" },
            { "gpkg_tile_matrix_tile_height_insert", "BEFORE INSERT ON gpkg_tile_matrix", "NEW.tile_height < 1", "insert on gpkg_tile_matrix violates constraint: tile_height cannot be less than 1" },
            { "gpkg_tile_matrix_tile_height_update", "BEFORE UPDATE OF tile_height ON gpkg_tile_matrix", "NEW.tile_height < 1", "update on gpkg_tile_matrix violates constraint: tile_height cannot be less than 1" },
            { "gpkg_tile_matrix_pixel_x_size_insert", "BEFORE INSERT ON gpkg_tile_matrix", "NOT (NEW.pixel_x_size > 0)", "insert on gpkg_tile_matrix violates constraint: pixel_x_size must be greater than 0" },
            { "gpkg_tile_matrix_pixel_x_size_update", "BEFORE UPDATE OF pixel_x_size ON gpkg_tile_matrix", "NOT (NEW.pixel_x_size > 0)", "update on gpkg_tile_matrix violates constraint: pixel_x_size must be greater than 0" },
            { "gpkg_tile_matrix_pixel_y_size_insert", "BEFORE INSERT ON gpkg_tile_matrix", "NOT (NEW.pixel_y_size > 0)", "insert on gpkg_tile_matrix violates constraint: pixel_y_size must be greater than 0" },
            { "gpkg_tile_matrix_pixel_y_size_update", "BEFORE UPDATE OF pixel_y_size ON gpkg_tile_matrix", "NOT (NEW.pixel_y_size > 0)", "update on gpkg_tile_matrix violates constraint: pixel_y_size must be greater than 0" }
        };

        for (std::size_t index = 0; index < sizeof(rules) / sizeof(rules[0]); index++)
        {
            std::ostringstream stream;
            stream << "CREATE TRIGGER IF NOT EXISTS " << QuoteIdentifier(rules[index].name) << " " << rules[index].eventSql << "\n"
                << "FOR EACH ROW BEGIN\n"
                << "  SELECT RAISE(ABORT, " << QuoteSqlString(rules[index].message) << ") WHERE " << rules[index].conditionSql << ";\n"
                << "END";
            outSqlList.push_back(stream.str());
        }
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

    static bool IsRectangleNearlyEqual(const GB_Rectangle& rect1, const GB_Rectangle& rect2)
    {
        if (!rect1.IsValid() || !rect2.IsValid())
        {
            return false;
        }

        return NearlyEqualDouble(rect1.minX, rect2.minX) && NearlyEqualDouble(rect1.minY, rect2.minY) && NearlyEqualDouble(rect1.maxX, rect2.maxX) && NearlyEqualDouble(rect1.maxY, rect2.maxY);
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
        if (!IsFinite(envelopeWidth) || !IsFinite(envelopeHeight) || !IsFinite(matrixWidth) || !IsFinite(matrixHeight))
        {
            return false;
        }

        return envelopeWidth > 0.0 && envelopeHeight > 0.0 && matrixWidth > 0.0 && matrixHeight > 0.0 && NearlyEqualDouble(envelopeWidth, matrixWidth) && NearlyEqualDouble(envelopeHeight, matrixHeight);
    }

    struct GeoGpkgTileMatrixResolution
    {
        int zoomLevel = 0;
        double pixelXSize = 0.0;
        double pixelYSize = 0.0;
    };

    static bool IsPixelSizeHalfOfPrevious(double previousPixelSize, double currentPixelSize)
    {
        if (previousPixelSize <= 0.0 || currentPixelSize <= 0.0)
        {
            return false;
        }

        return NearlyEqualDouble(previousPixelSize, currentPixelSize * 2.0);
    }

    static bool NeedsZoomOtherExtension(std::vector<GeoGpkgTileMatrixResolution> resolutions)
    {
        if (resolutions.size() < 2)
        {
            return false;
        }

        std::sort(resolutions.begin(), resolutions.end(), [](const GeoGpkgTileMatrixResolution& left, const GeoGpkgTileMatrixResolution& right) -> bool
            {
                return left.zoomLevel < right.zoomLevel;
            });

        for (std::size_t index = 1; index < resolutions.size(); index++)
        {
            const GeoGpkgTileMatrixResolution& previous = resolutions[index - 1];
            const GeoGpkgTileMatrixResolution& current = resolutions[index];
            if (current.zoomLevel != previous.zoomLevel + 1)
            {
                continue;
            }
            if (!IsPixelSizeHalfOfPrevious(previous.pixelXSize, current.pixelXSize) || !IsPixelSizeHalfOfPrevious(previous.pixelYSize, current.pixelYSize))
            {
                return true;
            }
        }
        return false;
    }

    static bool AppendOrRemoveZoomOtherExtensionSql(GB_SqliteTransaction& transaction, const std::string& tableNameUtf8, bool needZoomOtherExtension)
    {
        GB_SqliteParameterList parameters;
        parameters.push_back(tableNameUtf8);
        parameters.push_back("tile_data");
        parameters.push_back("gpkg_zoom_other");
        if (needZoomOtherExtension)
        {
            parameters.push_back("http://www.geopackage.org/spec/#extension_zoom_other_intervals");
            parameters.push_back("read-write");
            return transaction.Execute("INSERT OR REPLACE INTO gpkg_extensions(table_name, column_name, extension_name, definition, scope) VALUES(?, ?, ?, ?, ?)", parameters);
        }

        return transaction.Execute("DELETE FROM gpkg_extensions WHERE table_name = ? AND column_name = ? AND extension_name = ?", parameters);
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

    static std::string TrimAsciiSpaces(const std::string& textUtf8)
    {
        std::size_t begin = 0;
        while (begin < textUtf8.size() && (textUtf8[begin] == ' ' || textUtf8[begin] == '\t' || textUtf8[begin] == '\r' || textUtf8[begin] == '\n'))
        {
            begin++;
        }

        std::size_t end = textUtf8.size();
        while (end > begin && (textUtf8[end - 1] == ' ' || textUtf8[end - 1] == '\t' || textUtf8[end - 1] == '\r' || textUtf8[end - 1] == '\n'))
        {
            end--;
        }
        return textUtf8.substr(begin, end - begin);
    }

    static bool IsUnsignedDecimalText(const std::string& textUtf8)
    {
        if (textUtf8.empty())
        {
            return false;
        }

        for (std::size_t index = 0; index < textUtf8.size(); index++)
        {
            const char ch = textUtf8[index];
            if (ch < '0' || ch > '9')
            {
                return false;
            }
        }
        return true;
    }

    static bool NormalizeGeoPackageFieldType(const std::string& typeUtf8, std::string& outTypeSqlUtf8)
    {
        outTypeSqlUtf8.clear();
        const std::string type = ToUpperAsciiString(TrimAsciiSpaces(typeUtf8));
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
            "DATE",
            "DATETIME"
        };

        for (std::size_t index = 0; index < sizeof(supportedTypes) / sizeof(supportedTypes[0]); index++)
        {
            if (type == supportedTypes[index])
            {
                outTypeSqlUtf8 = type;
                return true;
            }
        }

        const std::size_t leftParen = type.find('(');
        if (type == "TEXT" || type == "BLOB")
        {
            outTypeSqlUtf8 = type;
            return true;
        }
        if (leftParen == std::string::npos || type.empty() || type[type.size() - 1] != ')')
        {
            return false;
        }

        const std::string baseType = TrimAsciiSpaces(type.substr(0, leftParen));
        const std::string sizeText = TrimAsciiSpaces(type.substr(leftParen + 1, type.size() - leftParen - 2));
        if ((baseType != "TEXT" && baseType != "BLOB") || !IsUnsignedDecimalText(sizeText))
        {
            return false;
        }

        bool ok = false;
        const long long maxSize = GB_Variant(sizeText).ToInt64(&ok);
        if (!ok || maxSize <= 0)
        {
            return false;
        }

        outTypeSqlUtf8 = baseType + "(" + sizeText + ")";
        return true;
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

    struct WkbTypeInfo
    {
        std::uint32_t geometryType = 0;
        bool hasZ = false;
        bool hasM = false;
        bool hasSrid = false;
        int coordinateDimension = 2;
    };

    struct WkbDimensionSummary
    {
        bool hasGeometry = false;
        bool hasZ = false;
        bool hasM = false;
        bool hasNoZ = false;
        bool hasNoM = false;
        bool hasSrid = false;
    };

    static bool DecodeWkbTypeInfo(std::uint32_t rawType, WkbTypeInfo& outTypeInfo)
    {
        outTypeInfo = WkbTypeInfo();

        const bool ewkbHasZ = (rawType & 0x80000000u) != 0;
        const bool ewkbHasM = (rawType & 0x40000000u) != 0;
        const bool ewkbHasSrid = (rawType & 0x20000000u) != 0;

        std::uint32_t type = rawType & 0x1fffffffu;
        bool isoHasZ = false;
        bool isoHasM = false;
        if (type >= 3000u && type < 4000u)
        {
            isoHasZ = true;
            isoHasM = true;
            type -= 3000u;
        }
        else if (type >= 2000u && type < 3000u)
        {
            isoHasM = true;
            type -= 2000u;
        }
        else if (type >= 1000u && type < 2000u)
        {
            isoHasZ = true;
            type -= 1000u;
        }
        else if (type >= 4000u)
        {
            return false;
        }

        if ((ewkbHasZ || ewkbHasM || ewkbHasSrid) && (isoHasZ || isoHasM))
        {
            return false;
        }
        if (type < 1u || type > 14u)
        {
            return false;
        }

        outTypeInfo.geometryType = type;
        outTypeInfo.hasZ = ewkbHasZ || isoHasZ;
        outTypeInfo.hasM = ewkbHasM || isoHasM;
        outTypeInfo.hasSrid = ewkbHasSrid;
        outTypeInfo.coordinateDimension = 2 + (outTypeInfo.hasZ ? 1 : 0) + (outTypeInfo.hasM ? 1 : 0);
        return true;
    }

    static void AddWkbDimensionSummary(const WkbTypeInfo& typeInfo, WkbDimensionSummary* summary)
    {
        if (summary == nullptr)
        {
            return;
        }

        summary->hasGeometry = true;
        if (typeInfo.hasZ)
        {
            summary->hasZ = true;
        }
        else
        {
            summary->hasNoZ = true;
        }

        if (typeInfo.hasM)
        {
            summary->hasM = true;
        }
        else
        {
            summary->hasNoM = true;
        }

        if (typeInfo.hasSrid)
        {
            summary->hasSrid = true;
        }
    }

    class WkbReader
    {
    public:
        WkbReader(const unsigned char* dataPtr, std::size_t size) : dataPtr_(dataPtr), size_(size)
        {
        }

        bool ReadByte(unsigned char& outValue)
        {
            if (!CanRead(1))
            {
                return false;
            }
            outValue = dataPtr_[position_];
            position_++;
            return true;
        }

        bool ReadUInt32(bool littleEndian, std::uint32_t& outValue)
        {
            if (!CanRead(4))
            {
                return false;
            }
            outValue = ::ReadUInt32(dataPtr_ + position_, littleEndian);
            position_ += 4;
            return true;
        }

        bool ReadDouble(bool littleEndian, double& outValue)
        {
            if (!CanRead(8))
            {
                return false;
            }
            outValue = ::ReadDouble(dataPtr_ + position_, littleEndian);
            position_ += 8;
            return true;
        }

        bool IsAtEnd() const
        {
            return position_ == size_;
        }

        std::size_t GetRemainingByteCount() const
        {
            return position_ <= size_ ? size_ - position_ : 0;
        }

    private:
        bool CanRead(std::size_t byteCount) const
        {
            return byteCount <= size_ && position_ <= size_ - byteCount;
        }

        const unsigned char* dataPtr_ = nullptr;
        std::size_t size_ = 0;
        std::size_t position_ = 0;
    };

    struct WkbPointXY
    {
        double x = 0.0;
        double y = 0.0;
        bool valid = false;
    };

    static bool AreWkbPointsSameXY(const WkbPointXY& point1, const WkbPointXY& point2)
    {
        return point1.valid && point2.valid && NearlyEqualDouble(point1.x, point2.x) && NearlyEqualDouble(point1.y, point2.y);
    }

    static double NormalizeAngleRadians(double angle)
    {
        const double twoPi = std::acos(-1.0) * 2.0;
        while (angle < 0.0)
        {
            angle += twoPi;
        }
        while (angle >= twoPi)
        {
            angle -= twoPi;
        }
        return angle;
    }

    static double GetCcwAngleDistance(double fromAngle, double toAngle)
    {
        const double twoPi = std::acos(-1.0) * 2.0;
        double distance = NormalizeAngleRadians(toAngle) - NormalizeAngleRadians(fromAngle);
        if (distance < 0.0)
        {
            distance += twoPi;
        }
        return distance;
    }

    static bool IsAngleOnCcwSweep(double startAngle, double testAngle, double endAngle)
    {
        return GetCcwAngleDistance(startAngle, testAngle) <= GetCcwAngleDistance(startAngle, endAngle) + GB_GpkgAngleEpsilon;
    }

    static bool ReadWkbPointXY(WkbReader& reader, bool littleEndian, int coordinateDimension, WkbPointXY& outPoint)
    {
        outPoint = WkbPointXY();
        if (coordinateDimension < 2)
        {
            return false;
        }

        if (!reader.ReadDouble(littleEndian, outPoint.x) || !reader.ReadDouble(littleEndian, outPoint.y))
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

        outPoint.valid = IsFinite(outPoint.x) && IsFinite(outPoint.y);
        return true;
    }

    static bool ExpandEnvelopeWithPoint(const WkbPointXY& point, GB_Rectangle& envelope)
    {
        if (!point.valid)
        {
            return false;
        }

        ExpandRectangleToInclude(envelope, point.x, point.y);
        return true;
    }

    static void ExpandEnvelopeWithCircularArc(const WkbPointXY& startPoint, const WkbPointXY& midPoint, const WkbPointXY& endPoint, GB_Rectangle& envelope)
    {
        ExpandEnvelopeWithPoint(startPoint, envelope);
        ExpandEnvelopeWithPoint(midPoint, envelope);
        ExpandEnvelopeWithPoint(endPoint, envelope);

        if (!startPoint.valid || !midPoint.valid || !endPoint.valid)
        {
            return;
        }

        const double x1 = startPoint.x;
        const double y1 = startPoint.y;
        const double x2 = midPoint.x;
        const double y2 = midPoint.y;
        const double x3 = endPoint.x;
        const double y3 = endPoint.y;
        const double determinant = 2.0 * (x1 * (y2 - y3) + x2 * (y3 - y1) + x3 * (y1 - y2));
        const double coordinateScale = std::max(1.0, std::max(std::max(std::fabs(x1), std::fabs(y1)), std::max(std::max(std::fabs(x2), std::fabs(y2)), std::max(std::fabs(x3), std::fabs(y3)))));
        if (std::fabs(determinant) <= std::numeric_limits<double>::epsilon() * coordinateScale * coordinateScale * 64.0)
        {
            return;
        }

        const double x1SquareY1Square = x1 * x1 + y1 * y1;
        const double x2SquareY2Square = x2 * x2 + y2 * y2;
        const double x3SquareY3Square = x3 * x3 + y3 * y3;
        const double centerX = (x1SquareY1Square * (y2 - y3) + x2SquareY2Square * (y3 - y1) + x3SquareY3Square * (y1 - y2)) / determinant;
        const double centerY = (x1SquareY1Square * (x3 - x2) + x2SquareY2Square * (x1 - x3) + x3SquareY3Square * (x2 - x1)) / determinant;
        const double radius = std::sqrt((x1 - centerX) * (x1 - centerX) + (y1 - centerY) * (y1 - centerY));
        if (!IsFinite(centerX) || !IsFinite(centerY) || !IsFinite(radius) || radius <= 0.0)
        {
            return;
        }

        const double startAngle = std::atan2(y1 - centerY, x1 - centerX);
        const double midAngle = std::atan2(y2 - centerY, x2 - centerX);
        const double endAngle = std::atan2(y3 - centerY, x3 - centerX);
        const bool isCcw = IsAngleOnCcwSweep(startAngle, midAngle, endAngle);
        const double halfPi = std::acos(-1.0) * 0.5;
        const double axisAngles[] = { 0.0, halfPi, halfPi * 2.0, halfPi * 3.0 };
        for (std::size_t index = 0; index < sizeof(axisAngles) / sizeof(axisAngles[0]); index++)
        {
            const double angle = axisAngles[index];
            const bool onArc = isCcw ? IsAngleOnCcwSweep(startAngle, angle, endAngle) : IsAngleOnCcwSweep(endAngle, angle, startAngle);
            if (onArc)
            {
                ExpandRectangleToInclude(envelope, centerX + radius * std::cos(angle), centerY + radius * std::sin(angle));
            }
        }
    }

    static bool ReadWkbPointCoordinates(WkbReader& reader, bool littleEndian, int coordinateDimension, GB_Rectangle& envelope)
    {
        WkbPointXY point;
        if (!ReadWkbPointXY(reader, littleEndian, coordinateDimension, point))
        {
            return false;
        }
        if (!point.valid && !(std::isnan(point.x) && std::isnan(point.y)))
        {
            return false;
        }

        ExpandEnvelopeWithPoint(point, envelope);
        return true;
    }

    static bool ReadWkbPointArrayEnvelope(WkbReader& reader, bool littleEndian, int coordinateDimension, int minimumNonEmptyPointCount, bool requireClosedRing, GB_Rectangle& envelope)
    {
        std::uint32_t pointCount = 0;
        if (!reader.ReadUInt32(littleEndian, pointCount))
        {
            return false;
        }

        const std::size_t pointByteCount = static_cast<std::size_t>(coordinateDimension) * sizeof(double);
        if (pointByteCount == 0 || pointCount > reader.GetRemainingByteCount() / pointByteCount)
        {
            return false;
        }
        if (pointCount != 0 && pointCount < static_cast<std::uint32_t>(minimumNonEmptyPointCount))
        {
            return false;
        }

        WkbPointXY firstPoint;
        WkbPointXY lastPoint;
        for (std::uint32_t pointIndex = 0; pointIndex < pointCount; pointIndex++)
        {
            WkbPointXY point;
            if (!ReadWkbPointXY(reader, littleEndian, coordinateDimension, point) || !point.valid)
            {
                return false;
            }
            if (pointIndex == 0)
            {
                firstPoint = point;
            }
            lastPoint = point;
            ExpandEnvelopeWithPoint(point, envelope);
        }

        if (requireClosedRing && pointCount != 0 && !AreWkbPointsSameXY(firstPoint, lastPoint))
        {
            return false;
        }
        return true;
    }

    static bool ReadWkbCircularStringEnvelope(WkbReader& reader, bool littleEndian, int coordinateDimension, GB_Rectangle& envelope)
    {
        std::uint32_t pointCount = 0;
        if (!reader.ReadUInt32(littleEndian, pointCount))
        {
            return false;
        }

        const std::size_t pointByteCount = static_cast<std::size_t>(coordinateDimension) * sizeof(double);
        if (pointByteCount == 0 || pointCount > reader.GetRemainingByteCount() / pointByteCount)
        {
            return false;
        }
        if (pointCount != 0 && (pointCount < 3 || (pointCount % 2) == 0))
        {
            return false;
        }

        WkbPointXY previousPreviousPoint;
        WkbPointXY previousPoint;
        for (std::uint32_t pointIndex = 0; pointIndex < pointCount; pointIndex++)
        {
            WkbPointXY point;
            if (!ReadWkbPointXY(reader, littleEndian, coordinateDimension, point) || !point.valid)
            {
                return false;
            }

            ExpandEnvelopeWithPoint(point, envelope);
            if (pointIndex >= 2 && (pointIndex % 2) == 0)
            {
                ExpandEnvelopeWithCircularArc(previousPreviousPoint, previousPoint, point, envelope);
            }

            previousPreviousPoint = previousPoint;
            previousPoint = point;
        }
        return true;
    }

    static bool IsCurveWkbGeometryType(std::uint32_t geometryType)
    {
        return geometryType == 2u || geometryType == 8u || geometryType == 9u || geometryType == 13u;
    }

    static bool IsSurfaceWkbGeometryType(std::uint32_t geometryType)
    {
        return geometryType == 3u || geometryType == 10u || geometryType == 14u;
    }

    static bool IsChildWkbGeometryTypeAllowed(std::uint32_t parentGeometryType, std::uint32_t childGeometryType)
    {
        switch (parentGeometryType)
        {
        case 4u:
            return childGeometryType == 1u;
        case 5u:
            return childGeometryType == 2u;
        case 6u:
            return childGeometryType == 3u;
        case 7u:
            return childGeometryType >= 1u && childGeometryType <= 14u;
        case 9u:
            return childGeometryType == 2u || childGeometryType == 8u;
        case 10u:
            return IsCurveWkbGeometryType(childGeometryType);
        case 11u:
            return IsCurveWkbGeometryType(childGeometryType);
        case 12u:
            return IsSurfaceWkbGeometryType(childGeometryType);
        case 13u:
            return IsCurveWkbGeometryType(childGeometryType);
        case 14u:
            return IsSurfaceWkbGeometryType(childGeometryType);
        default:
            return false;
        }
    }

    static bool ReadWkbGeometryEnvelope(WkbReader& reader, GB_Rectangle& envelope, int depth, WkbDimensionSummary* dimensionSummary = nullptr, std::uint32_t* outGeometryType = nullptr)
    {
        if (outGeometryType != nullptr)
        {
            *outGeometryType = 0;
        }
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

        WkbTypeInfo typeInfo;
        if (!DecodeWkbTypeInfo(rawType, typeInfo))
        {
            return false;
        }
        if (outGeometryType != nullptr)
        {
            *outGeometryType = typeInfo.geometryType;
        }
        AddWkbDimensionSummary(typeInfo, dimensionSummary);

        if (typeInfo.hasSrid)
        {
            std::uint32_t unusedSrid = 0;
            if (!reader.ReadUInt32(littleEndian, unusedSrid))
            {
                return false;
            }
        }

        switch (typeInfo.geometryType)
        {
        case 1u:
            return ReadWkbPointCoordinates(reader, littleEndian, typeInfo.coordinateDimension, envelope);
        case 2u:
            return ReadWkbPointArrayEnvelope(reader, littleEndian, typeInfo.coordinateDimension, 2, false, envelope);
        case 8u:
            return ReadWkbCircularStringEnvelope(reader, littleEndian, typeInfo.coordinateDimension, envelope);
        case 3u:
        {
            std::uint32_t ringCount = 0;
            if (!reader.ReadUInt32(littleEndian, ringCount))
            {
                return false;
            }
            if (ringCount > reader.GetRemainingByteCount() / sizeof(std::uint32_t))
            {
                return false;
            }
            for (std::uint32_t ringIndex = 0; ringIndex < ringCount; ringIndex++)
            {
                if (!ReadWkbPointArrayEnvelope(reader, littleEndian, typeInfo.coordinateDimension, 4, true, envelope))
                {
                    return false;
                }
            }
            return true;
        }
        case 4u:
        case 5u:
        case 6u:
        case 7u:
        case 9u:
        case 10u:
        case 11u:
        case 12u:
        case 13u:
        case 14u:
        {
            std::uint32_t geometryCount = 0;
            if (!reader.ReadUInt32(littleEndian, geometryCount))
            {
                return false;
            }
            if (geometryCount > reader.GetRemainingByteCount() / 5)
            {
                return false;
            }
            for (std::uint32_t geometryIndex = 0; geometryIndex < geometryCount; geometryIndex++)
            {
                std::uint32_t childGeometryType = 0;
                if (!ReadWkbGeometryEnvelope(reader, envelope, depth + 1, dimensionSummary, &childGeometryType))
                {
                    return false;
                }
                if (!IsChildWkbGeometryTypeAllowed(typeInfo.geometryType, childGeometryType))
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

    static bool ParseWkbEnvelopeFromData(const unsigned char* dataPtr, std::size_t size, GB_Rectangle* outEnvelope, WkbDimensionSummary* outDimensionSummary)
    {
        if (outEnvelope != nullptr)
        {
            outEnvelope->Reset();
        }
        if (outDimensionSummary != nullptr)
        {
            *outDimensionSummary = WkbDimensionSummary();
        }
        if (dataPtr == nullptr || size == 0)
        {
            return false;
        }

        GB_Rectangle envelope;
        WkbReader reader(dataPtr, size);
        if (!ReadWkbGeometryEnvelope(reader, envelope, 0, outDimensionSummary, nullptr) || !reader.IsAtEnd())
        {
            return false;
        }

        if (outEnvelope != nullptr)
        {
            *outEnvelope = envelope;
        }
        return true;
    }

    static bool ParseWkbEnvelope(const GB_ByteBuffer& wkb, GB_Rectangle* outEnvelope, WkbDimensionSummary* outDimensionSummary)
    {
        return ParseWkbEnvelopeFromData(wkb.empty() ? nullptr : wkb.data(), wkb.size(), outEnvelope, outDimensionSummary);
    }

    static bool TableExists(const GB_Sqlite& database, const std::string& tableNameUtf8, bool& outExists)
    {
        outExists = false;
        return database.TableExists(tableNameUtf8, outExists, true);
    }

    static bool TileMatrixSetExists(const GB_Sqlite& database, const std::string& tableNameUtf8, bool& outExists)
    {
        outExists = false;
        GB_Variant value;
        if (!database.ExecuteScalar("SELECT COUNT(*) FROM gpkg_tile_matrix_set t JOIN gpkg_contents c ON t.table_name = c.table_name WHERE t.table_name = ? AND c.data_type = 'tiles'", GB_SqliteParameterList(1, tableNameUtf8), value))
        {
            return false;
        }

        bool ok = false;
        outExists = value.ToInt64(&ok) > 0;
        return ok;
    }

    static bool HasTileRowsOutsideMatrix(const GB_Sqlite& database, const std::string& tableNameUtf8, int zoomLevel, int matrixWidth, int matrixHeight, bool& outHasInvalidTiles)
    {
        outHasInvalidTiles = false;
        if (!IsValidUserContentTableName(tableNameUtf8) || zoomLevel < 0 || matrixWidth < 1 || matrixHeight < 1)
        {
            return false;
        }

        GB_SqliteParameterList parameters;
        parameters.push_back(zoomLevel);
        parameters.push_back(matrixWidth);
        parameters.push_back(matrixHeight);
        const std::string sql = "SELECT COUNT(*) FROM " + QuoteIdentifier(tableNameUtf8) + " WHERE zoom_level = ? AND (tile_column < 0 OR tile_row < 0 OR tile_column >= ? OR tile_row >= ?)";
        GB_Variant value;
        if (!database.ExecuteScalar(sql, parameters, value))
        {
            return false;
        }

        bool ok = false;
        outHasInvalidTiles = value.ToInt64(&ok) > 0;
        return ok;
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

    static bool IsSqliteIntegerPrimaryKeyAliasType(const std::string& declaredTypeUtf8)
    {
        return ToUpperAsciiString(TrimAsciiSpaces(declaredTypeUtf8)) == "INTEGER";
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
            if (field.primaryKeyIndex > 0 && IsSqliteIntegerPrimaryKeyAliasType(field.typeUtf8))
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

    static const GB_SqliteTableFieldInfo* FindTableFieldInfoNoCase(const std::vector<GB_SqliteTableFieldInfo>& fields, const std::string& fieldNameUtf8)
    {
        const std::string lowerFieldName = ToLowerAsciiString(fieldNameUtf8);
        for (std::size_t index = 0; index < fields.size(); index++)
        {
            if (ToLowerAsciiString(fields[index].nameUtf8) == lowerFieldName)
            {
                return &fields[index];
            }
        }
        return nullptr;
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
        if (geometry.empty())
        {
            parameters.push_back(GB_Variant());
        }
        else
        {
            parameters.push_back(geometry);
        }
        for (std::map<std::string, GB_Variant>::const_iterator iter = attributes.begin(); iter != attributes.end(); ++iter)
        {
            parameters.push_back(iter->second);
        }
        return parameters;
    }

    static std::string BuildFeatureAttributeSqlKey(const std::map<std::string, GB_Variant>& attributes)
    {
        std::string key;
        for (std::map<std::string, GB_Variant>::const_iterator iter = attributes.begin(); iter != attributes.end(); ++iter)
        {
            key += iter->first;
            key.push_back('');
        }
        return key;
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

    static bool ReadWkbGeometryTypeNameFromData(const unsigned char* dataPtr, std::size_t size, std::string& outTypeNameUtf8)
    {
        outTypeNameUtf8.clear();
        if (dataPtr == nullptr || size < 5)
        {
            return false;
        }

        const unsigned char byteOrder = dataPtr[0];
        if (byteOrder != 0 && byteOrder != 1)
        {
            return false;
        }

        WkbTypeInfo typeInfo;
        if (!DecodeWkbTypeInfo(ReadUInt32(dataPtr + 1, byteOrder == 1), typeInfo))
        {
            return false;
        }

        outTypeNameUtf8 = WkbGeometryTypeToName(typeInfo.geometryType);
        return !outTypeNameUtf8.empty();
    }

    static bool ReadWkbGeometryTypeName(const GB_ByteBuffer& wkb, std::string& outTypeNameUtf8)
    {
        return ReadWkbGeometryTypeNameFromData(wkb.empty() ? nullptr : wkb.data(), wkb.size(), outTypeNameUtf8);
    }

    static bool IsWkbGeometryTypeCompatibleWithLayerGeometry(const std::string& layerGeometryTypeUtf8, const std::string& wkbGeometryTypeUtf8)
    {
        const std::string layerGeometryType = ToUpperAsciiString(layerGeometryTypeUtf8);
        const std::string wkbGeometryType = ToUpperAsciiString(wkbGeometryTypeUtf8);
        if (layerGeometryType == "GEOMETRY" || layerGeometryType == wkbGeometryType)
        {
            return true;
        }
        if (layerGeometryType == "CURVE")
        {
            return wkbGeometryType == "LINESTRING" || wkbGeometryType == "CIRCULARSTRING" || wkbGeometryType == "COMPOUNDCURVE";
        }
        if (layerGeometryType == "SURFACE")
        {
            return wkbGeometryType == "POLYGON" || wkbGeometryType == "CURVEPOLYGON";
        }
        if (layerGeometryType == "CURVEPOLYGON")
        {
            return wkbGeometryType == "POLYGON";
        }
        if (layerGeometryType == "GEOMETRYCOLLECTION")
        {
            return wkbGeometryType == "MULTIPOINT" || wkbGeometryType == "MULTICURVE" || wkbGeometryType == "MULTISURFACE" || wkbGeometryType == "MULTILINESTRING" || wkbGeometryType == "MULTIPOLYGON";
        }
        if (layerGeometryType == "MULTICURVE")
        {
            return wkbGeometryType == "MULTILINESTRING";
        }
        if (layerGeometryType == "MULTISURFACE")
        {
            return wkbGeometryType == "MULTIPOLYGON";
        }
        return false;
    }

    static bool ValidateWkbDimensionSummaryAgainstLayer(const GeoGpkgFeatureLayerInfo& layerInfo, const WkbDimensionSummary& dimensionSummary, std::string& outErrorMessageUtf8)
    {
        outErrorMessageUtf8.clear();

        if (dimensionSummary.hasSrid)
        {
            outErrorMessageUtf8 = u8"GeoPackageBinary 的 WKB 体不应包含 EWKB 内嵌 SRID，应使用 GeoPackage 几何头中的 srs_id。";
            return false;
        }
        if (layerInfo.z == 0 && dimensionSummary.hasZ)
        {
            outErrorMessageUtf8 = u8"图层声明不允许 Z 坐标，但要素 WKB 含 Z 维度。";
            return false;
        }
        if (layerInfo.z == 1 && dimensionSummary.hasNoZ)
        {
            outErrorMessageUtf8 = u8"图层声明必须包含 Z 坐标，但要素 WKB 存在二维或仅 M 维几何。";
            return false;
        }
        if (layerInfo.m == 0 && dimensionSummary.hasM)
        {
            outErrorMessageUtf8 = u8"图层声明不允许 M 坐标，但要素 WKB 含 M 维度。";
            return false;
        }
        if (layerInfo.m == 1 && dimensionSummary.hasNoM)
        {
            outErrorMessageUtf8 = u8"图层声明必须包含 M 坐标，但要素 WKB 存在无 M 维几何。";
            return false;
        }

        return true;
    }

    static bool AnalyzeGpkgGeometryForLayer(const GeoGpkgFeatureLayerInfo& layerInfo, const GB_ByteBuffer& gpkgGeometry, GeoGpkgGeometryHeaderInfo& outHeaderInfo, GB_Rectangle& outEnvelope, bool& outIsEmptyGeometry, std::string& outErrorMessageUtf8)
    {
        outHeaderInfo = GeoGpkgGeometryHeaderInfo();
        outEnvelope.Reset();
        outIsEmptyGeometry = false;
        outErrorMessageUtf8.clear();

        if (!GeoGpkg::ParseGeometryHeader(gpkgGeometry, outHeaderInfo) || !outHeaderInfo.valid)
        {
            outErrorMessageUtf8 = u8"GeoPackageBinary 几何头无效。";
            return false;
        }
        if (outHeaderInfo.extended)
        {
            outErrorMessageUtf8 = u8"当前模块不支持 User-Defined ExtendedGeoPackageBinary 几何编码。";
            return false;
        }
        if (outHeaderInfo.srsId != layerInfo.srsId)
        {
            outErrorMessageUtf8 = u8"要素 SRID 与图层登记 SRID 不一致。";
            return false;
        }
        if (gpkgGeometry.size() <= outHeaderInfo.headerSize)
        {
            outErrorMessageUtf8 = u8"GeoPackageBinary 缺少 WKB 几何体。";
            return false;
        }

        const unsigned char* wkbDataPtr = gpkgGeometry.data() + outHeaderInfo.headerSize;
        const std::size_t wkbSize = gpkgGeometry.size() - outHeaderInfo.headerSize;

        std::string wkbGeometryType;
        if (!ReadWkbGeometryTypeNameFromData(wkbDataPtr, wkbSize, wkbGeometryType))
        {
            outErrorMessageUtf8 = u8"无法读取要素 WKB 几何类型。";
            return false;
        }
        if (!IsWkbGeometryTypeCompatibleWithLayerGeometry(layerInfo.geometryTypeNameUtf8, wkbGeometryType))
        {
            outErrorMessageUtf8 = u8"要素 WKB 几何类型 " + wkbGeometryType + u8" 与图层登记几何类型 " + layerInfo.geometryTypeNameUtf8 + u8" 不一致。";
            return false;
        }

        WkbDimensionSummary dimensionSummary;
        if (!ParseWkbEnvelopeFromData(wkbDataPtr, wkbSize, &outEnvelope, &dimensionSummary))
        {
            outErrorMessageUtf8 = u8"WKB 结构无效，无法解析几何范围。";
            return false;
        }
        if (!ValidateWkbDimensionSummaryAgainstLayer(layerInfo, dimensionSummary, outErrorMessageUtf8))
        {
            return false;
        }
        if (outHeaderInfo.empty && outEnvelope.IsValid())
        {
            outErrorMessageUtf8 = u8"GeoPackageBinary 标记为空几何，但 WKB 中存在有效坐标。";
            return false;
        }
        if (!outHeaderInfo.empty && !outEnvelope.IsValid())
        {
            outErrorMessageUtf8 = u8"GeoPackageBinary 未标记为空几何，但 WKB 中没有有效二维坐标。";
            return false;
        }
        if (!outHeaderInfo.empty && outHeaderInfo.envelope.IsValid() && !IsRectangleNearlyEqual(outHeaderInfo.envelope, outEnvelope))
        {
            outErrorMessageUtf8 = u8"GeoPackageBinary 几何头 envelope 与 WKB 实际二维范围不一致。";
            return false;
        }

        outIsEmptyGeometry = outHeaderInfo.empty || !outEnvelope.IsValid();
        return true;
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

        if (geometry.size() <= headerInfo.headerSize)
        {
            outErrorMessageUtf8 = "GeoPackage geometry does not contain WKB body.";
            return false;
        }

        const unsigned char* wkbDataPtr = geometry.data() + headerInfo.headerSize;
        const std::size_t wkbSize = geometry.size() - headerInfo.headerSize;
        WkbDimensionSummary dimensionSummary;
        if (!ParseWkbEnvelopeFromData(wkbDataPtr, wkbSize, &outEnvelope, &dimensionSummary))
        {
            outEnvelope.Reset();
            outErrorMessageUtf8 = "Failed to calculate GeoPackage geometry envelope.";
            return false;
        }
        if (dimensionSummary.hasSrid)
        {
            outEnvelope.Reset();
            outErrorMessageUtf8 = "GeoPackage WKB body must not contain EWKB SRID.";
            return false;
        }

        if (!outEnvelope.IsValid())
        {
            outIsEmpty = true;
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

        GB_ByteBuffer geometry;
        bool isNull = false;
        if (!ExtractSqlGeometryArgument(arguments, geometry, isNull, outErrorMessageUtf8))
        {
            return false;
        }
        if (isNull)
        {
            return true;
        }

        GB_Rectangle envelope;
        bool isEmpty = false;
        if (!GetGeometryEnvelopeFromGpkgGeometry(geometry, envelope, isEmpty, outErrorMessageUtf8))
        {
            return false;
        }

        outResult = GB_Variant(isEmpty ? 1 : 0);
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

    static bool GeoGpkgSqlSridFunction(const std::vector<GB_Variant>& arguments, GB_Variant& outResult, std::string& outErrorMessageUtf8)
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

        outResult = GB_Variant(headerInfo.srsId);
        return true;
    }

    static bool GeoGpkgSqlGeometryTypeFunction(const std::vector<GB_Variant>& arguments, GB_Variant& outResult, std::string& outErrorMessageUtf8)
    {
        outResult = GB_Variant();

        GB_ByteBuffer geometry;
        bool isNull = false;
        if (!ExtractSqlGeometryArgument(arguments, geometry, isNull, outErrorMessageUtf8))
        {
            return false;
        }
        if (isNull)
        {
            return true;
        }

        GeoGpkgGeometryHeaderInfo headerInfo;
        if (!GeoGpkg::ParseGeometryHeader(geometry, headerInfo) || !headerInfo.valid || geometry.size() <= headerInfo.headerSize)
        {
            outErrorMessageUtf8 = "Invalid GeoPackage geometry header.";
            return false;
        }

        std::string geometryTypeNameUtf8;
        const unsigned char* wkbDataPtr = geometry.data() + headerInfo.headerSize;
        const std::size_t wkbSize = geometry.size() - headerInfo.headerSize;
        if (!ReadWkbGeometryTypeNameFromData(wkbDataPtr, wkbSize, geometryTypeNameUtf8))
        {
            outErrorMessageUtf8 = "Invalid GeoPackage WKB geometry type.";
            return false;
        }

        outResult = GB_Variant(geometryTypeNameUtf8);
        return true;
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

    if (options.initializeIfNeeded && !database_.IsReadOnly() && !InitializeCoreTablesNoLock(options.userVersion))
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
    std::lock_guard<std::mutex> guard(lastErrorLock_);
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
    options.innocuous = true;

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
    if (!database_.RegisterScalarFunction("ST_SRID", GeoGpkgSqlSridFunction, options))
    {
        return SetLastSqliteError(u8"注册 GeoPackage SQL 函数 ST_SRID 失败");
    }
    if (!database_.RegisterScalarFunction("ST_GeometryType", GeoGpkgSqlGeometryTypeFunction, options))
    {
        return SetLastSqliteError(u8"注册 GeoPackage SQL 函数 ST_GeometryType 失败");
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
    if (database_.IsReadOnly())
    {
        return SetLastError(u8"当前 GeoPackage 以只读方式打开，不能初始化核心表。 ");
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

    std::vector<std::string> coreTileMatrixTriggerSqlList;
    BuildCoreTileMatrixConstraintTriggerSqlList(coreTileMatrixTriggerSqlList);
    for (std::size_t index = 0; index < coreTileMatrixTriggerSqlList.size(); index++)
    {
        stream << coreTileMatrixTriggerSqlList[index] << ";\n";
    }

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
    if (!database_.IsOpen())
    {
        return SetLastError(u8"GeoPackage 尚未打开。 ");
    }
    if (database_.IsReadOnly())
    {
        return SetLastError(u8"当前 GeoPackage 以只读方式打开，不能写入默认空间参考。 ");
    }

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

    if (!database_.IsOpen())
    {
        return SetLastError(u8"GeoPackage 尚未打开。 ");
    }

    static const char* requiredBaseTables[] =
    {
        "gpkg_spatial_ref_sys",
        "gpkg_contents"
    };

    bool hasError = false;
    for (std::size_t index = 0; index < sizeof(requiredBaseTables) / sizeof(requiredBaseTables[0]); index++)
    {
        bool exists = false;
        if (!TableExists(database_, requiredBaseTables[index], exists))
        {
            return SetLastSqliteError(u8"检查 GeoPackage 核心表失败");
        }
        if (!exists)
        {
            hasError = true;
            if (outMessages != nullptr)
            {
                outMessages->push_back(u8"缺少核心表：" + std::string(requiredBaseTables[index]));
            }
        }
    }
    if (hasError)
    {
        return false;
    }

    GB_Variant featureContentCountValue;
    if (!database_.ExecuteScalar("SELECT COUNT(*) FROM gpkg_contents WHERE data_type = 'features'", featureContentCountValue))
    {
        return SetLastSqliteError(u8"检查 features 内容记录失败");
    }
    bool featureCountOk = false;
    const bool hasFeatureContents = featureContentCountValue.ToInt64(&featureCountOk) > 0;
    if (!featureCountOk)
    {
        return SetLastError(u8"gpkg_contents features 数量字段类型异常。 ");
    }
    if (hasFeatureContents)
    {
        bool exists = false;
        if (!TableExists(database_, "gpkg_geometry_columns", exists))
        {
            return SetLastSqliteError(u8"检查 gpkg_geometry_columns 失败");
        }
        if (!exists)
        {
            hasError = true;
            if (outMessages != nullptr)
            {
                outMessages->push_back(u8"存在 features 内容记录，但缺少 gpkg_geometry_columns。 ");
            }
        }
    }

    GB_Variant tileContentCountValue;
    if (!database_.ExecuteScalar("SELECT COUNT(*) FROM gpkg_contents WHERE data_type = 'tiles'", tileContentCountValue))
    {
        return SetLastSqliteError(u8"检查 tiles 内容记录失败");
    }
    bool tileCountOk = false;
    const bool hasTileContents = tileContentCountValue.ToInt64(&tileCountOk) > 0;
    if (!tileCountOk)
    {
        return SetLastError(u8"gpkg_contents tiles 数量字段类型异常。 ");
    }
    if (hasTileContents)
    {
        static const char* requiredTileTables[] = { "gpkg_tile_matrix_set", "gpkg_tile_matrix" };
        for (std::size_t index = 0; index < sizeof(requiredTileTables) / sizeof(requiredTileTables[0]); index++)
        {
            bool exists = false;
            if (!TableExists(database_, requiredTileTables[index], exists))
            {
                return SetLastSqliteError(u8"检查瓦片核心表失败");
            }
            if (!exists)
            {
                hasError = true;
                if (outMessages != nullptr)
                {
                    outMessages->push_back(u8"存在 tiles 内容记录，但缺少核心表：" + std::string(requiredTileTables[index]));
                }
            }
        }
    }
    if (hasError)
    {
        return false;
    }

    GB_Variant applicationIdValue;
    if (!database_.ExecuteScalar("PRAGMA application_id", applicationIdValue))
    {
        return SetLastSqliteError(u8"读取 application_id 失败");
    }
    bool applicationIdOk = false;
    const int applicationId = applicationIdValue.ToInt(&applicationIdOk);
    if (!applicationIdOk || applicationId != GB_GpkgApplicationId)
    {
        hasError = true;
        if (outMessages != nullptr)
        {
            outMessages->push_back(u8"SQLite header application_id 不是 GeoPackage 标识。 ");
        }
    }

    GB_Variant integrityValue;
    if (!database_.ExecuteScalar("PRAGMA integrity_check", integrityValue))
    {
        return SetLastSqliteError(u8"执行 integrity_check 失败");
    }
    bool integrityOk = false;
    const std::string integrityMessageUtf8 = integrityValue.ToString(&integrityOk);
    if (!integrityOk || integrityMessageUtf8 != "ok")
    {
        hasError = true;
        if (outMessages != nullptr)
        {
            outMessages->push_back(u8"integrity_check 未返回 ok。 ");
        }
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

    GB_SqliteResult contentResult;
    if (!database_.Query("SELECT table_name, data_type FROM gpkg_contents", contentResult))
    {
        return SetLastSqliteError(u8"检查 gpkg_contents 用户表失败");
    }
    for (std::size_t rowIndex = 0; rowIndex < contentResult.rows.size(); rowIndex++)
    {
        std::string tableNameUtf8;
        std::string dataTypeUtf8;
        QueryStringColumn(contentResult, rowIndex, "table_name", tableNameUtf8);
        QueryStringColumn(contentResult, rowIndex, "data_type", dataTypeUtf8);

        bool exists = false;
        if (!TableExists(database_, tableNameUtf8, exists))
        {
            return SetLastSqliteError(u8"检查 GeoPackage 用户内容表失败");
        }
        if (!exists)
        {
            hasError = true;
            if (outMessages != nullptr)
            {
                outMessages->push_back(u8"gpkg_contents 登记了不存在的用户表：" + tableNameUtf8);
            }
        }

        if (ParseContentType(dataTypeUtf8) == GeoGpkgContentType::Unknown)
        {
            hasError = true;
            if (outMessages != nullptr)
            {
                outMessages->push_back(u8"gpkg_contents.data_type 不是当前模块支持的类型：" + tableNameUtf8 + ", " + dataTypeUtf8);
            }
        }
    }

    bool hasGeometryColumnsTable = false;
    if (!TableExists(database_, "gpkg_geometry_columns", hasGeometryColumnsTable))
    {
        return SetLastSqliteError(u8"检查 gpkg_geometry_columns 是否存在失败");
    }
    if (hasGeometryColumnsTable)
    {
        GB_SqliteResult geometryColumnResult;
        const std::string geometryColumnSql =
            "SELECT g.table_name, g.column_name, g.geometry_type_name, g.srs_id, g.z, g.m, c.data_type, c.srs_id AS content_srs_id "
            "FROM gpkg_geometry_columns g LEFT JOIN gpkg_contents c ON g.table_name = c.table_name";
        if (!database_.Query(geometryColumnSql, geometryColumnResult))
        {
            return SetLastSqliteError(u8"检查 gpkg_geometry_columns 内容失败");
        }
        for (std::size_t rowIndex = 0; rowIndex < geometryColumnResult.rows.size(); rowIndex++)
        {
            std::string tableNameUtf8;
            std::string columnNameUtf8;
            std::string geometryTypeNameUtf8;
            std::string dataTypeUtf8;
            int srsId = 0;
            int contentSrsId = 0;
            int z = 0;
            int m = 0;
            QueryStringColumn(geometryColumnResult, rowIndex, "table_name", tableNameUtf8);
            QueryStringColumn(geometryColumnResult, rowIndex, "column_name", columnNameUtf8);
            QueryStringColumn(geometryColumnResult, rowIndex, "geometry_type_name", geometryTypeNameUtf8);
            QueryStringColumn(geometryColumnResult, rowIndex, "data_type", dataTypeUtf8);
            QueryIntColumn(geometryColumnResult, rowIndex, "srs_id", srsId);
            QueryIntColumn(geometryColumnResult, rowIndex, "content_srs_id", contentSrsId);
            QueryIntColumn(geometryColumnResult, rowIndex, "z", z);
            QueryIntColumn(geometryColumnResult, rowIndex, "m", m);

            if (dataTypeUtf8 != "features" || srsId != contentSrsId)
            {
                hasError = true;
                if (outMessages != nullptr)
                {
                    outMessages->push_back(u8"gpkg_geometry_columns 与 gpkg_contents 的 features 记录或 srs_id 不一致：" + tableNameUtf8 + "." + columnNameUtf8);
                }
            }
            if (!IsSupportedGeometryType(geometryTypeNameUtf8) || geometryTypeNameUtf8 != ToUpperAsciiString(geometryTypeNameUtf8) || z < 0 || z > 2 || m < 0 || m > 2)
            {
                hasError = true;
                if (outMessages != nullptr)
                {
                    outMessages->push_back(u8"gpkg_geometry_columns 存在非法几何类型或维度标记：" + tableNameUtf8 + "." + columnNameUtf8);
                }
            }

            std::vector<GB_SqliteTableFieldInfo> tableFields;
            if (!database_.GetTableFieldInfos(tableNameUtf8, tableFields, false))
            {
                return SetLastSqliteError(u8"检查矢量图层字段失败：" + tableNameUtf8);
            }
            const GB_SqliteTableFieldInfo* geometryField = FindTableFieldInfoNoCase(tableFields, columnNameUtf8);
            if (geometryField == nullptr || ToUpperAsciiString(TrimAsciiSpaces(geometryField->typeUtf8)) != geometryTypeNameUtf8)
            {
                hasError = true;
                if (outMessages != nullptr)
                {
                    outMessages->push_back(u8"矢量图层几何字段不存在或声明类型与 gpkg_geometry_columns 不一致：" + tableNameUtf8 + "." + columnNameUtf8);
                }
            }
        }
    }

    bool hasTileMatrixTable = false;
    if (!TableExists(database_, "gpkg_tile_matrix", hasTileMatrixTable))
    {
        return SetLastSqliteError(u8"检查 gpkg_tile_matrix 是否存在失败");
    }
    if (hasTileMatrixTable)
    {
        GB_SqliteResult tileMatrixResult;
        const std::string tileMatrixSql =
            "SELECT m.table_name, m.zoom_level, m.matrix_width, m.matrix_height, m.tile_width, m.tile_height, m.pixel_x_size, m.pixel_y_size, "
            "s.min_x, s.min_y, s.max_x, s.max_y, c.data_type, c.srs_id AS content_srs_id, s.srs_id AS matrix_set_srs_id "
            "FROM gpkg_tile_matrix m LEFT JOIN gpkg_tile_matrix_set s ON m.table_name = s.table_name "
            "LEFT JOIN gpkg_contents c ON m.table_name = c.table_name ORDER BY m.table_name, m.zoom_level";
        if (!database_.Query(tileMatrixSql, tileMatrixResult))
        {
            return SetLastSqliteError(u8"检查 gpkg_tile_matrix 内容失败");
        }
        std::map<std::string, GeoGpkgTileMatrixResolution> previousResolutionByTable;
        for (std::size_t rowIndex = 0; rowIndex < tileMatrixResult.rows.size(); rowIndex++)
        {
            std::string tableNameUtf8;
            std::string dataTypeUtf8;
            GeoGpkgTileMatrixInfo tileMatrixInfo;
            GB_Rectangle matrixSetEnvelope;
            int contentSrsId = 0;
            int matrixSetSrsId = 0;
            QueryStringColumn(tileMatrixResult, rowIndex, "table_name", tableNameUtf8);
            QueryStringColumn(tileMatrixResult, rowIndex, "data_type", dataTypeUtf8);
            QueryIntColumn(tileMatrixResult, rowIndex, "zoom_level", tileMatrixInfo.zoomLevel);
            QueryIntColumn(tileMatrixResult, rowIndex, "matrix_width", tileMatrixInfo.matrixWidth);
            QueryIntColumn(tileMatrixResult, rowIndex, "matrix_height", tileMatrixInfo.matrixHeight);
            QueryIntColumn(tileMatrixResult, rowIndex, "tile_width", tileMatrixInfo.tileWidth);
            QueryIntColumn(tileMatrixResult, rowIndex, "tile_height", tileMatrixInfo.tileHeight);
            QueryDoubleColumn(tileMatrixResult, rowIndex, "pixel_x_size", tileMatrixInfo.pixelXSize);
            QueryDoubleColumn(tileMatrixResult, rowIndex, "pixel_y_size", tileMatrixInfo.pixelYSize);
            QueryDoubleColumn(tileMatrixResult, rowIndex, "min_x", matrixSetEnvelope.minX);
            QueryDoubleColumn(tileMatrixResult, rowIndex, "min_y", matrixSetEnvelope.minY);
            QueryDoubleColumn(tileMatrixResult, rowIndex, "max_x", matrixSetEnvelope.maxX);
            QueryDoubleColumn(tileMatrixResult, rowIndex, "max_y", matrixSetEnvelope.maxY);
            QueryIntColumn(tileMatrixResult, rowIndex, "content_srs_id", contentSrsId);
            QueryIntColumn(tileMatrixResult, rowIndex, "matrix_set_srs_id", matrixSetSrsId);

            if (dataTypeUtf8 != "tiles" || contentSrsId != matrixSetSrsId)
            {
                hasError = true;
                if (outMessages != nullptr)
                {
                    outMessages->push_back(u8"gpkg_tile_matrix / gpkg_tile_matrix_set 与 gpkg_contents 的 tiles 记录或 srs_id 不一致：" + tableNameUtf8);
                }
            }

            tileMatrixInfo.tableNameUtf8 = tableNameUtf8;
            if (tileMatrixInfo.zoomLevel < 0 || tileMatrixInfo.matrixWidth < 1 || tileMatrixInfo.matrixHeight < 1 || tileMatrixInfo.tileWidth < 1 || tileMatrixInfo.tileHeight < 1 || tileMatrixInfo.pixelXSize <= 0.0 || tileMatrixInfo.pixelYSize <= 0.0 || !IsFinite(tileMatrixInfo.pixelXSize) || !IsFinite(tileMatrixInfo.pixelYSize))
            {
                hasError = true;
                if (outMessages != nullptr)
                {
                    outMessages->push_back(u8"gpkg_tile_matrix 存在非法瓦片矩阵参数：" + tableNameUtf8 + u8" zoom=" + ToString(tileMatrixInfo.zoomLevel));
                }
                continue;
            }
            if (!IsTileMatrixRangeCompatible(matrixSetEnvelope, tileMatrixInfo))
            {
                hasError = true;
                if (outMessages != nullptr)
                {
                    outMessages->push_back(u8"gpkg_tile_matrix 的矩阵尺寸、瓦片尺寸和像素大小与 gpkg_tile_matrix_set 范围不一致：" + tableNameUtf8 + u8" zoom=" + ToString(tileMatrixInfo.zoomLevel));
                }
            }

            const std::map<std::string, GeoGpkgTileMatrixResolution>::const_iterator previousIter = previousResolutionByTable.find(tableNameUtf8);
            if (previousIter != previousResolutionByTable.end() && ((tileMatrixInfo.pixelXSize > previousIter->second.pixelXSize && !NearlyEqualDouble(tileMatrixInfo.pixelXSize, previousIter->second.pixelXSize)) || (tileMatrixInfo.pixelYSize > previousIter->second.pixelYSize && !NearlyEqualDouble(tileMatrixInfo.pixelYSize, previousIter->second.pixelYSize))))
            {
                hasError = true;
                if (outMessages != nullptr)
                {
                    outMessages->push_back(u8"gpkg_tile_matrix 的 pixel_x_size / pixel_y_size 未按 zoom_level 升序非递增：" + tableNameUtf8);
                }
            }

            GeoGpkgTileMatrixResolution resolution;
            resolution.zoomLevel = tileMatrixInfo.zoomLevel;
            resolution.pixelXSize = tileMatrixInfo.pixelXSize;
            resolution.pixelYSize = tileMatrixInfo.pixelYSize;
            previousResolutionByTable[tableNameUtf8] = resolution;
        }
    }

    return !hasError;
}

bool GeoGpkg::AddOrUpdateSpatialRefSys(const GeoGpkgSpatialRefSys& srs)
{
    GB_WriteLockGuard guard(lock_);
    ClearLastError();

    if (!database_.IsOpen())
    {
        return SetLastError(u8"GeoPackage 尚未打开。 ");
    }
    if (database_.IsReadOnly())
    {
        return SetLastError(u8"当前 GeoPackage 以只读方式打开，不能写入空间参考。 ");
    }

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

    if (!database_.IsOpen())
    {
        return SetLastError(u8"GeoPackage 尚未打开。 ");
    }
    if (database_.IsReadOnly())
    {
        return SetLastError(u8"当前 GeoPackage 以只读方式打开，不能创建矢量图层。 ");
    }

    if (!IsValidUserContentTableName(tableNameUtf8) || !IsValidIdentifier(geometryColumnNameUtf8) || !IsValidIdentifier(primaryKeyColumnNameUtf8))
    {
        return SetLastError(u8"图层名、几何字段名或主键字段名非法，用户内容表不能使用 sqlite_、gpkg_ 或 rtree_ 前缀。 ");
    }

    if (ToLowerAsciiString(geometryColumnNameUtf8) == ToLowerAsciiString(primaryKeyColumnNameUtf8))
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
        std::string normalizedFieldTypeSqlUtf8;
        if (!NormalizeGeoPackageFieldType(field.typeUtf8, normalizedFieldTypeSqlUtf8))
        {
            return SetLastError(u8"不支持的 GeoPackage 字段类型：" + field.typeUtf8);
        }
        if (!IsValidSqlDefaultExpression(field.defaultSqlUtf8))
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
            stream << QuoteIdentifier(primaryKeyColumnNameUtf8) << " INTEGER PRIMARY KEY";
            stream << ", " << QuoteIdentifier(geometryColumnNameUtf8) << " " << geometryType;
            for (std::size_t index = 0; index < fields.size(); index++)
            {
                const GeoGpkgFieldDef& field = fields[index];
                std::string normalizedFieldTypeSqlUtf8;
                NormalizeGeoPackageFieldType(field.typeUtf8, normalizedFieldTypeSqlUtf8);
                stream << ", " << QuoteIdentifier(field.nameUtf8) << " " << normalizedFieldTypeSqlUtf8;
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

    std::vector<std::string> oldSpatialIndexTriggerNames;
    std::vector<std::string> oldTileConstraintTriggerNames;
    std::string oldRtreeNameUtf8;
    if (overwrite)
    {
        GeoGpkgFeatureLayerInfo oldLayerInfo;
        if (GetFeatureLayerInfoNoLock(tableNameUtf8, oldLayerInfo))
        {
            BuildSpatialIndexTriggerNames(oldLayerInfo.tableNameUtf8, oldLayerInfo.geometryColumnNameUtf8, oldSpatialIndexTriggerNames);
            oldRtreeNameUtf8 = MakeRTreeName(oldLayerInfo.tableNameUtf8, oldLayerInfo.geometryColumnNameUtf8);
        }
        BuildTileConstraintTriggerNames(tableNameUtf8, oldTileConstraintTriggerNames);
        ClearLastError();
    }

    if (!database_.ExecuteInTransaction([&](GB_SqliteTransaction& transaction) -> bool
        {
            if (overwrite)
            {
                for (std::size_t index = 0; index < oldSpatialIndexTriggerNames.size(); index++)
                {
                    if (!transaction.Execute("DROP TRIGGER IF EXISTS " + QuoteIdentifier(oldSpatialIndexTriggerNames[index])))
                    {
                        return false;
                    }
                }
                for (std::size_t index = 0; index < oldTileConstraintTriggerNames.size(); index++)
                {
                    if (!transaction.Execute("DROP TRIGGER IF EXISTS " + QuoteIdentifier(oldTileConstraintTriggerNames[index])))
                    {
                        return false;
                    }
                }
                if (!oldRtreeNameUtf8.empty() && !transaction.Execute("DROP TABLE IF EXISTS " + QuoteIdentifier(oldRtreeNameUtf8)))
                {
                    return false;
                }
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
                extensionParameters.push_back("gpkg_geom_" + geometryType);
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

        if (!FindTablePrimaryKeyColumn(database_, layer.tableNameUtf8, layer.primaryKeyColumnNameUtf8))
        {
            return SetLastError(u8"矢量图层缺少 INTEGER PRIMARY KEY 字段：" + layer.tableNameUtf8);
        }
        if (!database_.GetTableFieldInfos(layer.tableNameUtf8, layer.fields, false))
        {
            return SetLastSqliteError(u8"查询图层字段失败：" + layer.tableNameUtf8);
        }
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
    if (!database_.IsOpen())
    {
        return SetLastError(u8"GeoPackage 尚未打开。 ");
    }
    if (database_.IsReadOnly())
    {
        return SetLastError(u8"当前 GeoPackage 以只读方式打开，不能插入要素。 ");
    }
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
    std::map<std::string, std::string> insertSqlCacheUtf8;
    std::vector<std::string> featureSqlKeys;
    featureSqlKeys.reserve(features.size());

    GB_Rectangle insertedEnvelope;
    bool hasInsertedEnvelope = false;
    std::string validationErrorUtf8;
    for (std::size_t featureIndex = 0; featureIndex < features.size(); featureIndex++)
    {
        const GeoGpkgFeature& feature = features[featureIndex];
        std::set<std::string> usedAttributeNames;
        for (std::map<std::string, GB_Variant>::const_iterator iter = feature.attributes.begin(); iter != feature.attributes.end(); ++iter)
        {
            const std::string lowerAttributeNameUtf8 = ToLowerAsciiString(iter->first);
            if (!usedAttributeNames.insert(lowerAttributeNameUtf8).second)
            {
                return SetLastError(u8"要素属性字段重复：" + iter->first);
            }
            if (!IsValidIdentifier(iter->first) || writableAttributeNames.find(lowerAttributeNameUtf8) == writableAttributeNames.end())
            {
                return SetLastError(u8"要素属性字段不存在或不可写：" + iter->first);
            }
        }

        if (!feature.geometry.empty())
        {
            GeoGpkgGeometryHeaderInfo headerInfo;
            GB_Rectangle featureEnvelope;
            bool isEmptyGeometry = false;
            std::string geometryCompatibilityErrorUtf8;
            if (!AnalyzeGpkgGeometryForLayer(layerInfo, feature.geometry, headerInfo, featureEnvelope, isEmptyGeometry, geometryCompatibilityErrorUtf8))
            {
                return SetLastError(geometryCompatibilityErrorUtf8.empty() ? u8"要素 GeoPackageBinary 与图层登记信息不兼容。" : geometryCompatibilityErrorUtf8);
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
        }

        const std::string sqlKeyUtf8 = BuildFeatureAttributeSqlKey(feature.attributes);
        if (insertSqlCacheUtf8.find(sqlKeyUtf8) == insertSqlCacheUtf8.end())
        {
            std::string insertSqlUtf8;
            if (!AppendFeatureInsertSql(layerInfo, feature.attributes, writableAttributeNames, insertSqlUtf8))
            {
                return SetLastError(u8"构造要素插入 SQL 失败。");
            }
            insertSqlCacheUtf8.insert(std::make_pair(sqlKeyUtf8, insertSqlUtf8));
        }
        featureSqlKeys.push_back(sqlKeyUtf8);
    }

    std::vector<long long> insertedRowIds;
    if (outRowIds != nullptr)
    {
        insertedRowIds.reserve(features.size());
    }

    if (!database_.ExecuteInTransaction([&](GB_SqliteTransaction& transaction) -> bool
        {
            for (std::size_t featureIndex = 0; featureIndex < features.size(); featureIndex++)
            {
                const std::map<std::string, std::string>::const_iterator sqlIter = insertSqlCacheUtf8.find(featureSqlKeys[featureIndex]);
                if (sqlIter == insertSqlCacheUtf8.end())
                {
                    validationErrorUtf8 = u8"内部错误：要素插入 SQL 缓存缺失。";
                    return false;
                }

                const GeoGpkgFeature& feature = features[featureIndex];
                if (!transaction.Execute(sqlIter->second, BuildFeatureInsertParameters(feature.geometry, feature.attributes)))
                {
                    return false;
                }

                if (outRowIds != nullptr)
                {
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
    const std::string featureRtreeNameUtf8 = MakeRTreeName(layerInfo.tableNameUtf8, layerInfo.geometryColumnNameUtf8);
    if (!database_.TableExists(featureRtreeNameUtf8, hasSpatialIndex, true))
    {
        return SetLastSqliteError(u8"检查空间索引失败");
    }

    if (maxFeatureCount > 0)
    {
        outFeatures.reserve(std::min<std::size_t>(maxFeatureCount, GB_GpkgMaxQueryReserveCount));
    }

    if (hasSpatialIndex)
    {
        std::ostringstream stream;
        stream << "SELECT f.* FROM " << QuoteIdentifier(layerInfo.tableNameUtf8) << " f "
            << "JOIN " << QuoteIdentifier(featureRtreeNameUtf8) << " r ON f." << QuoteIdentifier(layerInfo.primaryKeyColumnNameUtf8) << " = r.id "
            << "WHERE r.minx <= ? AND r.maxx >= ? AND r.miny <= ? AND r.maxy >= ?";
        const GB_Rectangle queryEnvelope = ExpandRectangleForRTreeQuery(envelope);
        GB_SqliteParameterList parameters;
        parameters.push_back(queryEnvelope.maxX);
        parameters.push_back(queryEnvelope.minX);
        parameters.push_back(queryEnvelope.maxY);
        parameters.push_back(queryEnvelope.minY);

        bool buildFeatureFailed = false;
        bool geometryEnvelopeFailed = false;
        bool queryReachedLimit = false;
        std::string geometryEnvelopeErrorMessageUtf8;
        const bool queryOk = database_.QueryEach(stream.str(), parameters, [&](const std::vector<GB_SqliteColumnInfo>& columns, const std::vector<GB_Variant>& values) -> bool
            {
                GeoGpkgFeature feature;
                if (!BuildFeatureFromValues(columns, values, layerInfo, feature))
                {
                    buildFeatureFailed = true;
                    return false;
                }

                if (feature.geometry.empty())
                {
                    return true;
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
                    outFeatures.push_back(std::move(feature));
                    if (maxFeatureCount > 0 && outFeatures.size() >= maxFeatureCount)
                    {
                        queryReachedLimit = true;
                        return false;
                    }
                }
                return true;
            });
        if (buildFeatureFailed)
        {
            return SetLastError(u8"构造要素结果失败。 ");
        }
        if (geometryEnvelopeFailed)
        {
            return SetLastError(u8"计算要素几何范围失败：" + geometryEnvelopeErrorMessageUtf8);
        }
        if (!queryOk && !queryReachedLimit)
        {
            return SetLastSqliteError(u8"使用 RTree 查询要素失败");
        }
        return true;
    }

    bool buildFeatureFailed = false;
    bool geometryEnvelopeFailed = false;
    bool queryReachedLimit = false;
    std::string geometryEnvelopeErrorMessageUtf8;
    const std::string sql = "SELECT * FROM " + QuoteIdentifier(layerInfo.tableNameUtf8);
    const bool queryOk = database_.QueryEach(sql, [&](const std::vector<GB_SqliteColumnInfo>& columns, const std::vector<GB_Variant>& values) -> bool
        {
            GeoGpkgFeature feature;
            if (!BuildFeatureFromValues(columns, values, layerInfo, feature))
            {
                buildFeatureFailed = true;
                return false;
            }
            if (feature.geometry.empty())
            {
                return true;
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

            outFeatures.push_back(std::move(feature));
            if (maxFeatureCount > 0 && outFeatures.size() >= maxFeatureCount)
            {
                queryReachedLimit = true;
                return false;
            }
            return true;
        });
    if (buildFeatureFailed)
    {
        return SetLastError(u8"构造要素结果失败。 ");
    }
    if (geometryEnvelopeFailed)
    {
        return SetLastError(u8"计算要素几何范围失败：" + geometryEnvelopeErrorMessageUtf8);
    }
    if (!queryOk && !queryReachedLimit)
    {
        return SetLastSqliteError(u8"查询要素失败");
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
    if (!GetFeatureLayerInfoNoLock(tableNameUtf8, layerInfo))
    {
        return false;
    }
    const std::string requestedGeometryColumnName = geometryColumnNameUtf8.empty() ? layerInfo.geometryColumnNameUtf8 : geometryColumnNameUtf8;
    if (ToLowerAsciiString(requestedGeometryColumnName) != ToLowerAsciiString(layerInfo.geometryColumnNameUtf8))
    {
        return SetLastError(u8"指定几何字段不是 gpkg_geometry_columns 中登记的几何字段。 ");
    }

    const std::string rtreeNameUtf8 = MakeRTreeName(layerInfo.tableNameUtf8, layerInfo.geometryColumnNameUtf8);
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

    if (!database_.IsOpen())
    {
        return SetLastError(u8"GeoPackage 尚未打开。 ");
    }
    if (database_.IsReadOnly())
    {
        return SetLastError(u8"当前 GeoPackage 以只读方式打开，不能创建空间索引。 ");
    }

    GeoGpkgFeatureLayerInfo layerInfo;
    if (!GetFeatureLayerInfoNoLock(tableNameUtf8, layerInfo))
    {
        return false;
    }
    const std::string requestedGeometryColumnName = geometryColumnNameUtf8.empty() ? layerInfo.geometryColumnNameUtf8 : geometryColumnNameUtf8;
    if (ToLowerAsciiString(requestedGeometryColumnName) != ToLowerAsciiString(layerInfo.geometryColumnNameUtf8))
    {
        return SetLastError(u8"指定几何字段不是 gpkg_geometry_columns 中登记的几何字段。 ");
    }
    const std::string geometryColumnName = layerInfo.geometryColumnNameUtf8;

    const std::string rtreeNameUtf8 = MakeRTreeName(layerInfo.tableNameUtf8, geometryColumnName);
    const std::string quotedRtreeName = QuoteIdentifier(rtreeNameUtf8);
    const std::string quotedTableName = QuoteIdentifier(layerInfo.tableNameUtf8);
    const std::string quotedGeometryColumnName = QuoteIdentifier(geometryColumnName);
    const std::string quotedPrimaryKeyColumnName = QuoteIdentifier(layerInfo.primaryKeyColumnNameUtf8);
    const std::string triggerInsertName = QuoteIdentifier(MakeTriggerName(layerInfo.tableNameUtf8, geometryColumnName, "insert"));
    const std::string triggerUpdate2Name = QuoteIdentifier(MakeTriggerName(layerInfo.tableNameUtf8, geometryColumnName, "update2"));
    const std::string triggerUpdate4Name = QuoteIdentifier(MakeTriggerName(layerInfo.tableNameUtf8, geometryColumnName, "update4"));
    const std::string triggerUpdate5Name = QuoteIdentifier(MakeTriggerName(layerInfo.tableNameUtf8, geometryColumnName, "update5"));
    const std::string triggerUpdate6Name = QuoteIdentifier(MakeTriggerName(layerInfo.tableNameUtf8, geometryColumnName, "update6"));
    const std::string triggerUpdate7Name = QuoteIdentifier(MakeTriggerName(layerInfo.tableNameUtf8, geometryColumnName, "update7"));
    const std::string triggerDeleteName = QuoteIdentifier(MakeTriggerName(layerInfo.tableNameUtf8, geometryColumnName, "delete"));

    std::vector<std::string> schemaSqlList;
    std::vector<std::string> triggerNames;
    BuildSpatialIndexTriggerNames(layerInfo.tableNameUtf8, geometryColumnName, triggerNames);
    schemaSqlList.reserve(triggerNames.size() + 9);
    for (std::size_t index = 0; index < triggerNames.size(); index++)
    {
        schemaSqlList.push_back("DROP TRIGGER IF EXISTS " + QuoteIdentifier(triggerNames[index]));
    }
    schemaSqlList.push_back("DROP TABLE IF EXISTS " + quotedRtreeName);
    schemaSqlList.push_back("CREATE VIRTUAL TABLE " + quotedRtreeName + " USING rtree(id, minx, maxx, miny, maxy)");

    std::ostringstream insertTrigger;
    insertTrigger << "CREATE TRIGGER " << triggerInsertName << " AFTER INSERT ON " << quotedTableName << "\n"
        << "WHEN (NEW." << quotedGeometryColumnName << " IS NOT NULL AND NOT ST_IsEmpty(NEW." << quotedGeometryColumnName << "))\n"
        << "BEGIN\n"
        << "  INSERT OR REPLACE INTO " << quotedRtreeName << " VALUES (NEW." << quotedPrimaryKeyColumnName << ", ST_MinX(NEW." << quotedGeometryColumnName << "), ST_MaxX(NEW." << quotedGeometryColumnName << "), ST_MinY(NEW." << quotedGeometryColumnName << "), ST_MaxY(NEW." << quotedGeometryColumnName << "));\n"
        << "END";
    schemaSqlList.push_back(insertTrigger.str());

    std::ostringstream update2Trigger;
    update2Trigger << "CREATE TRIGGER " << triggerUpdate2Name << " AFTER UPDATE OF " << quotedGeometryColumnName << " ON " << quotedTableName << "\n"
        << "WHEN OLD." << quotedPrimaryKeyColumnName << " = NEW." << quotedPrimaryKeyColumnName << " AND (NEW." << quotedGeometryColumnName << " IS NULL OR ST_IsEmpty(NEW." << quotedGeometryColumnName << "))\n"
        << "BEGIN\n"
        << "  DELETE FROM " << quotedRtreeName << " WHERE id = OLD." << quotedPrimaryKeyColumnName << ";\n"
        << "END";
    schemaSqlList.push_back(update2Trigger.str());

    std::ostringstream update4Trigger;
    update4Trigger << "CREATE TRIGGER " << triggerUpdate4Name << " AFTER UPDATE ON " << quotedTableName << "\n"
        << "WHEN OLD." << quotedPrimaryKeyColumnName << " != NEW." << quotedPrimaryKeyColumnName << " AND (NEW." << quotedGeometryColumnName << " IS NULL OR ST_IsEmpty(NEW." << quotedGeometryColumnName << "))\n"
        << "BEGIN\n"
        << "  DELETE FROM " << quotedRtreeName << " WHERE id IN (OLD." << quotedPrimaryKeyColumnName << ", NEW." << quotedPrimaryKeyColumnName << ");\n"
        << "END";
    schemaSqlList.push_back(update4Trigger.str());

    std::ostringstream update5Trigger;
    update5Trigger << "CREATE TRIGGER " << triggerUpdate5Name << " AFTER UPDATE ON " << quotedTableName << "\n"
        << "WHEN OLD." << quotedPrimaryKeyColumnName << " != NEW." << quotedPrimaryKeyColumnName << " AND (NEW." << quotedGeometryColumnName << " IS NOT NULL AND NOT ST_IsEmpty(NEW." << quotedGeometryColumnName << "))\n"
        << "BEGIN\n"
        << "  DELETE FROM " << quotedRtreeName << " WHERE id = OLD." << quotedPrimaryKeyColumnName << ";\n"
        << "  INSERT OR REPLACE INTO " << quotedRtreeName << " VALUES (NEW." << quotedPrimaryKeyColumnName << ", ST_MinX(NEW." << quotedGeometryColumnName << "), ST_MaxX(NEW." << quotedGeometryColumnName << "), ST_MinY(NEW." << quotedGeometryColumnName << "), ST_MaxY(NEW." << quotedGeometryColumnName << "));\n"
        << "END";
    schemaSqlList.push_back(update5Trigger.str());

    std::ostringstream update6Trigger;
    update6Trigger << "CREATE TRIGGER " << triggerUpdate6Name << " AFTER UPDATE OF " << quotedGeometryColumnName << " ON " << quotedTableName << "\n"
        << "WHEN OLD." << quotedPrimaryKeyColumnName << " = NEW." << quotedPrimaryKeyColumnName << " AND (OLD." << quotedGeometryColumnName << " IS NOT NULL AND NOT ST_IsEmpty(OLD." << quotedGeometryColumnName << ")) AND (NEW." << quotedGeometryColumnName << " IS NOT NULL AND NOT ST_IsEmpty(NEW." << quotedGeometryColumnName << "))\n"
        << "BEGIN\n"
        << "  UPDATE " << quotedRtreeName << " SET minx = ST_MinX(NEW." << quotedGeometryColumnName << "), maxx = ST_MaxX(NEW." << quotedGeometryColumnName << "), miny = ST_MinY(NEW." << quotedGeometryColumnName << "), maxy = ST_MaxY(NEW." << quotedGeometryColumnName << ") WHERE id = NEW." << quotedPrimaryKeyColumnName << ";\n"
        << "END";
    schemaSqlList.push_back(update6Trigger.str());

    std::ostringstream update7Trigger;
    update7Trigger << "CREATE TRIGGER " << triggerUpdate7Name << " AFTER UPDATE OF " << quotedGeometryColumnName << " ON " << quotedTableName << "\n"
        << "WHEN OLD." << quotedPrimaryKeyColumnName << " = NEW." << quotedPrimaryKeyColumnName << " AND (OLD." << quotedGeometryColumnName << " IS NULL OR ST_IsEmpty(OLD." << quotedGeometryColumnName << ")) AND (NEW." << quotedGeometryColumnName << " IS NOT NULL AND NOT ST_IsEmpty(NEW." << quotedGeometryColumnName << "))\n"
        << "BEGIN\n"
        << "  INSERT INTO " << quotedRtreeName << " VALUES (NEW." << quotedPrimaryKeyColumnName << ", ST_MinX(NEW." << quotedGeometryColumnName << "), ST_MaxX(NEW." << quotedGeometryColumnName << "), ST_MinY(NEW." << quotedGeometryColumnName << "), ST_MaxY(NEW." << quotedGeometryColumnName << "));\n"
        << "END";
    schemaSqlList.push_back(update7Trigger.str());

    std::ostringstream deleteTrigger;
    deleteTrigger << "CREATE TRIGGER " << triggerDeleteName << " AFTER DELETE ON " << quotedTableName << "\n"
        << "WHEN OLD." << quotedGeometryColumnName << " IS NOT NULL\n"
        << "BEGIN\n"
        << "  DELETE FROM " << quotedRtreeName << " WHERE id = OLD." << quotedPrimaryKeyColumnName << ";\n"
        << "END";
    schemaSqlList.push_back(deleteTrigger.str());

    GB_SqliteParameterList extensionParameters;
    extensionParameters.push_back(layerInfo.tableNameUtf8);
    extensionParameters.push_back(geometryColumnName);
    extensionParameters.push_back("gpkg_rtree_index");
    extensionParameters.push_back("http://www.geopackage.org/spec/#extension_rtree");
    extensionParameters.push_back("write-only");

    const std::string clearSql = "DELETE FROM " + quotedRtreeName;
    const std::string insertSql = "INSERT OR REPLACE INTO " + quotedRtreeName + "(id, minx, maxx, miny, maxy) SELECT " + quotedPrimaryKeyColumnName + ", ST_MinX(" + quotedGeometryColumnName + "), ST_MaxX(" + quotedGeometryColumnName + "), ST_MinY(" + quotedGeometryColumnName + "), ST_MaxY(" + quotedGeometryColumnName + ") FROM " + quotedTableName + " WHERE " + quotedGeometryColumnName + " IS NOT NULL AND NOT ST_IsEmpty(" + quotedGeometryColumnName + ")";

    if (!database_.ExecuteInTransaction([&](GB_SqliteTransaction& transaction) -> bool
        {
            for (std::size_t index = 0; index < schemaSqlList.size(); index++)
            {
                if (!transaction.Execute(schemaSqlList[index]))
                {
                    return false;
                }
            }
            if (!transaction.Execute("INSERT OR REPLACE INTO gpkg_extensions(table_name, column_name, extension_name, definition, scope) VALUES(?, ?, ?, ?, ?)", extensionParameters))
            {
                return false;
            }
            if (!transaction.Execute(clearSql))
            {
                return false;
            }
            if (!transaction.Execute(insertSql))
            {
                return false;
            }
            return transaction.Execute("UPDATE gpkg_contents SET last_change = strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE table_name = ?", GB_SqliteParameterList(1, layerInfo.tableNameUtf8));
        }, GB_SqliteTransactionMode::Immediate))
    {
        return SetLastSqliteError(u8"创建或重建 RTree 空间索引失败");
    }

    return true;
}

bool GeoGpkg::DropSpatialIndex(const std::string& tableNameUtf8, const std::string& geometryColumnNameUtf8)
{
    GB_WriteLockGuard guard(lock_);
    return DropSpatialIndexNoLock(tableNameUtf8, geometryColumnNameUtf8);
}

bool GeoGpkg::DropSpatialIndexNoLock(const std::string& tableNameUtf8, const std::string& geometryColumnNameUtf8)
{
    ClearLastError();

    if (!database_.IsOpen())
    {
        return SetLastError(u8"GeoPackage 尚未打开。 ");
    }
    if (database_.IsReadOnly())
    {
        return SetLastError(u8"当前 GeoPackage 以只读方式打开，不能删除空间索引。 ");
    }

    GeoGpkgFeatureLayerInfo layerInfo;
    if (!GetFeatureLayerInfoNoLock(tableNameUtf8, layerInfo))
    {
        return false;
    }
    const std::string requestedGeometryColumnName = geometryColumnNameUtf8.empty() ? layerInfo.geometryColumnNameUtf8 : geometryColumnNameUtf8;
    if (ToLowerAsciiString(requestedGeometryColumnName) != ToLowerAsciiString(layerInfo.geometryColumnNameUtf8))
    {
        return SetLastError(u8"指定几何字段不是 gpkg_geometry_columns 中登记的几何字段。 ");
    }
    const std::string geometryColumnName = layerInfo.geometryColumnNameUtf8;
    const std::string rtreeNameUtf8 = MakeRTreeName(layerInfo.tableNameUtf8, geometryColumnName);

    std::vector<std::string> triggerNames;
    BuildSpatialIndexTriggerNames(layerInfo.tableNameUtf8, geometryColumnName, triggerNames);

    GB_SqliteParameterList parameters;
    parameters.push_back(layerInfo.tableNameUtf8);
    parameters.push_back(geometryColumnName);
    parameters.push_back("gpkg_rtree_index");
    if (!database_.ExecuteInTransaction([&](GB_SqliteTransaction& transaction) -> bool
        {
            for (std::size_t index = 0; index < triggerNames.size(); index++)
            {
                if (!transaction.Execute("DROP TRIGGER IF EXISTS " + QuoteIdentifier(triggerNames[index])))
                {
                    return false;
                }
            }
            if (!transaction.Execute("DROP TABLE IF EXISTS " + QuoteIdentifier(rtreeNameUtf8)))
            {
                return false;
            }
            if (!transaction.Execute("DELETE FROM gpkg_extensions WHERE table_name = ? AND column_name = ? AND extension_name = ?", parameters))
            {
                return false;
            }
            return transaction.Execute("UPDATE gpkg_contents SET last_change = strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE table_name = ?", GB_SqliteParameterList(1, layerInfo.tableNameUtf8));
        }, GB_SqliteTransactionMode::Immediate))
    {
        return SetLastSqliteError(u8"删除空间索引失败");
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

    if (!database_.IsOpen())
    {
        return SetLastError(u8"GeoPackage 尚未打开。 ");
    }
    if (database_.IsReadOnly())
    {
        return SetLastError(u8"当前 GeoPackage 以只读方式打开，不能重建空间索引。 ");
    }

    GeoGpkgFeatureLayerInfo layerInfo;
    if (!GetFeatureLayerInfoNoLock(tableNameUtf8, layerInfo))
    {
        return false;
    }
    const std::string requestedGeometryColumnName = geometryColumnNameUtf8.empty() ? layerInfo.geometryColumnNameUtf8 : geometryColumnNameUtf8;
    if (ToLowerAsciiString(requestedGeometryColumnName) != ToLowerAsciiString(layerInfo.geometryColumnNameUtf8))
    {
        return SetLastError(u8"指定几何字段不是 gpkg_geometry_columns 中登记的几何字段。 ");
    }
    const std::string geometryColumnName = layerInfo.geometryColumnNameUtf8;

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
            if (!transaction.Execute(insertSql))
            {
                return false;
            }
            return transaction.Execute("UPDATE gpkg_contents SET last_change = strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE table_name = ?", GB_SqliteParameterList(1, layerInfo.tableNameUtf8));
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

    if (!database_.IsOpen())
    {
        return SetLastError(u8"GeoPackage 尚未打开。 ");
    }
    if (database_.IsReadOnly())
    {
        return SetLastError(u8"当前 GeoPackage 以只读方式打开，不能创建瓦片矩阵集。 ");
    }

    if (!IsValidUserContentTableName(tableNameUtf8))
    {
        return SetLastError(u8"瓦片矩阵集表名非法，用户内容表不能使用 sqlite_、gpkg_ 或 rtree_ 前缀。 ");
    }
    if (!envelope.IsValid() || GetRectangleWidth(envelope) <= 0.0 || GetRectangleHeight(envelope) <= 0.0 || !IsFinite(envelope.minX) || !IsFinite(envelope.minY) || !IsFinite(envelope.maxX) || !IsFinite(envelope.maxY))
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
    const std::string createSql = "CREATE TABLE " + QuoteIdentifier(tableNameUtf8) + " (id INTEGER PRIMARY KEY, zoom_level INTEGER NOT NULL CHECK (zoom_level >= 0), tile_column INTEGER NOT NULL CHECK (tile_column >= 0), tile_row INTEGER NOT NULL CHECK (tile_row >= 0), tile_data BLOB NOT NULL, UNIQUE (zoom_level, tile_column, tile_row))";

    std::vector<std::string> oldSpatialIndexTriggerNames;
    std::vector<std::string> oldTileConstraintTriggerNames;
    std::string oldRtreeNameUtf8;
    if (overwrite)
    {
        GeoGpkgFeatureLayerInfo oldLayerInfo;
        if (GetFeatureLayerInfoNoLock(tableNameUtf8, oldLayerInfo))
        {
            BuildSpatialIndexTriggerNames(oldLayerInfo.tableNameUtf8, oldLayerInfo.geometryColumnNameUtf8, oldSpatialIndexTriggerNames);
            oldRtreeNameUtf8 = MakeRTreeName(oldLayerInfo.tableNameUtf8, oldLayerInfo.geometryColumnNameUtf8);
        }
        BuildTileConstraintTriggerNames(tableNameUtf8, oldTileConstraintTriggerNames);
        ClearLastError();
    }

    std::vector<std::string> tileConstraintSqlList;
    BuildTileConstraintTriggerSqlList(tableNameUtf8, tileConstraintSqlList);

    if (!database_.ExecuteInTransaction([&](GB_SqliteTransaction& transaction) -> bool
        {
            if (overwrite)
            {
                for (std::size_t index = 0; index < oldSpatialIndexTriggerNames.size(); index++)
                {
                    if (!transaction.Execute("DROP TRIGGER IF EXISTS " + QuoteIdentifier(oldSpatialIndexTriggerNames[index])))
                    {
                        return false;
                    }
                }
                for (std::size_t index = 0; index < oldTileConstraintTriggerNames.size(); index++)
                {
                    if (!transaction.Execute("DROP TRIGGER IF EXISTS " + QuoteIdentifier(oldTileConstraintTriggerNames[index])))
                    {
                        return false;
                    }
                }
                if (!oldRtreeNameUtf8.empty() && !transaction.Execute("DROP TABLE IF EXISTS " + QuoteIdentifier(oldRtreeNameUtf8)))
                {
                    return false;
                }
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
            for (std::size_t index = 0; index < tileConstraintSqlList.size(); index++)
            {
                if (!transaction.Execute(tileConstraintSqlList[index]))
                {
                    return false;
                }
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
    const std::string sql =
        "SELECT t.table_name, t.srs_id, t.min_x, t.min_y, t.max_x, t.max_y "
        "FROM gpkg_tile_matrix_set t JOIN gpkg_contents c ON t.table_name = c.table_name "
        "WHERE t.table_name = ? AND c.data_type = 'tiles'";
    if (!database_.Query(sql, GB_SqliteParameterList(1, tableNameUtf8), result, 1))
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

    if (!database_.IsOpen())
    {
        return SetLastError(u8"GeoPackage 尚未打开。 ");
    }
    if (database_.IsReadOnly())
    {
        return SetLastError(u8"当前 GeoPackage 以只读方式打开，不能写入瓦片矩阵。 ");
    }

    if (!IsValidUserContentTableName(tileMatrixInfo.tableNameUtf8) || tileMatrixInfo.zoomLevel < 0 || tileMatrixInfo.matrixWidth < 1 || tileMatrixInfo.matrixHeight < 1 || tileMatrixInfo.tileWidth < 1 || tileMatrixInfo.tileHeight < 1 || tileMatrixInfo.pixelXSize <= 0.0 || tileMatrixInfo.pixelYSize <= 0.0 || !IsFinite(tileMatrixInfo.pixelXSize) || !IsFinite(tileMatrixInfo.pixelYSize))
    {
        return SetLastError(u8"瓦片矩阵参数无效。 ");
    }

    bool tileTableExists = false;
    if (!database_.TableExists(tileMatrixInfo.tableNameUtf8, tileTableExists, false))
    {
        return SetLastSqliteError(u8"检查瓦片用户表失败");
    }
    if (!tileTableExists)
    {
        return SetLastError(u8"瓦片矩阵对应的瓦片用户表不存在：" + tileMatrixInfo.tableNameUtf8);
    }

    GB_SqliteResult matrixSetResult;
    const std::string matrixSetSql =
        "SELECT s.min_x, s.min_y, s.max_x, s.max_y "
        "FROM gpkg_tile_matrix_set s JOIN gpkg_contents c ON s.table_name = c.table_name "
        "WHERE s.table_name = ? AND c.data_type = 'tiles'";
    if (!database_.Query(matrixSetSql, GB_SqliteParameterList(1, tileMatrixInfo.tableNameUtf8), matrixSetResult, 1))
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

    GB_SqliteParameterList existingMatrixParameters;
    existingMatrixParameters.push_back(tileMatrixInfo.tableNameUtf8);
    existingMatrixParameters.push_back(tileMatrixInfo.zoomLevel);

    GB_SqliteResult existingMatricesResult;
    if (!database_.Query("SELECT zoom_level, pixel_x_size, pixel_y_size FROM gpkg_tile_matrix WHERE table_name = ? AND zoom_level <> ?", existingMatrixParameters, existingMatricesResult))
    {
        return SetLastSqliteError(u8"检查瓦片矩阵分辨率顺序失败");
    }
    for (std::size_t rowIndex = 0; rowIndex < existingMatricesResult.rows.size(); rowIndex++)
    {
        int existingZoomLevel = 0;
        double existingPixelXSize = 0.0;
        double existingPixelYSize = 0.0;
        QueryIntColumn(existingMatricesResult, rowIndex, "zoom_level", existingZoomLevel);
        QueryDoubleColumn(existingMatricesResult, rowIndex, "pixel_x_size", existingPixelXSize);
        QueryDoubleColumn(existingMatricesResult, rowIndex, "pixel_y_size", existingPixelYSize);
        const bool existingLowerZoomHasSmallerPixelSize = existingZoomLevel < tileMatrixInfo.zoomLevel && ((existingPixelXSize < tileMatrixInfo.pixelXSize && !NearlyEqualDouble(existingPixelXSize, tileMatrixInfo.pixelXSize)) || (existingPixelYSize < tileMatrixInfo.pixelYSize && !NearlyEqualDouble(existingPixelYSize, tileMatrixInfo.pixelYSize)));
        const bool existingHigherZoomHasLargerPixelSize = existingZoomLevel > tileMatrixInfo.zoomLevel && ((existingPixelXSize > tileMatrixInfo.pixelXSize && !NearlyEqualDouble(existingPixelXSize, tileMatrixInfo.pixelXSize)) || (existingPixelYSize > tileMatrixInfo.pixelYSize && !NearlyEqualDouble(existingPixelYSize, tileMatrixInfo.pixelYSize)));
        if (existingLowerZoomHasSmallerPixelSize || existingHigherZoomHasLargerPixelSize)
        {
            return SetLastError(u8"瓦片矩阵 pixel_x_size / pixel_y_size 必须随 zoom_level 升序保持非递增。 ");
        }
    }

    std::vector<GeoGpkgTileMatrixResolution> resolutions;
    resolutions.reserve(existingMatricesResult.rows.size() + 1);
    GeoGpkgTileMatrixResolution newResolution;
    newResolution.zoomLevel = tileMatrixInfo.zoomLevel;
    newResolution.pixelXSize = tileMatrixInfo.pixelXSize;
    newResolution.pixelYSize = tileMatrixInfo.pixelYSize;
    resolutions.push_back(newResolution);
    for (std::size_t rowIndex = 0; rowIndex < existingMatricesResult.rows.size(); rowIndex++)
    {
        GeoGpkgTileMatrixResolution resolution;
        QueryIntColumn(existingMatricesResult, rowIndex, "zoom_level", resolution.zoomLevel);
        QueryDoubleColumn(existingMatricesResult, rowIndex, "pixel_x_size", resolution.pixelXSize);
        QueryDoubleColumn(existingMatricesResult, rowIndex, "pixel_y_size", resolution.pixelYSize);
        resolutions.push_back(resolution);
    }
    const bool needZoomOtherExtension = NeedsZoomOtherExtension(resolutions);

    bool hasInvalidExistingTiles = false;
    if (!HasTileRowsOutsideMatrix(database_, tileMatrixInfo.tableNameUtf8, tileMatrixInfo.zoomLevel, tileMatrixInfo.matrixWidth, tileMatrixInfo.matrixHeight, hasInvalidExistingTiles))
    {
        return SetLastSqliteError(u8"检查已有瓦片范围失败");
    }
    if (hasInvalidExistingTiles)
    {
        return SetLastError(u8"已有瓦片行列号超出新的瓦片矩阵范围，不能写入该矩阵定义。 ");
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
    if (!database_.ExecuteInTransaction([&](GB_SqliteTransaction& transaction) -> bool
        {
            if (!transaction.Execute(sql, parameters))
            {
                return false;
            }
            if (!AppendOrRemoveZoomOtherExtensionSql(transaction, tileMatrixInfo.tableNameUtf8, needZoomOtherExtension))
            {
                return false;
            }
            return transaction.Execute("UPDATE gpkg_contents SET last_change = strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE table_name = ?", GB_SqliteParameterList(1, tileMatrixInfo.tableNameUtf8));
        }, GB_SqliteTransactionMode::Immediate))
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

    if (!database_.IsOpen())
    {
        return SetLastError(u8"GeoPackage 尚未打开。 ");
    }
    if (database_.IsReadOnly())
    {
        return SetLastError(u8"当前 GeoPackage 以只读方式打开，不能写入瓦片。 ");
    }

    if (!IsValidUserContentTableName(tableNameUtf8) || zoomLevel < 0 || tileColumn < 0 || tileRow < 0 || tileData.empty())
    {
        return SetLastError(u8"瓦片参数无效。 ");
    }

    GB_SqliteParameterList matrixParameters;
    matrixParameters.push_back(tableNameUtf8);
    matrixParameters.push_back(zoomLevel);
    GB_SqliteResult matrixResult;
    const std::string matrixSql =
        "SELECT m.matrix_width, m.matrix_height "
        "FROM gpkg_tile_matrix m JOIN gpkg_contents c ON m.table_name = c.table_name "
        "WHERE m.table_name = ? AND m.zoom_level = ? AND c.data_type = 'tiles'";
    if (!database_.Query(matrixSql, matrixParameters, matrixResult, 1))
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
    if (!database_.ExecuteInTransaction([&](GB_SqliteTransaction& transaction) -> bool
        {
            if (!transaction.Execute(sql, parameters))
            {
                return false;
            }
            return transaction.Execute("UPDATE gpkg_contents SET last_change = strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE table_name = ?", GB_SqliteParameterList(1, tableNameUtf8));
        }, GB_SqliteTransactionMode::Immediate))
    {
        return SetLastSqliteError(u8"写入瓦片失败");
    }
    return true;
}

bool GeoGpkg::PutTiles(const std::string& tableNameUtf8, const std::vector<GeoGpkgTile>& tiles)
{
    GB_WriteLockGuard guard(lock_);
    ClearLastError();

    if (!database_.IsOpen())
    {
        return SetLastError(u8"GeoPackage 尚未打开。 ");
    }
    if (database_.IsReadOnly())
    {
        return SetLastError(u8"当前 GeoPackage 以只读方式打开，不能批量写入瓦片。 ");
    }

    if (!IsValidUserContentTableName(tableNameUtf8))
    {
        return SetLastError(u8"瓦片表名非法。 ");
    }
    if (tiles.empty())
    {
        return true;
    }

    GB_SqliteResult matrixResult;
    const std::string matrixSql =
        "SELECT m.zoom_level, m.matrix_width, m.matrix_height "
        "FROM gpkg_tile_matrix m JOIN gpkg_contents c ON m.table_name = c.table_name "
        "WHERE m.table_name = ? AND c.data_type = 'tiles'";
    if (!database_.Query(matrixSql, GB_SqliteParameterList(1, tableNameUtf8), matrixResult))
    {
        return SetLastSqliteError(u8"查询瓦片矩阵失败");
    }
    if (matrixResult.rows.empty())
    {
        return SetLastError(u8"批量写入瓦片前必须先写入对应的瓦片矩阵定义。 ");
    }

    std::map<int, std::pair<int, int>> matrixSizeByZoomLevel;
    for (std::size_t rowIndex = 0; rowIndex < matrixResult.rows.size(); rowIndex++)
    {
        int zoomLevel = 0;
        int matrixWidth = 0;
        int matrixHeight = 0;
        QueryIntColumn(matrixResult, rowIndex, "zoom_level", zoomLevel);
        QueryIntColumn(matrixResult, rowIndex, "matrix_width", matrixWidth);
        QueryIntColumn(matrixResult, rowIndex, "matrix_height", matrixHeight);
        if (zoomLevel < 0 || matrixWidth < 1 || matrixHeight < 1)
        {
            return SetLastError(u8"瓦片矩阵定义存在非法值。 ");
        }
        matrixSizeByZoomLevel[zoomLevel] = std::make_pair(matrixWidth, matrixHeight);
    }

    for (std::size_t index = 0; index < tiles.size(); index++)
    {
        const GeoGpkgTile& tile = tiles[index];
        const std::map<int, std::pair<int, int>>::const_iterator matrixIter = matrixSizeByZoomLevel.find(tile.zoomLevel);
        if (tile.zoomLevel < 0 || tile.tileColumn < 0 || tile.tileRow < 0 || tile.tileData.empty() || matrixIter == matrixSizeByZoomLevel.end())
        {
            return SetLastError(u8"瓦片参数无效或缺少对应 zoom_level 的瓦片矩阵定义。 ");
        }
        if (tile.tileColumn >= matrixIter->second.first || tile.tileRow >= matrixIter->second.second)
        {
            return SetLastError(u8"瓦片行列号超出瓦片矩阵范围。 ");
        }
    }

    const std::string sql = "INSERT OR REPLACE INTO " + QuoteIdentifier(tableNameUtf8) + "(zoom_level, tile_column, tile_row, tile_data) VALUES(?, ?, ?, ?)";
    if (!database_.ExecuteInTransaction([&](GB_SqliteTransaction& transaction) -> bool
        {
            for (std::size_t index = 0; index < tiles.size(); index++)
            {
                const GeoGpkgTile& tile = tiles[index];
                GB_SqliteParameterList parameters;
                parameters.reserve(4);
                parameters.push_back(tile.zoomLevel);
                parameters.push_back(tile.tileColumn);
                parameters.push_back(tile.tileRow);
                parameters.push_back(tile.tileData);
                if (!transaction.Execute(sql, parameters))
                {
                    return false;
                }
            }
            return transaction.Execute("UPDATE gpkg_contents SET last_change = strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE table_name = ?", GB_SqliteParameterList(1, tableNameUtf8));
        }, GB_SqliteTransactionMode::Immediate))
    {
        return SetLastSqliteError(u8"批量写入瓦片失败");
    }
    return true;
}

bool GeoGpkg::GetTile(const std::string& tableNameUtf8, int zoomLevel, int tileColumn, int tileRow, GB_ByteBuffer& outTileData, bool& outExists) const
{
    GB_ReadLockGuard guard(lock_);
    ClearLastError();
    outTileData.clear();
    outExists = false;

    if (!IsValidUserContentTableName(tableNameUtf8) || zoomLevel < 0 || tileColumn < 0 || tileRow < 0)
    {
        return SetLastError(u8"瓦片参数无效。 ");
    }

    bool tileMatrixSetExists = false;
    if (!TileMatrixSetExists(database_, tableNameUtf8, tileMatrixSetExists))
    {
        return SetLastSqliteError(u8"检查瓦片矩阵集失败");
    }
    if (!tileMatrixSetExists)
    {
        return SetLastError(u8"瓦片矩阵集不存在：" + tableNameUtf8);
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

    if (!IsValidUserContentTableName(tableNameUtf8) || zoomLevel < 0)
    {
        return SetLastError(u8"瓦片参数无效。 ");
    }

    bool tileMatrixSetExists = false;
    if (!TileMatrixSetExists(database_, tableNameUtf8, tileMatrixSetExists))
    {
        return SetLastSqliteError(u8"检查瓦片矩阵集失败");
    }
    if (!tileMatrixSetExists)
    {
        return SetLastError(u8"瓦片矩阵集不存在：" + tableNameUtf8);
    }

    if (maxTileCount > 0)
    {
        outTiles.reserve(std::min<std::size_t>(maxTileCount, GB_GpkgMaxQueryReserveCount));
    }

    std::ostringstream stream;
    stream << "SELECT zoom_level, tile_column, tile_row, tile_data FROM " << QuoteIdentifier(tableNameUtf8) << " WHERE zoom_level = ? ORDER BY tile_row, tile_column";
    if (maxTileCount > 0)
    {
        stream << " LIMIT " << maxTileCount;
    }

    bool tileBlobFailed = false;
    const bool queryOk = database_.QueryEach(stream.str(), GB_SqliteParameterList(1, zoomLevel), [&](const std::vector<GB_SqliteColumnInfo>& columns, const std::vector<GB_Variant>& values) -> bool
        {
            GeoGpkgTile tile;
            for (std::size_t columnIndex = 0; columnIndex < columns.size() && columnIndex < values.size(); columnIndex++)
            {
                const std::string& columnNameUtf8 = columns[columnIndex].nameUtf8;
                const GB_Variant& value = values[columnIndex];
                if (columnNameUtf8 == "zoom_level")
                {
                    bool ok = false;
                    tile.zoomLevel = value.ToInt(&ok);
                    if (!ok)
                    {
                        tileBlobFailed = true;
                        return false;
                    }
                }
                else if (columnNameUtf8 == "tile_column")
                {
                    bool ok = false;
                    tile.tileColumn = value.ToInt(&ok);
                    if (!ok)
                    {
                        tileBlobFailed = true;
                        return false;
                    }
                }
                else if (columnNameUtf8 == "tile_row")
                {
                    bool ok = false;
                    tile.tileRow = value.ToInt(&ok);
                    if (!ok)
                    {
                        tileBlobFailed = true;
                        return false;
                    }
                }
                else if (columnNameUtf8 == "tile_data")
                {
                    bool ok = false;
                    tile.tileData = value.ToBinary(&ok);
                    if (!ok)
                    {
                        tileBlobFailed = true;
                        return false;
                    }
                }
            }

            if (tile.tileData.empty())
            {
                tileBlobFailed = true;
                return false;
            }

            outTiles.push_back(std::move(tile));
            return true;
        });
    if (tileBlobFailed)
    {
        return SetLastError(u8"读取瓦片字段失败。 ");
    }
    if (!queryOk)
    {
        return SetLastSqliteError(u8"读取瓦片列表失败");
    }
    return true;
}

bool GeoGpkg::DeleteTile(const std::string& tableNameUtf8, int zoomLevel, int tileColumn, int tileRow, bool* outDeleted)
{
    GB_WriteLockGuard guard(lock_);
    ClearLastError();
    if (outDeleted != nullptr)
    {
        *outDeleted = false;
    }

    if (!database_.IsOpen())
    {
        return SetLastError(u8"GeoPackage 尚未打开。 ");
    }
    if (database_.IsReadOnly())
    {
        return SetLastError(u8"当前 GeoPackage 以只读方式打开，不能删除瓦片。 ");
    }

    if (!IsValidUserContentTableName(tableNameUtf8) || zoomLevel < 0 || tileColumn < 0 || tileRow < 0)
    {
        return SetLastError(u8"瓦片参数无效。 ");
    }

    GB_SqliteResult matrixSetResult;
    if (!database_.Query("SELECT table_name FROM gpkg_tile_matrix_set WHERE table_name = ?", GB_SqliteParameterList(1, tableNameUtf8), matrixSetResult, 1))
    {
        return SetLastSqliteError(u8"查询瓦片矩阵集失败");
    }
    if (matrixSetResult.rows.empty())
    {
        return SetLastError(u8"瓦片矩阵集不存在：" + tableNameUtf8);
    }

    GB_SqliteParameterList parameters;
    parameters.push_back(zoomLevel);
    parameters.push_back(tileColumn);
    parameters.push_back(tileRow);

    long long changedCount = 0;
    const std::string sql = "DELETE FROM " + QuoteIdentifier(tableNameUtf8) + " WHERE zoom_level = ? AND tile_column = ? AND tile_row = ?";
    if (!database_.ExecuteInTransaction([&](GB_SqliteTransaction& transaction) -> bool
        {
            if (!transaction.Execute(sql, parameters))
            {
                return false;
            }

            GB_Variant changedValue;
            if (!transaction.ExecuteScalar("SELECT changes()", changedValue))
            {
                return false;
            }
            bool ok = false;
            changedCount = changedValue.ToInt64(&ok);
            if (!ok)
            {
                return false;
            }
            if (changedCount <= 0)
            {
                return true;
            }
            return transaction.Execute("UPDATE gpkg_contents SET last_change = strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE table_name = ?", GB_SqliteParameterList(1, tableNameUtf8));
        }, GB_SqliteTransactionMode::Immediate))
    {
        return SetLastSqliteError(u8"删除瓦片失败");
    }

    if (outDeleted != nullptr)
    {
        *outDeleted = changedCount > 0;
    }
    return true;
}

bool GeoGpkg::DropContentTable(const std::string& tableNameUtf8)
{
    GB_WriteLockGuard guard(lock_);
    ClearLastError();

    if (!database_.IsOpen())
    {
        return SetLastError(u8"GeoPackage 尚未打开。 ");
    }
    if (database_.IsReadOnly())
    {
        return SetLastError(u8"当前 GeoPackage 以只读方式打开，不能删除内容表。 ");
    }

    if (!IsValidUserContentTableName(tableNameUtf8))
    {
        return SetLastError(u8"GeoPackage 内容表名非法，不能删除 sqlite_、gpkg_ 或 rtree_ 前缀的保留对象。 ");
    }

    GeoGpkgFeatureLayerInfo layerInfo;
    std::vector<std::string> spatialIndexTriggerNames;
    std::vector<std::string> tileConstraintTriggerNames;
    std::string rtreeNameUtf8;
    if (GetFeatureLayerInfoNoLock(tableNameUtf8, layerInfo))
    {
        BuildSpatialIndexTriggerNames(layerInfo.tableNameUtf8, layerInfo.geometryColumnNameUtf8, spatialIndexTriggerNames);
        rtreeNameUtf8 = MakeRTreeName(layerInfo.tableNameUtf8, layerInfo.geometryColumnNameUtf8);
    }
    BuildTileConstraintTriggerNames(tableNameUtf8, tileConstraintTriggerNames);
    ClearLastError();

    if (!database_.ExecuteInTransaction([&](GB_SqliteTransaction& transaction) -> bool
        {
            for (std::size_t index = 0; index < spatialIndexTriggerNames.size(); index++)
            {
                if (!transaction.Execute("DROP TRIGGER IF EXISTS " + QuoteIdentifier(spatialIndexTriggerNames[index])))
                {
                    return false;
                }
            }
            for (std::size_t index = 0; index < tileConstraintTriggerNames.size(); index++)
            {
                if (!transaction.Execute("DROP TRIGGER IF EXISTS " + QuoteIdentifier(tileConstraintTriggerNames[index])))
                {
                    return false;
                }
            }
            if (!rtreeNameUtf8.empty() && !transaction.Execute("DROP TABLE IF EXISTS " + QuoteIdentifier(rtreeNameUtf8)))
            {
                return false;
            }
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

    GB_Rectangle calculatedEnvelope;
    WkbDimensionSummary dimensionSummary;
    if (!ParseWkbEnvelope(wkb, &calculatedEnvelope, &dimensionSummary) || dimensionSummary.hasSrid)
    {
        return false;
    }

    const bool wkbIsEmptyGeometry = !calculatedEnvelope.IsValid();
    if (empty && !wkbIsEmptyGeometry)
    {
        return false;
    }

    const bool isEmptyGeometry = empty || wkbIsEmptyGeometry;
    GB_Rectangle realEnvelope;
    if (envelope != nullptr)
    {
        realEnvelope = *envelope;
        if (isEmptyGeometry)
        {
            if (realEnvelope.IsValid())
            {
                return false;
            }
        }
        else if (!realEnvelope.IsValid() || !IsRectangleNearlyEqual(realEnvelope, calculatedEnvelope))
        {
            return false;
        }
    }
    else if (!isEmptyGeometry)
    {
        realEnvelope = calculatedEnvelope;
    }

    const bool useEnvelope = !isEmptyGeometry && realEnvelope.IsValid();
    const int envelopeCode = useEnvelope ? 1 : 0;
    const unsigned char flags = static_cast<unsigned char>(0x01 | ((envelopeCode & 0x07) << 1) | (isEmptyGeometry ? 0x10 : 0x00));

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
        double envelopeValues[8] = { 0.0 };
        for (int index = 0; index < envelopeDoubleCount; index++)
        {
            envelopeValues[index] = ReadDouble(envelopePtr + static_cast<std::size_t>(index) * sizeof(double), outHeaderInfo.littleEndian);
            if (!IsFinite(envelopeValues[index]))
            {
                return false;
            }
        }

        const double minX = envelopeValues[0];
        const double maxX = envelopeValues[1];
        const double minY = envelopeValues[2];
        const double maxY = envelopeValues[3];
        if (!AssignRectangle(outHeaderInfo.envelope, minX, minY, maxX, maxY))
        {
            return false;
        }
        for (int index = 4; index + 1 < envelopeDoubleCount; index += 2)
        {
            if (envelopeValues[index] > envelopeValues[index + 1])
            {
                return false;
            }
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

    WkbDimensionSummary dimensionSummary;
    if (!ParseWkbEnvelope(wkb, &outEnvelope, &dimensionSummary) || dimensionSummary.hasSrid)
    {
        outEnvelope.Reset();
        return false;
    }
    return outEnvelope.IsValid();
}

bool GeoGpkg::SetLastError(const std::string& messageUtf8) const
{
    std::lock_guard<std::mutex> guard(lastErrorLock_);
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
    {
        std::lock_guard<std::mutex> guard(lastErrorLock_);
        lastErrorUtf8_ = message;
    }
    return false;
}

void GeoGpkg::ClearLastError() const
{
    std::lock_guard<std::mutex> guard(lastErrorLock_);
    lastErrorUtf8_.clear();
}




namespace
{
    static bool IsValidTileImageCoordinate(const std::string& tableNameUtf8, int zoomLevel, int row, int col)
    {
        return IsValidUserContentTableName(tableNameUtf8) && zoomLevel >= 0 && row >= 0 && col >= 0;
    }

    static bool IsNonEmptyImage(const GB_Image& image)
    {
        return !image.IsEmpty() && image.GetWidth() > 0 && image.GetHeight() > 0 && image.GetChannels() > 0 && image.GetDepth() != GB_ImageDepth::Unknown;
    }

    static bool TryGetImageSizeAsInt(const GB_Image& image, int& outWidth, int& outHeight)
    {
        outWidth = 0;
        outHeight = 0;
        if (!IsNonEmptyImage(image))
        {
            return false;
        }
        if (image.GetWidth() > static_cast<std::size_t>(std::numeric_limits<int>::max()) || image.GetHeight() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        {
            return false;
        }

        outWidth = static_cast<int>(image.GetWidth());
        outHeight = static_cast<int>(image.GetHeight());
        return outWidth > 0 && outHeight > 0;
    }

    static bool BuildReadOnlyGeoGpkgOpenOptions(GeoGpkgOpenOptions& outOptions)
    {
        outOptions = GeoGpkgOpenOptions();
        outOptions.initializeIfNeeded = false;
        outOptions.sqliteOptions.openMode = GB_SqliteOpenMode::ReadOnly;
        outOptions.sqliteOptions.enableWal = false;
        return true;
    }

    static bool BuildReadWriteGeoGpkgOpenOptions(GeoGpkgOpenOptions& outOptions, bool createIfNeeded)
    {
        outOptions = GeoGpkgOpenOptions();
        outOptions.initializeIfNeeded = createIfNeeded;
        outOptions.sqliteOptions.openMode = createIfNeeded ? GB_SqliteOpenMode::ReadWriteCreate : GB_SqliteOpenMode::ReadWrite;
        return true;
    }

    static bool EnsureWritableGeoPackageFilePath(const std::string& filePathUtf8)
    {
        if (filePathUtf8.empty())
        {
            return false;
        }

        const GB_FileType fileType = GB_GetFileType(filePathUtf8);
        if (fileType == GB_FileType::RegularFile || fileType == GB_FileType::SymbolicLink)
        {
            return true;
        }
        if (fileType != GB_FileType::NotExists)
        {
            return false;
        }

        const std::string dirPathUtf8 = GB_GetDirectoryPath(filePathUtf8);
        if (!dirPathUtf8.empty() && !GB_IsDirectoryExists(dirPathUtf8))
        {
            return GB_CreateDirectory(dirPathUtf8);
        }
        return true;
    }


    static std::string NormalizeTileImageFileExt(const std::string& imageFileExtUtf8)
    {
        const std::string trimmedFileExtUtf8 = TrimAsciiSpaces(imageFileExtUtf8);
        std::string result = ToLowerAsciiString(trimmedFileExtUtf8.empty() ? ".png" : trimmedFileExtUtf8);
        if (!result.empty() && result[0] != '.')
        {
            result.insert(result.begin(), '.');
        }
        return result;
    }

    static bool IsSupportedTileImageFileExt(const std::string& normalizedFileExtUtf8)
    {
        return normalizedFileExtUtf8 == ".png" || normalizedFileExtUtf8 == ".jpg" || normalizedFileExtUtf8 == ".jpeg" || normalizedFileExtUtf8 == ".webp";
    }

    static bool RegisterWebpExtensionIfNeeded(GeoGpkg& geoGpkg, const std::string& tableNameUtf8, const std::string& normalizedFileExtUtf8)
    {
        if (normalizedFileExtUtf8 != ".webp")
        {
            return true;
        }

        GB_SqliteParameterList parameters;
        parameters.push_back(tableNameUtf8);
        parameters.push_back("tile_data");
        parameters.push_back("gpkg_webp");
        parameters.push_back("http://www.geopackage.org/spec/#extension_tiles_webp");
        parameters.push_back("read-write");
        return geoGpkg.GetSqlite().Execute("INSERT OR REPLACE INTO gpkg_extensions(table_name, column_name, extension_name, definition, scope) VALUES(?, ?, ?, ?, ?)", parameters);
    }

    static bool QuerySrsIdByDefinition(GB_Sqlite& database, const std::string& srsWktUtf8, int& outSrsId, bool& outFound)
    {
        outSrsId = 0;
        outFound = false;
        if (srsWktUtf8.empty())
        {
            return false;
        }

        GB_SqliteResult result;
        if (!database.Query("SELECT srs_id FROM gpkg_spatial_ref_sys WHERE definition = ? ORDER BY srs_id LIMIT 1", GB_SqliteParameterList(1, srsWktUtf8), result, 1))
        {
            return false;
        }
        if (result.rows.empty())
        {
            return true;
        }

        bool ok = false;
        outSrsId = result.GetValue(0, "srs_id").ToInt(&ok);
        outFound = ok;
        return ok;
    }

    static bool IsSrsIdUsed(GB_Sqlite& database, int srsId, bool& outUsed)
    {
        outUsed = false;
        GB_Variant value;
        if (!database.ExecuteScalar("SELECT COUNT(*) FROM gpkg_spatial_ref_sys WHERE srs_id = ?", GB_SqliteParameterList(1, srsId), value))
        {
            return false;
        }

        bool ok = false;
        outUsed = value.ToInt64(&ok) > 0;
        return ok;
    }

    static bool GenerateUserSrsId(GB_Sqlite& database, int& outSrsId)
    {
        outSrsId = 0;
        GB_Variant value;
        if (!database.ExecuteScalar("SELECT COALESCE(MAX(srs_id), 99999) FROM gpkg_spatial_ref_sys WHERE srs_id >= 100000", value))
        {
            return false;
        }

        bool ok = false;
        const long long maxSrsId = value.ToInt64(&ok);
        if (!ok || maxSrsId >= static_cast<long long>(std::numeric_limits<int>::max()))
        {
            return false;
        }

        outSrsId = static_cast<int>(maxSrsId + 1);
        return outSrsId > 0;
    }

    static bool EnsureSpatialRefSysByWkt(GeoGpkg& geoGpkg, const GeoGpkgTileImageWriteOptions& options, int& outSrsId)
    {
        outSrsId = 0;

        GB_Sqlite& database = geoGpkg.GetSqlite();
        if (!options.srsWktUtf8.empty())
        {
            bool foundByDefinition = false;
            if (!QuerySrsIdByDefinition(database, options.srsWktUtf8, outSrsId, foundByDefinition))
            {
                return false;
            }
            if (foundByDefinition)
            {
                return true;
            }
        }

        bool preferredSrsIdUsed = false;
        if (options.preferredSrsId > 0)
        {
            if (!IsSrsIdUsed(database, options.preferredSrsId, preferredSrsIdUsed))
            {
                return false;
            }
            if (!preferredSrsIdUsed)
            {
                outSrsId = options.preferredSrsId;
            }
            else if (options.srsWktUtf8.empty())
            {
                outSrsId = options.preferredSrsId;
                return true;
            }
        }

        if (options.srsWktUtf8.empty())
        {
            return false;
        }
        if (outSrsId == 0 && !GenerateUserSrsId(database, outSrsId))
        {
            return false;
        }

        GeoGpkgSpatialRefSys srs;
        srs.srsNameUtf8 = options.srsNameUtf8.empty() ? (u8"User defined SRS " + ToString(outSrsId)) : options.srsNameUtf8;
        srs.srsId = outSrsId;
        srs.organizationUtf8 = options.srsOrganizationUtf8.empty() ? "NONE" : options.srsOrganizationUtf8;
        srs.organizationCoordsysId = options.srsOrganizationCoordsysId != 0 ? options.srsOrganizationCoordsysId : outSrsId;
        srs.definitionUtf8 = options.srsWktUtf8;
        srs.descriptionUtf8 = u8"Created by GeoGpkgWriteTileImage.";
        return geoGpkg.AddOrUpdateSpatialRefSys(srs);
    }

    static bool GetTileMatrixInfoForZoom(GeoGpkg& geoGpkg, const std::string& tableNameUtf8, int zoomLevel, GeoGpkgTileMatrixInfo& outTileMatrixInfo, bool& outExists)
    {
        outTileMatrixInfo = GeoGpkgTileMatrixInfo();
        outExists = false;

        GB_SqliteParameterList parameters;
        parameters.push_back(tableNameUtf8);
        parameters.push_back(zoomLevel);

        GB_SqliteResult result;
        const std::string sql =
            "SELECT m.table_name, m.zoom_level, m.matrix_width, m.matrix_height, m.tile_width, m.tile_height, m.pixel_x_size, m.pixel_y_size "
            "FROM gpkg_tile_matrix m JOIN gpkg_contents c ON m.table_name = c.table_name "
            "WHERE m.table_name = ? AND m.zoom_level = ? AND c.data_type = 'tiles'";
        if (!geoGpkg.GetSqlite().Query(sql, parameters, result, 1))
        {
            return false;
        }
        if (result.rows.empty())
        {
            return true;
        }

        QueryStringColumn(result, 0, "table_name", outTileMatrixInfo.tableNameUtf8);
        QueryIntColumn(result, 0, "zoom_level", outTileMatrixInfo.zoomLevel);
        QueryIntColumn(result, 0, "matrix_width", outTileMatrixInfo.matrixWidth);
        QueryIntColumn(result, 0, "matrix_height", outTileMatrixInfo.matrixHeight);
        QueryIntColumn(result, 0, "tile_width", outTileMatrixInfo.tileWidth);
        QueryIntColumn(result, 0, "tile_height", outTileMatrixInfo.tileHeight);
        QueryDoubleColumn(result, 0, "pixel_x_size", outTileMatrixInfo.pixelXSize);
        QueryDoubleColumn(result, 0, "pixel_y_size", outTileMatrixInfo.pixelYSize);
        outExists = true;
        return true;
    }

    static bool BuildTileMatrixInfoFromOptions(const std::string& tableNameUtf8, int zoomLevel, const GeoGpkgTileMatrixSetInfo& tileMatrixSetInfo, const GB_Image& image, const GeoGpkgTileImageWriteOptions& options, GeoGpkgTileMatrixInfo& outTileMatrixInfo)
    {
        outTileMatrixInfo = GeoGpkgTileMatrixInfo();

        int imageWidth = 0;
        int imageHeight = 0;
        if (!TryGetImageSizeAsInt(image, imageWidth, imageHeight))
        {
            return false;
        }

        const int tileWidth = options.tileWidth > 0 ? options.tileWidth : imageWidth;
        const int tileHeight = options.tileHeight > 0 ? options.tileHeight : imageHeight;
        if (options.matrixWidth < 1 || options.matrixHeight < 1 || tileWidth < 1 || tileHeight < 1)
        {
            return false;
        }
        if (imageWidth != tileWidth || imageHeight != tileHeight)
        {
            return false;
        }
        if (!tileMatrixSetInfo.envelope.IsValid() || GetRectangleWidth(tileMatrixSetInfo.envelope) <= 0.0 || GetRectangleHeight(tileMatrixSetInfo.envelope) <= 0.0)
        {
            return false;
        }

        const double pixelXSize = GetRectangleWidth(tileMatrixSetInfo.envelope) / (static_cast<double>(options.matrixWidth) * static_cast<double>(tileWidth));
        const double pixelYSize = GetRectangleHeight(tileMatrixSetInfo.envelope) / (static_cast<double>(options.matrixHeight) * static_cast<double>(tileHeight));
        if (pixelXSize <= 0.0 || pixelYSize <= 0.0 || !IsFinite(pixelXSize) || !IsFinite(pixelYSize))
        {
            return false;
        }

        outTileMatrixInfo.tableNameUtf8 = tableNameUtf8;
        outTileMatrixInfo.zoomLevel = zoomLevel;
        outTileMatrixInfo.matrixWidth = options.matrixWidth;
        outTileMatrixInfo.matrixHeight = options.matrixHeight;
        outTileMatrixInfo.tileWidth = tileWidth;
        outTileMatrixInfo.tileHeight = tileHeight;
        outTileMatrixInfo.pixelXSize = pixelXSize;
        outTileMatrixInfo.pixelYSize = pixelYSize;
        return true;
    }

    static bool EnsureTileMatrixSetForImageWrite(GeoGpkg& geoGpkg, const std::string& tableNameUtf8, const GeoGpkgTileImageWriteOptions& options, GeoGpkgTileMatrixSetInfo& outTileMatrixSetInfo)
    {
        outTileMatrixSetInfo = GeoGpkgTileMatrixSetInfo();
        if (geoGpkg.GetTileMatrixSetInfo(tableNameUtf8, outTileMatrixSetInfo))
        {
            return true;
        }

        if (!options.envelope.IsValid() || GetRectangleWidth(options.envelope) <= 0.0 || GetRectangleHeight(options.envelope) <= 0.0 || options.matrixWidth < 1 || options.matrixHeight < 1)
        {
            return false;
        }

        int srsId = 0;
        if (!EnsureSpatialRefSysByWkt(geoGpkg, options, srsId))
        {
            return false;
        }
        if (!geoGpkg.CreateTileMatrixSet(tableNameUtf8, srsId, options.envelope, options.identifierUtf8, options.descriptionUtf8, false))
        {
            return false;
        }
        return geoGpkg.GetTileMatrixSetInfo(tableNameUtf8, outTileMatrixSetInfo);
    }

    static bool EnsureTileMatrixForImageWrite(GeoGpkg& geoGpkg, const std::string& tableNameUtf8, int zoomLevel, const GeoGpkgTileMatrixSetInfo& tileMatrixSetInfo, const GB_Image& image, const GeoGpkgTileImageWriteOptions& options, GeoGpkgTileMatrixInfo& outTileMatrixInfo)
    {
        bool exists = false;
        if (!GetTileMatrixInfoForZoom(geoGpkg, tableNameUtf8, zoomLevel, outTileMatrixInfo, exists))
        {
            return false;
        }
        if (exists)
        {
            int imageWidth = 0;
            int imageHeight = 0;
            return TryGetImageSizeAsInt(image, imageWidth, imageHeight) && imageWidth == outTileMatrixInfo.tileWidth && imageHeight == outTileMatrixInfo.tileHeight;
        }

        if (!BuildTileMatrixInfoFromOptions(tableNameUtf8, zoomLevel, tileMatrixSetInfo, image, options, outTileMatrixInfo))
        {
            return false;
        }
        return geoGpkg.AddOrUpdateTileMatrix(outTileMatrixInfo);
    }
}

bool GeoGpkgReadTileImage(const std::string& filePathUtf8, const std::string& tableNameUtf8, int zoomLevel, int row, int col, GB_Image& outImage, const GB_ImageLoadOptions& loadOptions)
{
    outImage.Clear();
    if (!GB_IsFileExists(filePathUtf8) || !IsValidTileImageCoordinate(tableNameUtf8, zoomLevel, row, col))
    {
        return false;
    }

    GeoGpkgOpenOptions openOptions;
    BuildReadOnlyGeoGpkgOpenOptions(openOptions);

    GeoGpkg geoGpkg;
    if (!geoGpkg.Open(filePathUtf8, openOptions))
    {
        return false;
    }

    GeoGpkgTileMatrixSetInfo tileMatrixSetInfo;
    if (!geoGpkg.GetTileMatrixSetInfo(tableNameUtf8, tileMatrixSetInfo))
    {
        return false;
    }

    bool exists = false;
    GB_ByteBuffer tileData;
    if (!geoGpkg.GetTile(tableNameUtf8, zoomLevel, col, row, tileData, exists) || !exists || tileData.empty())
    {
        return false;
    }

    GB_Image image;
    if (!image.LoadFromMemory(tileData, loadOptions) || !IsNonEmptyImage(image))
    {
        return false;
    }

    outImage = std::move(image);
    return true;
}

bool GeoGpkgWriteTileImage(const std::string& filePathUtf8, const std::string& tableNameUtf8, int zoomLevel, int row, int col, const GB_Image& image, const GeoGpkgTileImageWriteOptions& options)
{
    if (!EnsureWritableGeoPackageFilePath(filePathUtf8) || !IsValidTileImageCoordinate(tableNameUtf8, zoomLevel, row, col) || !IsValidUserContentTableName(tableNameUtf8) || !IsNonEmptyImage(image))
    {
        return false;
    }

    const bool createIfNeeded = !GB_IsFileExists(filePathUtf8);
    GeoGpkgOpenOptions openOptions;
    BuildReadWriteGeoGpkgOpenOptions(openOptions, createIfNeeded);

    GeoGpkg geoGpkg;
    if (!geoGpkg.Open(filePathUtf8, openOptions))
    {
        return false;
    }
    if (!createIfNeeded && !geoGpkg.InitializeCoreTables(openOptions.userVersion))
    {
        return false;
    }

    GeoGpkgTileMatrixSetInfo tileMatrixSetInfo;
    if (!EnsureTileMatrixSetForImageWrite(geoGpkg, tableNameUtf8, options, tileMatrixSetInfo))
    {
        return false;
    }

    GeoGpkgTileMatrixInfo tileMatrixInfo;
    if (!EnsureTileMatrixForImageWrite(geoGpkg, tableNameUtf8, zoomLevel, tileMatrixSetInfo, image, options, tileMatrixInfo))
    {
        return false;
    }
    if (col >= tileMatrixInfo.matrixWidth || row >= tileMatrixInfo.matrixHeight)
    {
        return false;
    }

    GB_ByteBuffer tileData;
    const std::string imageFileExtUtf8 = NormalizeTileImageFileExt(options.imageFileExtUtf8);
    if (!IsSupportedTileImageFileExt(imageFileExtUtf8))
    {
        return false;
    }
    if (!image.EncodeToMemory(tileData, imageFileExtUtf8, options.imageSaveOptions) || tileData.empty())
    {
        return false;
    }
    if (!RegisterWebpExtensionIfNeeded(geoGpkg, tableNameUtf8, imageFileExtUtf8))
    {
        return false;
    }

    return geoGpkg.PutTile(tableNameUtf8, zoomLevel, col, row, tileData);
}

bool GeoGpkgDeleteTile(const std::string& filePathUtf8, const std::string& tableNameUtf8, int zoomLevel, int row, int col)
{
    if (!GB_IsFileExists(filePathUtf8) || !IsValidTileImageCoordinate(tableNameUtf8, zoomLevel, row, col))
    {
        return false;
    }

    GeoGpkgOpenOptions openOptions;
    BuildReadWriteGeoGpkgOpenOptions(openOptions, false);

    GeoGpkg geoGpkg;
    if (!geoGpkg.Open(filePathUtf8, openOptions))
    {
        return false;
    }

    bool deleted = false;
    return geoGpkg.DeleteTile(tableNameUtf8, zoomLevel, col, row, &deleted) && deleted;
}
