#include "GeoCrs.h"

#include "DelayLoadRuntime.h"
#include "GB_ReadWriteLock.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cmath>
#include <limits>
#include <utility>

#include <cpl_conv.h>
#include <cpl_string.h>
#include <gdal_version.h>
#include <ogr_spatialref.h>

namespace
{
    std::string ToString(const char* text)
    {
        return text ? std::string(text) : std::string();
    }

    std::string TrimAscii(const std::string& text)
    {
        std::size_t beginIndex = 0;
        while (beginIndex < text.size() && std::isspace(static_cast<unsigned char>(text[beginIndex])) != 0)
        {
            beginIndex++;
        }

        std::size_t endIndex = text.size();
        while (endIndex > beginIndex && std::isspace(static_cast<unsigned char>(text[endIndex - 1])) != 0)
        {
            endIndex--;
        }

        return text.substr(beginIndex, endIndex - beginIndex);
    }

    std::string ToUpperAscii(const std::string& text)
    {
        std::string result = text;
        for (std::size_t i = 0; i < result.size(); i++)
        {
            result[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(result[i])));
        }
        return result;
    }


    bool StartsWithAsciiNoCase(const std::string& text, const char* prefix)
    {
        if (prefix == nullptr)
        {
            return false;
        }

        const std::size_t prefixLength = std::strlen(prefix);
        if (text.size() < prefixLength)
        {
            return false;
        }

        for (std::size_t i = 0; i < prefixLength; i++)
        {
            const char leftChar = static_cast<char>(std::toupper(static_cast<unsigned char>(text[i])));
            const char rightChar = static_cast<char>(std::toupper(static_cast<unsigned char>(prefix[i])));
            if (leftChar != rightChar)
            {
                return false;
            }
        }

        return true;
    }

    bool EndsWithAsciiNoCase(const std::string& text, const char* suffix)
    {
        if (suffix == nullptr)
        {
            return false;
        }

        const std::size_t suffixLength = std::strlen(suffix);
        if (text.size() < suffixLength)
        {
            return false;
        }

        const std::size_t beginIndex = text.size() - suffixLength;
        for (std::size_t i = 0; i < suffixLength; i++)
        {
            const char leftChar = static_cast<char>(std::toupper(static_cast<unsigned char>(text[beginIndex + i])));
            const char rightChar = static_cast<char>(std::toupper(static_cast<unsigned char>(suffix[i])));
            if (leftChar != rightChar)
            {
                return false;
            }
        }

        return true;
    }

    bool IsWindowsAbsolutePath(const std::string& text)
    {
        if (text.size() < 3)
        {
            return false;
        }

        const unsigned char driveChar = static_cast<unsigned char>(text[0]);
        const bool hasDriveLetter = std::isalpha(driveChar) != 0 && text[1] == ':';
        return hasDriveLetter && (text[2] == '\\' || text[2] == '/');
    }

    bool IsLikelyNetworkInput(const std::string& text)
    {
        return StartsWithAsciiNoCase(text, "http://") || StartsWithAsciiNoCase(text, "https://") || StartsWithAsciiNoCase(text, "/vsicurl/") || StartsWithAsciiNoCase(text, "/vsis3/") || StartsWithAsciiNoCase(text, "/vsigs/") || StartsWithAsciiNoCase(text, "/vsiaz/") || StartsWithAsciiNoCase(text, "/vsioss/") || StartsWithAsciiNoCase(text, "/vsiswift/");
    }

    bool IsLikelyFileInput(const std::string& text)
    {
        if (text.empty())
        {
            return false;
        }

        if (IsLikelyNetworkInput(text) || StartsWithAsciiNoCase(text, "/vsi") || StartsWithAsciiNoCase(text, "\\\\") || IsWindowsAbsolutePath(text) || StartsWithAsciiNoCase(text, "./") || StartsWithAsciiNoCase(text, ".\\") || StartsWithAsciiNoCase(text, "../") || StartsWithAsciiNoCase(text, "..\\"))
        {
            return true;
        }

        if (text.find('\\') != std::string::npos)
        {
            return true;
        }

        if (EndsWithAsciiNoCase(text, ".prj") || EndsWithAsciiNoCase(text, ".wkt") || EndsWithAsciiNoCase(text, ".xml") || EndsWithAsciiNoCase(text, ".projjson") || EndsWithAsciiNoCase(text, ".json"))
        {
            return true;
        }

        return false;
    }

    bool ContainsAsciiNoCase(const std::string& text, const char* pattern)
    {
        if (pattern == nullptr || pattern[0] == '\0')
        {
            return false;
        }

        return ToUpperAscii(text).find(ToUpperAscii(pattern)) != std::string::npos;
    }

    std::string GetLeadingIdentifierUpper(const std::string& text)
    {
        std::size_t beginIndex = 0;
        while (beginIndex < text.size() && std::isspace(static_cast<unsigned char>(text[beginIndex])) != 0)
        {
            beginIndex++;
        }

        std::size_t endIndex = beginIndex;
        while (endIndex < text.size())
        {
            const unsigned char currentChar = static_cast<unsigned char>(text[endIndex]);
            if (std::isalnum(currentChar) == 0 && currentChar != '_')
            {
                break;
            }
            endIndex++;
        }

        if (endIndex == beginIndex)
        {
            return std::string();
        }

        return ToUpperAscii(text.substr(beginIndex, endIndex - beginIndex));
    }

    bool IsLikelyWktInput(const std::string& text)
    {
        const std::string identifier = GetLeadingIdentifierUpper(text);
        if (identifier.empty())
        {
            return false;
        }

        static const char* const wktIdentifiers[] =
        {
            "GEOGCS", "GEOCCS", "PROJCS", "VERT_CS", "LOCAL_CS", "COMPD_CS",
            "GEODCRS", "GEOGCRS", "GEODETICCRS", "GEOCENTRICCRS", "PROJCRS", "PROJECTEDCRS", "VERTCRS", "VERTICALCRS",
            "COMPOUNDCRS", "BOUNDCRS", "ENGCRS", "ENGINEERINGCRS", "DERIVEDPROJCRS", "DERIVEDGEOGCRS", "DERIVEDVERTCRS",
            "PARAMETRICCRS", "TIMECRS", "ENSEMBLE", "DATUM", "ELLIPSOID", nullptr
        };

        for (std::size_t i = 0; wktIdentifiers[i] != nullptr; i++)
        {
            if (identifier == wktIdentifiers[i])
            {
                return true;
            }
        }

        return false;
    }

    bool IsLikelyProj4Input(const std::string& text)
    {
        const std::string trimmedText = TrimAscii(text);
        return StartsWithAsciiNoCase(trimmedText, "+proj=") || StartsWithAsciiNoCase(trimmedText, "+init=") || StartsWithAsciiNoCase(trimmedText, "+type=crs") || ContainsAsciiNoCase(trimmedText, " +proj=") || ContainsAsciiNoCase(trimmedText, "\t+proj=");
    }

    bool IsLikelyProjJsonInput(const std::string& text)
    {
        const std::string trimmedText = TrimAscii(text);
        return !trimmedText.empty() && trimmedText[0] == '{';
    }

    bool IsLikelyAuthorityInput(const std::string& text)
    {
        const std::string trimmedText = TrimAscii(text);
        if (StartsWithAsciiNoCase(trimmedText, "urn:ogc:def:crs:"))
        {
            return true;
        }

        const std::size_t colonIndex = trimmedText.find(':');
        if (colonIndex == std::string::npos || colonIndex == 0)
        {
            return false;
        }

        if (trimmedText.find('/') != std::string::npos && trimmedText.find('/') < colonIndex)
        {
            return false;
        }
        if (trimmedText.find('\\') != std::string::npos && trimmedText.find('\\') < colonIndex)
        {
            return false;
        }

        const std::string authorityName = ToUpperAscii(trimmedText.substr(0, colonIndex));
        static const char* const knownAuthorities[] =
        {
            "EPSG", "EPSGA", "ESRI", "IGNF", "OGC", "AUTO", "AUTO2", "CRS", "IAU_2015", nullptr
        };

        for (std::size_t i = 0; knownAuthorities[i] != nullptr; i++)
        {
            if (authorityName == knownAuthorities[i])
            {
                return true;
            }
        }

        if (authorityName.size() < 2 || authorityName.size() > 32)
        {
            return false;
        }

        for (std::size_t i = 0; i < authorityName.size(); i++)
        {
            const unsigned char currentChar = static_cast<unsigned char>(authorityName[i]);
            if (std::isalnum(currentChar) == 0 && currentChar != '_')
            {
                return false;
            }
        }

        return true;
    }

    bool IsLikelyWellKnownCrsNameInput(const std::string& text)
    {
        std::string normalizedText = ToUpperAscii(TrimAscii(text));
        normalizedText.erase(std::remove(normalizedText.begin(), normalizedText.end(), ' '), normalizedText.end());
        normalizedText.erase(std::remove(normalizedText.begin(), normalizedText.end(), '_'), normalizedText.end());
        normalizedText.erase(std::remove(normalizedText.begin(), normalizedText.end(), '-'), normalizedText.end());

        return normalizedText == "WGS84" || normalizedText == "WGS72" || normalizedText == "NAD27" || normalizedText == "NAD83" || normalizedText == "CRS84";
    }

    bool IsSafeNonFileUserInput(const std::string& text)
    {
        return IsLikelyWktInput(text) || IsLikelyProj4Input(text) || IsLikelyProjJsonInput(text) || IsLikelyAuthorityInput(text) || IsLikelyWellKnownCrsNameInput(text);
    }

    bool IsRejectedByAccessPolicy(const std::string& text, bool allowFileAccess, bool allowNetworkAccess)
    {
        if (!allowNetworkAccess && IsLikelyNetworkInput(text))
        {
            return true;
        }

#if GDAL_VERSION_NUM < 3090000
        if (!allowFileAccess && !(allowNetworkAccess && IsLikelyNetworkInput(text)) && !IsSafeNonFileUserInput(text))
        {
            return true;
        }
#else
        (void)allowFileAccess;
#endif

        return false;
    }

    bool IsFinite(double value)
    {
        return std::isfinite(value) != 0;
    }

    bool IsUnknownAreaValue(double value)
    {
        return !IsFinite(value) || std::fabs(value + 1000.0) < 1e-12;
    }

    double ClampLongitude(double longitude)
    {
        if (!IsFinite(longitude))
        {
            return longitude;
        }
        if (longitude < -180.0)
        {
            return -180.0;
        }
        if (longitude > 180.0)
        {
            return 180.0;
        }
        return longitude;
    }

    double ClampLatitude(double latitude)
    {
        if (!IsFinite(latitude))
        {
            return latitude;
        }
        if (latitude < -90.0)
        {
            return -90.0;
        }
        if (latitude > 90.0)
        {
            return 90.0;
        }
        return latitude;
    }

    void SetTraditionalGisOrder(OGRSpatialReference& spatialReference)
    {
        spatialReference.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    }

    void AssignSpatialReference(OGRSpatialReference& target, const OGRSpatialReference& source)
    {
#if GDAL_VERSION_NUM >= 3100000
        target.AssignAndSetThreadSafe(source);
#else
        target = source;
#endif
        SetTraditionalGisOrder(target);
    }

    void ClearSpatialReference(OGRSpatialReference& spatialReference)
    {
        spatialReference.Clear();
        SetTraditionalGisOrder(spatialReference);
    }

    std::string ExportWktNoLock(const OGRSpatialReference& spatialReference, const char* format, bool multiline)
    {
        if (spatialReference.IsEmpty())
        {
            return std::string();
        }

        const char* multilineOption = multiline ? "MULTILINE=YES" : "MULTILINE=NO";
        const char* options[] = { format, multilineOption, nullptr };
        char* wkt = nullptr;
        const OGRErr errorCode = spatialReference.exportToWkt(&wkt, options);
        if (errorCode != OGRERR_NONE || wkt == nullptr)
        {
            if (wkt != nullptr)
            {
                CPLFree(wkt);
            }
            return std::string();
        }

        const std::string result(wkt);
        CPLFree(wkt);
        return result;
    }

    std::string ExportPrettyWktNoLock(const OGRSpatialReference& spatialReference, bool simplify)
    {
        if (spatialReference.IsEmpty())
        {
            return std::string();
        }

        char* wkt = nullptr;
        const OGRErr errorCode = spatialReference.exportToPrettyWkt(&wkt, simplify ? TRUE : FALSE);
        if (errorCode != OGRERR_NONE || wkt == nullptr)
        {
            if (wkt != nullptr)
            {
                CPLFree(wkt);
            }
            return std::string();
        }

        const std::string result(wkt);
        CPLFree(wkt);
        return result;
    }

    std::string ExportProj4NoLock(const OGRSpatialReference& spatialReference)
    {
        if (spatialReference.IsEmpty())
        {
            return std::string();
        }

        char* proj4 = nullptr;
        const OGRErr errorCode = spatialReference.exportToProj4(&proj4);
        if (errorCode != OGRERR_NONE || proj4 == nullptr)
        {
            if (proj4 != nullptr)
            {
                CPLFree(proj4);
            }
            return std::string();
        }

        const std::string result(proj4);
        CPLFree(proj4);
        return result;
    }

    std::string ExportProjJsonNoLock(const OGRSpatialReference& spatialReference, bool multiline)
    {
#if GDAL_VERSION_NUM >= 3010000
        if (spatialReference.IsEmpty())
        {
            return std::string();
        }

        const char* multilineOption = multiline ? "MULTILINE=YES" : "MULTILINE=NO";
        const char* options[] = { multilineOption, nullptr };
        char* projJson = nullptr;
        const OGRErr errorCode = spatialReference.exportToPROJJSON(&projJson, options);
        if (errorCode != OGRERR_NONE || projJson == nullptr)
        {
            if (projJson != nullptr)
            {
                CPLFree(projJson);
            }
            return std::string();
        }

        const std::string result(projJson);
        CPLFree(projJson);
        return result;
#else
        (void)spatialReference;
        (void)multiline;
        return std::string();
#endif
    }

    std::string ExportOgcUrnNoLock(const OGRSpatialReference& spatialReference)
    {
#if GDAL_VERSION_NUM >= 3050000
        if (spatialReference.IsEmpty())
        {
            return std::string();
        }

        char* urn = spatialReference.GetOGCURN();
        if (urn == nullptr)
        {
            return std::string();
        }

        const std::string result(urn);
        CPLFree(urn);
        return result;
#else
        (void)spatialReference;
        return std::string();
#endif
    }

    std::string GetRootAuthorityStringNoLock(const OGRSpatialReference& spatialReference, bool onlyEpsgOrEsri)
    {
        const char* authorityName = spatialReference.GetAuthorityName(nullptr);
        const char* authorityCode = spatialReference.GetAuthorityCode(nullptr);
        if (authorityName == nullptr || authorityName[0] == '\0' || authorityCode == nullptr || authorityCode[0] == '\0')
        {
            return std::string();
        }

        const std::string upperAuthorityName = ToUpperAscii(authorityName);
        if (onlyEpsgOrEsri && upperAuthorityName != "EPSG" && upperAuthorityName != "ESRI")
        {
            return std::string();
        }

        return upperAuthorityName + ":" + authorityCode;
    }

    bool IsSameSpatialReferenceNoLock(const OGRSpatialReference& left, const OGRSpatialReference& right)
    {
        if (left.IsEmpty() || right.IsEmpty())
        {
            return false;
        }

        const char* options[] = { "IGNORE_DATA_AXIS_TO_SRS_AXIS_MAPPING=YES", "CRITERION=EQUIVALENT_EXCEPT_AXIS_ORDER_GEOGCRS", nullptr };
        return left.IsSame(&right, options) != FALSE;
    }

    std::string GetMatchedAuthorityStringNoLock(const OGRSpatialReference& spatialReference, const char* preferredAuthority, bool onlyEpsgOrEsri)
    {
        if (spatialReference.IsEmpty())
        {
            return std::string();
        }

        const std::string preferredAuthorityUpper = ToUpperAscii(ToString(preferredAuthority));
        if (preferredAuthorityUpper.empty() || preferredAuthorityUpper == "EPSG")
        {
            OGRSpatialReference candidate;
            AssignSpatialReference(candidate, spatialReference);
            if (candidate.AutoIdentifyEPSG() == OGRERR_NONE)
            {
                const std::string authorityString = GetRootAuthorityStringNoLock(candidate, onlyEpsgOrEsri);
                if (!authorityString.empty())
                {
                    return authorityString;
                }
            }
        }

#if GDAL_VERSION_NUM >= 3060000
        OGRSpatialReference* bestMatch = spatialReference.FindBestMatch(90, preferredAuthority, nullptr);
        if (bestMatch != nullptr)
        {
            SetTraditionalGisOrder(*bestMatch);
            std::string authorityString;
            if (IsSameSpatialReferenceNoLock(spatialReference, *bestMatch))
            {
                authorityString = GetRootAuthorityStringNoLock(*bestMatch, onlyEpsgOrEsri);
            }
            bestMatch->Release();
            return authorityString;
        }
#else
        (void)preferredAuthority;
#endif

        return std::string();
    }

    std::string GetAuthorityStringNoLock(const OGRSpatialReference& spatialReference, bool onlyEpsgOrEsri)
    {
        std::string authorityString = GetRootAuthorityStringNoLock(spatialReference, onlyEpsgOrEsri);
        if (!authorityString.empty())
        {
            return authorityString;
        }

        authorityString = GetMatchedAuthorityStringNoLock(spatialReference, "EPSG", onlyEpsgOrEsri);
        if (!authorityString.empty())
        {
            return authorityString;
        }

        authorityString = GetMatchedAuthorityStringNoLock(spatialReference, "ESRI", onlyEpsgOrEsri);
        if (!authorityString.empty())
        {
            return authorityString;
        }

        return std::string();
    }

    std::string MakeUniqueIdNoLock(const OGRSpatialReference& spatialReference)
    {
        if (spatialReference.IsEmpty())
        {
            return std::string();
        }

        const std::string authorityString = GetAuthorityStringNoLock(spatialReference, false);
        if (!authorityString.empty())
        {
            return "AUTHORITY:" + authorityString;
        }

        const std::string ogcUrn = ExportOgcUrnNoLock(spatialReference);
        if (!ogcUrn.empty())
        {
            return "URN:" + ogcUrn;
        }

        const std::string wkt = ExportWktNoLock(spatialReference, "FORMAT=WKT2_2019", false);
        if (!wkt.empty())
        {
            return "WKT2_2019:" + wkt;
        }

        const std::string projJson = ExportProjJsonNoLock(spatialReference, false);
        if (!projJson.empty())
        {
            return "PROJJSON:" + projJson;
        }

        const std::string proj4 = ExportProj4NoLock(spatialReference);
        if (!proj4.empty())
        {
            return "PROJ4:" + proj4;
        }

        const std::string name = ToString(spatialReference.GetName());
        return name.empty() ? std::string() : "NAME:" + name;
    }

    std::vector<GB_Rectangle> SplitAreaOfUse(double westLongitude, double southLatitude, double eastLongitude, double northLatitude)
    {
        std::vector<GB_Rectangle> rectangles;
        if (IsUnknownAreaValue(westLongitude) || IsUnknownAreaValue(southLatitude) || IsUnknownAreaValue(eastLongitude) || IsUnknownAreaValue(northLatitude))
        {
            return rectangles;
        }

        westLongitude = ClampLongitude(westLongitude);
        eastLongitude = ClampLongitude(eastLongitude);
        southLatitude = ClampLatitude(southLatitude);
        northLatitude = ClampLatitude(northLatitude);
        if (!IsFinite(westLongitude) || !IsFinite(eastLongitude) || !IsFinite(southLatitude) || !IsFinite(northLatitude))
        {
            return rectangles;
        }
        if (southLatitude > northLatitude)
        {
            std::swap(southLatitude, northLatitude);
        }

        if (westLongitude <= eastLongitude)
        {
            rectangles.push_back(GB_Rectangle(westLongitude, southLatitude, eastLongitude, northLatitude));
            return rectangles;
        }

        rectangles.push_back(GB_Rectangle(westLongitude, southLatitude, 180.0, northLatitude));
        rectangles.push_back(GB_Rectangle(-180.0, southLatitude, eastLongitude, northLatitude));
        return rectangles;
    }

    GeoCrs::UnitInfo MakeLinearUnitInfoNoLock(const OGRSpatialReference& spatialReference)
    {
        GeoCrs::UnitInfo result;
        if (spatialReference.IsEmpty())
        {
            return result;
        }

        const bool hasLinearCoordinateAxis = spatialReference.IsProjected() != FALSE || spatialReference.IsLocal() != FALSE || spatialReference.IsVertical() != FALSE || spatialReference.IsGeocentric() != FALSE || spatialReference.IsCompound() != FALSE;
        if (!hasLinearCoordinateAxis)
        {
            return result;
        }

        const char* unitName = nullptr;
        const double factor = spatialReference.GetLinearUnits(&unitName);
        if (!IsFinite(factor) || factor <= 0.0)
        {
            return result;
        }

        result.name = ToString(unitName);
        result.conversionFactor = factor;
        result.isValid = true;
        return result;
    }

    GeoCrs::UnitInfo MakeAngularUnitInfoNoLock(const OGRSpatialReference& spatialReference)
    {
        GeoCrs::UnitInfo result;
        if (spatialReference.IsEmpty())
        {
            return result;
        }

        const char* unitName = nullptr;
        const double factor = spatialReference.GetAngularUnits(&unitName);
        if (!IsFinite(factor) || factor <= 0.0)
        {
            return result;
        }

        result.name = ToString(unitName);
        result.conversionFactor = factor;
        result.isValid = true;
        return result;
    }
}

