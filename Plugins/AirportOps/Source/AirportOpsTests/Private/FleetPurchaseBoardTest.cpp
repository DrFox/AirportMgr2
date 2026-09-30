#include "CoreMinimal.h"
#include "Content/AirsideSettings.h"
#include "Model/VehicleCodes.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Present/OpsRuntime.h"
#include "Model/Flight.h"
#include "Model/FlightBoard.h"
#include "Model/GroundTraffic.h"
#include "Model/JobBoard.h"
#include "Model/Ledger.h"
#include "Model/OpsDefinition.h"
#include "Model/OpsEventBus.h"
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
		UOpsRuntime::ResolveVehicleCatalogue(*Board, *GetDefault<UScenario>());   // as Attach does (#430)
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
	const int32 Id = Board->Fleet().Add(TEXT("FUEL"), FleetTestDepotId(), EFleetOrigin::Bought, 0.0);
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
	TestEqual(TEXT("an unset home is refused"), Board->Fleet().Add(TEXT("FUEL"), FEntityInstanceId(), EFleetOrigin::Bought, 0.0), 0);
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
	Board->Fleet().Add(TEXT("FUEL"), FleetTestDepotId(), EFleetOrigin::Bought, 0.0);
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

	TestFalse(TEXT("a vehicle on its way to a job cannot leave"), Board->Fleet().Withdraw(Out, EFleetReason::Sold, 0.0));
	TestFalse(TEXT("nor an idle one with a queued job"), Board->Fleet().Withdraw(PromisedId, EFleetReason::Sold, 0.0));
	TestFalse(TEXT("and CanRemoveVehicle agrees with the sale"), Board->CanRemoveVehicle(PromisedId));
	TestTrue(TEXT("an idle one with nothing queued can be asked"), Board->CanRemoveVehicle(Free));
	const uint32 Before = Board->GetFleetRevision();
	const uint32 CompositionBefore = Board->GetFleetCompositionRevision();
	TestTrue(TEXT("and removed"), Board->Fleet().Withdraw(Free, EFleetReason::Sold, 0.0));
	TestNull(TEXT("it is gone from the board"), Board->FindVehicle(Free));
	TestTrue(TEXT("the fleet revision moves"), Board->GetFleetRevision() != Before);
	TestTrue(TEXT("and so does the composition"), Board->GetFleetCompositionRevision() != CompositionBefore);
	TestFalse(TEXT("an unknown id removes nothing"), Board->Fleet().Withdraw(12345, EFleetReason::Sold, 0.0));
	// A DEPOT'S REMOVAL HAS NO SUCH PRECONDITION: the vehicle out for a job goes with its depot (SyncFleet freed it first).
	TestTrue(TEXT("a vehicle whose depot is gone leaves whatever it was doing"), Board->Fleet().Withdraw(Out, EFleetReason::DepotRemoved, 0.0));
	TestNull(TEXT("and is gone"), Board->FindVehicle(Out));
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
	Board->StarterFleet = { TEXT("FUEL") };
	UEntityDefinition* Depot = UEntityDefinition::MakeFuelDepotTransient();
	Net->PlaceEntity(Depot, Depot->Anchors, FVector2D(0.0, 0.0), 0.0, 0.0, EServiceRole::Fuel, 1);

	Board->Tick(*Traffic, *Net, *Clock);
	if (!TestEqual(TEXT("setup: the starter truck is seeded"), Board->GetVehicles().Num(), 1)) { return false; }
	TestTrue(TEXT("setup: and sold"), Board->Fleet().Withdraw(Board->GetVehicles()[0].Id, EFleetReason::Sold, 0.0));

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

	// DATED BY THE FLEET'S COMPOSITION (#443), the counter that moves when a vehicle joins or leaves and NOT when one changes
	// state: bFuelServable reads which vehicles exist, never what they are doing.
	const uint32 FirstAt = Flights->VerdictFor(*Traffic, *Net, *Flight).FleetAt;
	TestEqual(TEXT("the verdict is dated by the fleet it was judged against"), FirstAt, Board->GetFleetCompositionRevision());
	Board->Fleet().Add(TEXT("FUEL"), FleetTestDepotId(), EFleetOrigin::Bought, 0.0);
	TestEqual(TEXT("a purchase re-dates it on the next ask"),
		Flights->VerdictFor(*Traffic, *Net, *Flight).FleetAt, Board->GetFleetCompositionRevision());
	TestTrue(TEXT("which is a different date"), FirstAt != Board->GetFleetCompositionRevision());
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFleetTransitionsDoNotReplanTest, "AirportOps.Model.FlightBoard.VehicleTransitionsDoNotReplanOffers",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFleetTransitionsDoNotReplanTest::RunTest(const FString&)
{
	// #443 (the #169 shape): FlightBoard keyed its WHOLE offer verdict on the fleet counter, which moves on every vehicle
	// TRANSITION since #428 - so a truck arriving or finishing a refill re-planned every pending offer's full arrival plan,
	// which reads no fleet. bFuelServable (CouldServe) reads which vehicles exist and never their state, so it is dated by
	// the composition counter alone, and Why by the board, the graph and occupancy.
	// STAGED WITHOUT TRAFFIC, so nothing but the transition can move a revision the verdict reads: a vehicle refilling at its
	// depot whose refill is due finishes on the board's Step (AtFacility -> Idle), the issue's own example.
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
	UEntityDefinition* Def = UEntityDefinition::MakeFuelDepotTransient();
	const FEntityInstanceId Depot = Net->PlaceEntity(Def, Def->Anchors, FVector2D(0.0, 0.0), 0.0, 0.0, EServiceRole::Fuel, 0);
	const int32 VehicleId = Board->AddVehicleForTest(TEXT("FUEL"), Depot, EServiceVehicleState::AtFacility, 5000.0).Id;

	Flights->VerdictFor(*Traffic, *Net, *Flight);
	const int32 Plans = Flights->GetWhyNotAcceptableCallsForTest();
	const uint32 TransitionsBefore = Board->GetFleetRevision();
	const uint32 CompositionBefore = Board->GetFleetCompositionRevision();

	Board->Step(*Traffic, *Net, *Clock);
	const FServiceVehicle* After = Board->FindVehicle(VehicleId);
	if (!TestNotNull(TEXT("the vehicle is still on the board"), After)) { return false; }
	TestEqual(TEXT("premise: its refill finished - the vehicle is Idle at home"),
		static_cast<int32>(After->State), static_cast<int32>(EServiceVehicleState::Idle));
	TestTrue(TEXT("the finished refill is a transition: the re-bid's counter moved"), Board->GetFleetRevision() != TransitionsBefore);
	TestEqual(TEXT("and it is not a change of composition"), Board->GetFleetCompositionRevision(), CompositionBefore);
	Flights->VerdictFor(*Traffic, *Net, *Flight);
	TestEqual(TEXT("so it re-plans nothing: the offer's arrival is not searched again"), Flights->GetWhyNotAcceptableCallsForTest(), Plans);

	// AND A REAL CHANGE OF COMPOSITION RE-JUDGES THE FUEL ANSWER, which is what the counter is for - still without a re-plan.
	Board->Fleet().Add(TEXT("FUEL"), Depot, EFleetOrigin::Bought, 0.0);
	TestEqual(TEXT("a vehicle joining re-dates the verdict by the new composition"),
		Flights->VerdictFor(*Traffic, *Net, *Flight).FleetAt, Board->GetFleetCompositionRevision());
	TestEqual(TEXT("without re-planning the arrival, which reads no fleet"), Flights->GetWhyNotAcceptableCallsForTest(), Plans);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFleetEveryChangeOwesTheSameTest, "AirportOps.Model.Fleet.EveryWayInAndOutOwesTheSameThings",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFleetEveryChangeOwesTheSameTest::RunTest(const FString&)
{
	// THE ISSUE #443 SHAPE, one row per way in and out: every change to the fleet owes its ledger line (or none, for a free
	// one), its FleetChanged event and both counters - whichever door's caller asked. Before, the seeding owed nothing, a
	// removed depot's credit came from the job board in its own wording with no event, and only a purchase and a sale
	// were announced.
	USimClock* Clock = NewObject<USimClock>(GetTransientPackage());
	ULedger* Ledger = NewObject<ULedger>(GetTransientPackage());
	Ledger->Clock = Clock;
	Ledger->Open(500000.0);
	UJobBoard* Board = FleetBoardWithSpecs();
	Board->Ledger = Ledger;
	FOpsEventBus Bus;
	TArray<FFleetChangedEvent> Seen;
	Bus.BeginWiring();
	Bus.Subscribe<FFleetChangedEvent>(EOpsTier::Presentation, TEXT("test"), [&Seen](const FFleetChangedEvent& E) { Seen.Add(E); });
	Bus.EndWiring();
	Board->Bus = &Bus;
	const FEntityInstanceId Depot = FleetTestDepotId();

	struct FStep { const TCHAR* Name; EFleetChange Change; double LedgerDelta; };
	auto Expect = [&](const FStep& Step, const int32 VehicleId, const double BalanceBefore, const uint32 FleetBefore, const uint32 CompositionBefore)
	{
		Bus.Drain();
		if (!TestEqual(FString::Printf(TEXT("%s: exactly one FleetChanged"), Step.Name), Seen.Num(), 1)) { Seen.Reset(); return; }
		TestEqual(FString::Printf(TEXT("%s: says what happened"), Step.Name), static_cast<int32>(Seen[0].Change), static_cast<int32>(Step.Change));
		TestEqual(FString::Printf(TEXT("%s: names the vehicle"), Step.Name), Seen[0].VehicleId, VehicleId);
		TestEqual(FString::Printf(TEXT("%s: and its depot"), Step.Name), Seen[0].Depot, Depot.Index);
		TestEqual(FString::Printf(TEXT("%s: the ledger moved by what the event says"), Step.Name), Ledger->Balance() - BalanceBefore, Step.LedgerDelta, 1e-6);
		TestEqual(FString::Printf(TEXT("%s: the event's amount is the money that moved (0 when free)"), Step.Name),
			FMath::Abs(Seen[0].Amount), FMath::Abs(Step.LedgerDelta), 1e-6);
		TestTrue(FString::Printf(TEXT("%s: the transition counter moved (the re-bid hears of it)"), Step.Name), Board->GetFleetRevision() != FleetBefore);
		TestTrue(FString::Printf(TEXT("%s: the composition counter moved (the offer verdict hears of it)"), Step.Name),
			Board->GetFleetCompositionRevision() != CompositionBefore);
		Seen.Reset();
	};

	double Balance = Ledger->Balance();
	uint32 Fleet = Board->GetFleetRevision();
	uint32 Composition = Board->GetFleetCompositionRevision();
	const int32 Bought = Board->Fleet().Add(TEXT("FUEL"), Depot, EFleetOrigin::Bought, 10.0);
	Expect({ TEXT("bought"), EFleetChange::Bought, -90000.0 }, Bought, Balance, Fleet, Composition);

	Balance = Ledger->Balance(); Fleet = Board->GetFleetRevision(); Composition = Board->GetFleetCompositionRevision();
	const int32 Seeded = Board->Fleet().Add(TEXT("UTILITY"), Depot, EFleetOrigin::Seeded, 10.0);
	Expect({ TEXT("seeded"), EFleetChange::Seeded, 0.0 }, Seeded, Balance, Fleet, Composition);
	TestEqual(TEXT("seeded: and it posted no ledger line at all"), Ledger->Entries().Num(), 1);

	Balance = Ledger->Balance(); Fleet = Board->GetFleetRevision(); Composition = Board->GetFleetCompositionRevision();
	TestTrue(TEXT("sold: an idle vehicle leaves"), Board->Fleet().Withdraw(Seeded, EFleetReason::Sold, 20.0));
	Expect({ TEXT("sold"), EFleetChange::Sold, 12500.0 }, Seeded, Balance, Fleet, Composition);

	Balance = Ledger->Balance(); Fleet = Board->GetFleetRevision(); Composition = Board->GetFleetCompositionRevision();
	TestTrue(TEXT("withdrawn: a vehicle leaves with its depot"), Board->Fleet().Withdraw(Bought, EFleetReason::DepotRemoved, 30.0));
	Expect({ TEXT("withdrawn"), EFleetChange::Withdrawn, 45000.0 }, Bought, Balance, Fleet, Composition);
	TestEqual(TEXT("every posting is a Fleet line, and a seeded vehicle posted none: buy, sale, removal"), Ledger->Entries().Num(), 3);
	for (const FLedgerEntry& Entry : Ledger->Entries())
	{
		TestEqual(TEXT("filed under Fleet"), static_cast<int32>(Entry.Category), static_cast<int32>(ELedgerCategory::Fleet));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFleetSeedingPublishesTest, "AirportOps.Model.Fleet.SeedingPublishesFleetChanged",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFleetSeedingPublishesTest::RunTest(const FString&)
{
	// THE STARTER FLEET USED TO JOIN THE BOARD WITH NO EVENT (#443): a FleetChanged subscriber saw the player's buys and
	// sells and missed the vehicles every starter depot begins with. Seeded through the Step, as production does it.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	USimClock* Clock = NewObject<USimClock>(GetTransientPackage());
	ULedger* Ledger = NewObject<ULedger>(GetTransientPackage());
	Ledger->Clock = Clock;
	Ledger->Open(1000.0);
	UJobBoard* Board = FleetBoardWithSpecs();
	Board->Ledger = Ledger;
	Board->StarterFleet = { TEXT("FUEL"), TEXT("UTILITY") };
	FOpsEventBus Bus;
	TArray<FFleetChangedEvent> Seen;
	Bus.BeginWiring();
	Bus.Subscribe<FFleetChangedEvent>(EOpsTier::Presentation, TEXT("test"), [&Seen](const FFleetChangedEvent& E) { Seen.Add(E); });
	Bus.EndWiring();
	Board->Bus = &Bus;
	UEntityDefinition* Depot = UEntityDefinition::MakeFuelDepotTransient();
	const FEntityInstanceId DepotId = Net->PlaceEntity(Depot, Depot->Anchors, FVector2D(0.0, 0.0), 0.0, 0.0, EServiceRole::Fuel, 1);

	const uint32 CompositionBefore = Board->GetFleetCompositionRevision();
	Board->Tick(*Traffic, *Net, *Clock);
	Bus.Drain();
	if (!TestEqual(TEXT("one starter truck of each of the two kinds is seeded"), Board->GetVehicles().Num(), 2)) { return false; }
	if (!TestEqual(TEXT("and each is announced"), Seen.Num(), 2)) { return false; }
	for (const FFleetChangedEvent& Event : Seen)
	{
		TestEqual(TEXT("as Seeded"), static_cast<int32>(Event.Change), static_cast<int32>(EFleetChange::Seeded));
		TestEqual(TEXT("at its depot"), Event.Depot, DepotId.Index);
		TestEqual(TEXT("free"), Event.Amount, 0.0, 1e-9);
		TestNotNull(TEXT("naming a vehicle that is on the board"), Board->FindVehicle(Event.VehicleId));
	}
	TestEqual(TEXT("and it charged nothing"), Ledger->Entries().Num(), 0);
	TestTrue(TEXT("the composition moved, so an offer's verdict re-judges"), Board->GetFleetCompositionRevision() != CompositionBefore);
	Seen.Reset();
	Board->Tick(*Traffic, *Net, *Clock);
	Bus.Drain();
	TestEqual(TEXT("a depot is seeded once: the next Step announces nothing"), Seen.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFleetSeedingReopensTest, "AirportOps.Model.Fleet.SeedingReopensRefusedJobs",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFleetSeedingReopensTest::RunTest(const FString&)
{
	// THE RE-OPEN BELONGS TO EVERY WAY IN, not only the purchase (#443): a job refused NoVehicles waits for "something
	// changes", and a starter fleet appearing under it is that change as much as a bought bowser is.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	UJobBoard* Board = FleetBoardWithSpecs();
	Board->StarterFleet = { TEXT("FUEL") };
	UEntityDefinition* Depot = UEntityDefinition::MakeFuelDepotTransient();
	Net->PlaceEntity(Depot, Depot->Anchors, FVector2D(0.0, 0.0), 0.0, 0.0, EServiceRole::Fuel, 1);
	const int32 JobId = Board->AddJobForTest(41, EServiceJobState::Unserviceable, EServiceRefusal::NoVehicles, Net->GetGuidelineRevision()).Id;
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	USimClock* Clock = NewObject<USimClock>(GetTransientPackage());

	// ONE TICK, the two passes of a drain in order: the seeding ("FleetSeed", through the door) re-opens the job, and the
	// Step's bid then judges it again. The job was refused at THIS graph revision, so the Step's own re-offer pass would not
	// touch it: what changed its reason is the seeding's re-open alone. Nothing to bid it to (no road), so it is refused
	// afresh - for the road, not for a vehicle.
	Board->Tick(*Traffic, *Net, *Clock);
	TestEqual(TEXT("the drain's seeding made the starter truck"), Board->GetVehicles().Num(), 1);
	const FServiceJob* After = Board->GetJobs().FindByPredicate([JobId](const FServiceJob& J) { return J.Id == JobId; });
	if (!TestNotNull(TEXT("the job is still on the board"), After)) { return false; }
	// THE EXACT POST-STATE, not merely "not NoVehicles" (#461 final review): a Why that moved to ANY other value passed the
	// old assertion, including a job the re-open had left Queued on nothing. Refused again, and named for the road.
	TestEqual(TEXT("and it was asked again: refused afresh, still Unserviceable"),
		static_cast<int32>(After->State), static_cast<int32>(EServiceJobState::Unserviceable));
	TestEqual(TEXT("for the road it has none of - NoRoad - not for the vehicle that now exists"),
		static_cast<int32>(After->Why), static_cast<int32>(EServiceRefusal::NoRoad));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFleetCatalogueDropsNoChassisTest, "AirportOps.Fleet.CatalogueDropsARowWithNoChassis",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFleetCatalogueDropsNoChassisTest::RunTest(const FString&)
{
	// THE JOIN'S REFUSAL (#430). A scenario row whose code Content builds no chassis for used to be offered, bought and
	// seeded, and then TypeFor gave it a zero chassis that VehicleFit::NoLargerThan passed on every stand, dispatched at
	// the default speed and drawn with the default mesh - and nothing warned. Now the resolve drops it with a Warning, and
	// every door behind it (the starter list, the fleet's Add, TypeFor) refuses the code by name.
	UJobBoard* Board = NewObject<UJobBoard>(GetTransientPackage());
	TMap<FName, FFuelVehicleSpec> Rows = GetDefault<UScenario>()->FuelVehicles;
	const FName Hovercraft(TEXT("HOVERCRAFT"));
	const FName Skateboard(TEXT("SKATEBOARD"));
	const FName Bowser(AirsideVehicleCodes::Fuel);
	Rows.Add(Hovercraft, FFuelVehicleSpec(500.0, 50.0, 1000.0, 10.0, INVTEXT("Hovercraft")));
	Rows.Add(Skateboard, FFuelVehicleSpec(500.0, 50.0, 1000.0, 10.0, INVTEXT("Skateboard")));

	// TWO WAYS TO HAVE NO CHASSIS, each its own branch of the join's check: Content builds nothing for the code (a None
	// vehicle - the hovercraft), or it answers a vehicle that names the code and has no axles (the skateboard, which the
	// resolver below hands back by hand: no Content resolver builds one today, and a zero wheelbase routes and fits as
	// nothing does).
	AddExpectedMessagePlain(TEXT("scenario vehicle row 'HOVERCRAFT' has no chassis"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedMessagePlain(TEXT("scenario vehicle row 'SKATEBOARD' has no chassis"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedMessagePlain(TEXT("starter fleet kind 'HOVERCRAFT' has no catalogue row"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	const int32 Kept = Board->Fleet().ResolveCatalogue(Rows, { Hovercraft, Bowser },
		[Skateboard](FName Code)
		{
			if (Code == Skateboard)
			{
				FVehicle Axleless;
				Axleless.TypeCode = Code;
				return Axleless;
			}
			return UAirsideSettings::ResolveVehicle(Code);
		});
	TestEqual(TEXT("every row with a chassis is kept, and only those"), Kept, Rows.Num() - 2);
	TestFalse(TEXT("the chassis-less row is not in the catalogue - so it is not for sale"), Board->GetCatalogue().Contains(Hovercraft));
	TestFalse(TEXT("nor is the row whose chassis names its code and has no axles"), Board->GetCatalogue().Contains(Skateboard));
	TestEqual(TEXT("the starter list keeps the kinds that exist, in its own order"), Board->StarterFleet, TArray<FName>{ Bowser });
	const FServiceVehicleType* Row = Board->GetCatalogue().Find(Bowser);
	if (!TestNotNull(TEXT("the bowser's row"), Row)) { return false; }
	TestTrue(TEXT("its chassis is Content's for its code"), Row->Vehicle.TypeCode == Bowser && Row->Vehicle.Chassis.HasAxles());
	TestEqual(TEXT("its tank is the scenario's"), Row->Capacity, Rows[Bowser].CapacityLitres);
	TestEqual(TEXT("its price is the scenario's"), Row->Price, Rows[Bowser].Price);
	TestEqual(TEXT("its resale is the scenario's one rule, asked at the join"), Row->ResaleValue, Rows[Bowser].ResaleValue());

	AddExpectedMessagePlain(TEXT("adding 'HOVERCRAFT' for depot"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	TestEqual(TEXT("the fleet's door refuses a kind with no row"),
		Board->Fleet().Add(Hovercraft, FleetTestDepotId(), EFleetOrigin::Bought, 0.0), 0);

	// ONCE PER CODE: a second ask says nothing new (the expected count is exact).
	AddExpectedMessagePlain(TEXT("vehicle kind HOVERCRAFT has no catalogue row"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	const FServiceVehicleType Unknown = Board->TypeFor(Hovercraft);
	TestTrue(TEXT("TypeFor answers a None row for it - which the bid skips - not a zero-size vehicle named HOVERCRAFT"),
		Unknown.TypeCode.IsNone() && !Unknown.Vehicle.Chassis.HasAxles());
	TestTrue(TEXT("and again, silently"), Board->TypeFor(Hovercraft).TypeCode.IsNone());
	return true;
}

#endif
