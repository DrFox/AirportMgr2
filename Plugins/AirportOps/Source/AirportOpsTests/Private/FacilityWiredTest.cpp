#include "CoreMinimal.h"
#include "Build/AnchorLink.h"
#include "Build/DepotKit.h"
#include "Content/AirportOpsSettings.h"
#include "Content/AirsideSettings.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/FacilityPurchases.h"
#include "Model/FuelSupply.h"
#include "Model/GroundTraffic.h"
#include "Model/JobBoard.h"
#include "Model/Ledger.h"
#include "Model/OpsDefinition.h"
#include "Model/OpsAlerts.h"
#include "Model/OpsEvents.h"
#include "Model/VehicleCodes.h"
#include "Model/RoadEntity.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadTraffic.h"
#include "Model/SimClock.h"
#include "OpsEventsTestListener.h"
#include "OpsSaveTestHelpers.h"
#include "Model/RoutePolicy.h"
#include "Model/RouteSearch.h"
#include "Present/AirsideBuildingsActor.h"
#include "Present/AirsideTraffic.h"
#include "Present/OpsRuntime.h"
#include "Present/PlotPresenter.h"
#include "Present/RoadEditFacade.h"
#include "Present/RoadEditHistory.h"
#include "Present/RoadNetworkActor.h"
#include "Testing/AirsideTestGraph.h"
#include "Testing/AirsideTestWorld.h"
#include "Tool/RoadEditTarget.h"

#if WITH_DEV_AUTOMATION_TESTS

// THE COMPOSITION TESTS FOR FACILITY PURCHASES (facility-upgrades spec §5): each fails if UOpsRuntime
// leaves a seam unwired - the bus's wake-up, the module hook, the undo checkpoint, the ceiling memo.

namespace
{
	/** FuelServiceWiredTest's LayFuelLine, prefixed for the unity build. */
	void FacilityWiredLine(URoadNetwork& Net, const FVector2D& From, const FVector2D& To, ETraversalClass Class,
		FGuidelineNodeId& OutA, FGuidelineNodeId& OutB)
	{
		OutA = Net.AddGuidelineNode(From);
		OutB = Net.AddGuidelineNode(To);
		FGuidelineEdge Edge;
		Edge.A = OutA;
		Edge.B = OutB;
		Edge.Control = (From + To) * 0.5;
		Edge.AllowedTraffic = FTrafficMask::Only(Class);
		Edge.AllowedTraffic.Add(ETraversalClass::Emergency);
		Edge.Direction = EGuidelineDir::Bidirectional;
		Edge.Width = 600.0;
		Edge.bDerived = true;
		Net.AddGuidelineEdge(MoveTemp(Edge));
	}

	/**
	 * THE SHED'S ROW OF A QUOTE, by kind. The quote holds one row per offer in map order (the tank joined the shed on
	 * 2026-10-02), so a test that read Modules[0] as "the shed" was reading whichever row the map listed first.
	 */
	const FModuleOfferQuote& FacilityWiredShedRow(const FFacilityQuote& Quote)
	{
		static const FModuleOfferQuote None;
		const FModuleOfferQuote* Row = Quote.Modules.FindByPredicate([](const FModuleOfferQuote& Each) { return Each.Module == EDepotModule::Shed; });
		return Row ? *Row : None;
	}

	struct FFacilityFuelField
	{
		FEntityInstanceId Stand;
		FEntityInstanceId Depot;
		FGuidelineNodeId TaxiSouth;
	};

	/**
	 * FuelServiceWiredTest's airport - a taxiway, a Code C stand, a service road with a spur to the stand's
	 * far edge - with the depot placed as the PLAYER's would be: its start kit (one shed, one bay) and NO
	 * starter trucks (R3). Plotless, so no plot solve is involved in this seam.
	 */
	FFacilityFuelField FacilityFuelField(URoadNetwork& Net)
	{
		FFacilityFuelField Out;
		constexpr double RoadY = -4000.0;
		FGuidelineNodeId TaxiNorth;
		FacilityWiredLine(Net, FVector2D(-10000.0, -10000.0), FVector2D(-10000.0, 10000.0), ETraversalClass::Aircraft, Out.TaxiSouth, TaxiNorth);

		UEntityDefinition* StandDef = UEntityDefinition::MakeStandTransient();
		Out.Stand = Net.PlaceEntity(StandDef, StandDef->Anchors, FVector2D(0.0, 0.0), 0.0, 3600.0, StandDef->PoseRole, StandDef->Trucks);

		double FarEdge = -TNumericLimits<double>::Max();
		for (const FVector2D& Corner : Net.GetEntity(Out.Stand)->Outline)
		{
			FarEdge = FMath::Max(FarEdge, Corner.X);
		}
		const double SpurX = FarEdge + 420.0;
		FGuidelineNodeId SpurFoot, SpurHead;
		FacilityWiredLine(Net, FVector2D(SpurX, RoadY), FVector2D(SpurX, 10000.0), ETraversalClass::GroundVehicle, SpurFoot, SpurHead);
		auto Join = [&Net](FGuidelineNodeId A, FGuidelineNodeId B)
		{
			FGuidelineEdge Edge;
			Edge.A = A;
			Edge.B = B;
			Edge.Control = (Net.GetGuidelineNode(A)->Position + Net.GetGuidelineNode(B)->Position) * 0.5;
			Edge.AllowedTraffic = FTrafficMask::Only(ETraversalClass::GroundVehicle);
			Edge.AllowedTraffic.Add(ETraversalClass::Emergency);
			Edge.Direction = EGuidelineDir::Bidirectional;
			Edge.Width = 600.0;
			Edge.bDerived = true;
			Net.AddGuidelineEdge(MoveTemp(Edge));
		};
		const FGuidelineNodeId RoadWest = Net.AddGuidelineNode(FVector2D(-20000.0, RoadY));
		const FGuidelineNodeId RoadEast = Net.AddGuidelineNode(FVector2D(20000.0, RoadY));
		Join(RoadWest, SpurFoot);
		Join(SpurFoot, RoadEast);

		UEntityDefinition* DepotDef = UEntityDefinition::MakeFuelDepotTransient();
		FEntityPlacement Placement;
		Placement.Definition = DepotDef;
		Placement.Anchors = DepotDef->Anchors;
		Placement.Position = FVector2D(12000.0, RoadY + 4000.0);
		Placement.Heading = UE_DOUBLE_PI * 0.5;
		Placement.PoseRole = DepotDef->PoseRole;
		Placement.Modules = { EDepotModule::Shed, EDepotModule::Tank, EDepotModule::Pump };
		Placement.Trucks = 0;
		Out.Depot = Net.PlaceEntity(Placement);
		FAnchorLink::Build(Net, UAirsideSettings::ResolveLargestServiceVehicle());
		return Out;
	}

	/** 50 x 24 m - Task 3's FacilityModuleWidePlot figure, which reserves a second shed. */
	TArray<FVector2D> FacilityWiredWidePlot()
	{
		return { FVector2D(0.0, 0.0), FVector2D(5000.0, 0.0), FVector2D(5000.0, 2400.0), FVector2D(0.0, 2400.0) };
	}

	/** An attached runtime, then the player's gesture: a plotted depot with its start kit, paid for. */
	FEntityInstanceId FacilityWiredDepot(FAirsideTestWorld& World, UOpsRuntime*& OutRuntime)
	{
		ARoadNetworkActor* Actor = World.Actor;
		Actor->PlaceNode(FVector2D(0.0, 40000.0));
		Actor->FuelDepotDefinition = UEntityDefinition::MakeFuelDepotTransient();
		OutRuntime = NewObject<UOpsRuntime>();
		OutRuntime->Attach(Actor);
		const TArray<FVector2D> Plot = FacilityWiredWidePlot();
		const int32 Index = Actor->PlaceEntityInPlot(Plot, Plot[0], Plot[1],
			{ EDepotModule::Shed, EDepotModule::Tank, EDepotModule::Pump }, EPlaceableEntity::FuelDepot);
		return Index != INDEX_NONE ? Actor->Network->EntityIdAt(Index) : FEntityInstanceId();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFacilityWakesBoardTest, "AirportOps.Present.Facility.PurchaseWakesTheBoard",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFacilityWakesBoardTest::RunTest(const FString&)
{
	// SPEC §5: a job waiting on an empty depot dispatches after buy + drain, with no tick polling. The seam
	// is WireBus's FleetChanged -> JobBoard pass; unwired, the job waits for an unrelated event.
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("an actor"), Actor)) { return false; }
	Actor->PlaceNode(FVector2D(0.0, 40000.0));
	if (!TestNotNull(TEXT("a network"), Actor->Network.Get())) { return false; }
	URoadNetwork& Net = *Actor->Network;
	const FFacilityFuelField Field = FacilityFuelField(Net);

	UOpsRuntime* Runtime = NewObject<UOpsRuntime>();
	Runtime->Attach(Actor);
	UJobBoard* Board = Runtime->GetJobBoard();

	const FRoutePlan Plan = TestGraph::Probe(Net, Field.TaxiSouth, Net.GetEntity(Field.Stand)->PoseNode, ETraversalClass::Aircraft);
	if (!TestTrue(TEXT("the aircraft routes to the stand"), Plan.IsValid())) { return false; }
	if (!TestTrue(TEXT("and dispatches"), Actor->DispatchAgent(Plan, UAirsideSettings::ResolveDefaultAirframe()))) { return false; }

