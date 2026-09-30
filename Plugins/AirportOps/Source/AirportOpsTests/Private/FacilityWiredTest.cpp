#include "CoreMinimal.h"
#include "Build/AnchorLink.h"
#include "Build/DepotKit.h"
#include "Content/AirportOpsSettings.h"
#include "Content/AirsideSettings.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/FacilityPurchases.h"
#include "Model/GroundTraffic.h"
#include "Model/JobBoard.h"
#include "Model/Ledger.h"
#include "Model/OpsDefinition.h"
#include "Model/RoadEntity.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadTraffic.h"
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
		Before.Modules.Num() == 1 && Before.Modules[0].Reserved >= 2)) { return false; }
	const UPlotPresenter* Plots = TestWorld.Buildings->GetPlotPresenter();
	const int32 Lit = Plots->GetModuleCount();

	const FPurchaseResult Shed = Runtime->BuyModule(Depot, EDepotModule::Shed);
	TestTrue(TEXT("the shed is bought through the runtime's hooks"), Shed.Succeeded());
	TestEqual(TEXT("and one more bay is lit - the module hook reached the facade and the rebuild"), Plots->GetModuleCount(), Lit + 1);
	TestEqual(TEXT("the quote now owns two"), Runtime->QuoteFacility(Depot).Modules[0].Owned, 2);
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
	const FFuelVehicleSpec Spec = Board->SpecFor(TEXT("FUEL"));

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
	TestEqual(TEXT("nor built"), Runtime->QuoteFacility(Depot).Modules[0].Owned, 1);
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
	const int32 WideSheds = Runtime->QuoteFacility(Wide).Modules[0].Reserved;
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
	TestTrue(TEXT("and it holds fewer sheds than the wide one"), Q.Modules.Num() == 1 && Q.Modules[0].Reserved < WideSheds);
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

	const int32 WideSheds = Runtime->QuoteFacility(First).Modules[0].Reserved;
	if (!TestTrue(TEXT("setup: a wide plot reserves a second shed"), WideSheds >= 2)) { return false; }

	TArray<FVector2D> Narrow = Second;
	Narrow[1].X = Second[0].X + 2000.0;
	Narrow[2].X = Second[0].X + 2000.0;
	int32 NarrowSheds = INDEX_NONE;
	{
		FRoadEditScope Edit(nullptr, Actor->Network, TEXT("re-plot"));
		if (!TestTrue(TEXT("setup: the plot is narrowed inside the scope"),
			FRoadNetworkTestAccess(*Actor->Network).SetEntityOutlineForTest(Depot, Narrow))) { return false; }
		NarrowSheds = Runtime->QuoteFacility(Depot).Modules[0].Reserved;
		TestTrue(TEXT("control: the narrow plot holds fewer sheds - the memo now holds THAT"), NarrowSheds < WideSheds);
		const int32 Solves = Runtime->ReservationSolvesForTest();

		URoadNetwork* const LiveBefore = Actor->Network;
		TestTrue(TEXT("the scope rolls back"), Edit.Rollback());
		TestTrue(TEXT("in place: the network object is the one the memo was filled against"), Actor->Network == LiveBefore);

		const int32 After = Runtime->QuoteFacility(Depot).Modules[0].Reserved;
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
	const TArray<FVector2D> Plot = FacilityWiredWidePlot();
	const int32 Index = Actor->PlaceEntityInPlot(Plot, Plot[0], Plot[1], Owned, EPlaceableEntity::FuelDepot);
	if (!TestTrue(TEXT("setup: the depot is placed"), Index != INDEX_NONE)) { return false; }
	const FEntityInstanceId Depot = Actor->Network->EntityIdAt(Index);
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
	const FFacilityQuote Q = Shop->Quote(*TestWorld.Actor->Network, Depot);
	TestTrue(TEXT("the shed row refuses NotAFacility"),
		Q.Modules.Num() == 1 && Q.Modules[0].Refusal == EPurchaseRefusal::NotAFacility);
	return true;
}

#endif
