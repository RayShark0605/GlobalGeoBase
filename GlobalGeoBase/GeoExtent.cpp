#include "GeoExtent.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace
{
    constexpr static double minLongitude = -180.0;
    constexpr static double maxLongitude = 180.0;
    constexpr static double minLatitude = -90.0;
    constexpr static double maxLatitude = 90.0;
    constexpr static double coordinateTolerance = 1e-12;
    constexpr static std::size_t maxNormalizedRectangleCount = 2;

    struct NormalizedRectangleSet
    {
        GB_Rectangle rectangles[maxNormalizedRectangleCount];
        std::size_t count = 0;

        void Clear()
        {
            count = 0;
            for (std::size_t i = 0; i < maxNormalizedRectangleCount; i++)
            {
                rectangles[i].Reset();
            }
        }

        void PushBack(const GB_Rectangle& rectangle)
        {
            if (count >= maxNormalizedRectangleCount)
            {
                return;
            }

            rectangles[count] = rectangle;
            count++;
        }
    };

    double GetNan()
    {
        return std::numeric_limits<double>::quiet_NaN();
    }

    bool IsFinite(double value)
    {
        return std::isfinite(value);
    }

    double NormalizeTolerance(double tolerance)
    {
        if (!IsFinite(tolerance))
        {
            return 0.0;
        }

        return std::fabs(tolerance);
    }

    bool IsFiniteRectangle(const GB_Rectangle& rectangle)
    {
        return IsFinite(rectangle.minX) && IsFinite(rectangle.minY) && IsFinite(rectangle.maxX) && IsFinite(rectangle.maxY);
    }

    bool IsInRange(double value, double minValue, double maxValue, double tolerance)
    {
        const double safeTolerance = NormalizeTolerance(tolerance);
        return IsFinite(value) && value >= minValue - safeTolerance && value <= maxValue + safeTolerance;
    }

    bool IsNearEqual(double leftValue, double rightValue, double tolerance)
    {
        const double safeTolerance = NormalizeTolerance(tolerance);
        return IsFinite(leftValue) && IsFinite(rightValue) && std::fabs(leftValue - rightValue) <= safeTolerance;
    }

    double Clamp(double value, double minValue, double maxValue)
    {
        if (value < minValue)
        {
            return minValue;
        }
        if (value > maxValue)
        {
            return maxValue;
        }
        return value;
    }

    double NormalizeLongitude(double longitude)
    {
        if (!IsFinite(longitude))
        {
            return longitude;
        }

        if (longitude < minLongitude || longitude > maxLongitude)
        {
            longitude = std::fmod(longitude + 180.0, 360.0);
            if (longitude < 0.0)
            {
                longitude += 360.0;
            }
            longitude -= 180.0;
        }

        if (IsNearEqual(longitude, maxLongitude, coordinateTolerance))
        {
            return maxLongitude;
        }
        if (IsNearEqual(longitude, minLongitude, coordinateTolerance))
        {
            return minLongitude;
        }
        return longitude;
    }

    double SafeDifference(double maxValue, double minValue)
    {
        const double value = maxValue - minValue;
        return IsFinite(value) ? value : GetNan();
    }

    double SafeMidpoint(double leftValue, double rightValue)
    {
        if (!IsFinite(leftValue) || !IsFinite(rightValue))
        {
            return GetNan();
        }

        const double value = leftValue * 0.5 + rightValue * 0.5;
        return IsFinite(value) ? value : GetNan();
    }

    GB_Rectangle MakeRawRectangle(double minX, double minY, double maxX, double maxY)
    {
        GB_Rectangle rectangle;
        rectangle.minX = minX;
        rectangle.minY = minY;
        rectangle.maxX = maxX;
        rectangle.maxY = maxY;
        return rectangle;
    }

    bool IsValidPlanarRectangle(const GB_Rectangle& rectangle)
    {
        return IsFiniteRectangle(rectangle) && rectangle.minX <= rectangle.maxX && rectangle.minY <= rectangle.maxY;
    }

    bool IsValidGeographicRectangle(const GB_Rectangle& rectangle)
    {
        if (!IsFiniteRectangle(rectangle))
        {
            return false;
        }

        if (rectangle.minY > rectangle.maxY)
        {
            return false;
        }

        if (!IsInRange(rectangle.minX, minLongitude, maxLongitude, coordinateTolerance) || !IsInRange(rectangle.maxX, minLongitude, maxLongitude, coordinateTolerance))
        {
            return false;
        }

        if (!IsInRange(rectangle.minY, minLatitude, maxLatitude, coordinateTolerance) || !IsInRange(rectangle.maxY, minLatitude, maxLatitude, coordinateTolerance))
        {
            return false;
        }

        return true;
    }

    GB_Rectangle MakeClampedGeographicRectangle(const GB_Rectangle& rectangle)
    {
        return MakeRawRectangle(Clamp(rectangle.minX, minLongitude, maxLongitude), Clamp(rectangle.minY, minLatitude, maxLatitude), Clamp(rectangle.maxX, minLongitude, maxLongitude), Clamp(rectangle.maxY, minLatitude, maxLatitude));
    }

    double GeographicWidth(const GB_Rectangle& clampedRectangle)
    {
        double value = GetNan();
        if (clampedRectangle.minX > clampedRectangle.maxX)
        {
            value = maxLongitude - clampedRectangle.minX + clampedRectangle.maxX - minLongitude;
        }
        else
        {
            value = clampedRectangle.maxX - clampedRectangle.minX;
        }

        return IsFinite(value) ? value : GetNan();
    }

    bool IsPointInRectangle(const GB_Rectangle& rectangle, const GB_Point2d& point, double tolerance)
    {
        const double safeTolerance = NormalizeTolerance(tolerance);
        if (!IsFiniteRectangle(rectangle) || !IsFinite(point.x) || !IsFinite(point.y))
        {
            return false;
        }

        return point.x >= rectangle.minX - safeTolerance && point.x <= rectangle.maxX + safeTolerance && point.y >= rectangle.minY - safeTolerance && point.y <= rectangle.maxY + safeTolerance;
    }

    bool IsRectangleContains(const GB_Rectangle& rectangle, const GB_Rectangle& other, double tolerance)
    {
        const double safeTolerance = NormalizeTolerance(tolerance);
        if (!IsFiniteRectangle(rectangle) || !IsFiniteRectangle(other))
        {
            return false;
        }

        return other.minX >= rectangle.minX - safeTolerance && other.maxX <= rectangle.maxX + safeTolerance && other.minY >= rectangle.minY - safeTolerance && other.maxY <= rectangle.maxY + safeTolerance;
    }

    bool TryIntersectRectangles(const GB_Rectangle& leftRectangle, const GB_Rectangle& rightRectangle, double tolerance, GB_Rectangle* outRectangle)
    {
        if (outRectangle == nullptr)
        {
            return false;
        }

        outRectangle->Reset();
        if (!IsValidPlanarRectangle(leftRectangle) || !IsValidPlanarRectangle(rightRectangle))
        {
            return false;
        }

        const double safeTolerance = NormalizeTolerance(tolerance);
        double minX = std::max(leftRectangle.minX, rightRectangle.minX);
        double minY = std::max(leftRectangle.minY, rightRectangle.minY);
        double maxX = std::min(leftRectangle.maxX, rightRectangle.maxX);
        double maxY = std::min(leftRectangle.maxY, rightRectangle.maxY);

        if (minX > maxX + safeTolerance || minY > maxY + safeTolerance)
        {
            return false;
        }

        if (minX > maxX)
        {
            const double middleX = SafeMidpoint(minX, maxX);
            minX = middleX;
            maxX = middleX;
        }
        if (minY > maxY)
        {
            const double middleY = SafeMidpoint(minY, maxY);
            minY = middleY;
            maxY = middleY;
        }

        *outRectangle = MakeRawRectangle(minX, minY, maxX, maxY);
        return outRectangle->IsValid();
    }

    bool IsSameRectangle(const GB_Rectangle& leftRectangle, const GB_Rectangle& rightRectangle, double tolerance)
    {
        const double safeTolerance = NormalizeTolerance(tolerance);
        return IsNearEqual(leftRectangle.minX, rightRectangle.minX, safeTolerance) && IsNearEqual(leftRectangle.minY, rightRectangle.minY, safeTolerance) && IsNearEqual(leftRectangle.maxX, rightRectangle.maxX, safeTolerance) && IsNearEqual(leftRectangle.maxY, rightRectangle.maxY, safeTolerance);
    }

    bool BuildNormalizedRectangleSet(const GeoExtent& extent, NormalizedRectangleSet* outSet)
    {
        if (outSet == nullptr)
        {
            return false;
        }

        outSet->Clear();
        if (!extent.IsValid())
        {
            return false;
        }

        if (!extent.crs.IsGeographic())
        {
            outSet->PushBack(extent.rectangle);
            return true;
        }

        const GB_Rectangle clampedRectangle = MakeClampedGeographicRectangle(extent.rectangle);
        if (clampedRectangle.minX <= clampedRectangle.maxX)
        {
            outSet->PushBack(clampedRectangle);
            return true;
        }

        outSet->PushBack(MakeRawRectangle(clampedRectangle.minX, clampedRectangle.minY, maxLongitude, clampedRectangle.maxY));
        outSet->PushBack(MakeRawRectangle(minLongitude, clampedRectangle.minY, clampedRectangle.maxX, clampedRectangle.maxY));
        return true;
    }

    void PushUniqueExtent(std::vector<GeoExtent>* extents, const GeoExtent& extent, double tolerance)
    {
        if (extents == nullptr || !extent.IsValid())
        {
            return;
        }

        const double safeTolerance = NormalizeTolerance(tolerance);
        for (const GeoExtent& existingExtent : *extents)
        {
            if (existingExtent.HasSameCrs(extent) && IsSameRectangle(existingExtent.rectangle, extent.rectangle, safeTolerance))
            {
                return;
            }
        }

        extents->push_back(extent);
    }
}

