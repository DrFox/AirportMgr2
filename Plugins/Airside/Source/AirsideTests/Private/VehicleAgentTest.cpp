#include "CoreMinimal.h"
#include "Content/AirsideSettings.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadEntity.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadTraffic.h"
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
	const FAirframe Van = UAirsideSettings::ResolveDefaultVehicle();
	TestTrue(TEXT("a vehicle can move about an airport"), Van.Ground.IsSet());
	TestEqual(TEXT("and says what it is"), Van.TypeCode, FName(TEXT("FUEL")));
	TestEqual(TEXT("with no wing to fit through a turn"), Van.Wingspan, 0.0);

	// A VEHICLE NEVER FLIES, so Climb and Approach stay at their struct defaults and nothing
	// may read them. Pinned because the alternative - filling them in - is authored numbers
	// nothing reads, which this codebase has shipped three times over.
	TestFalse(TEXT("no landing figures: a truck declines to land"), Van.Approach.IsSet());

	// It turns far harder than an aeroplane. Asserted as a relation rather than a number, so
	// tuning either figure does not break this and reversing them does.
	TestTrue(TEXT("a van slews its wheel harder than an airframe"),
		Van.Ground.MaxTurnRateDegPerSec
			> UAirsideSettings::ResolveDefaultAirframe().Ground.MaxTurnRateDegPerSec);

	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("a world"), World)) { return false; }
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
	Context.SetCurrentWorld(World);
	ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };

	ARoadNetworkActor* Actor = World->SpawnActor<ARoadNetworkActor>();
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

	FRouteQuery Query;
	Query.Start = A;
	Query.Goal = B;
	Query.Class = ETraversalClass::GroundVehicle;
	const FRoutePlan Plan = RouteSearch::Find(Net, Query);
	if (!TestTrue(TEXT("the road routes"), Plan.IsValid())) { return false; }

	if (!TestTrue(TEXT("the truck dispatches"),
		Actor->DispatchAgent(Plan, Van, ETraversalClass::GroundVehicle))) { return false; }

	const int32 Id = Actor->GetTraffic()->GetNewestAgentId();
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