struct GeoCrs::Impl
{
    struct RuntimeInitializer
    {
        RuntimeInitializer()
        {
            InitializeRuntime();
        }
    };

    RuntimeInitializer runtimeInitializer;
    OGRSpatialReference spatialReference;
    mutable GB_ReadWriteLock cacheLock;

    mutable bool uniqueIdCached = false;
    mutable std::string uniqueId;

    mutable bool nameCached = false;
    mutable std::string name;

    mutable bool ellipsoidNameCached = false;
    mutable std::string ellipsoidName;

    mutable bool isGeographicCached = false;
    mutable bool isGeographic = false;

    mutable bool isProjectedCached = false;
    mutable bool isProjected = false;

    mutable bool isLocalCached = false;
    mutable bool isLocal = false;

    mutable bool isCompoundCached = false;
    mutable bool isCompound = false;

    mutable bool isDynamicCached = false;
    mutable bool isDynamic = false;

    mutable bool isCustomCached = false;
    mutable bool isCustom = true;

    mutable bool isLongitudeLatitudeOrderCached = false;
    mutable bool isLongitudeLatitudeOrder = false;

    mutable bool usesTraditionalGisOrderCached = false;
    mutable bool usesTraditionalGisOrder = true;

    mutable bool wktCached = false;
    mutable std::string wkt;