GeoExtent::GeoExtent()
{
}

GeoExtent::GeoExtent(const GeoCrs& inputCrs)
    : crs(inputCrs)
{
}

GeoExtent::GeoExtent(const GeoCrs& inputCrs, const GB_Rectangle& inputRectangle)
    : crs(inputCrs), rectangle(inputRectangle)
{
}

GeoExtent::GeoExtent(const std::string& crsUserInput, bool allowFileAccess, bool allowNetworkAccess)
    : crs(crsUserInput, allowFileAccess, allowNetworkAccess)
{
}

GeoExtent::GeoExtent(const std::string& crsUserInput, const GB_Rectangle& inputRectangle, bool allowFileAccess, bool allowNetworkAccess)
    : crs(crsUserInput, allowFileAccess, allowNetworkAccess), rectangle(inputRectangle)
{
}

GeoExtent GeoExtent::FromGeographicBounds(const GeoCrs& inputCrs, double westLongitude, double southLatitude, double eastLongitude, double northLatitude)
{
    GeoExtent result(inputCrs);
    result.rectangle = MakeRawRectangle(westLongitude, southLatitude, eastLongitude, northLatitude);

    if (!result.crs.IsGeographic() || !result.IsValid())
    {
        result.Reset();
    }

    return result;
}

void GeoExtent::Reset()
{
    crs.Reset();
    rectangle.Reset();
}

