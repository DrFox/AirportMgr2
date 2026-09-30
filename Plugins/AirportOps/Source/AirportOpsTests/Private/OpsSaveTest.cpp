#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "OpsSaveTestHelpers.h"
#include "Model/AirsideCapability.h"
#include "Model/Flight.h"
#include "Model/FlightBoard.h"
#include "Model/JobBoard.h"
#include "Model/OpsSave.h"
#include "Model/Pricing.h"
#include "Model/OfferGenerator.h"
#include "Model/RoadNetwork.h"
#include "Model/SimClock.h"
#include "Profiles/RoadProfile.h"
#include "Testing/AirsideTestGraph.h"

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
	URoadProfile* Runway = TestProfiles::Runway();
	Runway->Rename(TEXT("OpsSaveTest_RunwayProfile"), GetTransientPackage());

	URoadNetwork* Source = NewObject<URoadNetwork>();
	const FRoadNodeId A = Source->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId B = Source->AddNode(FVector2D(40000.0, 0.0));
	Source->AddStraightSegment(A, B, Runway);
	// Remove and re-add a node so the free list and a bumped generation are part of the state.
	const FRoadNodeId Spare = Source->AddNode(FVector2D(0.0, 5000.0));
	Source->RemoveNode(Spare);

	USimClock* Clock = NewObject<USimClock>();
	Clock->SetUniformDay(600.0);
	Clock->SetSpeed(ESimSpeed::X4);
	Clock->Advance(3.0);
	const double SavedNow = Clock->Now();

	// An empty board: this test is about the clock and the network, and a board with no
	// flights is what a game that never opened the inbox actually saves.
	UFlightBoard* Board = NewObject<UFlightBoard>();
	UJobBoard* Fuel = NewObject<UJobBoard>();

	FOpsSnapshot Snapshot;
	OpsSave::Capture(OpsSaveTest::Persistents(*Clock, *Board, *Fuel), *Source, Snapshot);
	TestTrue(TEXT("the snapshot holds a blob for the clock and the network"),
		Snapshot.Blobs.FindRef(TEXT("Clock")).Bytes.Num() > 0 && Snapshot.Blobs.FindRef(TEXT("Network")).Bytes.Num() > 0);
	TestTrue(TEXT("and one for fuel, new since issue #105 item 8"),
		Snapshot.Blobs.Contains(TEXT("Fuel")));

	URoadNetwork* Restored = NewObject<URoadNetwork>();
	USimClock* RestoredClock = NewObject<USimClock>();
	UFlightBoard* RestoredBoard = NewObject<UFlightBoard>();
	UJobBoard* RestoredFuel = NewObject<UJobBoard>();
	if (!TestTrue(TEXT("restore succeeds"),
		OpsSave::Restore(Snapshot, OpsSaveTest::Persistents(*RestoredClock, *RestoredBoard, *RestoredFuel), *Restored))) { return false; }

	TestEqual(TEXT("game time survives"), RestoredClock->Now(), SavedNow, 1e-9);
	TestEqual(TEXT("speed survives"), RestoredClock->GetSpeed(), ESimSpeed::X4);
	// THE DAY'S SHAPE IS NOT THE SAVE'S (#449, reversing what this asserted): day and night lengths are the scenario's
	// design figures, Transient on the clock and re-applied by UOpsRuntime::ApplyScenarioFigures after a load - the
	// roster's ruling, so a retune reaches an old save. The restored clock keeps its own; the save's 350/250 are gone.
	// ENFORCED BY (the runtime half): AirportOps.Present.RuntimeLoad.DesignFiguresAreTheScenarios
	TestEqual(TEXT("daylight length is the receiving clock's, not the save's"), RestoredClock->RealSecondsDaylight,
		GetDefault<USimClock>()->RealSecondsDaylight, 1e-9);
	TestEqual(TEXT("and so is the night's"), RestoredClock->RealSecondsNight, GetDefault<USimClock>()->RealSecondsNight, 1e-9);

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