    mutable bool prettyWktCached = false;
    mutable std::string prettyWkt;

    mutable bool simplifiedPrettyWktCached = false;
    mutable std::string simplifiedPrettyWkt;

    mutable bool proj4Cached = false;
    mutable std::string proj4;

    mutable bool compactProjJsonCached = false;
    mutable std::string compactProjJson;

    mutable bool multilineProjJsonCached = false;
    mutable std::string multilineProjJson;

    mutable bool epsgOrEsriCodeCached = false;
    mutable std::string epsgOrEsriCode;

    mutable bool ogcUrnCached = false;
    mutable std::string ogcUrn;

    mutable bool linearUnitsCached = false;
    mutable UnitInfo linearUnits;

    mutable bool angularUnitsCached = false;
    mutable UnitInfo angularUnits;

    mutable bool metersPerUnitCached = false;
    mutable double metersPerUnit = 0.0;

    mutable bool semiMajorCached = false;
    mutable double semiMajor = 0.0;

    mutable bool semiMinorCached = false;
    mutable double semiMinor = 0.0;

    mutable bool invFlatteningCached = false;
    mutable double invFlattening = 0.0;

    mutable bool coordinateEpochCached = false;
    mutable double coordinateEpoch = 0.0;

    mutable bool areaOfUseCached = false;
    mutable std::vector<GB_Rectangle> areaOfUse;
    mutable std::string areaOfUseName;

