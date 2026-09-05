#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/AirsideCapability.h"
#include "Model/OpsSave.h"
#include "Model/RoadNetwork.h"
#include "Model/SimClock.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOpsSaveRoundTripTest,
	"AirportOps.Model.Save.RoundTrip",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FOpsSaveRoundTripTest::RunTest(const FString& Parameters)
{
	// A transient profile is NOT an asset, so its path cannot be restored by loading. The
	// network's runway must therefore be recognised from a profile the loader can FIND, which
	// in the real game is a content asset already in memory. For the test: keep the same
	// profile object alive across capture and restore under a stable name in the transient
	// package, and assert the restored segments point back at it - which is exactly what
	// FObjectAndNameAsStringProxyArchive does for an object it can find by path.
	URoadProfile* Runway = URoadProfile::MakeTransient(4500.0, 1500.0, 450.0);
	Runway->bContinuousThroughJunctions = true;
	Runway->Rename(TEXT("OpsSaveTest_RunwayProfile"), GetTransientPackage());

	URoadNetwork* Source = NewObject<URoadNetwork>();
	const FRoadNodeId A = Source->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId B = Source->AddNode(FVector2D(40000.0, 0.0));
	Source->AddStraightSegment(A, B, Runway);
	// Remove and re-add a node so the free list and a bumped generation are part of the state.
	const FRoadNodeId Spare = Source->AddNode(FVector2D(0.0, 5000.0));
	Source->RemoveNode(Spare);

	USimClock* Clock = NewObject<USimClock>();
	Clock->RealSecondsPerGameDay = 600.0;
	Clock->SetSpeed(ESimSpeed::X4);
	Clock->Advance(3.0);
	const double SavedNow = Clock->Now();

	FOpsSnapshot Snapshot;
	OpsSave::Capture(*Clock, *Source, Snapshot);
	TestTrue(TEXT("the snapshot holds bytes for both objects"), Snapshot.Clock.Num() > 0 && Snapshot.Network.Num() > 0);

	URoadNetwork* Restored = NewObject<URoadNetwork>();
	USimClock* RestoredClock = NewObject<USimClock>();
	if (!TestTrue(TEXT("restore succeeds"), OpsSave::Restore(Snapshot, *RestoredClock, *Restored))) { return false; }

	TestEqual(TEXT("game time survives"), RestoredClock->Now(), SavedNow, 1e-9);
	TestEqual(TEXT("speed survives"), RestoredClock->GetSpeed(), ESimSpeed::X4);
	TestEqual(TEXT("day length survives"), RestoredClock->RealSecondsPerGameDay, 600.0, 1e-9);

	const FAirsideCapability Cap = AirsideCapability::Summarise(*Restored);
	TestEqual(TEXT("the runway is still a runway after load - the profile reference resolved"), Cap.Runways.Num(), 1);
	TestEqual(TEXT("with its length"), Cap.LongestRunway(), 40000.0, 1.0);

	// Handles: the SAME id must still name the same node, generation included.
	const FRoadNode* NodeB = Restored->GetNode(B);
	if (TestNotNull(TEXT("a pre-save handle resolves on the restored network"), NodeB))
	{
		TestEqual(TEXT("to the node it named"), NodeB->Position, FVector2D(40000.0, 0.0));
	}
	TestNull(TEXT("a handle removed before the save stays dead after it"), Restored->GetNode(Spare));

	// The free list came too: the next AddNode reuses the freed slot with a higher generation.
	const FRoadNodeId Reused = Restored->AddNode(FVector2D(1.0, 1.0));
	TestEqual(TEXT("the freed slot is recycled"), Reused.Index, Spare.Index);
	TestTrue(TEXT("with a newer generation than the dead handle"), Reused.Generation > Spare.Generation);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOpsSaveSlotTest,
	"AirportOps.Model.Save.Slot",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FOpsSaveSlotTest::RunTest(const FString& Parameters)
{
	FOpsSnapshot Out;
	Out.Clock = { 1, 2, 3 };
	Out.Network = { 9, 8 };
	const FString Slot = TEXT("AirportOpsTest_Slot");
	if (!TestTrue(TEXT("a snapshot writes to a slot"), OpsSave::WriteSlot(Slot, Out))) { return false; }

	FOpsSnapshot In;
	if (!TestTrue(TEXT("and reads back"), OpsSave::ReadSlot(Slot, In))) { return false; }
	TestEqual(TEXT("byte-identical clock"), In.Clock, Out.Clock);
	TestEqual(TEXT("byte-identical network"), In.Network, Out.Network);
	TestEqual(TEXT("version tag carried"), In.Version, Out.Version);

	FOpsSnapshot Missing;
	TestFalse(TEXT("a slot that does not exist reads false, not garbage"),
		OpsSave::ReadSlot(TEXT("AirportOpsTest_NoSuchSlot"), Missing));
	return true;
}

#endif