/**
 * THE SAVE BUG (issue #191 item 1, #98 partial). ResumeSpeed used to live on UOpsRuntime,
 * which is not (and should not be - it is Present/ composition, not saved state) among
 * Persistents(): a game saved while paused reloaded with ResumeSpeed at its constructor
 * default, X1, no matter what speed the player had actually paused from. Moving ResumeSpeed
 * onto USimClock - the object OpsSave actually saves - fixes it by construction, per OpsSave's
 * own "the RULE": a model object's non-Transient UPROPERTYs are its saved state.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOpsSavePauseResumeSpeedSurvivesTest,
	"AirportOps.Model.Save.PauseResumeSpeedSurvives",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FOpsSavePauseResumeSpeedSurvivesTest::RunTest(const FString& Parameters)
{
	USimClock* Clock = NewObject<USimClock>();
	Clock->SetSpeed(ESimSpeed::X4);
	Clock->TogglePause();
	if (!TestEqual(TEXT("set up paused, as the report describes"), Clock->GetSpeed(), ESimSpeed::Paused))
	{
		return false;
	}

	UFlightBoard* Board = NewObject<UFlightBoard>();
	UJobBoard* Fuel = NewObject<UJobBoard>();
	URoadNetwork* Network = NewObject<URoadNetwork>();
	FOpsSnapshot Snapshot;
	OpsSave::Capture(OpsSaveTest::Persistents(*Clock, *Board, *Fuel), *Network, Snapshot);

	USimClock* RestoredClock = NewObject<USimClock>();
	UFlightBoard* RestoredBoard = NewObject<UFlightBoard>();
	UJobBoard* RestoredFuel = NewObject<UJobBoard>();
	URoadNetwork* RestoredNetwork = NewObject<URoadNetwork>();
	if (!TestTrue(TEXT("restore succeeds"),
		OpsSave::Restore(Snapshot,
			OpsSaveTest::Persistents(*RestoredClock, *RestoredBoard, *RestoredFuel), *RestoredNetwork)))
	{
		return false;
	}

	TestEqual(TEXT("a save while paused reloads still paused"),
		RestoredClock->GetSpeed(), ESimSpeed::Paused);
	RestoredClock->TogglePause();
	TestEqual(TEXT("unpausing after a load resumes the speed that was paused FROM, not the "
		"constructor default - the bug this test pins"), RestoredClock->GetSpeed(), ESimSpeed::X4);
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
	Source->SetUniformDay(900.0);
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
	UJobBoard* RestoredFuel = NewObject<UJobBoard>();
	if (!TestTrue(TEXT("restore succeeds against a legacy snapshot"),
		OpsSave::Restore(Legacy, OpsSaveTest::Persistents(*RestoredClock, *RestoredBoard, *RestoredFuel), *RestoredNetwork)))
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
	UJobBoard* Fuel = NewObject<UJobBoard>();
	// A truck "on its way home" from a PRE-load session - exactly the state a load's
	// ClearAgents makes stale, and exactly what leaked before this fix: nothing ever
	// touched GoingHome across a load, so this entry outlived the truck it named forever.
	Fuel->AddVehicleForTest(TEXT("FUEL"), FEntityInstanceId(), EServiceVehicleState::ToFacility, 0.0);
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
	Fuel->AddVehicleForTest(TEXT("FUEL"), FEntityInstanceId(), EServiceVehicleState::ToFacility, 0.0);
	if (!TestEqual(TEXT("set up again with one truck going home, for the round trip"),
		Fuel->TrucksGoingHomeForTest(), 1))
	{
		return false;
	}
	USimClock* Clock = NewObject<USimClock>();
	URoadNetwork* Net = NewObject<URoadNetwork>();
	UFlightBoard* Board = NewObject<UFlightBoard>();
	FOpsSnapshot RoundTrip;
	OpsSave::Capture(OpsSaveTest::Persistents(*Clock, *Board, *Fuel), *Net, RoundTrip);

	USimClock* RestoredClock = NewObject<USimClock>();
	URoadNetwork* RestoredNet = NewObject<URoadNetwork>();
	UFlightBoard* RestoredBoard = NewObject<UFlightBoard>();
	UJobBoard* RestoredFuel = NewObject<UJobBoard>();
	if (!TestTrue(TEXT("round-trip restore succeeds"),
		OpsSave::Restore(RoundTrip, OpsSaveTest::Persistents(*RestoredClock, *RestoredBoard, *RestoredFuel), *RestoredNet)))
	{
		return false;
	}
	TestEqual(TEXT("a full Capture/Restore round trip does not resurrect the stale truck"),
		RestoredFuel->TrucksGoingHomeForTest(), 0);
	return true;
}

/**
 * THE FLEET IS SAVED (spec 2026-09-28-service-vehicle-lifecycle §6 stage 3). The player will BUY
 * vehicles; losing them on a load would be losing property. A vehicle out on a job at save time comes
 * back Idle at its depot with what it was carrying - its agent and its queue named things a load
 * clears (agents, and the jobs of aircraft that are not restored), so those do not survive.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOpsSaveFleetSurvivesALoadTest,
	"AirportOps.Model.Save.FleetSurvivesALoad",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FOpsSaveFleetSurvivesALoadTest::RunTest(const FString& Parameters)
{
	FEntityInstanceId Depot;
	Depot.Index = 3;
	Depot.Generation = 1;

	UJobBoard* Fuel = NewObject<UJobBoard>();
	FServiceVehicle& Out = Fuel->AddVehicleForTest(TEXT("FUEL"), Depot, EServiceVehicleState::ToJob, 400.0);
	Out.AgentId = 17;
	Out.CurrentJob = 5;
	Out.Queue = { 6, 7 };
	const int32 SavedId = Out.Id;

	USimClock* Clock = NewObject<USimClock>();
	URoadNetwork* Net = NewObject<URoadNetwork>();
	UFlightBoard* Board = NewObject<UFlightBoard>();
	FOpsSnapshot Snapshot;
	OpsSave::Capture(OpsSaveTest::Persistents(*Clock, *Board, *Fuel), *Net, Snapshot);

	UJobBoard* RestoredFuel = NewObject<UJobBoard>();
	if (!TestTrue(TEXT("restore succeeds"), OpsSave::Restore(Snapshot,
		OpsSaveTest::Persistents(*NewObject<USimClock>(), *NewObject<UFlightBoard>(), *RestoredFuel), *NewObject<URoadNetwork>())))
	{
		return false;
	}

	if (!TestEqual(TEXT("the vehicle survives the load"), RestoredFuel->GetVehicles().Num(), 1)) { return false; }
	const FServiceVehicle& Back = RestoredFuel->GetVehicles()[0];
	TestEqual(TEXT("the same vehicle"), Back.Id, SavedId);
	TestEqual(TEXT("of the same kind"), Back.TypeCode, FName(TEXT("FUEL")));
	TestTrue(TEXT("at the same depot"), Back.Home == Depot);
	TestEqual(TEXT("carrying what it carried"), Back.Cargo, 400.0, 1e-9);
	TestEqual(TEXT("idle at home"), static_cast<int32>(Back.State), static_cast<int32>(EServiceVehicleState::Idle));
	TestEqual(TEXT("with no agent - agents are never saved"), Back.AgentId, 0);
	TestEqual(TEXT("and no job - its aircraft are not restored"), Back.CurrentJob, 0);
	TestEqual(TEXT("and an empty queue"), Back.Queue.Num(), 0);
	TestTrue(TEXT("a vehicle added after the load gets a fresh id, not the restored one's"),
		RestoredFuel->AddVehicleForTest(TEXT("FUEL"), Depot, EServiceVehicleState::Idle, 0.0).Id != SavedId);
	return true;
}

/**
 * A RESTORE IS A CHANGE (#426 rows 4 and 5). A load deserialises INTO the live objects, so no view sees a new object -
 * it sees the same ledger and the same network, and asks their revisions whether anything moved. The ledger bumped
 * only on Post/RollUp/Open, so the bar's balance and the ledger rows kept the pre-load money until the next fee; the
 * network's EditRevision is a plain field the in-place Serialize never touched, so every cache keyed on it (the
 * deletion plan, the ghost) kept its pre-load answer. IN PLACE, the way UOpsRuntime::LoadFromSlot restores.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOpsSaveRestoreMovesRevisionsTest,
	"AirportOps.Model.Save.RestoreMovesTheRevisions",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FOpsSaveRestoreMovesRevisionsTest::RunTest(const FString& Parameters)
{
	USimClock* Clock = NewObject<USimClock>();
	UFlightBoard* Board = NewObject<UFlightBoard>();
	UJobBoard* Fuel = NewObject<UJobBoard>();
	ULedger* Ledger = NewObject<ULedger>();
	UPricing* Pricing = NewObject<UPricing>();
	URoadNetwork* Net = NewObject<URoadNetwork>();
	Net->AddNode(FVector2D(0.0, 0.0));
	// THE GUIDELINES DERIVED FROM THIS ROAD, as a rebuild leaves them - the load below must not un-derive them.
	Net->MarkGuidelinesDerived();

	Ledger->Post(0.0, ELedgerCategory::LandingFee, 500.0, FText::FromString(TEXT("saved fee")));
	const double SavedBalance = Ledger->Balance();
	FOpsSnapshot Snapshot;
	OpsSave::Capture(OpsSaveTest::Persistents(*Clock, *Board, *Fuel, *Ledger, *Pricing), *Net, Snapshot);

	// AFTER THE SAVE: money and an edit the load must take back, and the numbers a view would remember.
	Ledger->Post(0.0, ELedgerCategory::LandingFee, 250.0, FText::FromString(TEXT("unsaved fee")));
	Net->AddNode(FVector2D(5000.0, 0.0));
	// THE CONTROL: an edit DOES leave the guidelines behind the road, so the check after the load can go red.
	if (!TestTrue(TEXT("an edit leaves the guidelines behind the road - or the check after the load measures nothing"),
		Net->AreGuidelinesBehindRoad())) { return false; }
	Net->MarkGuidelinesDerived();
	const int32 LedgerSeen = Ledger->Revision();
	const uint32 EditSeen = Net->GetEditRevision();
	const uint32 GuidelineSeen = Net->GetGuidelineRevision();

	if (!TestTrue(TEXT("restore succeeds"),
		OpsSave::Restore(Snapshot, OpsSaveTest::Persistents(*Clock, *Board, *Fuel, *Ledger, *Pricing), *Net))) { return false; }

	TestEqual(TEXT("the balance is the saved one"), Ledger->Balance(), SavedBalance, 1e-9);
	TestTrue(TEXT("and the ledger's revision moved - the bar and the ledger rows are gated on it, and would show the "
		"unsaved fee until the next post"), Ledger->Revision() != LedgerSeen);
	TestEqual(TEXT("the network is the saved one"), Net->GetNodes().Num(), 1);
	TestTrue(TEXT("and its EditRevision moved - a cache keyed on it (the deletion plan, the ghost) would otherwise "
		"answer for the pre-load graph"), Net->GetEditRevision() != EditSeen);
	TestTrue(TEXT("and its GuidelineRevision moved - the guideline arrays were replaced with the road, and a table derived "
		"from them (FNodeReachCache, the pose-node index, the ops runtime's network poll) must hear it"),
		Net->GetGuidelineRevision() != GuidelineSeen);
	TestFalse(TEXT("but the guidelines are NOT behind the road: the two arrived together, and a load that said otherwise "
		"would refuse every plan until a rebuild - which an editor undo never runs"), Net->AreGuidelinesBehindRoad());
	return true;
}

/**
 * #426, THE LEDGER'S HALF OF (b): a snapshot with NO "Ledger" blob (from before the ledger) restores no money - so the
 * replaced session's must not survive it. UFlightBoard's and UJobBoard's OnBeforeRestore reason.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOpsSaveNoLedgerBlobResetsMoneyTest,
	"AirportOps.Model.Save.NoLedgerBlobResetsTheMoney",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FOpsSaveNoLedgerBlobResetsMoneyTest::RunTest(const FString& Parameters)
{
	ULedger* Ledger = NewObject<ULedger>();
	Ledger->Open(1000.0);
	Ledger->Post(0.0, ELedgerCategory::LandingFee, 500.0, FText::FromString(TEXT("the replaced session's fee")));
	const int32 Seen = Ledger->Revision();

	FOpsSnapshot NoLedger;
	NoLedger.Version = 6;
	if (!TestTrue(TEXT("restore succeeds"), OpsSave::Restore(NoLedger, OpsSaveTest::Persistents(*NewObject<USimClock>(),
		*NewObject<UFlightBoard>(), *NewObject<UJobBoard>(), *Ledger, *NewObject<UPricing>()), *NewObject<URoadNetwork>()))) { return false; }

	TestEqual(TEXT("no ledger blob, no rows: the replaced session's fee is gone"), Ledger->Entries().Num(), 0);
	TestEqual(TEXT("and the balance is this session's opening money, not the replaced session's"), Ledger->Balance(), 1000.0, 1e-9);
	TestTrue(TEXT("and the revision moved, so the bar re-reads it"), Ledger->Revision() != Seen);
	return true;
}

/**
 * #426 (b): A SNAPSHOT WITH NO "Flights" BLOB (a v1 save, or one from before the board) restores no flights - so the
 * flights of the session being replaced must go. UFlightBoard::Serialize retires them, but OpsSave only calls it when
 * there is a blob; OnBeforeRestore runs whatever the snapshot holds (UJobBoard's reason - FuelResetsOnRestore above).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOpsSaveNoFlightsBlobRetiresBoardTest,
	"AirportOps.Model.Save.NoFlightsBlobRetiresTheBoard",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FOpsSaveNoFlightsBlobRetiresBoardTest::RunTest(const FString& Parameters)
{
	USimClock* Clock = NewObject<USimClock>();
	UFlightBoard* Board = NewObject<UFlightBoard>();
	UJobBoard* Fuel = NewObject<UJobBoard>();
	UFlight* PreLoad = NewObject<UFlight>(Board);
	Board->AddOffer(*Clock, PreLoad);
	if (!TestEqual(TEXT("the session being replaced has an offer"), Board->Offers().Num(), 1)) { return false; }
	const uint32 Seen = Board->Revision();

	FOpsSnapshot NoFlights;
	NoFlights.Version = 6;
	if (!TestTrue(TEXT("restore succeeds"),
		OpsSave::Restore(NoFlights, OpsSaveTest::Persistents(*Clock, *Board, *Fuel), *NewObject<URoadNetwork>()))) { return false; }

	TestEqual(TEXT("no flights blob, no flights: the pre-load offer is gone"), Board->Offers().Num(), 0);
	TestEqual(TEXT("and nothing is live"), Board->Live().Num(), 0);
	TestTrue(TEXT("RETIRED, not merely dropped - a view holding it reads null, as after a blob's load"),
		!IsValid(PreLoad));
	TestTrue(TEXT("and the board's revision moved, so the inbox re-reads"), Board->Revision() != Seen);
	return true;
}

/**
 * #459: THE ACTOR'S FALLBACK PROFILE IS SAVED AS "THE DEFAULT", NOT AS A PATH. It lives in the transient package, so its
 * path names nothing in any other process - and in this one, possibly something else. OpsSave writes it as null, which
 * URoadNetwork::ProfileFor reads as DefaultProfile and the loading actor re-resolves. Measured in the same process, where a
 * written path WOULD re-find the live object: so a null after the round trip proves nothing was written. Every OTHER
 * reference is still written as before - a test's transient runway round-trips in-process, and content assets anywhere.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOpsSaveFallbackProfileAsDefaultTest,
	"AirportOps.Model.Save.FallbackProfileIsSavedAsTheDefault",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FOpsSaveFallbackProfileAsDefaultTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>();
	// STAND-INS FOR TWO ACTORS' FALLBACKS, marked as ARoadNetworkActor::ResolveProfile marks its own: this actor's (the
	// default), and the EDITOR actor's that roads the editor laid still name in a PIE copy (#465 review) - NOT the default.
	URoadProfile* Fallback = TestProfiles::Taxiway();
	Fallback->bActorFallback = true;
	URoadProfile* EditorFallback = TestProfiles::Taxiway();
	EditorFallback->bActorFallback = true;
	URoadProfile* Other = TestProfiles::Runway();       // a transient profile that is NOT a fallback
	Net->DefaultProfile = Fallback;
	const FRoadSegmentId Laid = Net->AddStraightSegment(Net->AddNode(FVector2D(0.0, 0.0)), Net->AddNode(FVector2D(20000.0, 0.0)), Fallback);
	const FRoadSegmentId EditorLaid = Net->AddStraightSegment(Net->AddNode(FVector2D(0.0, 20000.0)), Net->AddNode(FVector2D(20000.0, 20000.0)), EditorFallback);
	const FRoadSegmentId Strip = Net->AddStraightSegment(Net->AddNode(FVector2D(0.0, -50000.0)), Net->AddNode(FVector2D(60000.0, -50000.0)), Other);

	FOpsSnapshot Snapshot;
	OpsSave::Capture(TArray<IOpsPersistent*>(), *Net, Snapshot);
	URoadNetwork* Loaded = NewObject<URoadNetwork>();
	if (!TestTrue(TEXT("restore succeeds"), OpsSave::Restore(Snapshot, TArray<IOpsPersistent*>(), *Loaded))) { return false; }

	const FRoadSegment* LoadedLaid = Loaded->GetSegment(Laid);
	const FRoadSegment* LoadedEditorLaid = Loaded->GetSegment(EditorLaid);
	const FRoadSegment* LoadedStrip = Loaded->GetSegment(Strip);
	if (!TestTrue(TEXT("every road came back"), LoadedLaid != nullptr && LoadedEditorLaid != nullptr && LoadedStrip != nullptr)) { return false; }
	TestNull(TEXT("the road laid with the fallback came back with no profile of its own - 'the default' - though the fallback "
		"is alive in this process, so a written path would have re-found it"), LoadedLaid->Profile.Get());
	TestNull(TEXT("and so did the road laid with ANOTHER actor's fallback, which is not the network's default - the save keys "
		"on the marker, not on DefaultProfile"), LoadedEditorLaid->Profile.Get());
	TestNull(TEXT("and so did DefaultProfile, for the loading actor to re-resolve"), Loaded->DefaultProfile.Get());
	TestTrue(TEXT("any other reference is written as before - the runway's profile round-trips"), LoadedStrip->Profile == Other);
	TestTrue(TEXT("and the saving network is untouched - the save wrote, it did not edit"),
		Net->GetSegment(Laid)->Profile == Fallback && Net->DefaultProfile == Fallback);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOpsSaveDesignFiguresAreNotSavedTest,
	"AirportOps.Model.Save.DesignFiguresAreNotSaved",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FOpsSaveDesignFiguresAreNotSavedTest::RunTest(const FString& Parameters)
{
	// #449: THE GAME IS SAVED, THE DESIGN IS NOT. Each receiver written with its design figures moved and read into a
	// fresh one: the design comes back as the class's default, and the one game figure beside it on each object comes
	// back as saved (the CONTROL - a blob that restored nothing would pass the rest). The runtime's re-apply cannot mask
	// this: no runtime is involved, so a saved design figure reads here as the save's value.
	auto RoundTrip = [](UObject& From, UObject& Into)
	{
		TArray<uint8> Bytes;
		OpsSave::SerializeObject(From, Bytes);
		OpsSave::DeserializeObject(Into, Bytes);
	};

	{
		// THE PLAYER'S LEVER is the pricing's game figure; elasticity, refund, fuel price and currency are the design.
		UPricing* Saved = NewObject<UPricing>(GetTransientPackage());
		const UPricing* Defaults = GetDefault<UPricing>();
		Saved->LandingFeeMultiplier = 1.3;
		Saved->Elasticity = Defaults->Elasticity + 0.25;
		Saved->RefundFraction = Defaults->RefundFraction + 0.25;
		Saved->FuelPricePerLitre = Defaults->FuelPricePerLitre + 1.0;
		Saved->CurrencySymbol = TEXT("X");
		UPricing* Loaded = NewObject<UPricing>(GetTransientPackage());
		RoundTrip(*Saved, *Loaded);
		TestEqual(TEXT("CONTROL: the player's lever came back"), Loaded->LandingFeeMultiplier, 1.3, 1e-12);
		TestEqual(TEXT("the elasticity is the design's, not the save's"), Loaded->Elasticity, Defaults->Elasticity, 1e-12);
		TestEqual(TEXT("so is the refund"), Loaded->RefundFraction, Defaults->RefundFraction, 1e-12);
		TestEqual(TEXT("and the fuel price"), Loaded->FuelPricePerLitre, Defaults->FuelPricePerLitre, 1e-12);
		TestEqual(TEXT("and the currency"), Loaded->CurrencySymbol, Defaults->CurrencySymbol);
	}
	{
		// THE CLOCK: Now is the game; the day's shape is the design.
		USimClock* Saved = NewObject<USimClock>(GetTransientPackage());
		const USimClock* Defaults = GetDefault<USimClock>();
		Saved->SetSpeed(ESimSpeed::X1);
		Saved->Advance(123.0);
		Saved->DawnHour = Defaults->DawnHour + 1.0;
		Saved->DuskHour = Defaults->DuskHour - 1.0;
		Saved->RealSecondsDaylight = Defaults->RealSecondsDaylight + 100.0;
		Saved->RealSecondsNight = Defaults->RealSecondsNight + 100.0;
		const double SavedNow = Saved->Now();
		USimClock* Loaded = NewObject<USimClock>(GetTransientPackage());
		RoundTrip(*Saved, *Loaded);
		TestTrue(TEXT("CONTROL: the clock ran"), SavedNow > 0.0);
		TestEqual(TEXT("CONTROL: game time came back"), Loaded->Now(), SavedNow, 1e-9);
		TestEqual(TEXT("dawn is the design's, not the save's"), Loaded->DawnHour, Defaults->DawnHour, 1e-12);
		TestEqual(TEXT("so is dusk"), Loaded->DuskHour, Defaults->DuskHour, 1e-12);
		TestEqual(TEXT("and the day's length"), Loaded->RealSecondsDaylight, Defaults->RealSecondsDaylight, 1e-12);
		TestEqual(TEXT("and the night's"), Loaded->RealSecondsNight, Defaults->RealSecondsNight, 1e-12);
	}
	{
		// THE JOB BOARD: its refill rate is the design (the catalogue is Transient too, and resolved, not a figure).
		UJobBoard* Saved = NewObject<UJobBoard>(GetTransientPackage());
		Saved->RefillLitresPerMinutePerPump = GetDefault<UJobBoard>()->RefillLitresPerMinutePerPump + 111.0;
		UJobBoard* Loaded = NewObject<UJobBoard>(GetTransientPackage());
		RoundTrip(*Saved, *Loaded);
		TestEqual(TEXT("the refill rate is the design's, not the save's"), Loaded->RefillLitresPerMinutePerPump,
			GetDefault<UJobBoard>()->RefillLitresPerMinutePerPump, 1e-12);
	}
	{
		// THE OFFER GENERATOR: the dropped-offer count is the game; the inbox cap is the design.
		UOfferGenerator* Saved = NewObject<UOfferGenerator>(GetTransientPackage());
		Saved->DroppedOffers = 5;
		Saved->MaxPendingOffers = GetDefault<UOfferGenerator>()->MaxPendingOffers + 3;
		UOfferGenerator* Loaded = NewObject<UOfferGenerator>(GetTransientPackage());
		RoundTrip(*Saved, *Loaded);
		TestEqual(TEXT("CONTROL: the dropped offers came back"), Loaded->DroppedOffers, 5);
		TestEqual(TEXT("the inbox cap is the design's, not the save's"), Loaded->MaxPendingOffers,
			GetDefault<UOfferGenerator>()->MaxPendingOffers);
	}
	return true;
}

#endif
