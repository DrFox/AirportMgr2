#include "CoreMinimal.h"
#include "Build/AnchorLink.h"
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

#endif