	constexpr float Step = 1.0f / 30.0f;
	for (int32 Tick = 0; Tick < 20000; ++Tick)
	{
		Actor->Tick(Step);
		Runtime->Tick(Step);
		if (Board->GetJobs().Num() > 0 && Board->GetJobs()[0].State == EServiceJobState::Unserviceable) { break; }
	}
	if (!TestEqual(TEXT("parking made one job"), Board->GetJobs().Num(), 1)) { return false; }
	TestEqual(TEXT("refused: the depot has no vehicles (R3)"),
		static_cast<int32>(Board->GetJobs()[0].Why), static_cast<int32>(EServiceRefusal::NoVehicles));

	const int32 Quiet = Board->StepCountForTest();
	for (int32 Tick = 0; Tick < 60; ++Tick) { Actor->Tick(Step); Runtime->Tick(Step); }
	TestEqual(TEXT("a refused job is not polled - two quiet seconds run no step"), Board->StepCountForTest(), Quiet);

	const FName Kind = Board->VehiclesFor(EIcaoCode::C).TypeCode;
	const FPurchaseResult Bought = Runtime->BuyVehicle(Field.Depot, Kind);
	if (!TestTrue(TEXT("the depot's one bay takes a vehicle"), Bought.Succeeded())) { return false; }
	TestEqual(TEXT("nothing ran inside the command - the pass waits for the drain"), Board->StepCountForTest(), Quiet);

