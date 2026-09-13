#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/AirsideCapability.h"
#include "Model/FlightBoard.h"
#include "Model/FuelService.h"
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

	// An empty board: this test is about the clock and the network, and a board with no
	// flights is what a game that never opened the inbox actually saves.
	UFlightBoard* Board = NewObject<UFlightBoard>();
	UFuelService* Fuel = NewObject<UFuelService>();

	FOpsSnapshot Snapshot;
	OpsSave::Capture(*Clock, *Source, *Board, *Fuel, Snapshot);
	TestTrue(TEXT("the snapshot holds a blob for the clock and the network"),
		Snapshot.Blobs.FindRef(TEXT("Clock")).Bytes.Num() > 0 && Snapshot.Blobs.FindRef(TEXT("Network")).Bytes.Num() > 0);
	TestTrue(TEXT("and one for fuel, new since issue #105 item 8"),
		Snapshot.Blobs.Contains(TEXT("Fuel")));

	URoadNetwork* Restored = NewObject<URoadNetwork>();
	USimClock* RestoredClock = NewObject<USimClock>();
	UFlightBoard* RestoredBoard = NewObject<UFlightBoard>();
	UFuelService* RestoredFuel = NewObject<UFuelService>();
	if (!TestTrue(TEXT("restore succeeds"),
		OpsSave::Restore(Snapshot, *RestoredClock, *Restored, *RestoredBoard, *RestoredFuel))) { return false; }

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
	Out.Blobs.Add(TEXT("Clock"), FOpsBlob{ TArray<uint8>{ 1, 2, 3 } });
	Out.Blobs.Add(TEXT("Network"), FOpsBlob{ TArray<uint8>{ 9, 8 } });
	const FString Slot = TEXT("AirportOpsTest_Slot");
	if (!TestTrue(TEXT("a snapshot writes to a slot"), OpsSave::WriteSlot(Slot, Out))) { return false; }

	FOpsSnapshot In;
	if (!TestTrue(TEXT("and reads back"), OpsSave::ReadSlot(Slot, In))) { return false; }
	TestEqual(TEXT("byte-identical clock blob"), In.Blobs.FindRef(TEXT("Clock")).Bytes, Out.Blobs.FindRef(TEXT("Clock")).Bytes);
	TestEqual(TEXT("byte-identical network blob"), In.Blobs.FindRef(TEXT("Network")).Bytes, Out.Blobs.FindRef(TEXT("Network")).Bytes);
	TestEqual(TEXT("version tag carried"), In.Version, Out.Version);

	FOpsSnapshot Missing;
	TestFalse(TEXT("a slot that does not exist reads false, not garbage"),
		OpsSave::ReadSlot(TEXT("AirportOpsTest_NoSuchSlot"), Missing));
	return true;
}

