#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadProfileTest,
	"RoadNet.Model.Profile",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadProfileTest::RunTest(const FString& Parameters)
{
	URoadProfile* Taxiway = URoadProfile::MakeTransient(2300.0, 1500.0);

	TestEqual(TEXT("total width"), Taxiway->GetTotalWidth(), 2300.0);
	TestEqual(TEXT("left half"), Taxiway->GetHalfWidthLeft(), 1150.0);
	TestEqual(TEXT("right half"), Taxiway->GetHalfWidthRight(), 1150.0);
	TestEqual(TEXT("fillet radius"), Taxiway->PreferredFilletRadius, 1500.0);

	// Asymmetric: centreline pushed toward the left edge.
	URoadProfile* Offset = URoadProfile::MakeTransient(2000.0, 1000.0);
	Offset->CentrelineOffset = 500.0;
	TestEqual(TEXT("asymmetric left"), Offset->GetHalfWidthLeft(), 500.0);
	TestEqual(TEXT("asymmetric right"), Offset->GetHalfWidthRight(), 1500.0);

	// Multi-band widths sum.
	URoadProfile* Road = NewObject<URoadProfile>(GetTransientPackage());
	FProfileBand Shoulder; Shoulder.Width = 300.0; Shoulder.Type = ERoadBandType::Shoulder;
	FProfileBand LaneBand; LaneBand.Width = 700.0; LaneBand.Type = ERoadBandType::Lane;
	Road->Bands.Add(Shoulder);
	Road->Bands.Add(LaneBand);
	Road->Bands.Add(LaneBand);
	Road->Bands.Add(Shoulder);
	TestEqual(TEXT("summed width"), Road->GetTotalWidth(), 2000.0);
	TestEqual(TEXT("summed half"), Road->GetHalfWidthLeft(), 1000.0);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