    Impl()
    {
        SetTraditionalGisOrder(spatialReference);
    }

    void ClearCachesUnlocked() const
    {
        uniqueIdCached = false;
        uniqueId.clear();
        nameCached = false;
        name.clear();
        ellipsoidNameCached = false;
        ellipsoidName.clear();
        isGeographicCached = false;
        isGeographic = false;
        isProjectedCached = false;
        isProjected = false;
        isLocalCached = false;
        isLocal = false;
        isCompoundCached = false;
        isCompound = false;
        isDynamicCached = false;
        isDynamic = false;
        isCustomCached = false;
        isCustom = true;
        isLongitudeLatitudeOrderCached = false;
        isLongitudeLatitudeOrder = false;
        usesTraditionalGisOrderCached = false;
        usesTraditionalGisOrder = true;
        wktCached = false;
        wkt.clear();
        prettyWktCached = false;
        prettyWkt.clear();
        simplifiedPrettyWktCached = false;
        simplifiedPrettyWkt.clear();
        proj4Cached = false;
        proj4.clear();
        compactProjJsonCached = false;
        compactProjJson.clear();
        multilineProjJsonCached = false;
        multilineProjJson.clear();
        epsgOrEsriCodeCached = false;
        epsgOrEsriCode.clear();
        ogcUrnCached = false;
        ogcUrn.clear();
        linearUnitsCached = false;
        linearUnits = UnitInfo();
        angularUnitsCached = false;
        angularUnits = UnitInfo();
        metersPerUnitCached = false;
        metersPerUnit = 0.0;
        semiMajorCached = false;
        semiMajor = 0.0;
        semiMinorCached = false;
        semiMinor = 0.0;
        invFlatteningCached = false;
        invFlattening = 0.0;
        coordinateEpochCached = false;
        coordinateEpoch = 0.0;
        areaOfUseCached = false;
        areaOfUse.clear();
        areaOfUseName.clear();
    }
};

