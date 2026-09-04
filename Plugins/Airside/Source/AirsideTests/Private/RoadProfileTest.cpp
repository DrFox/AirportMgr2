#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadProfileTest,
	"Airside.Model.Profile",
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

	// A taxiway declares ONE guideline, not two lanes. An aircraft occupies the full width
	// with its nose wheel on the line; there is no second lane to be in. Parent R3's
	// "per-lane turn paths" is corrected by spec 3 to guideline-level for exactly this.
	{
		URoadProfile* TaxiwayGuideline = URoadProfile::MakeTransient(2300.0, 1500.0);
		TestEqual(TEXT("a taxiway declares one guideline"), TaxiwayGuideline->Guidelines.Num(), 1);
		TestEqual(TEXT("centred on the profile"), TaxiwayGuideline->Guidelines[0].CentreOffset, 0.0);
		TestEqual(TEXT("carrying aircraft"),
			TaxiwayGuideline->Guidelines[0].Class, ETraversalClass::Aircraft);
		TestEqual(TEXT("in both directions"),
			TaxiwayGuideline->Guidelines[0].Direction, EGuidelineDir::Bidirectional);
	}

	// A road is the case that recovers the original per-lane meaning: two guidelines,
	// mirrored offsets, opposing directions. Built by hand because MakeTransient only ever
	// produces the symmetric single-guideline profile.
	{
		URoadProfile* RoadGuidelines = NewObject<URoadProfile>(GetTransientPackage());

		FProfileBand Lane;
		Lane.Width = 350.0;
		Lane.Type = ERoadBandType::Lane;
		RoadGuidelines->Bands.Add(Lane);
		RoadGuidelines->Bands.Add(Lane);

		FProfileGuideline Left;
		Left.CentreOffset = 175.0;
		Left.Class = ETraversalClass::GroundVehicle;
		Left.Direction = EGuidelineDir::AToB;
		Left.Width = 350.0;

		FProfileGuideline Right = Left;
		Right.CentreOffset = -175.0;
		Right.Direction = EGuidelineDir::BToA;

		RoadGuidelines->Guidelines.Add(Left);
		RoadGuidelines->Guidelines.Add(Right);

		// The declared offsets only mean anything against the profile's OWN geometry, and
		// that geometry IS production code. Each lane guideline sits at the centre of its
		// band, half a band-width either side of the centreline. Asserting instead that
		// 175 == -(-175) would compare two literals this test wrote itself and could never
		// fail; these three exercise GetTotalWidth and GetHalfWidth* on a two-band profile,
		// a shape no other test covers.
		TestEqual(TEXT("a two-lane road declares two guidelines"), RoadGuidelines->Guidelines.Num(), 2);
		TestEqual(TEXT("the road is as wide as its two bands"), RoadGuidelines->GetTotalWidth(), 700.0);
		TestEqual(TEXT("split evenly about the centreline"), RoadGuidelines->GetHalfWidthLeft(), 350.0);
		TestEqual(TEXT("the left guideline sits at the centre of its band"),
			RoadGuidelines->Guidelines[0].CentreOffset, RoadGuidelines->GetHalfWidthLeft() * 0.5);
		TestEqual(TEXT("and the right guideline mirrors it"),
			RoadGuidelines->Guidelines[1].CentreOffset, -RoadGuidelines->GetHalfWidthRight() * 0.5);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