	Runtime->Tick(Step);
	TestTrue(TEXT("the drain ran the job board pass"), Board->StepCountForTest() > Quiet);
	const FServiceJob& Job = Board->GetJobs()[0];
	TestTrue(TEXT("the waiting job is now the bought vehicle's"),
		Job.VehicleId == Bought.VehicleId
		&& (Job.State == EServiceJobState::Queued || Job.State == EServiceJobState::Underway));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFacilityAttachTest, "AirportOps.Present.Facility.AttachCopiesTheOffers",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFacilityAttachTest::RunTest(const FString&)
{
	// THE SCENARIO'S CATALOGUE REACHES THE SHOP AT ATTACH (UScenario::ModuleOffers' own comment): unwired,
	// the shop holds no offers and the card has no Buy Shed row at all - a refusal with no reason on screen.
	UOpsRuntime* Unattached = NewObject<UOpsRuntime>();
	TestFalse(TEXT("unattached, the forwarder quotes no facility"), Unattached->QuoteFacility(FEntityInstanceId()).IsFacility());
	TestEqual(TEXT("and refuses a buy NotAFacility rather than crash"),
		static_cast<int32>(Unattached->BuyVehicle(FEntityInstanceId(), TEXT("FUEL")).Refusal), static_cast<int32>(EPurchaseRefusal::NotAFacility));

	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("an actor"), Actor)) { return false; }
	Actor->PlaceNode(FVector2D(0.0, 40000.0));
	if (!TestNotNull(TEXT("a network"), Actor->Network.Get())) { return false; }
	const FFacilityFuelField Field = FacilityFuelField(*Actor->Network);

	UOpsRuntime* Runtime = NewObject<UOpsRuntime>();
	Runtime->Attach(Actor);
	const UScenario* Scenario = UAirportOpsSettings::ResolveDefaultScenario(*Runtime->GetCatalog());
	if (!TestNotNull(TEXT("a scenario"), Scenario)) { return false; }
	TestEqual(TEXT("the shop holds the scenario's module offers"),
		Runtime->GetFacilityPurchases()->ModuleOffers.Num(), Scenario->ModuleOffers.Num());
	const FFacilityQuote Q = Runtime->QuoteFacility(Field.Depot);
	TestTrue(TEXT("the depot quotes as a facility through the forwarder"), Q.IsFacility());
	TestEqual(TEXT("with one module row per offer"), Q.Modules.Num(), Scenario->ModuleOffers.Num());
	TestTrue(TEXT("the shed among them"), Q.Modules.ContainsByPredicate([](const FModuleOfferQuote& Row) { return Row.Module == EDepotModule::Shed; }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFacilityShedRelightsTest, "AirportOps.Present.Facility.ShedPurchaseRelightsASlot",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFacilityShedRelightsTest::RunTest(const FString&)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor"), TestWorld.Actor)) { return false; }
	UOpsRuntime* Runtime = nullptr;
	const FEntityInstanceId Depot = FacilityWiredDepot(TestWorld, Runtime);
	if (!TestTrue(TEXT("setup: the depot is placed"), Depot.IsSet())) { return false; }

	const FFacilityQuote Before = Runtime->QuoteFacility(Depot);
	if (!TestTrue(TEXT("setup: the runtime's ceiling hook sees a second shed slot"),
		FacilityWiredShedRow(Before).Reserved >= 2)) { return false; }
	const UPlotPresenter* Plots = TestWorld.Buildings->GetPlotPresenter();
	const int32 Lit = Plots->GetModuleCount();

	const FPurchaseResult Shed = Runtime->BuyModule(Depot, EDepotModule::Shed);
	TestTrue(TEXT("the shed is bought through the runtime's hooks"), Shed.Succeeded());
	TestEqual(TEXT("and one more bay is lit - the module hook reached the facade and the rebuild"), Plots->GetModuleCount(), Lit + 1);
	TestEqual(TEXT("the quote now owns two"), FacilityWiredShedRow(Runtime->QuoteFacility(Depot)).Owned, 2);
	TestEqual(TEXT("and has two bays"), Runtime->QuoteFacility(Depot).Bays, 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFacilityUndoCheckpointTest, "AirportOps.Present.Facility.ShedClearsUndoVehicleDoesNot",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFacilityUndoCheckpointTest::RunTest(const FString&)
{
	// R8: a shed is a checkpoint; a vehicle trade never touches undo.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor"), TestWorld.Actor)) { return false; }
	UOpsRuntime* Runtime = nullptr;
	const FEntityInstanceId Depot = FacilityWiredDepot(TestWorld, Runtime);
	URoadEditFacade* Facade = TestWorld.Actor->GetEditFacade();
	if (!TestTrue(TEXT("setup: placing the depot is undoable"), Depot.IsSet() && Facade->CanUndo())) { return false; }

	TestTrue(TEXT("a vehicle is bought"), Runtime->BuyVehicle(Depot, TEXT("FUEL")).Succeeded());
	TestTrue(TEXT("and the build history is untouched"), Facade->CanUndo());
	TestTrue(TEXT("a shed is bought"), Runtime->BuyModule(Depot, EDepotModule::Shed).Succeeded());
	TestFalse(TEXT("and the history is gone - an undo would drop the shed and keep the money"), Facade->CanUndo());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFacilityQuoteMemoTest, "AirportOps.Present.Facility.QuoteSolvesOncePerDepot",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFacilityQuoteMemoTest::RunTest(const FString&)
{
	// REVIEW FOCUS 4: the inspector asks every tick; the ceiling is a plot solve. Asked once per depot.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor"), TestWorld.Actor)) { return false; }
	UOpsRuntime* Runtime = nullptr;
	const FEntityInstanceId Depot = FacilityWiredDepot(TestWorld, Runtime);
	if (!TestTrue(TEXT("setup: the depot is placed"), Depot.IsSet())) { return false; }
	const int32 Before = Runtime->ReservationSolvesForTest();
	Runtime->QuoteFacility(Depot);
	Runtime->QuoteFacility(Depot);
	Runtime->BuyModule(Depot, EDepotModule::Shed);
	Runtime->QuoteFacility(Depot);
	TestEqual(TEXT("three quotes and a purchase solve the plot once - modules do not change what it holds"),
		Runtime->ReservationSolvesForTest(), Before + 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFacilitySellForwardsTest, "AirportOps.Present.Facility.SellForwarderRemovesAndCredits",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFacilitySellForwardsTest::RunTest(const FString&)
{
	// THE SELL FORWARDER REACHES THE RUNTIME'S OWN BOARD AND LEDGER: a shop wired to a second board or
	// ledger would refuse, or remove the vehicle and credit nothing the player can see.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor"), TestWorld.Actor)) { return false; }
	UOpsRuntime* Runtime = nullptr;
	const FEntityInstanceId Depot = FacilityWiredDepot(TestWorld, Runtime);
	if (!TestTrue(TEXT("setup: the depot is placed"), Depot.IsSet())) { return false; }
	const FPurchaseResult Bought = Runtime->BuyVehicle(Depot, TEXT("FUEL"));
	if (!TestTrue(TEXT("setup: a vehicle is bought"), Bought.Succeeded())) { return false; }
	UJobBoard* Board = Runtime->GetJobBoard();
	const int32 Fleet = Board->VehiclesAt(Depot);
	const double Balance = Runtime->GetLedger()->Balance();
	// THE SCENARIO'S OWN ROW, not the catalogue's resolved ResaleValue: the credit is checked against the authored rule.
	// The RUNTIME's scenario - the one Attach resolved the catalogue from - not the class default.
	const UScenario* Scenario = UAirportOpsSettings::ResolveDefaultScenario(*Runtime->GetCatalog());
	if (!TestNotNull(TEXT("the runtime's scenario"), Scenario)) { return false; }
	const FFuelVehicleSpec Spec = Scenario->FuelVehicles.FindChecked(TEXT("FUEL"));

	const FPurchaseResult Sold = Runtime->SellVehicle(Bought.VehicleId);
	TestTrue(TEXT("the idle vehicle sells through the forwarder"), Sold.Succeeded());
	TestEqual(TEXT("and leaves the runtime's board"), Board->VehiclesAt(Depot), Fleet - 1);
	TestEqual(TEXT("credited at the resale fraction"), Sold.Amount, Spec.Price * Spec.ResaleFraction);
	TestEqual(TEXT("into the runtime's ledger"), Runtime->GetLedger()->Balance(), Balance + Sold.Amount);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFacilityUpgradeWakesTest, "AirportOps.Present.Facility.UpgradeEventRunsTheBoardPass",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFacilityUpgradeWakesTest::RunTest(const FString&)
{
	// WireBus's FacilityUpgraded -> JobBoard subscription, ALONE: the event is published by hand after the
	// board has gone quiet, because a real BuyModule also rebuilds the network, and FNetworkChangedEvent
	// would run the pass whether or not this subscription existed.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor"), TestWorld.Actor)) { return false; }
	UOpsRuntime* Runtime = nullptr;
	const FEntityInstanceId Depot = FacilityWiredDepot(TestWorld, Runtime);
	if (!TestTrue(TEXT("setup: the depot is placed"), Depot.IsSet())) { return false; }
	UJobBoard* Board = Runtime->GetJobBoard();
	constexpr float Step = 1.0f / 30.0f;
	for (int32 Tick = 0; Tick < 10; ++Tick) { TestWorld.Actor->Tick(Step); Runtime->Tick(Step); }
	const int32 Quiet = Board->StepCountForTest();
	for (int32 Tick = 0; Tick < 30; ++Tick) { TestWorld.Actor->Tick(Step); Runtime->Tick(Step); }
	if (!TestEqual(TEXT("setup: the board is quiet"), Board->StepCountForTest(), Quiet)) { return false; }

	Runtime->GetBus().Publish(FFacilityUpgradedEvent{ Depot.Index, EDepotModule::Shed, 0.0 });
	Runtime->Tick(Step);
	TestTrue(TEXT("the drain ran the job board pass - a new bay can serve a waiting job"), Board->StepCountForTest() > Quiet);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFacilityDragRefusesTest, "AirportOps.Present.Facility.ShedRefusedDuringADrag",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFacilityDragRefusesTest::RunTest(const FString&)
{
	// A PURCHASE WITH A DRAG OPEN: the facade's ClearHistory would drop the drag's pending snapshot, and the
	// drag's EndInteractiveEdit would then land an edit with no undo step. Refused, nothing charged.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor"), TestWorld.Actor)) { return false; }
	UOpsRuntime* Runtime = nullptr;
	const FEntityInstanceId Depot = FacilityWiredDepot(TestWorld, Runtime);
	if (!TestTrue(TEXT("setup: the depot is placed"), Depot.IsSet())) { return false; }
	URoadEditFacade* Facade = TestWorld.Actor->GetEditFacade();
	const double Balance = Runtime->GetLedger()->Balance();

	Facade->BeginInteractiveEdit(TEXT("drag"));
	const FPurchaseResult Shed = Runtime->BuyModule(Depot, EDepotModule::Shed);
	TestEqual(TEXT("the shed is refused while the drag is open"),
		static_cast<int32>(Shed.Refusal), static_cast<int32>(EPurchaseRefusal::NotAFacility));
	TestEqual(TEXT("and nothing is charged"), Runtime->GetLedger()->Balance(), Balance);
	TestEqual(TEXT("nor built"), FacilityWiredShedRow(Runtime->QuoteFacility(Depot)).Owned, 1);
	Facade->EndInteractiveEdit(false);
	TestTrue(TEXT("the depot's placement is still undoable - the history was not cleared"), Facade->CanUndo());
	TestTrue(TEXT("with the drag closed, the same shed is bought"), Runtime->BuyModule(Depot, EDepotModule::Shed).Succeeded());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFacilityMemoInvalidatesTest, "AirportOps.Present.Facility.NewNetworkResolvesTheCeiling",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFacilityMemoInvalidatesTest::RunTest(const FString&)
{
	// THE MEMO'S ONE INVALIDATION: ClearNetwork replaces the network object, and the first depot on the new
	// one takes the SAME handle as the old one did. A memo keyed on the handle alone would keep quoting the
	// wide plot's second shed slot for a narrow plot that holds one.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor"), TestWorld.Actor)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	UOpsRuntime* Runtime = nullptr;
	const FEntityInstanceId Wide = FacilityWiredDepot(TestWorld, Runtime);
	if (!TestTrue(TEXT("setup: the wide depot is placed"), Wide.IsSet())) { return false; }
	const int32 WideSheds = FacilityWiredShedRow(Runtime->QuoteFacility(Wide)).Reserved;
	if (!TestTrue(TEXT("setup: the wide plot reserves a second shed"), WideSheds >= 2)) { return false; }
	const int32 Solves = Runtime->ReservationSolvesForTest();

	Actor->ClearNetwork();
	Actor->PlaceNode(FVector2D(0.0, 40000.0));
	const TArray<FVector2D> Narrow = { FVector2D(0.0, 0.0), FVector2D(2000.0, 0.0), FVector2D(2000.0, 2400.0), FVector2D(0.0, 2400.0) };
	const int32 Index = Actor->PlaceEntityInPlot(Narrow, Narrow[0], Narrow[1],
		{ EDepotModule::Shed, EDepotModule::Tank, EDepotModule::Pump }, EPlaceableEntity::FuelDepot);
	if (!TestTrue(TEXT("setup: the narrow depot is placed"), Index != INDEX_NONE)) { return false; }
	const FEntityInstanceId NarrowId = Actor->Network->EntityIdAt(Index);
	if (!TestTrue(TEXT("setup: it reuses the old handle - the case a handle-only memo gets wrong"), NarrowId == Wide)) { return false; }

	const FFacilityQuote Q = Runtime->QuoteFacility(NarrowId);
	TestTrue(TEXT("the narrow plot's ceiling is solved afresh"), Runtime->ReservationSolvesForTest() > Solves);
	TestTrue(TEXT("and it holds fewer sheds than the wide one"), FacilityWiredShedRow(Q).Reserved < WideSheds);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFacilityRollbackResolvesTest, "AirportOps.Present.Facility.RollbackResolvesTheCeiling",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFacilityRollbackResolvesTest::RunTest(const FString&)
{
	// AN IN-PLACE RESTORE KEEPS THE NETWORK OBJECT (issue #437, and #460's review): a rolled-back edit puts the
	// old contents back into the SAME network, so a memo keyed on the pointer alone would keep quoting whatever
	// was solved in the failed edit. Two wide depots; the second's plot is narrowed inside a scope (a stand-in
	// for any edit that re-plots), quoted there - the memo now holds the NARROW ceiling - and the scope rolls back.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor"), TestWorld.Actor)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	UOpsRuntime* Runtime = nullptr;
	const FEntityInstanceId First = FacilityWiredDepot(TestWorld, Runtime);
	if (!TestTrue(TEXT("setup: the first depot is placed"), First.IsSet())) { return false; }
	TArray<FVector2D> Second = FacilityWiredWidePlot();
	for (FVector2D& Corner : Second) { Corner.Y += 20000.0; }
	const int32 SecondIndex = Actor->PlaceEntityInPlot(Second, Second[0], Second[1],
		{ EDepotModule::Shed, EDepotModule::Tank, EDepotModule::Pump }, EPlaceableEntity::FuelDepot);
	if (!TestTrue(TEXT("setup: the second depot is placed"), SecondIndex != INDEX_NONE)) { return false; }
	const FEntityInstanceId Depot = Actor->Network->EntityIdAt(SecondIndex);

	const int32 WideSheds = FacilityWiredShedRow(Runtime->QuoteFacility(First)).Reserved;
	if (!TestTrue(TEXT("setup: a wide plot reserves a second shed"), WideSheds >= 2)) { return false; }

	TArray<FVector2D> Narrow = Second;
	Narrow[1].X = Second[0].X + 2000.0;
	Narrow[2].X = Second[0].X + 2000.0;
	int32 NarrowSheds = INDEX_NONE;
	{
		FRoadEditScope Edit(nullptr, Actor->Network, TEXT("re-plot"));
		if (!TestTrue(TEXT("setup: the plot is narrowed inside the scope"),
			FRoadNetworkTestAccess(*Actor->Network).SetEntityOutlineForTest(Depot, Narrow))) { return false; }
		NarrowSheds = FacilityWiredShedRow(Runtime->QuoteFacility(Depot)).Reserved;
		TestTrue(TEXT("control: the narrow plot holds fewer sheds - the memo now holds THAT"), NarrowSheds < WideSheds);
		const int32 Solves = Runtime->ReservationSolvesForTest();

		URoadNetwork* const LiveBefore = Actor->Network;
		TestTrue(TEXT("the scope rolls back"), Edit.Rollback());
		TestTrue(TEXT("in place: the network object is the one the memo was filled against"), Actor->Network == LiveBefore);

		const int32 After = FacilityWiredShedRow(Runtime->QuoteFacility(Depot)).Reserved;
		TestTrue(TEXT("the ceiling was solved afresh - a pointer-keyed memo would answer from the failed edit"),
			Runtime->ReservationSolvesForTest() > Solves);
		TestEqual(TEXT("and it is the restored (wide) plot's, not the narrowed one's"), After, WideSheds);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFacilityCapabilitySeamTest, "AirportOps.Present.Facility.CapabilityReadsTheRuntimesPlotSolve",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFacilityCapabilitySeamTest::RunTest(const FString&)
{
	// THE COMPOSITION FOR #443's CAPABILITY: the shop and the board read what a depot's SEATED modules give it through the
	// runtime's one plot solve. A depot placed through the player's gesture with far more pumps and sheds than its plot can
	// hold - the presenter drops the rest - must quote the bays the plot SEATS and count the pumps it SEATS. Unwired (the
	// hooks left null), each falls back to the owned list and reports all fifty.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor"), TestWorld.Actor)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	Actor->PlaceNode(FVector2D(0.0, 40000.0));
	Actor->FuelDepotDefinition = UEntityDefinition::MakeFuelDepotTransient();
	UOpsRuntime* Runtime = NewObject<UOpsRuntime>();
	Runtime->Attach(Actor);
	TArray<EDepotModule> Owned;
	for (int32 I = 0; I < 50; ++I) { Owned.Add(EDepotModule::Shed); Owned.Add(EDepotModule::Pump); }
	// THROUGH THE MODEL since #266: the gesture (PlaceEntityInPlot) now refuses a plot that cannot seat what it starts with,
	// so a depot owning more than its plot holds is what only an old save or a content change makes. No Tick below, so
	// the runtime's repair pass has not run and the excess is still owned when the seat is read.
	const TArray<FVector2D> Plot = FacilityWiredWidePlot();
	FEntityPlacement Placement;
	Placement.Definition = Actor->FuelDepotDefinition;
	Placement.Anchors = Actor->FuelDepotDefinition->Anchors;
	Placement.Position = (Plot[0] + Plot[1]) * 0.5;
	Placement.Heading = UE_DOUBLE_HALF_PI;
	Placement.PoseRole = EServiceRole::Fuel;
	Placement.Outline = Plot;
	Placement.FrontageEdge = 0;   // the frontage is edge 0, Position its midpoint - see FEntityInstance::FrontageEdge
	Placement.Modules = Owned;
	const FEntityInstanceId Depot = Actor->Network->PlaceEntity(Placement);
	if (!TestTrue(TEXT("setup: the depot is placed"), Depot.IsSet())) { return false; }
	Actor->RebuildMesh();
	const FEntityInstance& Placed = *Actor->Network->GetEntity(Depot);

	// THE ORACLE IS THE PLOT SOLVE ITSELF, asked here directly: what the presenter draws from.
	const TArray<PlotYard::FKitSpec> Specs = Actor->ResolveDepotKits();
	const TOptional<PlotYard::FReservation> Reserved = DepotKit::ReservationOf(Placed, Specs);
	if (!TestTrue(TEXT("setup: the plot solves"), Reserved.IsSet())) { return false; }
	const int32 SeatedSheds = Reserved->CeilingFor(static_cast<int32>(EDepotModule::Shed));
	const int32 SeatedPumps = Reserved->CeilingFor(static_cast<int32>(EDepotModule::Pump));
	if (!TestTrue(TEXT("setup: the plot holds fewer than fifty of each"), SeatedSheds < 50 && SeatedPumps < 50 && SeatedSheds > 0 && SeatedPumps > 0)) { return false; }
	const FFacilityQuote Quote = Runtime->QuoteFacility(Depot);
	TestEqual(TEXT("the card quotes the bays the plot seats - one per seated shed"), Quote.Bays, SeatedSheds);
	TestEqual(TEXT("and the board counts the pumps it seats, not the fifty owned"),
		Runtime->GetJobBoard()->PumpsAt(Depot, Placed), SeatedPumps);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFacilityDetachClearsHooksTest, "AirportOps.Present.Facility.DetachClearsTheHooks",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFacilityDetachClearsHooksTest::RunTest(const FString&)
{
	// THE HOOKS GO WITH THE ACTOR: after Attach(nullptr) (Detach, then nothing), the shop asked directly about
	// the old actor's depot must refuse the module - a hook left set would build into a field nobody drives.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor"), TestWorld.Actor)) { return false; }
	UOpsRuntime* Runtime = nullptr;
	const FEntityInstanceId Depot = FacilityWiredDepot(TestWorld, Runtime);
	if (!TestTrue(TEXT("setup: the depot is placed"), Depot.IsSet())) { return false; }
	UFacilityPurchases* Shop = Runtime->GetFacilityPurchases();
	TestTrue(TEXT("setup: the board reads the same ceiling, copied from the shop's at attach (#443)"),
		static_cast<bool>(Runtime->GetJobBoard()->ModuleCeilingOf));

	Runtime->Attach(nullptr);
	TestFalse(TEXT("the board's ceiling is cleared with the shop's - a hook left set would seat modules against a dead plot"),
		static_cast<bool>(Runtime->GetJobBoard()->ModuleCeilingOf));
	TestFalse(TEXT("the ceiling hook is cleared"), static_cast<bool>(Shop->ReservedSlotsOf));
	TestFalse(TEXT("the module hook is cleared"), static_cast<bool>(Shop->ApplyModulePurchase));
	TestFalse(TEXT("and the repair's removal hook (#266) - left set, it would remove modules from a field nobody drives"),
		static_cast<bool>(Shop->ApplyModuleRemoval));
	const FFacilityQuote Q = Shop->Quote(*TestWorld.Actor->Network, Depot);
	TestEqual(TEXT("one row per offer survives the detach"), Q.Modules.Num(), Runtime->GetFacilityPurchases()->ModuleOffers.Num());
	TestTrue(TEXT("every module row refuses NotAFacility"),
		Q.Modules.Num() > 0 && !Q.Modules.ContainsByPredicate([](const FModuleOfferQuote& Row) { return Row.Refusal != EPurchaseRefusal::NotAFacility; }));
	return true;
}

namespace
{
	/** LogAirportOps at Warning, verbatim - the repair's line (#266). Unbuffered for FLogLineSpy's reason (issue #216). */
	struct FRepairWarningSpy : public FOutputDevice
	{
		TArray<FString> Lines;
		virtual bool CanBeUsedOnMultipleThreads() const override { return true; }
		virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& InCategory) override
		{
			if (InCategory == FName(TEXT("LogAirportOps")) && Verbosity == ELogVerbosity::Warning)
			{
				Lines.Add(FString(V));
			}
		}
	};

	/** Owned modules of Kind on Depot, read off the model. */
	int32 FacilityWiredOwned(const ARoadNetworkActor& Actor, FEntityInstanceId Depot, EDepotModule Kind)
	{
		const FEntityInstance* Entity = Actor.Network->GetEntity(Depot);
		int32 Count = 0;
		for (const EDepotModule Each : Entity != nullptr ? Entity->Modules : TArray<EDepotModule>()) { Count += Each == Kind ? 1 : 0; }
		return Count;
	}

	/** Refund lines on the ledger. */
	int32 FacilityWiredRefunds(const ULedger& Ledger)
	{
		return Ledger.Entries().FilterByPredicate([](const FLedgerEntry& E) { return E.Category == ELedgerCategory::Refund; }).Num();
	}
}

/**
 * #266 ITEM 4: THE PURCHASE CEILING IS THE PRESENTER'S SEAT, FOR EVERY MODULE KIND. The shop refuses a module once owned
 * reaches ReservedSlotsOf (the runtime's plot solve); the presenter stands min(owned, ceiling) of each. If the two ever
 * read different plots, a Buy would light for a module the presenter then drops - or grey one it would stand. Fifty of ONE
 * kind per depot, so the presenter's module count is that kind's seat and its drop count the rest.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFacilityCeilingIsTheSeatTest, "AirportOps.Present.Facility.PurchaseCeilingIsThePresentersSeatForEveryKind",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFacilityCeilingIsTheSeatTest::RunTest(const FString&)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor"), TestWorld.Actor)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	Actor->FuelDepotDefinition = UEntityDefinition::MakeFuelDepotTransient();
	UOpsRuntime* Runtime = NewObject<UOpsRuntime>();
	Runtime->Attach(Actor);
	const UFacilityPurchases* Shop = Runtime->GetFacilityPurchases();
	if (!TestTrue(TEXT("setup: the runtime wired the shop's ceiling hook"), static_cast<bool>(Shop->ReservedSlotsOf))) { return false; }
	const UPlotPresenter* Plots = TestWorld.Buildings->GetPlotPresenter();
	const TArray<FVector2D> Plot = FacilityWiredWidePlot();
	for (int32 Kind = 0; Kind < FDepotCapability::KindCount; ++Kind)
	{
		const EDepotModule Module = static_cast<EDepotModule>(Kind);
		const FString Name = UEnum::GetValueAsString(Module);
		Actor->ClearNetwork();
		FEntityPlacement Placement;
		Placement.Definition = Actor->FuelDepotDefinition;
		Placement.Anchors = Actor->FuelDepotDefinition->Anchors;
		Placement.Position = (Plot[0] + Plot[1]) * 0.5;
		Placement.Heading = UE_DOUBLE_HALF_PI;
		Placement.PoseRole = EServiceRole::Fuel;
		Placement.Outline = Plot;
		Placement.FrontageEdge = 0;   // the frontage is edge 0, Position its midpoint - see FEntityInstance::FrontageEdge
		for (int32 I = 0; I < 50; ++I) { Placement.Modules.Add(Module); }
		const FEntityInstanceId Depot = Actor->Network->PlaceEntity(Placement);
		Actor->RebuildMesh();
		const FEntityInstance* Placed = Actor->Network->GetEntity(Depot);
		if (!TestNotNull(*FString::Printf(TEXT("fifty %s: the depot is placed"), *Name), Placed)) { return false; }
		const int32 Ceiling = Shop->ReservedSlotsOf(Depot, *Placed, Module);
		if (!TestTrue(*FString::Printf(TEXT("fifty %s: the plot holds some and fewer than fifty - or this proves nothing"), *Name),
			Ceiling > 0 && Ceiling < 50)) { return false; }
		TestEqual(*FString::Printf(TEXT("fifty %s: the purchase ceiling is exactly what the presenter stands"), *Name), Plots->GetModuleCount(), Ceiling);
		TestEqual(*FString::Printf(TEXT("fifty %s: and the rest is what it drops"), *Name), Plots->GetDroppedCount(), 50 - Ceiling);
	}

	// A PLOTLESS DEPOT HAS NO CEILING TO BUY INTO: the runtime's hook answers 0 for no plot, so nothing can be bought into one
	// - which is why an unplotted depot WITH modules has no production path (FDepotCapability's legacy note).
	Actor->ClearNetwork();
	UEntityDefinition* Plain = Actor->FuelDepotDefinition;
	const FEntityInstanceId Plotless = Actor->Network->PlaceEntity(Plain, Plain->Anchors, FVector2D(30000.0, 0.0), 0.0, 0.0, EServiceRole::Fuel, 0);
	TestEqual(TEXT("a plotless depot's shed is refused: no plot, no slot"),
		static_cast<int32>(Runtime->BuyModule(Plotless, EDepotModule::Shed).Refusal), static_cast<int32>(EPurchaseRefusal::NoSlotReserved));
	return true;
}

/**
 * #266 THE REPAIR, THROUGH THE RUNTIME, ON A NETWORK CHANGE (owner 2026-09-30, option b): a depot whose plot SHRANK under
 * it - a stand-in for a kit, layout or frontage-recovery change - owns a shed the plot no longer seats. The next network
 * change runs the ModuleRepair pass: the shed leaves through the facade's door (the presenter drops nothing, undo holds
 * nothing), its price comes back as a Refund line, the log names the depot and the modules, and the player gets a Warning
 * toast. Unwired - no pass, no hook, no subscriber - the shed stays owned, unplaced and charged upkeep.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFacilityRepairWiredTest, "AirportOps.Present.Facility.RepairRemovesAndRefundsUnseated",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFacilityRepairWiredTest::RunTest(const FString&)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor"), TestWorld.Actor)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	UOpsRuntime* Runtime = nullptr;
	const FEntityInstanceId Depot = FacilityWiredDepot(TestWorld, Runtime);
	if (!TestTrue(TEXT("setup: the depot is placed"), Depot.IsSet())) { return false; }
	if (!TestTrue(TEXT("setup: a second shed is bought"), Runtime->BuyModule(Depot, EDepotModule::Shed).Succeeded())) { return false; }
	constexpr float Step = 1.0f / 30.0f;
	Runtime->Tick(Step);
	ULedger* Ledger = Runtime->GetLedger();
	if (!TestEqual(TEXT("setup: the attach's catch-up finds nothing to repair on a depot that seats what it owns"), FacilityWiredRefunds(*Ledger), 0)) { return false; }
	UOpsEventsTestListener* Listener = NewObject<UOpsEventsTestListener>();
	Runtime->GetEvents()->OnPurchase.AddDynamic(Listener, &UOpsEventsTestListener::OnPurchase);

	// THE PLOT SHRINKS UNDER THE DEPOT: 20 m of frontage centred on the same gate, so the frontage recovered from the pose
	// is still the road edge and only the room changes.
	const FVector2D Gate = Actor->Network->GetEntity(Depot)->Position;
	const TArray<FVector2D> Narrow = { Gate + FVector2D(-1000.0, 0.0), Gate + FVector2D(1000.0, 0.0),
		Gate + FVector2D(1000.0, 2400.0), Gate + FVector2D(-1000.0, 2400.0) };
	if (!TestTrue(TEXT("setup: the plot is narrowed"), FRoadNetworkTestAccess(*Actor->Network).SetEntityOutlineForTest(Depot, Narrow))) { return false; }

	// A NETWORK CHANGE - the player lays a road elsewhere - is what the pass listens for. It also moves the edit revision the
	// runtime's ceiling memo is keyed on, which the test-only outline write above does not (no production path re-plots).
	const uint32 Revision = Actor->Network->GetGuidelineRevision();
	const int32 A = Actor->PlaceNode(FVector2D(-30000.0, 30000.0));
	const int32 B = Actor->PlaceNode(FVector2D(-10000.0, 30000.0));
	static_cast<IRoadEditTarget*>(Actor)->ConnectNodes(A, B, ERoadKind::ServiceRoad, INDEX_NONE);
	if (!TestTrue(TEXT("setup: laying a road moved the guideline revision - the change the runtime publishes"),
		Actor->Network->GetGuidelineRevision() != Revision)) { return false; }
	const int32 Seats = Runtime->GetFacilityPurchases()->ReservedSlotsOf(Depot, *Actor->Network->GetEntity(Depot), EDepotModule::Shed);
	if (!TestTrue(FString::Printf(TEXT("setup: the narrow plot seats fewer than the two sheds owned (%d)"), Seats), Seats < 2)) { return false; }
	const UPlotPresenter* Plots = TestWorld.Buildings->GetPlotPresenter();
	if (!TestEqual(TEXT("setup: before the pass, the presenter drops the shed the plot cannot seat"), Plots->GetDroppedCount(), 2 - Seats)) { return false; }
	const double Balance = Ledger->Balance();
	// THE SHOP'S OWN SHED OFFER, as Attach copied it from the scenario - the price the refund is made at, read rather than
	// retyped because the figures are retuned (x0.4 on 2026-10-02); ScenarioPricesTheCatalogue pins them.
	const FModuleOffer& Shed = Runtime->GetFacilityPurchases()->ModuleOffers.FindChecked(EDepotModule::Shed);

	FRepairWarningSpy Spy;
	GLog->AddOutputDevice(&Spy);
	Runtime->Tick(Step);
	GLog->RemoveOutputDevice(&Spy);

	TestEqual(TEXT("the pass removed the unseated sheds: owned == placed"), FacilityWiredOwned(*Actor, Depot, EDepotModule::Shed), Seats);
	TestEqual(TEXT("through the facade's door - the presenter was rebuilt and drops nothing"), Plots->GetDroppedCount(), 0);
	TestFalse(TEXT("and the repair left nothing to undo"), Actor->GetEditFacade()->CanUndo());
	TestEqual(TEXT("refunded at the shed's price"), Ledger->Balance(), Balance + (2 - Seats) * Shed.Price, 1e-6);
	TestEqual(TEXT("on one Refund line"), FacilityWiredRefunds(*Ledger), 1);
	const FString Expected = FString::Printf(TEXT("depot %d at"), Depot.Index);
	TestTrue(FString::Printf(TEXT("the log names the depot and the modules (%s)"), *FString::Join(Spy.Lines, TEXT(" | "))),
		Spy.Lines.ContainsByPredicate([&Expected](const FString& L) { return L.Contains(TEXT("Repair: ")) && L.Contains(Expected) && L.Contains(TEXT("owned 2 Shed")); }));
	// THE TOAST'S FACTS (#445 item 7: the words and the Warning are the widget's, by the kind - AirportMgr.UI.ToastsWordSavesAndPurchases).
	TestTrue(FString::Printf(TEXT("the player is toasted the refund (%s)"), *FString::Join(Listener->Seen, TEXT(" | "))),
		Listener->CountOf(TEXT("buy:ModulesRefunded")) == 1);
	if (const FOpsPurchase* Refund = Listener->Purchases.FindByPredicate([](const FOpsPurchase& P) { return P.Kind == EOpsPurchaseKind::ModulesRefunded; }))
	{
		TestEqual(TEXT("counting the sheds removed"), Refund->Count, 2 - Seats);
		TestEqual(TEXT("with the refund as posted"), Refund->Amount, (2 - Seats) * Shed.Price, 1e-6);
	}
	// The depot's one tank (FacilityWiredDepot's kit) stands and is charged too, now that the tank has an offer.
	const FModuleOffer& Tank = Runtime->GetFacilityPurchases()->ModuleOffers.FindChecked(EDepotModule::Tank);
	TestEqual(TEXT("upkeep charges only the standing sheds, and the tank"), Runtime->GetFacilityPurchases()->DailyUpkeep(*Actor->Network).Modules,
		Seats * Shed.UpkeepPerDay + Tank.UpkeepPerDay, 1e-9);

	Runtime->Tick(Step);
	TestEqual(TEXT("once repaired, the next frame repairs nothing"), FacilityWiredRefunds(*Ledger), 1);
	return true;
}

/**
 * #266 THE REPAIR ON ATTACH AND ON LOAD: a depot owning more sheds than its plot seats - the state an old save or a kit
 * change leaves - is repaired on the first frame after the runtime attaches, and again on the first frame after a LOAD
 * that brings the over-owned depot back. Both ride the catch-up MarkAllDirty, which is what makes a load's repair land
 * AFTER the ledger is restored: run inside the restore, the refund would be overwritten by the saved balance.
 *
 * THE SAVE IS ONE FROM BEFORE THE REPAIR EXISTED: SaveToSlot drains the bus first, so a save made by this build is always
 * repaired - a property worth having, and the reason the removal hook is unset while saving here, as a build without #266
 * had none. The runtime is then attached afresh, as a new session is.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFacilityRepairAfterLoadTest, "AirportOps.Present.Facility.RepairRunsAfterALoad",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFacilityRepairAfterLoadTest::RunTest(const FString&)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor"), TestWorld.Actor)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	Actor->PlaceNode(FVector2D(0.0, 40000.0));
	Actor->FuelDepotDefinition = UEntityDefinition::MakeFuelDepotTransient();
	const TArray<FVector2D> Plot = FacilityWiredWidePlot();
	const int32 Index = Actor->PlaceEntityInPlot(Plot, Plot[0], Plot[1], DepotKit::StarterModules(), EPlaceableEntity::FuelDepot);
	if (!TestTrue(TEXT("setup: the depot is placed"), Index != INDEX_NONE)) { return false; }
	const FEntityInstanceId Depot = Actor->Network->EntityIdAt(Index);
	for (int32 I = 0; I < 20; ++I) { Actor->Network->AddEntityModule(Depot, EDepotModule::Shed); }
	Actor->RebuildMesh();

	UOpsRuntime* Runtime = NewObject<UOpsRuntime>();
	Runtime->Attach(Actor);
	const int32 Seats = Runtime->GetFacilityPurchases()->ReservedSlotsOf(Depot, *Actor->Network->GetEntity(Depot), EDepotModule::Shed);
	if (!TestTrue(FString::Printf(TEXT("setup: the plot seats fewer than the twenty-one sheds (%d)"), Seats), Seats > 0 && Seats < 21)) { return false; }
	const OpsSaveTest::FScopedSlot Slot(TEXT("AirportOpsTest_RepairAfterLoad"));
	Runtime->GetFacilityPurchases()->ApplyModuleRemoval = nullptr;
	if (!TestTrue(TEXT("setup: saved while over-owned"), Runtime->SaveToSlot(Slot))) { return false; }
	if (!TestEqual(TEXT("setup: with no removal hook the save's own drain removed nothing"), FacilityWiredOwned(*Actor, Depot, EDepotModule::Shed), 21)) { return false; }
	ULedger* Ledger = Runtime->GetLedger();
	const double Saved = Ledger->Balance();
	// SAVED IS THE SCENARIO'S OPENING BALANCE (nothing was bought) and the refund is the shop's shed price, both read: on
	// 2026-10-02 the opening fell 500,000 -> 260,000 and the shed 40,000 -> 16,000, and with 15 sheds over the ceiling the
	// new answer (260,000 + 15 x 16,000 = 500,000) happened to equal the OLD opening - a retyped 40,000 read as "no refund".
	const double ShedPrice = Runtime->GetFacilityPurchases()->ModuleOffers.FindChecked(EDepotModule::Shed).Price;

	Runtime->Attach(Actor);
	TestEqual(TEXT("setup: the new session opens at the saved balance"), Ledger->Balance(), Saved, 1e-6);
	constexpr float Step = 1.0f / 30.0f;
	Runtime->Tick(Step);
	TestEqual(TEXT("the attach's first frame repairs: owned == placed"), FacilityWiredOwned(*Actor, Depot, EDepotModule::Shed), Seats);
	TestEqual(TEXT("and refunds the excess"), Ledger->Balance(), Saved + (21 - Seats) * ShedPrice, 1e-6);

	if (!TestTrue(TEXT("the over-owned save loads"), Runtime->LoadFromSlot(Slot))) { return false; }
	TestEqual(TEXT("setup: the load brought the twenty-one sheds back"), FacilityWiredOwned(*Actor, Depot, EDepotModule::Shed), 21);
	TestEqual(TEXT("setup: and the balance from before the refund"), Ledger->Balance(), Saved, 1e-6);
	Runtime->Tick(Step);
	TestEqual(TEXT("the load's first frame repairs again: owned == placed"), FacilityWiredOwned(*Actor, Depot, EDepotModule::Shed), Seats);
	TestEqual(TEXT("refunded once, on top of the restored balance"), Ledger->Balance(), Saved + (21 - Seats) * ShedPrice, 1e-6);
	TestEqual(TEXT("on one Refund line - the loaded ledger had none"), FacilityWiredRefunds(*Ledger), 1);
	TestEqual(TEXT("and the presenter drops nothing"), TestWorld.Buildings->GetPlotPresenter()->GetDroppedCount(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFleetEveryBuyableTypeHasAChassisTest, "AirportOps.Fleet.EveryBuyableTypeHasAChassis",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFleetEveryBuyableTypeHasAChassisTest::RunTest(const FString&)
{
	// #430'S PIN. A buyable type is a scenario row, and every one must reach the RUNTIME's catalogue - the one Attach
	// resolved - with a chassis: a code, and axles. RED BEFORE THE FIX with a rig row (below): the chassis came from the
	// first stand letter that sent the code, and no letter sends the rig, so TypeFor answered a zero wheelbase.
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("an actor"), Actor)) { return false; }
	Actor->PlaceNode(FVector2D(0.0, 40000.0));
	UOpsRuntime* Runtime = NewObject<UOpsRuntime>();
	Runtime->Attach(Actor);
	const UScenario* Scenario = UAirportOpsSettings::ResolveDefaultScenario(*Runtime->GetCatalog());
	if (!TestNotNull(TEXT("the runtime's scenario"), Scenario)) { return false; }
	TestTrue(TEXT("the scenario sells at least one kind - a pin over nothing measures nothing"), Scenario->FuelVehicles.Num() > 0);
	for (const TPair<FName, FFuelVehicleSpec>& Row : Scenario->FuelVehicles)
	{
		const FServiceVehicleType* Kind = Runtime->GetJobBoard()->GetCatalogue().Find(Row.Key);
		if (!TestNotNull(FString::Printf(TEXT("scenario row %s is in the runtime's catalogue"), *Row.Key.ToString()), Kind)) { continue; }
		TestFalse(FString::Printf(TEXT("%s has a chassis TypeCode"), *Row.Key.ToString()), Kind->Vehicle.TypeCode.IsNone());
		TestTrue(FString::Printf(TEXT("%s has a wheelbase"), *Row.Key.ToString()), Kind->Vehicle.Chassis.Wheelbase() > 0.0);
	}

	// A TANKER LATER IS A DATA ROW (facility-upgrades spec R4), through the same resolve the runtime ran: the rig, which
	// Content builds and no stand letter was designed for, is a whole vehicle - its code, its axles and its trailer.
	UScenario* WithTanker = NewObject<UScenario>(GetTransientPackage());
	WithTanker->FuelVehicles = Scenario->FuelVehicles;
	WithTanker->FuelVehicles.Add(FName(AirsideVehicleCodes::Rig), FFuelVehicleSpec(30000.0, 500.0, 150000.0, 800.0, INVTEXT("Tanker")));
	UJobBoard* Board = NewObject<UJobBoard>(GetTransientPackage());
	UOpsRuntime::ResolveVehicleCatalogue(*Board, *WithTanker);
	for (const TPair<FName, FFuelVehicleSpec>& Row : WithTanker->FuelVehicles)
	{
		const FServiceVehicleType Kind = Board->TypeFor(Row.Key);
		TestFalse(FString::Printf(TEXT("%s resolves to a chassis with a TypeCode"), *Row.Key.ToString()), Kind.Vehicle.TypeCode.IsNone());
		TestTrue(FString::Printf(TEXT("%s resolves to a chassis with a wheelbase"), *Row.Key.ToString()), Kind.Vehicle.Chassis.Wheelbase() > 0.0);
	}
	TestTrue(TEXT("the rig row is the rig: it tows its tank"), Board->TypeFor(AirsideVehicleCodes::Rig).Vehicle.Tow.Num() > 0);
	return true;
}

// THE FUEL SUPPLY'S COMPOSITION (fuel-supply plan task 6): each fails if UOpsRuntime leaves one of the supply's seams unwired -
// the tanks' capacity, the board's handle, the new game's stock, the day beat, the spot poll, the save, the delivery's wake-up.
namespace
{
	/**
	 * THE BOARD'S HANDLE ON THE SUPPLY, mutable - the stock a refill would draw, which the runtime's own getter keeps const.
	 * Reaching it this way also asserts the link: a board with no supply returns null and the test stops there.
	 */
	UFuelSupply* FacilityWiredBoardSupply(UOpsRuntime& Runtime)
	{
		return Runtime.GetJobBoard()->FuelSupply.Get();
	}

	/** Seated tanks at Depot, read off the network and the plot's ceiling - NOT through UJobBoard::FuelCapacityLitres, which is
	 *  what the runtime wires and so cannot be its own witness. */
	int32 FacilityWiredSeatedTanks(UOpsRuntime& Runtime, const ARoadNetworkActor& Actor, FEntityInstanceId Depot)
	{
		const FEntityInstance* Entity = Actor.Network->GetEntity(Depot);
		if (Entity == nullptr) { return 0; }
		const int32 Owned = FacilityWiredOwned(Actor, Depot, EDepotModule::Tank);
		return FMath::Min(Owned, Runtime.GetFacilityPurchases()->ReservedSlotsOf(Depot, *Entity, EDepotModule::Tank));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFuelAttachWiresTheSupplyTest, "AirportOps.Present.Fuel.AttachWiresTheSupply",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFuelAttachWiresTheSupplyTest::RunTest(const FString&)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor"), TestWorld.Actor)) { return false; }
	UOpsRuntime* Runtime = nullptr;
	const FEntityInstanceId Depot = FacilityWiredDepot(TestWorld, Runtime);
	if (!TestTrue(TEXT("setup: the starter depot is placed"), Depot.IsSet())) { return false; }
	const UScenario* Scenario = UAirportOpsSettings::ResolveDefaultScenario(*Runtime->GetCatalog());
	if (!TestNotNull(TEXT("a scenario"), Scenario)) { return false; }

	const UFuelSupply* Supply = Runtime->GetFuelSupply();
	if (!TestNotNull(TEXT("the runtime owns a supply"), Supply)) { return false; }
	TestTrue(TEXT("the board draws from the same supply"), Runtime->GetJobBoard()->FuelSupply == Supply);
	TestTrue(TEXT("which pays from the runtime's ledger"), Supply->Ledger == Runtime->GetLedger());
	TestEqual(TEXT("its figures are the scenario's"), Supply->Figures.LitresPerTank, Scenario->FuelSupply.LitresPerTank, 1e-9);
	const int32 Tanks = FacilityWiredSeatedTanks(*Runtime, *TestWorld.Actor, Depot);
	if (!TestTrue(FString::Printf(TEXT("CONTROL: the starter depot seats a tank (%d)"), Tanks), Tanks > 0)) { return false; }
	TestEqual(TEXT("capacity is the seated tanks x LitresPerTank"), Supply->Capacity(), Tanks * Scenario->FuelSupply.LitresPerTank, 1e-6);
	TestEqual(TEXT("a new game opens at the scenario's starting stock"), Supply->StockLitres, Scenario->FuelSupply.StartingStockLitres, 1e-9);

	Runtime->Detach();
	TestFalse(TEXT("a detach takes the capacity hook back - it read the old actor's network"), static_cast<bool>(Supply->CapacityOf));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFuelDayEndDeliversTest, "AirportOps.Present.Fuel.DayEndDeliversTheContract",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFuelDayEndDeliversTest::RunTest(const FString&)
{
	// THE CONTRACT RIDES THE DAY'S END (FDayEndedEvent, published by the upkeep beat): unwired, a signed contract charges
	// nothing and delivers nothing, and the player who signed it waits for fuel that never comes.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor"), TestWorld.Actor)) { return false; }
	UOpsRuntime* Runtime = nullptr;
	if (!TestTrue(TEXT("setup: the starter depot is placed"), FacilityWiredDepot(TestWorld, Runtime).IsSet())) { return false; }
	UFuelSupply* Supply = FacilityWiredBoardSupply(*Runtime);
	if (!TestNotNull(TEXT("setup: the board holds the supply"), Supply)) { return false; }
	Supply->Draw(10000.0);   // room for a day of the smallest tier - a new game's tank is full
	const double Before = Supply->StockLitres;
	const double Day = Supply->Figures.ContractTiers[0].LitresPerDay;
	if (!TestEqual(TEXT("tier 0 is signed through the forwarder"), Runtime->SignFuelContract(0), EFuelOrderRefusal::None)) { return false; }

	USimClock* Clock = Runtime->GetClock();
	Clock->SetUniformDay(USimClock::SecondsPerDay);   // 1 game s per real s
	Runtime->RearmRepeatingSchedules();   // the day's length changed under the booking, as the upkeep tests re-book
	const double Midnight = Clock->NextDayStart();
	while (Clock->Now() < Midnight - 61.0) { Runtime->Tick(60.0); }
	TestEqual(TEXT("nothing is delivered before the day ends"), Supply->StockLitres, Before, 1e-9);
	Runtime->Tick(60.0);
	Runtime->Tick(60.0);
	TestEqual(TEXT("the day's litres arrived at midnight"), Supply->StockLitres, Before + Day, 1e-6);
	TestTrue(TEXT("and were paid for on a FuelPurchase line"), Runtime->GetLedger()->Entries().ContainsByPredicate(
		[](const FLedgerEntry& Row) { return Row.Category == ELedgerCategory::FuelPurchase; }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFuelSpotSurvivesSaveTest, "AirportOps.Present.Fuel.SpotOrderSurvivesASave",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFuelSpotSurvivesSaveTest::RunTest(const FString&)
{
	// REVIEW FOCUS 3: THE ORDER'S DUE TIME IS SAVED, THE CLOCK'S QUEUE IS NOT. A per-order clock entry booked at the order
	// would be gone in a fresh session; the repeating poll is re-armed with the other repeaters, so a load mid-delay still
	// delivers what was paid for. And the load is NOT a new game: the stock is the save's, not the scenario's opening.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor"), TestWorld.Actor)) { return false; }
	UOpsRuntime* Runtime = nullptr;
	if (!TestTrue(TEXT("setup: the starter depot is placed"), FacilityWiredDepot(TestWorld, Runtime).IsSet())) { return false; }
	UFuelSupply* Supply = FacilityWiredBoardSupply(*Runtime);
	if (!TestNotNull(TEXT("setup: the board holds the supply"), Supply)) { return false; }
	Supply->Draw(20000.0);
	const double Stock = Supply->StockLitres;
	if (!TestEqual(TEXT("a spot order is taken"), Runtime->OrderSpotFuel(10000.0), EFuelOrderRefusal::None)) { return false; }
	const OpsSaveTest::FScopedSlot Slot(TEXT("AirportOpsTest_FuelSpotSurvivesASave"));
	if (!TestTrue(TEXT("saved mid-delay"), Runtime->SaveToSlot(Slot))) { return false; }
	Runtime->Detach();

	UOpsRuntime* Fresh = NewObject<UOpsRuntime>();
	Fresh->Attach(TestWorld.Actor);
	if (!TestTrue(TEXT("loaded into a fresh session"), Fresh->LoadFromSlot(Slot))) { return false; }
	const UFuelSupply* Loaded = Fresh->GetFuelSupply();
	TestEqual(TEXT("the stock is the save's - a load is not a new game"), Loaded->StockLitres, Stock, 1e-9);
	if (!TestEqual(TEXT("the order is still on the way"), Loaded->SpotOrders.Num(), 1)) { return false; }
	const double Due = Loaded->SpotOrders[0].DueAt;

	USimClock* Clock = Fresh->GetClock();
	Clock->SetUniformDay(USimClock::SecondsPerDay);   // 1 game s per real s; the poll books in game seconds, so nothing re-books
	for (int32 Step = 0; Step < 400 && Clock->Now() < Due + 120.0; ++Step) { Fresh->Tick(60.0); }
	TestTrue(TEXT("setup: the clock passed the due time"), Clock->Now() >= Due + 120.0);
	TestEqual(TEXT("the order arrived in the loaded session"), Loaded->StockLitres, Stock + 10000.0, 1e-6);
	TestEqual(TEXT("and is no longer on the way"), Loaded->SpotOrders.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFuelLoadReplacesSessionTest, "AirportOps.Present.Fuel.LoadReplacesTheSessionsFuel",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFuelLoadReplacesSessionTest::RunTest(const FString&)
{
	// A LOAD REPLACES THE SESSION, including what the save holds as DEFAULTS: tagged serialisation writes no property equal
	// to the class default, so a save with an empty tank, no orders and no contract carries none of the three - and a load
	// that did not clear them first kept this session's order, contract and stock (UAirport::OnBeforeRestore's reason).
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor"), TestWorld.Actor)) { return false; }
	UOpsRuntime* Runtime = nullptr;
	if (!TestTrue(TEXT("setup: the starter depot is placed"), FacilityWiredDepot(TestWorld, Runtime).IsSet())) { return false; }
	UFuelSupply* Supply = FacilityWiredBoardSupply(*Runtime);
	if (!TestNotNull(TEXT("setup: the board holds the supply"), Supply)) { return false; }
	Supply->Draw(Supply->StockLitres);
	const OpsSaveTest::FScopedSlot Slot(TEXT("AirportOpsTest_FuelLoadReplacesTheSession"));
	if (!TestTrue(TEXT("saved dry, with nothing ordered"), Runtime->SaveToSlot(Slot))) { return false; }

	Supply->Receive(5000.0);
	TestEqual(TEXT("setup: an order after the save"), Runtime->OrderSpotFuel(10000.0), EFuelOrderRefusal::None);
	TestEqual(TEXT("setup: and a contract"), Runtime->SignFuelContract(0), EFuelOrderRefusal::None);
	if (!TestTrue(TEXT("the save loads"), Runtime->LoadFromSlot(Slot))) { return false; }
	TestEqual(TEXT("the stock is the save's empty tank"), Supply->StockLitres, 0.0, 1e-9);
	TestEqual(TEXT("no order the save did not make"), Supply->SpotOrders.Num(), 0);
	TestEqual(TEXT("and no contract"), Supply->Contract.Tier, static_cast<int32>(INDEX_NONE));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFuelDeliveryReopensWiredTest, "AirportOps.Present.Fuel.DeliveryReopensARefusedJob",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFuelDeliveryReopensWiredTest::RunTest(const FString&)
{
	// THE SEAM AT THE COMPOSITION (refactor contract): a delivery arriving through the runtime - the spot poll on the clock,
	// the event on the bus, the board's re-open - wakes a job refused NoFuelStock. UJobBoard's own test calls
	// ReopenStockRefusals by hand, so it passes with every runtime link cut.
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("an actor"), Actor)) { return false; }
	Actor->PlaceNode(FVector2D(0.0, 40000.0));
	if (!TestNotNull(TEXT("a network"), Actor->Network.Get())) { return false; }
	URoadNetwork& Net = *Actor->Network;
	const FFacilityFuelField Field = FacilityFuelField(Net);

	UOpsRuntime* Runtime = NewObject<UOpsRuntime>();
	Runtime->Attach(Actor);
	UJobBoard* Board = Runtime->GetJobBoard();
	UFuelSupply* Supply = FacilityWiredBoardSupply(*Runtime);
	if (!TestNotNull(TEXT("setup: the board holds the supply"), Supply)) { return false; }
	Supply->Draw(Supply->StockLitres);
	Board->AddVehicleForTest(Board->VehiclesFor(EIcaoCode::C).TypeCode, Field.Depot, EServiceVehicleState::Idle, 0.0);

	const FRoutePlan Plan = TestGraph::Probe(Net, Field.TaxiSouth, Net.GetEntity(Field.Stand)->PoseNode, ETraversalClass::Aircraft);
	if (!TestTrue(TEXT("the aircraft routes to the stand"), Plan.IsValid())) { return false; }
	if (!TestTrue(TEXT("and dispatches"), Actor->DispatchAgent(Plan, UAirsideSettings::ResolveDefaultAirframe()))) { return false; }
	constexpr float Step = 1.0f / 30.0f;
	auto Refused = [Board]() { return Board->GetJobs().Num() > 0 && Board->GetJobs()[0].State == EServiceJobState::Unserviceable; };
	for (int32 Tick = 0; Tick < 20000 && !Refused(); ++Tick) { Actor->Tick(Step); Runtime->Tick(Step); }
	if (!TestTrue(TEXT("setup: parking made a refused job"), Refused())) { return false; }
	if (!TestEqual(TEXT("setup: refused for want of stock"), Board->GetJobs()[0].Why, EServiceRefusal::NoFuelStock)) { return false; }

	// A SHORT DELAY so the test waits game minutes, not two hours - a design figure, which the next attach or load re-applies.
	Supply->Figures.SpotDelaySeconds = 60.0;
	if (!TestEqual(TEXT("fuel is ordered through the forwarder"), Runtime->OrderSpotFuel(10000.0), EFuelOrderRefusal::None)) { return false; }
	const double Due = Supply->SpotOrders[0].DueAt;
	for (int32 Tick = 0; Tick < 20000 && Refused(); ++Tick) { Actor->Tick(Step); Runtime->Tick(Step); }
	TestTrue(TEXT("CONTROL: no sooner than the order was due"), Runtime->GetClock()->Now() >= Due);
	TestEqual(TEXT("CONTROL: the order arrived"), Supply->SpotOrders.Num(), 0);
	TestFalse(TEXT("the refused job was re-opened by the delivery"), Refused());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFuelLowWokenTest, "AirportOps.Present.Fuel.FuelLowIsWokenByFuelEvents",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFuelLowWokenTest::RunTest(const FString&)
{
	// THE SEAM AT THE COMPOSITION: FuelLow is derived from the supply, and the alerts pass runs only when something marks it.
	// The model test hands Recompute a supply by hand, so it passes with every runtime link cut. Measured as the pass running
	// (RecomputeCountForTest) and as the alert standing, because a recompute that is woken but not given the supply raises
	// nothing, and a supply that is given but never woken is read once and goes stale.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor"), TestWorld.Actor)) { return false; }
	UOpsRuntime* Runtime = nullptr;
	const FEntityInstanceId Depot = FacilityWiredDepot(TestWorld, Runtime);
	if (!TestTrue(TEXT("setup: the starter depot is placed"), Depot.IsSet())) { return false; }
	UFuelSupply* Supply = FacilityWiredBoardSupply(*Runtime);
	if (!TestNotNull(TEXT("setup: the board holds the supply"), Supply)) { return false; }
	UOpsAlerts* Alerts = Runtime->GetAlerts();
	auto Standing = [Alerts]() { return Alerts->GetAlerts().ContainsByPredicate([](const FOpsAlert& A) { return A.Key.Kind == EAlertKind::FuelLow; }); };
	for (int32 Settle = 0; Settle < 3; ++Settle) { Runtime->Tick(0.0); }
	TestFalse(TEXT("CONTROL: a full tank raises nothing"), Standing());

	// A SIGNING publishes nothing and posts nothing, so only its forwarder can wake the pass. Cancelling it posts a charge, which wakes
	// the pass by FMoneyPostedEvent (mutation-checked 2026-10-03: no forwarder mark is needed).
	Supply->StockLitres = 0.1 * Supply->Capacity();   // no wake yet: a Draw publishes nothing
	TestFalse(TEXT("CONTROL: nothing woke the pass, so the alert is not up yet"), Standing());
	int32 Count = Alerts->RecomputeCountForTest();
	if (!TestEqual(TEXT("a contract is signed through the forwarder"), Runtime->SignFuelContract(0), EFuelOrderRefusal::None)) { return false; }
	Runtime->Tick(0.0);
	TestEqual(TEXT("signing ran the alerts pass"), Alerts->RecomputeCountForTest(), Count + 1);
	TestFalse(TEXT("and a contracted airport is not low"), Standing());
	Supply->Figures.CancelFraction = 0.0;
	Count = Alerts->RecomputeCountForTest();
	TestEqual(TEXT("cancelled through the forwarder"), Runtime->CancelFuelContract(), EFuelOrderRefusal::None);
	Runtime->Tick(0.0);
	TestTrue(TEXT("cancelling ran the alerts pass"), Alerts->RecomputeCountForTest() > Count);
	TestTrue(TEXT("and the low stock, uncontracted, is an alert"), Standing());

	// A SPOT ORDER AND ITS DELIVERY: the order counts as stock, the delivery is the event the pass is subscribed to.
	Supply->Figures.SpotDelaySeconds = 60.0;
	if (!TestEqual(TEXT("fuel is ordered through the forwarder"), Runtime->OrderSpotFuel(0.3 * Supply->Capacity()), EFuelOrderRefusal::None)) { return false; }
	Runtime->Tick(0.0);
	TestFalse(TEXT("an order on its way clears it"), Standing());
	Count = Alerts->RecomputeCountForTest();
	const double Due = Supply->SpotOrders[0].DueAt;
	for (int32 Tick = 0; Tick < 2000 && Supply->SpotOrders.Num() > 0; ++Tick) { Runtime->Tick(10.0); }
	TestTrue(TEXT("CONTROL: the order was delivered"), Supply->SpotOrders.Num() == 0 && Runtime->GetClock()->Now() >= Due);
	TestTrue(TEXT("the delivery ran the alerts pass"), Alerts->RecomputeCountForTest() > Count);
	TestFalse(TEXT("and 40% of the tanks is not low"), Standing());

	// A TANK BOUGHT GROWS THE CAPACITY under a stock that was fine: 26% of the old tanks is under a quarter of any larger set. The
	// alert must follow - woken by the JobBoard pass (FFacilityUpgradedEvent) and the network change, not by a subscription of its own.
	const double OldCapacity = Supply->Capacity();
	Supply->StockLitres = 0.26 * OldCapacity;
	if (!TestTrue(TEXT("setup: a tank is bought"), Runtime->BuyModule(Depot, EDepotModule::Tank).Succeeded())) { return false; }
	Runtime->Tick(0.0);
	TestTrue(TEXT("CONTROL: the tanks grew"), Supply->Capacity() > OldCapacity);
	TestTrue(TEXT("and 26% of the old tanks is now low"), Standing());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFuelOrdersAreToastedTest, "AirportOps.Present.Fuel.OrdersAreToasted",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFuelOrdersAreToastedTest::RunTest(const FString&)
{
	// A FUEL ORDER SPENDS MONEY, AND THE PLAYER HEARS OF IT the way a shed or a bowser is heard of: one purchase toast per success
	// through the runtime's forwarders, the nouns and the figure on it, and none for a refusal.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor"), TestWorld.Actor)) { return false; }
	UOpsRuntime* Runtime = nullptr;
	if (!TestTrue(TEXT("setup: the starter depot is placed"), FacilityWiredDepot(TestWorld, Runtime).IsSet())) { return false; }
	UFuelSupply* Supply = FacilityWiredBoardSupply(*Runtime);
	if (!TestNotNull(TEXT("setup: the board holds the supply"), Supply)) { return false; }
	UOpsEventsTestListener* Listener = NewObject<UOpsEventsTestListener>();
	Runtime->GetEvents()->OnPurchase.AddDynamic(Listener, &UOpsEventsTestListener::OnPurchase);
	Supply->Draw(Supply->StockLitres);

	if (!TestEqual(TEXT("a spot order is taken"), Runtime->OrderSpotFuel(10000.0), EFuelOrderRefusal::None)) { return false; }
	if (TestEqual(TEXT("and toasted once"), Listener->CountOf(TEXT("buy:FuelOrdered")), 1))
	{
		const FOpsPurchase& Ordered = Listener->Purchases.Last();
		TestEqual(TEXT("at what it cost"), Ordered.Amount, 10000.0 * Supply->Figures.SpotPricePerLitre, 1e-6);
		TestTrue(FString::Printf(TEXT("naming the litres ('%s')"), *Ordered.Name.ToString()), Ordered.Name.ToString().Contains(TEXT("10,000")));
		TestFalse(TEXT("with the money worded"), Ordered.Money.IsEmpty());
	}
	if (!TestEqual(TEXT("the smallest contract signs"), Runtime->SignFuelContract(0), EFuelOrderRefusal::None)) { return false; }
	if (TestEqual(TEXT("and is toasted once"), Listener->CountOf(TEXT("buy:FuelContractSigned")), 1))
	{
		const FFuelContractTier& Tier = Supply->Figures.ContractTiers[0];
		TestEqual(TEXT("at ONE DAY's cost - the day end charges it"), Listener->Purchases.Last().Amount, Tier.LitresPerDay * Tier.PricePerLitre, 1e-6);
	}
	const int32 Before = Listener->Purchases.Num();
	TestEqual(TEXT("a skipped tier is refused"), Runtime->SignFuelContract(2), EFuelOrderRefusal::AlreadyContracted);
	TestEqual(TEXT("and toasts nothing"), Listener->Purchases.Num(), Before);
	// AN UPGRADE IS TOASTED AS A SIGNING, at the new tier's day.
	if (TestEqual(TEXT("the next tier is an upgrade"), Runtime->SignFuelContract(1), EFuelOrderRefusal::None)
		&& TestEqual(TEXT("and is toasted as signed"), Listener->CountOf(TEXT("buy:FuelContractSigned")), 2))
	{
		const FFuelContractTier& Up = Supply->Figures.ContractTiers[1];
		TestEqual(TEXT("at the new tier's day"), Listener->Purchases.Last().Amount, Up.LitresPerDay * Up.PricePerLitre, 1e-6);
	}
	const double Charge = Supply->Quote(0.0).CancelCharge;
	if (!TestEqual(TEXT("the contract cancels"), Runtime->CancelFuelContract(), EFuelOrderRefusal::None)) { return false; }
	if (TestEqual(TEXT("and is toasted once"), Listener->CountOf(TEXT("buy:FuelContractCancelled")), 1))
	{
		TestEqual(TEXT("at the charge it was quoted"), Listener->Purchases.Last().Amount, Charge, 1e-6);
	}
	TestEqual(TEXT("a cancel with nothing running is refused"), Runtime->CancelFuelContract(), EFuelOrderRefusal::NoContract);
	TestEqual(TEXT("and toasts nothing"), Listener->CountOf(TEXT("buy:FuelContractCancelled")), 1);
	return true;
}

#endif