GeoCrs::GeoCrs()
    : impl_(new Impl())
{
}

GeoCrs::GeoCrs(const std::string& userInput, bool allowFileAccess, bool allowNetworkAccess)
    : impl_(new Impl())
{
    SetFromUserInput(userInput, allowFileAccess, allowNetworkAccess);
}

GeoCrs::GeoCrs(const OGRSpatialReference& spatialReference)
    : impl_(new Impl())
{
    SetFromOgrSpatialReference(spatialReference);
}

GeoCrs::GeoCrs(const GeoCrs& other)
    : impl_(new Impl())
{
    GB_ReadLockGuard otherReadGuard(other.impl_->cacheLock);
    AssignSpatialReference(impl_->spatialReference, other.impl_->spatialReference);
}

GeoCrs& GeoCrs::operator=(const GeoCrs& other)
{
    if (this == &other)
    {
        return *this;
    }

    OGRSpatialReference copiedSpatialReference;
    {
        GB_ReadLockGuard otherReadGuard(other.impl_->cacheLock);
        AssignSpatialReference(copiedSpatialReference, other.impl_->spatialReference);
    }

    GB_WriteLockGuard writeGuard(impl_->cacheLock);
    AssignSpatialReference(impl_->spatialReference, copiedSpatialReference);
    impl_->ClearCachesUnlocked();
    return *this;
}

GeoCrs::GeoCrs(GeoCrs&& other)
    : impl_(new Impl())
{
    impl_.swap(other.impl_);
}

GeoCrs& GeoCrs::operator=(GeoCrs&& other)
{
    if (this == &other)
    {
        return *this;
    }

    std::unique_ptr<Impl> emptyImpl(new Impl());
    impl_ = std::move(other.impl_);
    other.impl_ = std::move(emptyImpl);
    return *this;
}

GeoCrs::~GeoCrs() = default;

GeoCrs GeoCrs::FromUserInput(const std::string& userInput, bool allowFileAccess, bool allowNetworkAccess)
{
    return GeoCrs(userInput, allowFileAccess, allowNetworkAccess);
}

bool GeoCrs::TryFromUserInput(const std::string& userInput, GeoCrs* crs, bool allowFileAccess, bool allowNetworkAccess)
{
    if (crs == nullptr)
    {
        return false;
    }

    GeoCrs newCrs;
    if (!newCrs.SetFromUserInput(userInput, allowFileAccess, allowNetworkAccess))
    {
        crs->Reset();
        return false;
    }

    *crs = std::move(newCrs);
    return true;
}

bool GeoCrs::SetFromUserInput(const std::string& userInput, bool allowFileAccess, bool allowNetworkAccess)
{
    if (!InitializeRuntime())
    {
        Reset();
        return false;
    }

    const std::string trimmedUserInput = TrimAscii(userInput);
    if (trimmedUserInput.empty())
    {
        Reset();
        return false;
    }

    if (IsRejectedByAccessPolicy(trimmedUserInput, allowFileAccess, allowNetworkAccess))
    {
        Reset();
        return false;
    }

    OGRSpatialReference newSpatialReference;
#if GDAL_VERSION_NUM >= 3090000
    char** options = nullptr;
    options = CSLSetNameValue(options, "ALLOW_FILE_ACCESS", allowFileAccess ? "YES" : "NO");
    options = CSLSetNameValue(options, "ALLOW_NETWORK_ACCESS", allowNetworkAccess ? "YES" : "NO");
    const OGRErr errorCode = newSpatialReference.SetFromUserInput(trimmedUserInput.c_str(), options);
    CSLDestroy(options);
#else
    const OGRErr errorCode = newSpatialReference.SetFromUserInput(trimmedUserInput.c_str());
#endif
    if (errorCode != OGRERR_NONE || newSpatialReference.IsEmpty())
    {
        Reset();
        return false;
    }

    SetTraditionalGisOrder(newSpatialReference);

    GB_WriteLockGuard writeGuard(impl_->cacheLock);
    AssignSpatialReference(impl_->spatialReference, newSpatialReference);
    impl_->ClearCachesUnlocked();
    return true;
}

bool GeoCrs::SetFromOgrSpatialReference(const OGRSpatialReference& spatialReference)
{
    if (!InitializeRuntime())
    {
        Reset();
        return false;
    }

    if (spatialReference.IsEmpty())
    {
        Reset();
        return false;
    }

    OGRSpatialReference newSpatialReference;
    AssignSpatialReference(newSpatialReference, spatialReference);

    GB_WriteLockGuard writeGuard(impl_->cacheLock);
    AssignSpatialReference(impl_->spatialReference, newSpatialReference);
    impl_->ClearCachesUnlocked();
    return true;
}

void GeoCrs::Reset()
{
    GB_WriteLockGuard writeGuard(impl_->cacheLock);
    ClearSpatialReference(impl_->spatialReference);
    impl_->ClearCachesUnlocked();
}

bool GeoCrs::IsValid() const
{
    GB_ReadLockGuard readGuard(impl_->cacheLock);
    return !impl_->spatialReference.IsEmpty();
}

GeoCrs::operator bool() const
{
    return IsValid();
}

std::string GeoCrs::GetUniqueId() const
{
    {
        GB_ReadLockGuard readGuard(impl_->cacheLock);
        if (impl_->uniqueIdCached)
        {
            return impl_->uniqueId;
        }
    }

    GB_WriteLockGuard writeGuard(impl_->cacheLock);
    if (!impl_->uniqueIdCached)
    {
        impl_->uniqueId = MakeUniqueIdNoLock(impl_->spatialReference);
        impl_->uniqueIdCached = true;
    }
    return impl_->uniqueId;
}

bool GeoCrs::IsSame(const GeoCrs& other) const
{
    if (this == &other)
    {
        return IsValid();
    }

    OGRSpatialReference leftSpatialReference;
    if (!CopyToOgrSpatialReference(leftSpatialReference))
    {
        return false;
    }

    OGRSpatialReference rightSpatialReference;
    if (!other.CopyToOgrSpatialReference(rightSpatialReference))
    {
        return false;
    }

    return IsSameSpatialReferenceNoLock(leftSpatialReference, rightSpatialReference);
}