/**
 * THE v3-AND-EARLIER SHIM. A save from before FOpsSnapshot::Blobs existed has its bytes
 * under the OLD tagged-property names (Clock/Network/Flights) and no Blobs at all - this
 * fails if OpsSave::Restore ever stops migrating them before the generic per-object pass.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOpsSaveLegacyShimTest,
	"AirportOps.Model.Save.LegacyShim",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FOpsSaveLegacyShimTest::RunTest(const FString& Parameters)
{
	USimClock* Source = NewObject<USimClock>();
	Source->RealSecondsPerGameDay = 900.0;
	Source->Advance(5.0);
	const double SavedNow = Source->Now();

	FOpsSnapshot Legacy;
	Legacy.Version = 3;
	OpsSave::SerializeObject(*Source, Legacy.Clock);
	// Network and Flights deliberately left empty - a v1/v2/v3 game with an empty board and,
	// for this test, an unimportant network - so the shim's "absent stays absent" path is
	// exercised too.
	TestTrue(TEXT("the legacy blob is populated the old way"), Legacy.Clock.Num() > 0);
	TestTrue(TEXT("not the new way"), Legacy.Blobs.Num() == 0);

	USimClock* RestoredClock = NewObject<USimClock>();
	URoadNetwork* RestoredNetwork = NewObject<URoadNetwork>();
	UFlightBoard* RestoredBoard = NewObject<UFlightBoard>();
	UFuelService* RestoredFuel = NewObject<UFuelService>();
	if (!TestTrue(TEXT("restore succeeds against a legacy snapshot"),
		OpsSave::Restore(Legacy, *RestoredClock, *RestoredNetwork, *RestoredBoard, *RestoredFuel)))
	{
		return false;
	}
	TestEqual(TEXT("the clock's legacy bytes still landed"), RestoredClock->Now(), SavedNow, 1e-9);
	return true;
}

/**
 * THE BUG THIS EXISTS FOR (traced, issue #105 item 8). GoingHome named a truck that a load's
 * ClearAgents had just removed, and nothing ever reset the map - so TrucksOutFor(Depot) over-
 * counted for the rest of the session. OnBeforeRestore fires even with NO Fuel blob at all
 * (an old save), which is the case that actually shipped broken.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOpsSaveFuelResetOnRestoreTest,
	"AirportOps.Model.Save.FuelResetsOnRestore",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FOpsSaveFuelResetOnRestoreTest::RunTest(const FString& Parameters)
{
	UFuelService* Fuel = NewObject<UFuelService>();
	// A truck "on its way home" from a PRE-load session - exactly the state a load's
	// ClearAgents makes stale, and exactly what leaked before this fix: nothing ever
	// touched GoingHome across a load, so this entry outlived the truck it named forever.
	Fuel->SetGoingHomeForTest(1, FEntityInstanceId());
	if (!TestEqual(TEXT("set up with one truck going home"), Fuel->TrucksGoingHomeForTest(), 1))
	{
		return false;
	}

	// A v4 snapshot with NO Fuel blob at all is exactly the shape every save made before this
	// issue has - RestoreBlob must still call OnBeforeRestore for it, which is the case that
	// actually shipped broken (fuel was never saved OR reset).
	FOpsSnapshot NoFuelBlob;
	NoFuelBlob.Version = 4;
	OpsSave::RestoreBlob(NoFuelBlob, *Fuel);
	TestEqual(TEXT("OnBeforeRestore cleared it even with no blob for this object"),
		Fuel->TrucksGoingHomeForTest(), 0);

	// PR #137 REVIEW: THE LEAK CAME BACK a different way once Demands/GoingHome actually got
	// a real Fuel blob to be written into. Non-Transient, Capture serialised the truck
	// straight into it, and RestoreBlob's OnBeforeRestore (which clears both) ran BEFORE the
	// deserialise that then overwrote them right back FROM the blob - so a full Capture/
	// Restore round trip is the one path that actually proves the fix, not RestoreBlob alone
	// against a snapshot built by hand. Both fields are UPROPERTY(Transient) now for exactly
	// this reason.
	Fuel->SetGoingHomeForTest(2, FEntityInstanceId());
	if (!TestEqual(TEXT("set up again with one truck going home, for the round trip"),
		Fuel->TrucksGoingHomeForTest(), 1))
	{
		return false;
	}
	USimClock* Clock = NewObject<USimClock>();
	URoadNetwork* Net = NewObject<URoadNetwork>();
	UFlightBoard* Board = NewObject<UFlightBoard>();
	FOpsSnapshot RoundTrip;
	OpsSave::Capture(*Clock, *Net, *Board, *Fuel, RoundTrip);

	USimClock* RestoredClock = NewObject<USimClock>();
	URoadNetwork* RestoredNet = NewObject<URoadNetwork>();
	UFlightBoard* RestoredBoard = NewObject<UFlightBoard>();
	UFuelService* RestoredFuel = NewObject<UFuelService>();
	if (!TestTrue(TEXT("round-trip restore succeeds"),
		OpsSave::Restore(RoundTrip, *RestoredClock, *RestoredNet, *RestoredBoard, *RestoredFuel)))
	{
		return false;
	}
	TestEqual(TEXT("a full Capture/Restore round trip does not resurrect the stale truck"),
		RestoredFuel->TrucksGoingHomeForTest(), 0);
	return true;
}

#endif
