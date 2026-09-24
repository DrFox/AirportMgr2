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

	// TWO LANES, one each way (spec 2026-09-23). Until then a road was one bidirectional
	// centreline, and nothing could route on a side of it.
	if (!TestEqual(TEXT("two lanes: one each way"), Road->Guidelines.Num(), 2)) { return false; }

	const FProfileGuideline& Forward = Road->Guidelines[0];
	const FProfileGuideline& Back    = Road->Guidelines[1];
	for (const FProfileGuideline* Lane : { &Forward, &Back })
	{
		TestEqual(TEXT("it admits ground vehicles, not aircraft"),
			static_cast<int32>(Lane->Class), static_cast<int32>(ETraversalClass::GroundVehicle));
		TestEqual(TEXT("MaxWingspan 0 - unlimited, because no wing ever uses it"), Lane->MaxWingspan, 0.0);
		TestEqual(TEXT("lane width is the per-lane figure"), Lane->Width, 300.0);
	}
	TestEqual(TEXT("the offsets mirror across the centreline"), Forward.CentreOffset, -Back.CentreOffset);
	// Authored for RIGHT-hand traffic. The model's left (positive offset) is PerpCCW of the
	// tangent, and in UE's left-handed frame that is screen-RIGHT of travel - so the lane
	// running A to B sits at the POSITIVE offset. See Airside.Build.TwoWay.LanesDerived.
	TestEqual(TEXT("each lane centred in its own half"), Forward.CentreOffset, 150.0);
	TestEqual(TEXT("the lane at the positive offset runs A to B"),
		static_cast<int32>(Forward.Direction), static_cast<int32>(EGuidelineDir::AToB));
	TestEqual(TEXT("the other runs B to A"),
		static_cast<int32>(Back.Direction), static_cast<int32>(EGuidelineDir::BToA));
	TestEqual(TEXT("kerb to kerb: two 3 m lanes and two 0.6 m kerbs"), Road->GetTotalWidth(), 720.0);

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