bool GeoExtent::IsValid() const
{
    if (!crs.IsValid())
    {
        return false;
    }

    if (crs.IsGeographic())
    {
        return IsValidGeographicRectangle(rectangle);
    }

    return IsValidPlanarRectangle(rectangle);
}

bool GeoExtent::IsCrossesAntimeridian() const
{
    if (!IsValid() || !crs.IsGeographic())
    {
        return false;
    }

    const GB_Rectangle clampedRectangle = MakeClampedGeographicRectangle(rectangle);
    return clampedRectangle.minX > clampedRectangle.maxX;
}

bool GeoExtent::HasSameCrs(const GeoExtent& other) const
{
    return crs.IsValid() && other.crs.IsValid() && crs.IsSame(other.crs);
}

double GeoExtent::Width() const
{
    if (!IsValid())
    {
        return GetNan();
    }

    if (crs.IsGeographic())
    {
        const GB_Rectangle clampedRectangle = MakeClampedGeographicRectangle(rectangle);
        return GeographicWidth(clampedRectangle);
    }

    return SafeDifference(rectangle.maxX, rectangle.minX);
}

double GeoExtent::Height() const
{
    if (!IsValid())
    {
        return GetNan();
    }

    if (crs.IsGeographic())
    {
        const GB_Rectangle clampedRectangle = MakeClampedGeographicRectangle(rectangle);
        return SafeDifference(clampedRectangle.maxY, clampedRectangle.minY);
    }

    return SafeDifference(rectangle.maxY, rectangle.minY);
}

GB_Point2d GeoExtent::Center() const
{
    if (!IsValid())
    {
        return GB_Point2d(GetNan(), GetNan());
    }

    if (!crs.IsGeographic())
    {
        return GB_Point2d(SafeMidpoint(rectangle.minX, rectangle.maxX), SafeMidpoint(rectangle.minY, rectangle.maxY));
    }

    const GB_Rectangle clampedRectangle = MakeClampedGeographicRectangle(rectangle);
    double centerX = SafeMidpoint(clampedRectangle.minX, clampedRectangle.maxX);
    if (clampedRectangle.minX > clampedRectangle.maxX)
    {
        centerX = NormalizeLongitude(clampedRectangle.minX + GeographicWidth(clampedRectangle) * 0.5);
    }

    return GB_Point2d(centerX, SafeMidpoint(clampedRectangle.minY, clampedRectangle.maxY));
}