bool GeoCrs::operator==(const GeoCrs& other) const
{
    return IsSame(other);
}

bool GeoCrs::operator!=(const GeoCrs& other) const
{
    return !IsSame(other);
}

std::string GeoCrs::GetName() const
{
    {
        GB_ReadLockGuard readGuard(impl_->cacheLock);
        if (impl_->nameCached)
        {
            return impl_->name;
        }
    }

    GB_WriteLockGuard writeGuard(impl_->cacheLock);
    if (!impl_->nameCached)
    {
        impl_->name = ToString(impl_->spatialReference.GetName());
        impl_->nameCached = true;
    }
    return impl_->name;
}

std::string GeoCrs::GetReferenceEllipsoidName() const
{
    {
        GB_ReadLockGuard readGuard(impl_->cacheLock);
        if (impl_->ellipsoidNameCached)
        {
            return impl_->ellipsoidName;
        }
    }

    GB_WriteLockGuard writeGuard(impl_->cacheLock);
    if (!impl_->ellipsoidNameCached)
    {
        impl_->ellipsoidName = ToString(impl_->spatialReference.GetAttrValue("SPHEROID", 0));
        if (impl_->ellipsoidName.empty())
        {
            impl_->ellipsoidName = ToString(impl_->spatialReference.GetAttrValue("ELLIPSOID", 0));
        }
        impl_->ellipsoidNameCached = true;
    }
    return impl_->ellipsoidName;
}

bool GeoCrs::IsGeographic() const
{
    {
        GB_ReadLockGuard readGuard(impl_->cacheLock);
        if (impl_->isGeographicCached)
        {
            return impl_->isGeographic;
        }
    }

    GB_WriteLockGuard writeGuard(impl_->cacheLock);
    if (!impl_->isGeographicCached)
    {
        impl_->isGeographic = impl_->spatialReference.IsGeographic() != FALSE;
        impl_->isGeographicCached = true;
    }
    return impl_->isGeographic;
}

bool GeoCrs::IsProjected() const
{
    {
        GB_ReadLockGuard readGuard(impl_->cacheLock);
        if (impl_->isProjectedCached)
        {
            return impl_->isProjected;
        }
    }

    GB_WriteLockGuard writeGuard(impl_->cacheLock);
    if (!impl_->isProjectedCached)
    {
        impl_->isProjected = impl_->spatialReference.IsProjected() != FALSE;
        impl_->isProjectedCached = true;
    }
    return impl_->isProjected;
}

bool GeoCrs::IsLocal() const
{
    {
        GB_ReadLockGuard readGuard(impl_->cacheLock);
        if (impl_->isLocalCached)
        {
            return impl_->isLocal;
        }
    }

    GB_WriteLockGuard writeGuard(impl_->cacheLock);
    if (!impl_->isLocalCached)
    {
        impl_->isLocal = impl_->spatialReference.IsLocal() != FALSE;
        impl_->isLocalCached = true;
    }
    return impl_->isLocal;
}

bool GeoCrs::IsCompound() const
{
    {
        GB_ReadLockGuard readGuard(impl_->cacheLock);
        if (impl_->isCompoundCached)
        {
            return impl_->isCompound;
        }
    }

    GB_WriteLockGuard writeGuard(impl_->cacheLock);
    if (!impl_->isCompoundCached)
    {
        impl_->isCompound = impl_->spatialReference.IsCompound() != FALSE;
        impl_->isCompoundCached = true;
    }
    return impl_->isCompound;
}

bool GeoCrs::IsDynamic() const
{
    {
        GB_ReadLockGuard readGuard(impl_->cacheLock);
        if (impl_->isDynamicCached)
        {
            return impl_->isDynamic;
        }
    }

    GB_WriteLockGuard writeGuard(impl_->cacheLock);
    if (!impl_->isDynamicCached)
    {
#if GDAL_VERSION_NUM >= 3040000
        impl_->isDynamic = impl_->spatialReference.IsDynamic();
#if GDAL_VERSION_NUM >= 3080000
        if (!impl_->isDynamic)
        {
            impl_->isDynamic = impl_->spatialReference.HasPointMotionOperation();
        }
#endif
#else
        impl_->isDynamic = false;
#endif
        impl_->isDynamicCached = true;
    }
    return impl_->isDynamic;
}

bool GeoCrs::IsCustom() const
{
    {
        GB_ReadLockGuard readGuard(impl_->cacheLock);
        if (impl_->isCustomCached)
        {
            return impl_->isCustom;
        }
    }

    GB_WriteLockGuard writeGuard(impl_->cacheLock);
    if (!impl_->isCustomCached)
    {
        impl_->isCustom = !impl_->spatialReference.IsEmpty() && GetAuthorityStringNoLock(impl_->spatialReference, false).empty();
        impl_->isCustomCached = true;
    }
    return impl_->isCustom;
}

bool GeoCrs::IsLongitudeLatitudeOrder() const
{
    {
        GB_ReadLockGuard readGuard(impl_->cacheLock);
        if (impl_->isLongitudeLatitudeOrderCached)
        {
            return impl_->isLongitudeLatitudeOrder;
        }
    }

    GB_WriteLockGuard writeGuard(impl_->cacheLock);
    if (!impl_->isLongitudeLatitudeOrderCached)
    {
        impl_->isLongitudeLatitudeOrder = !impl_->spatialReference.IsEmpty() && impl_->spatialReference.IsGeographic() != FALSE && impl_->spatialReference.GetAxisMappingStrategy() == OAMS_TRADITIONAL_GIS_ORDER;
        impl_->isLongitudeLatitudeOrderCached = true;
    }
    return impl_->isLongitudeLatitudeOrder;
}

bool GeoCrs::UsesTraditionalGisOrder() const
{
    {
        GB_ReadLockGuard readGuard(impl_->cacheLock);
        if (impl_->usesTraditionalGisOrderCached)
        {
            return impl_->usesTraditionalGisOrder;
        }
    }

    GB_WriteLockGuard writeGuard(impl_->cacheLock);
    if (!impl_->usesTraditionalGisOrderCached)
    {
        impl_->usesTraditionalGisOrder = !impl_->spatialReference.IsEmpty() && impl_->spatialReference.GetAxisMappingStrategy() == OAMS_TRADITIONAL_GIS_ORDER;
        impl_->usesTraditionalGisOrderCached = true;
    }
    return impl_->usesTraditionalGisOrder;
}

