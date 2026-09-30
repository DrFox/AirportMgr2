#include "CoreMinimal.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/Flight.h"
#include "Model/FlightBoard.h"
#include "Model/GroundTraffic.h"
#include "Model/JobBoard.h"
#include "Model/OpsDefinition.h"
#include "Model/OpsSave.h"
#include "Model/RoadNetwork.h"
#include "Model/SimClock.h"
#include "Model/StandAllocator.h"

#if WITH_DEV_AUTOMATION_TESTS

// UJobBoard'S HALF OF FLEET PURCHASE (facility-upgrades spec §3): the board owns Vehicles, so a bought
// vehicle enters and a sold one leaves only through these two doors.

namespace
{
	/** A board with the scenario's figures and a depot id to be home. Prefixed: unity build. */
	UJobBoard* FleetBoardWithSpecs()
	{
		UJobBoard* Board = NewObject<UJobBoard>(GetTransientPackage());
		Board->VehicleSpecs = GetDefault<UScenario>()->FuelVehicles;
		return Board;
	}

	FEntityInstanceId FleetTestDepotId()
	{
		FEntityInstanceId Id;
		Id.Index = 2;
		Id.Generation = 1;
		return Id;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFleetPurchasedIdleFullTest, "AirportOps.Model.Fleet.PurchasedVehicleIsIdleAndFull",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFleetPurchasedIdleFullTest::RunTest(const FString&)
{
	UJobBoard* Board = FleetBoardWithSpecs();
	const uint32 Before = Board->GetFleetRevision();
	const int32 Id = Board->AddPurchasedVehicle(TEXT("FUEL"), FleetTestDepotId());
	if (!TestTrue(TEXT("a purchase issues a vehicle id"), Id != 0)) { return false; }
	const FServiceVehicle* Vehicle = Board->FindVehicle(Id);
	if (!TestNotNull(TEXT("and the vehicle is on the board"), Vehicle)) { return false; }
	TestEqual(TEXT("idle at home - a vehicle at home is not on the road"),
		static_cast<int32>(Vehicle->State), static_cast<int32>(EServiceVehicleState::Idle));
	TestTrue(TEXT("at the depot it was bought for"), Vehicle->Home == FleetTestDepotId());
	TestEqual(TEXT("delivered full (spec §3)"), Vehicle->Cargo, 10000.0, 1e-9);
	TestEqual(TEXT("with no agent"), Vehicle->AgentId, 0);
	TestTrue(TEXT("the fleet revision moves, so a re-bid sees it"), Board->GetFleetRevision() != Before);
	TestEqual(TEXT("counted at its depot"), Board->VehiclesAt(FleetTestDepotId()), 1);
	TestEqual(TEXT("an unset home is refused"), Board->AddPurchasedVehicle(TEXT("FUEL"), FEntityInstanceId()), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFleetPurchaseReopensTest, "AirportOps.Model.Fleet.PurchaseReopensRefusedJobs",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFleetPurchaseReopensTest::RunTest(const FString&)
{
	// A REFUSED JOB IS TERMINAL until the guideline revision moves - and buying the first vehicle moves
	// no guideline. Without this the aircraft that was refused NoVehicles waits out its turnaround.
	UJobBoard* Board = FleetBoardWithSpecs();
	FServiceJob& Job = Board->AddJobForTest(41, EServiceJobState::Unserviceable, EServiceRefusal::NoVehicles, 0);
	const int32 JobId = Job.Id;
	Board->AddPurchasedVehicle(TEXT("FUEL"), FleetTestDepotId());
	const FServiceJob* After = Board->GetJobs().FindByPredicate([JobId](const FServiceJob& J) { return J.Id == JobId; });
	if (!TestNotNull(TEXT("the job is still on the board"), After)) { return false; }
	TestEqual(TEXT("and open again, for the next pass to bid"),
		static_cast<int32>(After->State), static_cast<int32>(EServiceJobState::Open));
	TestEqual(TEXT("with its old reason cleared"), static_cast<int32>(After->Why), static_cast<int32>(EServiceRefusal::None));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFleetOnlyIdleLeavesTest, "AirportOps.Model.Fleet.OnlyAnIdleVehicleLeaves",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFleetOnlyIdleLeavesTest::RunTest(const FString&)
{
	// R5: SOLD ONLY WHEN PARKED with nothing promised. A queued job is a promise another bid was judged
	// against; a vehicle out has an agent on the road.
	UJobBoard* Board = FleetBoardWithSpecs();
	const int32 Out = Board->AddVehicleForTest(TEXT("FUEL"), FleetTestDepotId(), EServiceVehicleState::ToJob, 0.0).Id;
	FServiceVehicle& Promised = Board->AddVehicleForTest(TEXT("FUEL"), FleetTestDepotId(), EServiceVehicleState::Idle, 0.0);
	Promised.Queue = { 99 };
	const int32 PromisedId = Promised.Id;
	const int32 Free = Board->AddVehicleForTest(TEXT("UTILITY"), FleetTestDepotId(), EServiceVehicleState::Idle, 0.0).Id;

	TestFalse(TEXT("a vehicle on its way to a job cannot leave"), Board->RemoveVehicle(Out));
	TestFalse(TEXT("nor an idle one with a queued job"), Board->RemoveVehicle(PromisedId));
	TestFalse(TEXT("and CanRemoveVehicle agrees with RemoveVehicle"), Board->CanRemoveVehicle(PromisedId));
	TestTrue(TEXT("an idle one with nothing queued can be asked"), Board->CanRemoveVehicle(Free));
	const uint32 Before = Board->GetFleetRevision();
	TestTrue(TEXT("and removed"), Board->RemoveVehicle(Free));
	TestNull(TEXT("it is gone from the board"), Board->FindVehicle(Free));
	TestTrue(TEXT("the fleet revision moves"), Board->GetFleetRevision() != Before);
	TestFalse(TEXT("an unknown id removes nothing"), Board->RemoveVehicle(12345));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFleetSoldStaysSoldTest, "AirportOps.Model.Fleet.SoldStarterFleetIsNotReseededAfterLoad",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFleetSoldStaysSoldTest::RunTest(const FString&)
{
	// REVIEW FOCUS 1: a starter depot whose fleet the player sold must not regrow after a load. The
	// placeholder seeds any depot missing from SeededDepots, which used to be Transient and so empty
	// after every load.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	USimClock* Clock = NewObject<USimClock>(GetTransientPackage());
	UJobBoard* Board = FleetBoardWithSpecs();
	Board->DefaultFleetTypes = { TEXT("FUEL") };
	UEntityDefinition* Depot = UEntityDefinition::MakeFuelDepotTransient();
	Net->PlaceEntity(Depot, Depot->Anchors, FVector2D(0.0, 0.0), 0.0, 0.0, EServiceRole::Fuel, 1);

	Board->Tick(*Traffic, *Net, *Clock);
	if (!TestEqual(TEXT("setup: the starter truck is seeded"), Board->GetVehicles().Num(), 1)) { return false; }
	TestTrue(TEXT("setup: and sold"), Board->RemoveVehicle(Board->GetVehicles()[0].Id));

	FOpsSnapshot Snapshot;
	OpsSave::CaptureBlob(*Board, Snapshot);
	OpsSave::RestoreBlob(Snapshot, *Board);
	Board->Tick(*Traffic, *Net, *Clock);
	TestEqual(TEXT("the load does not seed the sold-out depot again"), Board->GetVehicles().Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFleetVerdictDatedTest, "AirportOps.Model.Fleet.OfferVerdictIsDatedByTheFleet",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFleetVerdictDatedTest::RunTest(const FString&)
{
	// REVIEW FOCUS 2: the offer row's "Accept (no fuel)" is cached; a purchase must re-date it or the
	// row goes on saying no fuel after the bowser arrived.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	USimClock* Clock = NewObject<USimClock>(GetTransientPackage());
	UFlightBoard* Flights = NewObject<UFlightBoard>(GetTransientPackage());
	Flights->Allocator = NewObject<UStandAllocator>(GetTransientPackage());
	UJobBoard* Board = FleetBoardWithSpecs();
	Flights->Fuel = Board;
	UFlight* Flight = NewObject<UFlight>(GetTransientPackage());
	Flight->OfferWindowSeconds = 60.0;
	Flight->OfferSecondsLeft = 60.0;
	Flights->AddOffer(*Clock, Flight);

	const uint32 FirstAt = Flights->VerdictFor(*Traffic, *Net, *Flight).FleetAt;
	TestEqual(TEXT("the verdict is dated by the fleet it was judged against"), FirstAt, Board->GetFleetRevision());
	Board->AddPurchasedVehicle(TEXT("FUEL"), FleetTestDepotId());
	TestEqual(TEXT("a purchase re-dates it on the next ask"),
		Flights->VerdictFor(*Traffic, *Net, *Flight).FleetAt, Board->GetFleetRevision());
	TestTrue(TEXT("which is a different date"), FirstAt != Board->GetFleetRevision());
	return true;
}

#endif
