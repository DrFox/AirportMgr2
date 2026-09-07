#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadTraffic.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FServiceRoadProfileTest,
	"Airside.Build.RoadProfileGuideline",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FServiceRoadProfileTest::RunTest(const FString& Parameters)
{
	URoadProfile* Road = URoadProfile::MakeServiceRoadTransient();
	if (!TestNotNull(TEXT("a service road profile"), Road)) { return false; }

	// ONE guideline, because a road is a single shared lane in this slice - two mirrored
	// ones are what FProfileGuideline's header calls the case that recovers "lane", and
	// nothing routes on a side yet.
	if (!TestEqual(TEXT("exactly one guideline"), Road->Guidelines.Num(), 1)) { return false; }

	const FProfileGuideline& Lane = Road->Guidelines[0];
	TestEqual(TEXT("it admits ground vehicles, not aircraft"),
		static_cast<int32>(Lane.Class), static_cast<int32>(ETraversalClass::GroundVehicle));
	TestEqual(TEXT("bidirectional: one lane shared both ways"),
		static_cast<int32>(Lane.Direction), static_cast<int32>(EGuidelineDir::Bidirectional));
	TestEqual(TEXT("MaxWingspan 0 - unlimited, because no wing ever uses it"), Lane.MaxWingspan, 0.0);
	TestEqual(TEXT("centred on the road"), Lane.CentreOffset, 0.0);

	// KERBS, not run-offs. The edge-treatment note: a road is kerbed and a taxiway has a
	// paved run-off, and the difference is what tells the two apart on the ground.
	int32 Kerbs = 0;
	for (const FProfileBand& Band : Road->Bands)
	{
		Kerbs += Band.Type == ERoadBandType::Curb ? 1 : 0;
	}
	TestEqual(TEXT("a kerb each side"), Kerbs, 2);

	// NOT CONTINUOUS: a road gives way to a paved junction like a taxiway, and only a
	// runway runs unbroken through one.
	TestFalse(TEXT("not continuous through junctions"), Road->bContinuousThroughJunctions);

	// NO EXIT LENGTH. Exit arcs are a runway's own grading (see URoadProfile::ExitLength);
	// a service road meeting a taxiway is an ordinary junction with no exit to grade.
	TestEqual(TEXT("no exit length"), Road->ExitLength, 0.0);

	TestTrue(TEXT("narrower than a taxiway - a lane, not a movement area"),
		Road->GetTotalWidth() < 2300.0);
	return true;
}

#endif