std::string GeoCrs::ExportToWkt() const
{
    {
        GB_ReadLockGuard readGuard(impl_->cacheLock);
        if (impl_->wktCached)
        {
            return impl_->wkt;
        }
    }

    GB_WriteLockGuard writeGuard(impl_->cacheLock);
    if (!impl_->wktCached)
    {
        impl_->wkt = ExportWktNoLock(impl_->spatialReference, "FORMAT=WKT2_2019", false);
        impl_->wktCached = true;
    }
    return impl_->wkt;
}

std::string GeoCrs::ExportToPrettyWkt(bool simplify) const
{
    if (simplify)
    {
        {
            GB_ReadLockGuard readGuard(impl_->cacheLock);
            if (impl_->simplifiedPrettyWktCached)
            {
                return impl_->simplifiedPrettyWkt;
            }
        }

        GB_WriteLockGuard writeGuard(impl_->cacheLock);
        if (!impl_->simplifiedPrettyWktCached)
        {
            impl_->simplifiedPrettyWkt = ExportPrettyWktNoLock(impl_->spatialReference, true);
            impl_->simplifiedPrettyWktCached = true;
        }
        return impl_->simplifiedPrettyWkt;
    }

    {
        GB_ReadLockGuard readGuard(impl_->cacheLock);
        if (impl_->prettyWktCached)
        {
            return impl_->prettyWkt;
        }
    }

    GB_WriteLockGuard writeGuard(impl_->cacheLock);
    if (!impl_->prettyWktCached)
    {
        impl_->prettyWkt = ExportPrettyWktNoLock(impl_->spatialReference, false);
        impl_->prettyWktCached = true;
    }
    return impl_->prettyWkt;
}

std::string GeoCrs::ExportToProj4() const
{
    {
        GB_ReadLockGuard readGuard(impl_->cacheLock);
        if (impl_->proj4Cached)
        {
            return impl_->proj4;
        }
    }

    GB_WriteLockGuard writeGuard(impl_->cacheLock);
    if (!impl_->proj4Cached)
    {
        impl_->proj4 = ExportProj4NoLock(impl_->spatialReference);
        impl_->proj4Cached = true;
    }
    return impl_->proj4;
}

std::string GeoCrs::ExportToProjJson(bool multiline) const
{
    if (multiline)
    {
        {
            GB_ReadLockGuard readGuard(impl_->cacheLock);
            if (impl_->multilineProjJsonCached)
            {
                return impl_->multilineProjJson;
            }
        }

        GB_WriteLockGuard writeGuard(impl_->cacheLock);
        if (!impl_->multilineProjJsonCached)
        {
            impl_->multilineProjJson = ExportProjJsonNoLock(impl_->spatialReference, true);
            impl_->multilineProjJsonCached = true;
        }
        return impl_->multilineProjJson;
    }

    {
        GB_ReadLockGuard readGuard(impl_->cacheLock);
        if (impl_->compactProjJsonCached)
        {
            return impl_->compactProjJson;
        }
    }

    GB_WriteLockGuard writeGuard(impl_->cacheLock);
    if (!impl_->compactProjJsonCached)
    {
        impl_->compactProjJson = ExportProjJsonNoLock(impl_->spatialReference, false);
        impl_->compactProjJsonCached = true;
    }
    return impl_->compactProjJson;
}

std::string GeoCrs::GetEpsgOrEsriCode() const
{
    {
        GB_ReadLockGuard readGuard(impl_->cacheLock);
        if (impl_->epsgOrEsriCodeCached)
        {
            return impl_->epsgOrEsriCode;
        }
    }

    GB_WriteLockGuard writeGuard(impl_->cacheLock);
    if (!impl_->epsgOrEsriCodeCached)
    {
        impl_->epsgOrEsriCode = GetAuthorityStringNoLock(impl_->spatialReference, true);
        impl_->epsgOrEsriCodeCached = true;
    }
    return impl_->epsgOrEsriCode;
}

std::string GeoCrs::GetOgcUrn() const
{
    {
        GB_ReadLockGuard readGuard(impl_->cacheLock);
        if (impl_->ogcUrnCached)
        {
            return impl_->ogcUrn;
        }
    }

    GB_WriteLockGuard writeGuard(impl_->cacheLock);
    if (!impl_->ogcUrnCached)
    {
        impl_->ogcUrn = ExportOgcUrnNoLock(impl_->spatialReference);
        impl_->ogcUrnCached = true;
    }
    return impl_->ogcUrn;
}

GeoCrs::UnitInfo GeoCrs::GetLinearUnits() const
{
    {
        GB_ReadLockGuard readGuard(impl_->cacheLock);
        if (impl_->linearUnitsCached)
        {
            return impl_->linearUnits;
        }
    }

    GB_WriteLockGuard writeGuard(impl_->cacheLock);
    if (!impl_->linearUnitsCached)
    {
        impl_->linearUnits = MakeLinearUnitInfoNoLock(impl_->spatialReference);
        impl_->linearUnitsCached = true;
    }
    return impl_->linearUnits;
}

GeoCrs::UnitInfo GeoCrs::GetAngularUnits() const
{
    {
        GB_ReadLockGuard readGuard(impl_->cacheLock);
        if (impl_->angularUnitsCached)
        {
            return impl_->angularUnits;
        }
    }

    GB_WriteLockGuard writeGuard(impl_->cacheLock);
    if (!impl_->angularUnitsCached)
    {
        impl_->angularUnits = MakeAngularUnitInfoNoLock(impl_->spatialReference);
        impl_->angularUnitsCached = true;
    }
    return impl_->angularUnits;
}

