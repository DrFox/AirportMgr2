#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadTraffic.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

// The drive side is ONE airport-wide setting (spec 2026-09-23 §2), stored on the network so
// it saves and undoes with everything else the player built.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDriveSideDefaultTest,
	"Airside.Model.DriveSide.Default",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDriveSideDefaultTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	TestEqual(TEXT("right-hand traffic until the player says otherwise"),
		static_cast<int32>(Net->GetDriveSide()), static_cast<int32>(EDriveSide::Right));

	const uint32 Before = Net->GetEditRevision();
	TestTrue(TEXT("a change reports itself"), Net->SetDriveSide(EDriveSide::Left));
	TestEqual(TEXT("but moves no node or segment, so the road-graph clock (ghost, deletion plan) stands"),
		static_cast<int64>(Net->GetEditRevision()), static_cast<int64>(Before));
	TestFalse(TEXT("setting the side it already has is a no-op, so no undo step"),
		Net->SetDriveSide(EDriveSide::Left));

	// Profiles are authored for right-hand traffic; OffsetFor is where the side is applied.
	FProfileGuideline Lane;
	Lane.CentreOffset = -150.0;
	TestEqual(TEXT("right drive keeps the authored offset"), Lane.OffsetFor(EDriveSide::Right), -150.0);
	TestEqual(TEXT("left drive mirrors it across the centreline"), Lane.OffsetFor(EDriveSide::Left), 150.0);

	FProfileGuideline Centre;
	TestEqual(TEXT("a centreline guideline does not move under either side"), Centre.OffsetFor(EDriveSide::Left), 0.0);
	return true;
}

#endif
