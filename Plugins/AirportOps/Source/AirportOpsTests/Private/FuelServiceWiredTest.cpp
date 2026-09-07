#include "CoreMinimal.h"
#include "Build/AnchorLink.h"
#include "Content/AirsideSettings.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/FuelService.h"
#include "Model/GroundTraffic.h"
#include "Model/InspectFacts.h"
#include "Model/RoadAgent.h"
#include "Model/RoadEntity.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadTraffic.h"
#include "Model/RouteSearch.h"
#include "Present/AirsideTraffic.h"
#include "Present/OpsRuntime.h"
#include "Present/RoadAgentActor.h"
#include "Present/RoadNetworkActor.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	void LayFuelLine(URoadNetwork& Net, const FVector2D& From, const FVector2D& To,
		ETraversalClass Class, FGuidelineNodeId& OutA, FGuidelineNodeId& OutB)
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
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFuelServiceWiredTest, "AirportOps.Present.FuelServiceWired",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFuelServiceWiredTest::RunTest(const FString& Parameters)
{
	// THE COMPOSITION TEST FOR THE SEAM, and the only one that fails if the relay or the
	// tick is left unwired. Every piece below has its own world-free test; this is the one
	// that proves they are joined - the same job AirportOps.Present.Runtime does for
	// save/load, spawned in a real world with a real actor and a real tick loop.
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("a world"), World)) { return false; }
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
	Context.SetCurrentWorld(World);
	ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };

	ARoadNetworkActor* Actor = World->SpawnActor<ARoadNetworkActor>();
	if (!TestNotNull(TEXT("the actor"), Actor)) { return false; }

	// A node first: the actor's Network is built lazily by the first edit.
	Actor->PlaceNode(FVector2D(0.0, 40000.0));
	if (!TestNotNull(TEXT("a network"), Actor->Network.Get())) { return false; }

	URoadNetwork& Net = *Actor->Network;

	// The same geometry the world-free fixture uses - see AirportOps.Ops.FuelService for
	// where each ray goes and why.
	constexpr double RoadY = -6000.0;
	FGuidelineNodeId TaxiSouth, TaxiNorth, RoadWest, RoadEast;
	LayFuelLine(Net, FVector2D(-10000.0, -10000.0), FVector2D(-10000.0, 10000.0),
		ETraversalClass::Aircraft, TaxiSouth, TaxiNorth);
	LayFuelLine(Net, FVector2D(-20000.0, RoadY), FVector2D(20000.0, RoadY),
		ETraversalClass::GroundVehicle, RoadWest, RoadEast);

	UEntityDefinition* StandDef = UEntityDefinition::MakeStandTransient();
	UEntityDefinition* DepotDef = UEntityDefinition::MakeFuelDepotTransient();
	const FEntityInstanceId Stand = Net.PlaceEntity(StandDef, StandDef->Anchors,
		FVector2D(0.0, 0.0), 0.0, 3600.0, StandDef->PoseRole, StandDef->Trucks);
	const FEntityInstanceId Depot = Net.PlaceEntity(DepotDef, DepotDef->Anchors,
		FVector2D(12000.0, RoadY + 4000.0), UE_DOUBLE_PI * 0.5, 0.0, DepotDef->PoseRole,
		DepotDef->Trucks);
	FAnchorLink::Build(Net);

	UOpsRuntime* Runtime = NewObject<UOpsRuntime>();
	if (!TestNotNull(TEXT("the runtime owns a fuel service"), Runtime->GetFuelService())) { return false; }
	Runtime->Attach(Actor);

	// Taxi an aircraft in to the stand.
	FRouteQuery Query;
	Query.Start = TaxiSouth;
	Query.Goal = Net.GetEntity(Stand)->PoseNode;
	Query.Class = ETraversalClass::Aircraft;
	const FRoutePlan Plan = RouteSearch::Find(Net, Query);
	if (!TestTrue(TEXT("the aircraft routes to the stand"), Plan.IsValid())) { return false; }
	if (!TestTrue(TEXT("and dispatches"),
		Actor->DispatchAgent(Plan, UAirsideSettings::ResolveDefaultAirframe()))) { return false; }
	const int32 Aircraft = Actor->GetTraffic()->GetNewestAgentId();

	// BOTH TICKS, in the order the game runs them: the actor advances the traffic, the
	// runtime advances the clock and the service. Nothing calls the service by hand.
	constexpr float Step = 1.0f / 30.0f;
	int32 TruckId = 0;
	bool bSawVehicleAgent = false;
	bool bSawTruckWithAView = false;
	bool bSawFuelling = false;

	for (int32 Tick = 0; Tick < 12000; ++Tick)
	{
		Actor->Tick(Step);
		Runtime->Tick(Step);

		for (const FRoadAgent& Agent : Actor->GetTraffic()->GetModel()->GetAgents())
		{
			if (Agent.Class != ETraversalClass::GroundVehicle)
			{
				continue;
			}
			bSawVehicleAgent = true;
			TruckId = Agent.Id;
			bSawTruckWithAView |= Actor->GetTraffic()->GetAgentView(Agent.Id) != nullptr;
		}

		for (const FFuelDemand& Demand : Runtime->GetFuelService()->GetDemands())
		{
			bSawFuelling |= Demand.State == EFuelDemandState::Fuelling;
		}

		if (bSawFuelling && bSawVehicleAgent && TruckId != 0
			&& Actor->GetTraffic()->GetModel()->FindAgent(TruckId) == nullptr)
		{
			break;
		}
	}

	// 1. The RELAY: an aircraft parking reached the service at all.
	TestEqual(TEXT("parking made a demand"), Runtime->GetFuelService()->GetDemands().Num(), 1);
	TestEqual(TEXT("for the aircraft that parked"),
		Runtime->GetFuelService()->GetDemands()[0].AircraftId, Aircraft);
	TestEqual(TEXT("at the stand it parked on"),
		Runtime->GetFuelService()->GetDemands()[0].Stand, Stand);
	TestEqual(TEXT("served by the depot"),
		Runtime->GetFuelService()->GetDemands()[0].Depot, Depot);

	// 2. The TICK: a GroundVehicle agent appeared, with a view of its own, and fuelled.
	TestTrue(TEXT("a ground vehicle agent appeared"), bSawVehicleAgent);
	TestTrue(TEXT("with a view of its own"), bSawTruckWithAView);
	TestTrue(TEXT("and it fuelled the aircraft"), bSawFuelling);

	// 3. And went away again. A truck does not fly off, so only the service retires it -
	// this is the end of the round trip the whole slice exists to produce.
	TestNull(TEXT("the truck is gone once it is home"),
		Actor->GetTraffic()->GetModel()->FindAgent(TruckId));
	TestEqual(TEXT("done"), static_cast<int32>(Runtime->GetFuelService()->GetDemands()[0].State),
		static_cast<int32>(EFuelDemandState::Done));

	// 4. THE PANEL'S SEAM. FAgentFacts::Fuel is filled by the ops layer, not by DescribeAgent
	// - so the field must be empty out of Airside and non-empty once the service is asked.
	FAgentFacts AirsideFacts;
	if (!TestTrue(TEXT("the aircraft describes"),
		InspectFacts::DescribeAgent(*Actor->GetTraffic()->GetModel(), &Net, Aircraft, AirsideFacts)))
	{
		return false;
	}
	TestTrue(TEXT("Airside leaves the fuel line empty - it must not know what fuel is"),
		AirsideFacts.Fuel.IsEmpty());
	TestEqual(TEXT("and the ops layer fills it"),
		Runtime->GetFuelService()->DescribeAgent(Aircraft), FString(TEXT("done")));

	// 5. And the depot's own card can tell itself from a stand.
	FStandFacts DepotFacts;
	if (!TestTrue(TEXT("the depot describes"),
		InspectFacts::DescribeStand(Actor->GetTraffic()->GetModel(), Net, Depot.Index, DepotFacts)))
	{
		return false;
	}
	TestEqual(TEXT("as a service installation, not a stand"),
		static_cast<int32>(DepotFacts.PoseRole), static_cast<int32>(EServiceRole::Fuel));
	TestTrue(TEXT("and it is on a road"), DepotFacts.bReachable);
	return true;
}

#endif
