#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Content/AirsideSettings.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadEntity.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadTraffic.h"
#include "Model/RoutePolicy.h"
#include "Model/RouteSearch.h"
#include "Present/AirsideTraffic.h"
#include "Present/RoadAgentActor.h"
#include "Present/RoadNetworkActor.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVehicleAgentTest,
	"Airside.Present.VehicleAgentView",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVehicleAgentTest::RunTest(const FString& Parameters)
{
	// The performance bundle first, world-free. FGroundPerformance::IsSet is what every
	// caller checks before moving anything, so it is what this must satisfy - a truck that
	// fails it freezes on the line with nothing to say why.
	const FVehicle Van = UAirsideSettings::ResolveDefaultVehicle();
	TestTrue(TEXT("a vehicle can move about an airport"), Van.Chassis.Ground.IsSet());
	TestEqual(TEXT("and says what it is"), Van.TypeCode, FName(TEXT("FUEL")));

	// A VEHICLE NEVER FLIES - and since 2026-09-23 it has no climb or approach to leave unset.
	// That used to be pinned here as Approach.IsSet() == false on a zeroed FAirframe; it is
	// pinned below instead, on the dispatched agent, as AsAircraft() == null.

	// It turns far harder than an aeroplane. Asserted as a relation rather than a number, so
	// tuning either figure does not break this and reversing them does.
	TestTrue(TEXT("a van slews its wheel harder than an airframe"),
		Van.Chassis.Ground.MaxTurnRateDegPerSec
			> UAirsideSettings::ResolveDefaultAirframe().Chassis.Ground.MaxTurnRateDegPerSec);

	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("the actor"), Actor)) { return false; }

	// A NETWORK HAS TO BE MADE FIRST. A freshly spawned actor's Network is null until an
	// edit builds one lazily (URoadEditFacade::EnsureNetwork), so a test that reached
	// straight for Actor->Network dereferenced null - which is what this line's absence cost
	// on the first run. One node is enough, and it is what every other actor test does.
	Actor->PlaceNode(FVector2D(0.0, 30000.0));
	if (!TestNotNull(TEXT("the actor has a network"), Actor->Network.Get())) { return false; }

	URoadNetwork& Net = *Actor->Network;
	const FGuidelineNodeId A = Net.AddGuidelineNode(FVector2D(0.0, 0.0), /*bDerived=*/false);
	const FGuidelineNodeId B = Net.AddGuidelineNode(FVector2D(20000.0, 0.0), /*bDerived=*/false);
	{
		FGuidelineEdge Edge;
		Edge.A = A;
		Edge.B = B;
		Edge.Control = FVector2D(10000.0, 0.0);
		Edge.AllowedTraffic = FTrafficMask::Only(ETraversalClass::GroundVehicle);
		Edge.Direction = EGuidelineDir::Bidirectional;
		Edge.bDerived = false;
		Net.AddGuidelineEdge(MoveTemp(Edge));
	}

	// #312: was a hand-built FRouteQuery that skipped AvoidRunways.
	const FRoutePlan Plan = TestGraph::Probe(Net, A, B, ETraversalClass::GroundVehicle);
	if (!TestTrue(TEXT("the road routes"), Plan.IsValid())) { return false; }

	if (!TestTrue(TEXT("the truck dispatches"),
		Actor->DispatchAgent(Plan, Van, ETraversalClass::GroundVehicle))) { return false; }

	const int32 Id = Actor->GetTraffic()->GetNewestAgentId();

	// THE BUNDLE SEAM (2026-09-23): the FVehicle overload of DispatchAgent must reach
	// FRoadAgent::StartDrive. Unwired - say it fell through to the FAirframe path - the agent
	// would carry a default airframe whose climb answers IsSet() true and whose chassis pivots.
	const FRoadAgent* Truck = Actor->GetTraffic()->GetModel()->FindAgent(Id);
	if (!TestNotNull(TEXT("the model holds the truck"), Truck)) { return false; }
	TestEqual(TEXT("it was started as a vehicle"), Truck->GetBody(), EAgentBody::Vehicle);
	TestNull(TEXT("so it has no flight data to read"), Truck->AsAircraft());
	TestEqual(TEXT("it rolls on the vehicle's own chassis"),
		Truck->Chassis().Wheelbase(), Van.Chassis.Wheelbase(), 0.001);
	TestEqual(TEXT("with no wing to fit through a turn"), Truck->Wingspan(), 0.0);
	TestEqual(TEXT("and the inspector still names it"), Truck->TypeCode(), FName(TEXT("FUEL")));
	TestEqual(TEXT("a truck has no propeller to spool"), Truck->LastMotion.EngineRPM, 0.0);

	ARoadAgentActor* View = Actor->GetTraffic()->GetAgentView(Id);
	if (!TestNotNull(TEXT("a view was spawned for it"), View)) { return false; }

	// THE SEAM: a GroundVehicle agent is dressed as a vehicle, not as an aircraft. Without
	// this it is the aircraft placeholder cube at 4 m x 4 m x 2 m - which reads as a second
	// aeroplane rather than as a van, and is eight times the vehicle footprint the traffic
	// model is actually keeping clear for it.
	if (!TestTrue(TEXT("and dressed as a vehicle"), View->HasVehicleBodyForTest())) { return false; }

	// SIZED FROM THE ARBITER'S OWN FIGURE, so what the player sees stopping at a junction is
	// the length that actually stopped. Asserted against FTrafficRules rather than a literal,
	// because a second copy of that number is exactly the drift this codebase's "one struct
	// per thing" rule exists to prevent.
	TestEqual(TEXT("the body is as long as the footprint the arbiter reserves"),
		View->PlaceholderSizeForTest().X,
		Actor->TrafficRules.FootprintFor(ETraversalClass::GroundVehicle));

	// An AIRCRAFT agent is still dressed the old way - the branch must not have swallowed
	// the case it was grown out of.
	if (!TestTrue(TEXT("an aircraft also dispatches"),
		Actor->DispatchAgent(Plan, UAirsideSettings::ResolveDefaultAirframe(),
			ETraversalClass::Aircraft))) { return false; }
	ARoadAgentActor* Plane = Actor->GetTraffic()->GetAgentView(Actor->GetTraffic()->GetNewestAgentId());
	if (!TestNotNull(TEXT("with a view of its own"), Plane)) { return false; }
	TestFalse(TEXT("and is NOT dressed as a vehicle"), Plane->HasVehicleBodyForTest());
	return true;
}

#endif
