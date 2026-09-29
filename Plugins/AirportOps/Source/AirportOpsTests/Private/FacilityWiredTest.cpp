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

#endif