double GeoCrs::GetMetersPerUnit() const
{
    {
        GB_ReadLockGuard readGuard(impl_->cacheLock);
        if (impl_->metersPerUnitCached)
        {
            return impl_->metersPerUnit;
        }
    }

    GB_WriteLockGuard writeGuard(impl_->cacheLock);
    if (!impl_->metersPerUnitCached)
    {
        impl_->metersPerUnit = 0.0;
        if (!impl_->spatialReference.IsEmpty())
        {
            const bool shouldUseLinearUnits = impl_->spatialReference.IsProjected() != FALSE || impl_->spatialReference.IsLocal() != FALSE || impl_->spatialReference.IsCompound() != FALSE || impl_->spatialReference.IsVertical() != FALSE || impl_->spatialReference.IsGeocentric() != FALSE;
            if (shouldUseLinearUnits)
            {
                const UnitInfo linearUnits = MakeLinearUnitInfoNoLock(impl_->spatialReference);
                impl_->metersPerUnit = linearUnits.isValid ? linearUnits.conversionFactor : 0.0;
            }

            if (impl_->metersPerUnit <= 0.0 && impl_->spatialReference.IsGeographic() != FALSE)
            {
                const UnitInfo angularUnits = MakeAngularUnitInfoNoLock(impl_->spatialReference);
                OGRErr errorCode = OGRERR_NONE;
                const double semiMajor = impl_->spatialReference.GetSemiMajor(&errorCode);
                if (angularUnits.isValid && errorCode == OGRERR_NONE && IsFinite(semiMajor) && semiMajor > 0.0)
                {
                    impl_->metersPerUnit = angularUnits.conversionFactor * semiMajor;
                }
            }
        }
        impl_->metersPerUnitCached = true;
    }
    return impl_->metersPerUnit;
}

bool GeoCrs::TryGetMetersPerUnit(double* metersPerUnit) const
{
    if (metersPerUnit == nullptr)
    {
        return false;
    }

    const double result = GetMetersPerUnit();
    if (!IsFinite(result) || result <= 0.0)
    {
        *metersPerUnit = 0.0;
        return false;
    }

    *metersPerUnit = result;
    return true;
}

double GeoCrs::GetSemiMajor() const
{
    {
        GB_ReadLockGuard readGuard(impl_->cacheLock);
        if (impl_->semiMajorCached)
        {
            return impl_->semiMajor;
        }
    }

    GB_WriteLockGuard writeGuard(impl_->cacheLock);
    if (!impl_->semiMajorCached)
    {
        impl_->semiMajor = 0.0;
        OGRErr errorCode = OGRERR_NONE;
        const double result = impl_->spatialReference.GetSemiMajor(&errorCode);
        if (errorCode == OGRERR_NONE && IsFinite(result) && result > 0.0)
        {
            impl_->semiMajor = result;
        }
        impl_->semiMajorCached = true;
    }
    return impl_->semiMajor;
}

double GeoCrs::GetSemiMinor() const
{
    {
        GB_ReadLockGuard readGuard(impl_->cacheLock);
        if (impl_->semiMinorCached)
        {
            return impl_->semiMinor;
        }
    }

    GB_WriteLockGuard writeGuard(impl_->cacheLock);
    if (!impl_->semiMinorCached)
    {
        impl_->semiMinor = 0.0;
        OGRErr errorCode = OGRERR_NONE;
        const double result = impl_->spatialReference.GetSemiMinor(&errorCode);
        if (errorCode == OGRERR_NONE && IsFinite(result) && result > 0.0)
        {
            impl_->semiMinor = result;
        }
        impl_->semiMinorCached = true;
    }
    return impl_->semiMinor;
}

double GeoCrs::GetInvFlattening() const
{
    {
        GB_ReadLockGuard readGuard(impl_->cacheLock);
        if (impl_->invFlatteningCached)
        {
            return impl_->invFlattening;
        }
    }

    GB_WriteLockGuard writeGuard(impl_->cacheLock);
    if (!impl_->invFlatteningCached)
    {
        impl_->invFlattening = 0.0;
        OGRErr errorCode = OGRERR_NONE;
        const double result = impl_->spatialReference.GetInvFlattening(&errorCode);
        if (errorCode == OGRERR_NONE && IsFinite(result) && result > 0.0)
        {
            impl_->invFlattening = result;
        }
        impl_->invFlatteningCached = true;
    }
    return impl_->invFlattening;
}

double GeoCrs::GetCoordinateEpoch() const
{
    {
        GB_ReadLockGuard readGuard(impl_->cacheLock);
        if (impl_->coordinateEpochCached)
        {
            return impl_->coordinateEpoch;
        }
    }

    GB_WriteLockGuard writeGuard(impl_->cacheLock);
    if (!impl_->coordinateEpochCached)
    {
#if GDAL_VERSION_NUM >= 3040000
        const double result = impl_->spatialReference.GetCoordinateEpoch();
        impl_->coordinateEpoch = IsFinite(result) ? result : 0.0;
#else
        impl_->coordinateEpoch = 0.0;
#endif
        impl_->coordinateEpochCached = true;
    }
    return impl_->coordinateEpoch;
}

std::vector<GB_Rectangle> GeoCrs::GetGeographicAreaOfUse() const
{
    {
        GB_ReadLockGuard readGuard(impl_->cacheLock);
        if (impl_->areaOfUseCached)
        {
            return impl_->areaOfUse;
        }
    }

    GB_WriteLockGuard writeGuard(impl_->cacheLock);
    if (!impl_->areaOfUseCached)
    {
        double westLongitude = 0.0;
        double southLatitude = 0.0;
        double eastLongitude = 0.0;
        double northLatitude = 0.0;
        const char* areaName = nullptr;
        if (!impl_->spatialReference.IsEmpty() && impl_->spatialReference.GetAreaOfUse(&westLongitude, &southLatitude, &eastLongitude, &northLatitude, &areaName))
        {
            impl_->areaOfUse = SplitAreaOfUse(westLongitude, southLatitude, eastLongitude, northLatitude);
            impl_->areaOfUseName = ToString(areaName);
        }
        else
        {
            impl_->areaOfUse.clear();
            impl_->areaOfUseName.clear();
        }
        impl_->areaOfUseCached = true;
    }
    return impl_->areaOfUse;
}

std::string GeoCrs::GetAreaOfUseName() const
{
    (void)GetGeographicAreaOfUse();

    GB_ReadLockGuard readGuard(impl_->cacheLock);
    return impl_->areaOfUseName;
}

bool GeoCrs::TryGetGeographicAreaOfUse(std::vector<GB_Rectangle>* rectangles, std::string* areaName) const
{
    if (rectangles == nullptr)
    {
        return false;
    }

    *rectangles = GetGeographicAreaOfUse();
    if (areaName != nullptr)
    {
        *areaName = GetAreaOfUseName();
    }
    return !rectangles->empty();
}

const OGRSpatialReference& GeoCrs::GetOgrSpatialReference() const
{
    return impl_->spatialReference;
}

bool GeoCrs::CopyToOgrSpatialReference(OGRSpatialReference& spatialReference) const
{
    GB_ReadLockGuard readGuard(impl_->cacheLock);
    if (impl_->spatialReference.IsEmpty())
    {
        ClearSpatialReference(spatialReference);
        return false;
    }

    AssignSpatialReference(spatialReference, impl_->spatialReference);
    return true;
}
