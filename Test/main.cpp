#include "GeoCrs.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace
{
    struct TestStatus
    {
        int numPassed = 0;
        int numFailed = 0;
    };

    bool IsNearlyEqual(double actualValue, double expectedValue, double tolerance)
    {
        return std::fabs(actualValue - expectedValue) <= tolerance;
    }

    bool ContainsText(const std::string& text, const std::string& subText)
    {
        return text.find(subText) != std::string::npos;
    }

    void PrintTitle(const char* title)
    {
        std::printf("\n========== %s ==========\n", title);
    }

    void PrintKeyValue(const char* key, const std::string& value)
    {
        std::printf("%s: %s\n", key, value.empty() ? "<empty>" : value.c_str());
    }

    void PrintKeyValue(const char* key, bool value)
    {
        std::printf("%s: %s\n", key, value ? "true" : "false");
    }

    void PrintKeyValue(const char* key, double value)
    {
        std::printf("%s: %.15g\n", key, value);
    }

    void PrintUnitInfo(const char* key, const GeoCrs::UnitInfo& unitInfo)
    {
        if (!unitInfo.isValid)
        {
            std::printf("%s: <invalid>\n", key);
            return;
        }

        std::printf("%s: name=%s, conversionFactor=%.15g\n", key, unitInfo.name.c_str(), unitInfo.conversionFactor);
    }

    void PrintCrsBrief(const char* title, const GeoCrs& crs)
    {
        PrintTitle(title);
        PrintKeyValue("IsValid", crs.IsValid());
        if (!crs.IsValid())
        {
            return;
        }

        PrintKeyValue("Name", crs.GetName());
        PrintKeyValue("UniqueId", crs.GetUniqueId());
        PrintKeyValue("Authority", crs.GetEpsgOrEsriCode());
        PrintKeyValue("IsGeographic", crs.IsGeographic());
        PrintKeyValue("IsProjected", crs.IsProjected());
        PrintKeyValue("IsLocal", crs.IsLocal());
        PrintKeyValue("IsCompound", crs.IsCompound());
        PrintKeyValue("IsDynamic", crs.IsDynamic());
        PrintKeyValue("IsCustom", crs.IsCustom());
        PrintKeyValue("UsesTraditionalGisOrder", crs.UsesTraditionalGisOrder());
        PrintKeyValue("IsLongitudeLatitudeOrder", crs.IsLongitudeLatitudeOrder());
        PrintKeyValue("ReferenceEllipsoidName", crs.GetReferenceEllipsoidName());
        PrintKeyValue("SemiMajor", crs.GetSemiMajor());
        PrintKeyValue("SemiMinor", crs.GetSemiMinor());
        PrintKeyValue("InvFlattening", crs.GetInvFlattening());
        PrintKeyValue("MetersPerUnit", crs.GetMetersPerUnit());
        PrintKeyValue("CoordinateEpoch", crs.GetCoordinateEpoch());
        PrintUnitInfo("LinearUnits", crs.GetLinearUnits());
        PrintUnitInfo("AngularUnits", crs.GetAngularUnits());
        std::printf("WktLength: %llu\n", static_cast<unsigned long long>(crs.ExportToWkt().size()));
        std::printf("PrettyWktLength: %llu\n", static_cast<unsigned long long>(crs.ExportToPrettyWkt(false).size()));
        std::printf("SimplifiedPrettyWktLength: %llu\n", static_cast<unsigned long long>(crs.ExportToPrettyWkt(true).size()));
        std::printf("Proj4Length: %llu\n", static_cast<unsigned long long>(crs.ExportToProj4().size()));
        std::printf("ProjJsonLength: %llu\n", static_cast<unsigned long long>(crs.ExportToProjJson(false).size()));
    }

    void RecordResult(bool condition, const char* testName, TestStatus* testStatus)
    {
        if (testStatus == nullptr)
        {
            return;
        }

        if (condition)
        {
            testStatus->numPassed++;
            std::printf("[PASS] %s\n", testName);
            return;
        }

        testStatus->numFailed++;
        std::printf("[FAIL] %s\n", testName);
    }

    void ExpectTrue(bool condition, const char* testName, TestStatus* testStatus)
    {
        RecordResult(condition, testName, testStatus);
    }

    void ExpectFalse(bool condition, const char* testName, TestStatus* testStatus)
    {
        RecordResult(!condition, testName, testStatus);
    }

    void ExpectEqual(const std::string& actualValue, const std::string& expectedValue, const char* testName, TestStatus* testStatus)
    {
        const bool isEqual = actualValue == expectedValue;
        RecordResult(isEqual, testName, testStatus);
        if (!isEqual)
        {
            std::printf("       actual  : %s\n", actualValue.c_str());
            std::printf("       expected: %s\n", expectedValue.c_str());
        }
    }

    void ExpectNearlyEqual(double actualValue, double expectedValue, double tolerance, const char* testName, TestStatus* testStatus)
    {
        const bool isEqual = IsNearlyEqual(actualValue, expectedValue, tolerance);
        RecordResult(isEqual, testName, testStatus);
        if (!isEqual)
        {
            std::printf("       actual   : %.15g\n", actualValue);
            std::printf("       expected : %.15g\n", expectedValue);
            std::printf("       tolerance: %.15g\n", tolerance);
        }
    }

    void ExpectNotEmpty(const std::string& value, const char* testName, TestStatus* testStatus)
    {
        RecordResult(!value.empty(), testName, testStatus);
    }

    void ExpectContains(const std::string& text, const std::string& subText, const char* testName, TestStatus* testStatus)
    {
        const bool containsText = ContainsText(text, subText);
        RecordResult(containsText, testName, testStatus);
        if (!containsText)
        {
            std::printf("       text   : %s\n", text.c_str());
            std::printf("       subText: %s\n", subText.c_str());
        }
    }

    void TestDefaultState(TestStatus* testStatus)
    {
        PrintTitle("TestDefaultState");

        const GeoCrs crs;
        ExpectFalse(crs.IsValid(), "default constructed CRS should be invalid", testStatus);
        ExpectFalse(static_cast<bool>(crs), "default constructed CRS bool conversion should be false", testStatus);
        ExpectEqual(crs.GetUniqueId(), std::string(), "invalid CRS unique id should be empty", testStatus);
        ExpectEqual(crs.GetName(), std::string(), "invalid CRS name should be empty", testStatus);
        ExpectEqual(crs.GetEpsgOrEsriCode(), std::string(), "invalid CRS authority should be empty", testStatus);
        ExpectEqual(crs.GetOgcUrn(), std::string(), "invalid CRS urn should be empty", testStatus);
        ExpectEqual(crs.ExportToWkt(), std::string(), "invalid CRS WKT should be empty", testStatus);
        ExpectEqual(crs.ExportToPrettyWkt(), std::string(), "invalid CRS pretty WKT should be empty", testStatus);
        ExpectEqual(crs.ExportToProj4(), std::string(), "invalid CRS PROJ4 should be empty", testStatus);
        ExpectEqual(crs.ExportToProjJson(), std::string(), "invalid CRS PROJJSON should be empty", testStatus);
        ExpectFalse(crs.GetLinearUnits().isValid, "invalid CRS linear unit should be invalid", testStatus);
        ExpectFalse(crs.GetAngularUnits().isValid, "invalid CRS angular unit should be invalid", testStatus);
        ExpectNearlyEqual(crs.GetMetersPerUnit(), 0.0, 0.0, "invalid CRS meters per unit should be zero", testStatus);
        ExpectNearlyEqual(crs.GetSemiMajor(), 0.0, 0.0, "invalid CRS semi major should be zero", testStatus);
        ExpectNearlyEqual(crs.GetSemiMinor(), 0.0, 0.0, "invalid CRS semi minor should be zero", testStatus);
        ExpectNearlyEqual(crs.GetInvFlattening(), 0.0, 0.0, "invalid CRS inverse flattening should be zero", testStatus);
        ExpectNearlyEqual(crs.GetCoordinateEpoch(), 0.0, 0.0, "invalid CRS coordinate epoch should be zero", testStatus);
    }

    void TestEpsg4326(TestStatus* testStatus)
    {
        PrintTitle("TestEpsg4326");

        const GeoCrs crs("EPSG:4326", false, false);
        PrintCrsBrief("EPSG:4326 summary", crs);

        ExpectTrue(crs.IsValid(), "EPSG:4326 should be valid", testStatus);
        ExpectTrue(static_cast<bool>(crs), "EPSG:4326 bool conversion should be true", testStatus);
        ExpectEqual(crs.GetEpsgOrEsriCode(), "EPSG:4326", "EPSG:4326 authority should be stable", testStatus);
        ExpectTrue(crs.IsGeographic(), "EPSG:4326 should be geographic", testStatus);
        ExpectFalse(crs.IsProjected(), "EPSG:4326 should not be projected", testStatus);
        ExpectFalse(crs.IsLocal(), "EPSG:4326 should not be local", testStatus);
        ExpectFalse(crs.IsCompound(), "EPSG:4326 should not be compound", testStatus);
        ExpectFalse(crs.IsDynamic(), "EPSG:4326 should not be dynamic", testStatus);
        ExpectFalse(crs.IsCustom(), "EPSG:4326 should not be custom", testStatus);
        ExpectTrue(crs.UsesTraditionalGisOrder(), "EPSG:4326 should use traditional GIS order", testStatus);
        ExpectTrue(crs.IsLongitudeLatitudeOrder(), "EPSG:4326 should expose longitude-latitude order", testStatus);
        ExpectNotEmpty(crs.GetName(), "EPSG:4326 name should not be empty", testStatus);
        ExpectNotEmpty(crs.GetUniqueId(), "EPSG:4326 unique id should not be empty", testStatus);
        ExpectNotEmpty(crs.ExportToWkt(), "EPSG:4326 WKT should not be empty", testStatus);
        ExpectNotEmpty(crs.ExportToPrettyWkt(false), "EPSG:4326 pretty WKT should not be empty", testStatus);
        ExpectNotEmpty(crs.ExportToPrettyWkt(true), "EPSG:4326 simplified pretty WKT should not be empty", testStatus);
        ExpectNotEmpty(crs.ExportToProjJson(false), "EPSG:4326 PROJJSON should not be empty", testStatus);
        ExpectContains(crs.ExportToWkt(), "WGS", "EPSG:4326 WKT should contain WGS keyword", testStatus);

        const GeoCrs::UnitInfo angularUnits = crs.GetAngularUnits();
        ExpectTrue(angularUnits.isValid, "EPSG:4326 angular unit should be valid", testStatus);
        ExpectNearlyEqual(angularUnits.conversionFactor, 0.017453292519943295, 1e-15, "EPSG:4326 angular unit factor should be degree to radian", testStatus);
        ExpectNearlyEqual(crs.GetSemiMajor(), 6378137.0, 1e-6, "EPSG:4326 semi major should match WGS84", testStatus);
        ExpectNearlyEqual(crs.GetInvFlattening(), 298.257223563, 1e-9, "EPSG:4326 inverse flattening should match WGS84", testStatus);
        ExpectTrue(crs.GetMetersPerUnit() > 100000.0, "EPSG:4326 meters per unit should be angular arc estimate", testStatus);
    }

    void TestEquivalentInputs(TestStatus* testStatus)
    {
        PrintTitle("TestEquivalentInputs");

        const GeoCrs epsg4326("EPSG:4326", false, false);
        const GeoCrs wgs84 = GeoCrs::FromUserInput("WGS84", false, false);
        const GeoCrs epsg4490("EPSG:4490", false, false);
        const GeoCrs epsg4490Urn("urn:ogc:def:crs:EPSG::4490", false, false);

        ExpectTrue(wgs84.IsValid(), "WGS84 user input should be valid", testStatus);
        ExpectTrue(epsg4326.IsSame(wgs84), "EPSG:4326 should be equivalent to WGS84 input", testStatus);
        ExpectTrue(epsg4326 == wgs84, "operator== should use CRS equivalence", testStatus);
        ExpectFalse(epsg4326 != wgs84, "operator!= should use CRS equivalence", testStatus);

        ExpectTrue(epsg4490.IsValid(), "EPSG:4490 should be valid", testStatus);
        ExpectTrue(epsg4490Urn.IsValid(), "EPSG:4490 URN should be valid", testStatus);
        ExpectTrue(epsg4490.IsSame(epsg4490Urn), "EPSG:4490 should equal its URN form", testStatus);
        ExpectFalse(epsg4490.IsSame(epsg4326), "EPSG:4490 should not be equivalent to EPSG:4326", testStatus);
    }

    void TestEpsg4490(TestStatus* testStatus)
    {
        PrintTitle("TestEpsg4490");

        const GeoCrs crs("EPSG:4490", false, false);
        PrintCrsBrief("EPSG:4490 summary", crs);

        ExpectTrue(crs.IsValid(), "EPSG:4490 should be valid", testStatus);
        ExpectEqual(crs.GetEpsgOrEsriCode(), "EPSG:4490", "EPSG:4490 authority should be stable", testStatus);
        ExpectTrue(crs.IsGeographic(), "EPSG:4490 should be geographic", testStatus);
        ExpectFalse(crs.IsProjected(), "EPSG:4490 should not be projected", testStatus);
        ExpectTrue(crs.UsesTraditionalGisOrder(), "EPSG:4490 should use traditional GIS order", testStatus);
        ExpectTrue(crs.IsLongitudeLatitudeOrder(), "EPSG:4490 should expose longitude-latitude order", testStatus);
        ExpectNearlyEqual(crs.GetSemiMajor(), 6378137.0, 1e-6, "EPSG:4490 semi major should match CGCS2000", testStatus);
        ExpectNearlyEqual(crs.GetInvFlattening(), 298.257222101, 1e-9, "EPSG:4490 inverse flattening should match CGCS2000", testStatus);

        std::vector<GB_Rectangle> rectangles;
        std::string areaName;
        const bool hasArea = crs.TryGetGeographicAreaOfUse(&rectangles, &areaName);
        ExpectTrue(hasArea, "EPSG:4490 should have geographic area of use", testStatus);
        ExpectTrue(!rectangles.empty(), "EPSG:4490 area of use rectangles should not be empty", testStatus);
        ExpectTrue(!areaName.empty(), "EPSG:4490 area name should not be empty", testStatus);
        ExpectTrue(crs.GetAreaOfUseName() == areaName || !crs.GetAreaOfUseName().empty(), "EPSG:4490 area name getter should be usable", testStatus);
        ExpectTrue(!crs.GetGeographicAreaOfUse().empty(), "EPSG:4490 area getter should not be empty", testStatus);
    }

    void TestEpsg3857(TestStatus* testStatus)
    {
        PrintTitle("TestEpsg3857");

        const GeoCrs crs("EPSG:3857", false, false);
        PrintCrsBrief("EPSG:3857 summary", crs);

        ExpectTrue(crs.IsValid(), "EPSG:3857 should be valid", testStatus);
        ExpectEqual(crs.GetEpsgOrEsriCode(), "EPSG:3857", "EPSG:3857 authority should be stable", testStatus);
        ExpectFalse(crs.IsGeographic(), "EPSG:3857 should not be geographic", testStatus);
        ExpectTrue(crs.IsProjected(), "EPSG:3857 should be projected", testStatus);
        ExpectFalse(crs.IsLongitudeLatitudeOrder(), "EPSG:3857 should not be longitude-latitude order", testStatus);
        ExpectTrue(crs.UsesTraditionalGisOrder(), "EPSG:3857 should use traditional GIS order", testStatus);
        ExpectNotEmpty(crs.GetName(), "EPSG:3857 name should not be empty", testStatus);
        ExpectNotEmpty(crs.ExportToWkt(), "EPSG:3857 WKT should not be empty", testStatus);
        ExpectNotEmpty(crs.ExportToProjJson(false), "EPSG:3857 PROJJSON should not be empty", testStatus);

        const GeoCrs::UnitInfo linearUnits = crs.GetLinearUnits();
        ExpectTrue(linearUnits.isValid, "EPSG:3857 linear unit should be valid", testStatus);
        ExpectNearlyEqual(linearUnits.conversionFactor, 1.0, 1e-12, "EPSG:3857 linear unit should be metre", testStatus);
        ExpectNearlyEqual(crs.GetMetersPerUnit(), 1.0, 1e-12, "EPSG:3857 meters per unit should be 1", testStatus);
    }

    void TestProj4Input(TestStatus* testStatus)
    {
        PrintTitle("TestProj4Input");

        const GeoCrs crs("+proj=longlat +datum=WGS84 +no_defs +type=crs", false, false);
        PrintCrsBrief("PROJ4 longlat summary", crs);

        ExpectTrue(crs.IsValid(), "PROJ4 longlat CRS should be valid", testStatus);
        ExpectTrue(crs.IsGeographic(), "PROJ4 longlat CRS should be geographic", testStatus);
        ExpectFalse(crs.IsProjected(), "PROJ4 longlat CRS should not be projected", testStatus);
        ExpectTrue(crs.UsesTraditionalGisOrder(), "PROJ4 longlat CRS should use traditional GIS order", testStatus);
        ExpectTrue(crs.IsLongitudeLatitudeOrder(), "PROJ4 longlat CRS should expose longitude-latitude order", testStatus);
        ExpectNotEmpty(crs.ExportToWkt(), "PROJ4 longlat CRS WKT should not be empty", testStatus);
        ExpectNotEmpty(crs.ExportToProj4(), "PROJ4 longlat CRS PROJ4 export should not be empty", testStatus);
    }

    void TestTryFromUserInput(TestStatus* testStatus)
    {
        PrintTitle("TestTryFromUserInput");

        GeoCrs crs;
        ExpectTrue(GeoCrs::TryFromUserInput("EPSG:4326", &crs, false, false), "TryFromUserInput should accept EPSG:4326", testStatus);
        ExpectTrue(crs.IsValid(), "TryFromUserInput output CRS should be valid", testStatus);
        ExpectEqual(crs.GetEpsgOrEsriCode(), "EPSG:4326", "TryFromUserInput output authority should be EPSG:4326", testStatus);

        ExpectFalse(GeoCrs::TryFromUserInput("", &crs, false, false), "TryFromUserInput should reject empty input", testStatus);
        ExpectFalse(crs.IsValid(), "empty input should reset output CRS", testStatus);

        ExpectFalse(GeoCrs::TryFromUserInput("not a coordinate reference system", &crs, false, false), "TryFromUserInput should reject nonsense input", testStatus);
        ExpectFalse(crs.IsValid(), "nonsense input should reset output CRS", testStatus);

        ExpectFalse(GeoCrs::TryFromUserInput("https://example.com/not_existing.prj", &crs, true, false), "network input should fail when network access is disabled", testStatus);
        ExpectFalse(crs.IsValid(), "rejected network input should reset output CRS", testStatus);

        ExpectFalse(GeoCrs::TryFromUserInput("C:\\Temp\\not_existing.prj", &crs, false, true), "file input should fail when file access is disabled", testStatus);
        ExpectFalse(crs.IsValid(), "rejected file input should reset output CRS", testStatus);

        ExpectFalse(GeoCrs::TryFromUserInput("EPSG:4326", nullptr, false, false), "TryFromUserInput should reject null output pointer", testStatus);
    }

    void TestResetAndSet(TestStatus* testStatus)
    {
        PrintTitle("TestResetAndSet");

        GeoCrs crs("EPSG:4326", false, false);
        ExpectTrue(crs.IsValid(), "CRS should be valid before reset", testStatus);
        crs.Reset();
        ExpectFalse(crs.IsValid(), "CRS should be invalid after reset", testStatus);
        ExpectEqual(crs.GetUniqueId(), std::string(), "reset CRS unique id should be empty", testStatus);
        ExpectEqual(crs.ExportToWkt(), std::string(), "reset CRS WKT should be empty", testStatus);

        ExpectTrue(crs.SetFromUserInput("EPSG:3857", false, false), "SetFromUserInput should accept EPSG:3857 after reset", testStatus);
        ExpectTrue(crs.IsValid(), "reset CRS should become valid after SetFromUserInput", testStatus);
        ExpectTrue(crs.IsProjected(), "reset CRS should become projected after setting EPSG:3857", testStatus);
        ExpectEqual(crs.GetEpsgOrEsriCode(), "EPSG:3857", "reset CRS authority should become EPSG:3857", testStatus);

        ExpectFalse(crs.SetFromUserInput("bad crs", false, false), "SetFromUserInput should reject invalid text", testStatus);
        ExpectFalse(crs.IsValid(), "invalid SetFromUserInput should reset CRS", testStatus);
    }

    void TestCopyAndMove(TestStatus* testStatus)
    {
        PrintTitle("TestCopyAndMove");

        const GeoCrs sourceCrs("EPSG:4326", false, false);
        const GeoCrs copiedCrs(sourceCrs);
        ExpectTrue(sourceCrs.IsSame(copiedCrs), "copy constructor should preserve CRS equivalence", testStatus);
        ExpectEqual(copiedCrs.GetEpsgOrEsriCode(), "EPSG:4326", "copy constructor should preserve authority", testStatus);

        GeoCrs assignedCrs;
        assignedCrs = sourceCrs;
        ExpectTrue(sourceCrs.IsSame(assignedCrs), "copy assignment should preserve CRS equivalence", testStatus);
        ExpectEqual(assignedCrs.GetEpsgOrEsriCode(), "EPSG:4326", "copy assignment should preserve authority", testStatus);

        GeoCrs moveSourceCrs("EPSG:3857", false, false);
        GeoCrs movedCrs(std::move(moveSourceCrs));
        ExpectTrue(movedCrs.IsValid(), "move constructor target should be valid", testStatus);
        ExpectEqual(movedCrs.GetEpsgOrEsriCode(), "EPSG:3857", "move constructor target should preserve authority", testStatus);
        ExpectFalse(moveSourceCrs.IsValid(), "move constructor source should be reset", testStatus);

        GeoCrs moveAssignedSourceCrs("EPSG:4490", false, false);
        GeoCrs moveAssignedCrs;
        moveAssignedCrs = std::move(moveAssignedSourceCrs);
        ExpectTrue(moveAssignedCrs.IsValid(), "move assignment target should be valid", testStatus);
        ExpectEqual(moveAssignedCrs.GetEpsgOrEsriCode(), "EPSG:4490", "move assignment target should preserve authority", testStatus);
        ExpectFalse(moveAssignedSourceCrs.IsValid(), "move assignment source should be reset", testStatus);
    }

    void TestCacheStability(TestStatus* testStatus)
    {
        PrintTitle("TestCacheStability");

        const GeoCrs crs("EPSG:4326", false, false);
        const std::string firstUniqueId = crs.GetUniqueId();
        const std::string secondUniqueId = crs.GetUniqueId();
        const std::string firstWkt = crs.ExportToWkt();
        const std::string secondWkt = crs.ExportToWkt();
        const std::string firstPrettyWkt = crs.ExportToPrettyWkt(false);
        const std::string secondPrettyWkt = crs.ExportToPrettyWkt(false);
        const std::string firstSimplifiedPrettyWkt = crs.ExportToPrettyWkt(true);
        const std::string secondSimplifiedPrettyWkt = crs.ExportToPrettyWkt(true);
        const std::string firstProj4 = crs.ExportToProj4();
        const std::string secondProj4 = crs.ExportToProj4();
        const std::string firstProjJson = crs.ExportToProjJson(false);
        const std::string secondProjJson = crs.ExportToProjJson(false);

        ExpectEqual(firstUniqueId, secondUniqueId, "GetUniqueId result should be stable", testStatus);
        ExpectEqual(firstWkt, secondWkt, "ExportToWkt result should be stable", testStatus);
        ExpectEqual(firstPrettyWkt, secondPrettyWkt, "ExportToPrettyWkt(false) result should be stable", testStatus);
        ExpectEqual(firstSimplifiedPrettyWkt, secondSimplifiedPrettyWkt, "ExportToPrettyWkt(true) result should be stable", testStatus);
        ExpectEqual(firstProj4, secondProj4, "ExportToProj4 result should be stable", testStatus);
        ExpectEqual(firstProjJson, secondProjJson, "ExportToProjJson result should be stable", testStatus);
    }
}

int main(int argc, char* argv[])
{
    (void)argc;
    (void)argv;

    std::printf("GeoCrs public API test program\n");

    TestStatus testStatus;
    TestDefaultState(&testStatus);
    TestEpsg4326(&testStatus);
    TestEquivalentInputs(&testStatus);
    TestEpsg4490(&testStatus);
    TestEpsg3857(&testStatus);
    TestProj4Input(&testStatus);
    TestTryFromUserInput(&testStatus);
    TestResetAndSet(&testStatus);
    TestCopyAndMove(&testStatus);
    TestCacheStability(&testStatus);

    PrintTitle("Summary");
    std::printf("Passed: %d\n", testStatus.numPassed);
    std::printf("Failed: %d\n", testStatus.numFailed);

    return testStatus.numFailed == 0 ? 0 : 1;
}