bool GeoExtent::IsContains(const GB_Point2d& point, double tolerance) const
{
    const double safeTolerance = NormalizeTolerance(tolerance);
    if (!IsValid() || !IsFinite(point.x) || !IsFinite(point.y))
    {
        return false;
    }

    if (!crs.IsGeographic())
    {
        return IsPointInRectangle(rectangle, point, safeTolerance);
    }

    if (!IsInRange(point.x, minLongitude, maxLongitude, safeTolerance) || !IsInRange(point.y, minLatitude, maxLatitude, safeTolerance))
    {
        return false;
    }

    const GB_Rectangle clampedRectangle = MakeClampedGeographicRectangle(rectangle);
    const double longitude = Clamp(point.x, minLongitude, maxLongitude);
    const double latitude = Clamp(point.y, minLatitude, maxLatitude);
    if (latitude < clampedRectangle.minY - safeTolerance || latitude > clampedRectangle.maxY + safeTolerance)
    {
        return false;
    }

    if (clampedRectangle.minX <= clampedRectangle.maxX)
    {
        return longitude >= clampedRectangle.minX - safeTolerance && longitude <= clampedRectangle.maxX + safeTolerance;
    }

    return longitude >= clampedRectangle.minX - safeTolerance || longitude <= clampedRectangle.maxX + safeTolerance;
}

bool GeoExtent::IsContains(const GeoExtent& other, double tolerance) const
{
    if (!HasSameCrs(other))
    {
        return false;
    }

    const double safeTolerance = NormalizeTolerance(tolerance);
    NormalizedRectangleSet currentRectangles;
    NormalizedRectangleSet otherRectangles;
    if (!BuildNormalizedRectangleSet(*this, &currentRectangles) || !BuildNormalizedRectangleSet(other, &otherRectangles))
    {
        return false;
    }

    for (std::size_t i = 0; i < otherRectangles.count; i++)
    {
        bool contained = false;
        for (std::size_t j = 0; j < currentRectangles.count; j++)
        {
            if (IsRectangleContains(currentRectangles.rectangles[j], otherRectangles.rectangles[i], safeTolerance))
            {
                contained = true;
                break;
            }
        }

        if (!contained)
        {
            return false;
        }
    }

    return true;
}

bool GeoExtent::IsIntersects(const GeoExtent& other, double tolerance) const
{
    if (!HasSameCrs(other))
    {
        return false;
    }

    const double safeTolerance = NormalizeTolerance(tolerance);
    NormalizedRectangleSet currentRectangles;
    NormalizedRectangleSet otherRectangles;
    if (!BuildNormalizedRectangleSet(*this, &currentRectangles) || !BuildNormalizedRectangleSet(other, &otherRectangles))
    {
        return false;
    }

    for (std::size_t i = 0; i < currentRectangles.count; i++)
    {
        for (std::size_t j = 0; j < otherRectangles.count; j++)
        {
            GB_Rectangle intersection;
            if (TryIntersectRectangles(currentRectangles.rectangles[i], otherRectangles.rectangles[j], safeTolerance, &intersection))
            {
                return true;
            }
        }
    }

    return false;
}

std::vector<GeoExtent> GeoExtent::Intersected(const GeoExtent& other, double tolerance) const
{
    std::vector<GeoExtent> extents;
    if (!HasSameCrs(other))
    {
        return extents;
    }

    const double safeTolerance = NormalizeTolerance(tolerance);
    NormalizedRectangleSet currentRectangles;
    NormalizedRectangleSet otherRectangles;
    if (!BuildNormalizedRectangleSet(*this, &currentRectangles) || !BuildNormalizedRectangleSet(other, &otherRectangles))
    {
        return extents;
    }

    extents.reserve(currentRectangles.count * otherRectangles.count);
    for (std::size_t i = 0; i < currentRectangles.count; i++)
    {
        for (std::size_t j = 0; j < otherRectangles.count; j++)
        {
            GB_Rectangle intersection;
            if (TryIntersectRectangles(currentRectangles.rectangles[i], otherRectangles.rectangles[j], safeTolerance, &intersection))
            {
                PushUniqueExtent(&extents, GeoExtent(crs, intersection), safeTolerance);
            }
        }
    }

    return extents;
}

std::vector<GB_Rectangle> GeoExtent::GetNormalizedRectangles() const
{
    std::vector<GB_Rectangle> rectangles;
    NormalizedRectangleSet rectangleSet;
    if (!BuildNormalizedRectangleSet(*this, &rectangleSet))
    {
        return rectangles;
    }

    rectangles.reserve(rectangleSet.count);
    for (std::size_t i = 0; i < rectangleSet.count; i++)
    {
        rectangles.push_back(rectangleSet.rectangles[i]);
    }

    return rectangles;
}

std::vector<GeoExtent> GeoExtent::GetNormalizedExtents() const
{
    std::vector<GeoExtent> extents;
    NormalizedRectangleSet rectangleSet;
    if (!BuildNormalizedRectangleSet(*this, &rectangleSet))
    {
        return extents;
    }

    extents.reserve(rectangleSet.count);
    for (std::size_t i = 0; i < rectangleSet.count; i++)
    {
        extents.push_back(GeoExtent(crs, rectangleSet.rectangles[i]));
    }

    return extents;
}
