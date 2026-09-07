#include "CoreMinimal.h"
#include "Content/AirsideSettings.h"
#include "Misc/AutomationTest.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadAgent.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadTraffic.h"
#include "Model/RouteSearch.h"
#include "Model/TrafficOccupancy.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTruckCrossesTaxiwayTest,
	"Airside.Traffic.TruckCrossesTaxiway",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTruckCrossesTaxiwayTest::RunTest(const FString& Parameters)
{
	// A hand-authored crossing: a taxiway east-west through the origin and a road
	// north-south through it, sharing the CENTRE node. Hand-authored rather than solved,
	// like every M2 traffic fixture, because what is under test is the ARBITER and a fixture
	// that had to pave a junction first would fail for reasons that are not this test's.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());

	const FGuidelineNodeId Centre = Net->AddGuidelineNode(FVector2D(0.0, 0.0), false);
	const FGuidelineNodeId West   = Net->AddGuidelineNode(FVector2D(-30000.0, 0.0), false);
	const FGuidelineNodeId East   = Net->AddGuidelineNode(FVector2D(30000.0, 0.0), false);
	const FGuidelineNodeId South  = Net->AddGuidelineNode(FVector2D(0.0, -8000.0), false);
	const FGuidelineNodeId North  = Net->AddGuidelineNode(FVector2D(0.0, 30000.0), false);

	auto Link = [Net](FGuidelineNodeId A, FGuidelineNodeId B, ETraversalClass Class)
	{
		FGuidelineEdge Edge;
		Edge.A = A;
		Edge.B = B;
		Edge.Control = (Net->GetGuidelineNode(A)->Position + Net->GetGuidelineNode(B)->Position) * 0.5;
		Edge.AllowedTraffic = FTrafficMask::Only(Class);
		Edge.Direction = EGuidelineDir::Bidirectional;
		Edge.Width = 600.0;
		Edge.bDerived = false;
		Net->AddGuidelineEdge(MoveTemp(Edge));
	};
	Link(West, Centre, ETraversalClass::Aircraft);
	Link(Centre, East, ETraversalClass::Aircraft);
	Link(South, Centre, ETraversalClass::GroundVehicle);
	Link(Centre, North, ETraversalClass::GroundVehicle);

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());

	auto Route = [Net](FGuidelineNodeId Start, FGuidelineNodeId Goal, ETraversalClass Class)
	{
		FRouteQuery Query;
		Query.Start = Start;
		Query.Goal = Goal;
		Query.Class = Class;
		return RouteSearch::Find(*Net, Query);
	};

	const FRoutePlan AircraftPlan = Route(West, East, ETraversalClass::Aircraft);
	const FRoutePlan TruckPlan = Route(South, North, ETraversalClass::GroundVehicle);
	if (!TestTrue(TEXT("the aircraft routes along the taxiway"), AircraftPlan.IsValid())) { return false; }
	if (!TestTrue(TEXT("the truck routes across it"), TruckPlan.IsValid())) { return false; }

	// EACH ONLY EVER GETS ITS OWN CLASS OF LINE. Pinned here as well as in
	// Airside.Build.RoadCrossesTaxiway, because that test measures what the BUILDER derives
	// and this one measures what the SEARCH will actually hand an agent.
	TestFalse(TEXT("a truck cannot route down the taxiway"),
		Route(West, East, ETraversalClass::GroundVehicle).IsValid());
	TestFalse(TEXT("and an aircraft cannot route down the road"),
		Route(South, North, ETraversalClass::Aircraft).IsValid());

	// A SLOW TAXI, and the reason is the whole design of this fixture.
	//
	// Dispatching both at once and hoping they meet does NOT test priority: measured on the
	// first attempt, the two crossed 788 uu apart with NEITHER ever stopping, because their
	// claim windows on the shared node were about three seconds each and simply missed. That
	// test would have passed a build in which class priority did nothing at all.
	//
	// So the contest is ARRANGED rather than hoped for: the aircraft taxis at 2 m/s (a real
	// figure - a heavy on a tight apron does about this), which widens the window it holds
	// the crossing for to something a truck can arrive inside, and the truck is not
	// dispatched until the table SAYS the aircraft holds the node. What is then measured is
	// who gives way, which is the only thing this test is about.
	FAirframe SlowPlane = UAirsideSettings::ResolveDefaultAirframe();
	SlowPlane.Ground.Taxi.SpeedCap = 200.0;

	const int32 Aircraft = Traffic->DispatchAgent(Net, AircraftPlan, SlowPlane,
		ETraversalClass::Aircraft, 0.0);
	if (!TestTrue(TEXT("the aircraft is under way"), Aircraft != 0)) { return false; }

	// Run it up to the crossing and stop when the TABLE says it holds the node - not when a
	// stopwatch says it ought to. The claim is the thing the truck will be refused by, so
	// the claim is what this waits for.
	bool bPlaneHoldsCrossing = false;
	for (int32 Step = 0; Step < 6000 && !bPlaneHoldsCrossing; ++Step)
	{
		Traffic->Advance(1.0 / 30.0, Net);
		int32 Holder = 0;
		bPlaneHoldsCrossing =
			Traffic->GetOccupancy().IsHeld(FTrafficResource::OfNode(Centre), 0, &Holder)
			&& Holder == Aircraft;
	}
	if (!TestTrue(TEXT("the aircraft comes to hold the crossing"), bPlaneHoldsCrossing)) { return false; }

	const int32 Truck = Traffic->DispatchAgent(Net, TruckPlan,
		UAirsideSettings::ResolveDefaultVehicle(), ETraversalClass::GroundVehicle, 0.0);
	if (!TestTrue(TEXT("and the truck sets off into it"), Truck != 0)) { return false; }

	// Drive them both and watch the closest they ever get. AIRCRAFT OVER VEHICLE is a fact
	// about the CLASSES (TraversalPriority, spec 5.4) and needs no authoring at the junction
	// - which is exactly the property worth measuring, because a crossing that happened to
	// work by timing would pass a test that only checked arrival.
	const FVector2D CrossingAt = Net->GetGuidelineNode(Centre)->Position;

	double Closest = TNumericLimits<double>::Max();

	// THE SAFETY PROPERTY: how near the truck ever got to the crossing WHILE THE AIRCRAFT
	// HELD IT. That is what "gives way" means mechanically, and unlike a plain separation it
	// is something the arbiter actually promises.
	double NearestWhileHeld = TNumericLimits<double>::Max();

	bool bTruckWaited = false;
	bool bTruckArrived = false;
	int32 TruckStoppedSteps = 0;

	for (int32 Step = 0; Step < 6000; ++Step)
	{
		Traffic->Advance(1.0 / 30.0, Net);

		const FRoadAgent* Plane = Traffic->FindAgent(Aircraft);
		const FRoadAgent* Van = Traffic->FindAgent(Truck);
		if (Van == nullptr)
		{
			break;
		}
		bTruckArrived |= Van->Phase == EAgentPhase::Parked;
		TruckStoppedSteps += (Van->Phase == EAgentPhase::Taxiing
			&& Van->LastMotion.GroundSpeed <= 0.0) ? 1 : 0;

		int32 Holder = 0;
		if (Traffic->GetOccupancy().IsHeld(FTrafficResource::OfNode(Centre), 0, &Holder)
			&& Holder == Aircraft)
		{
			NearestWhileHeld = FMath::Min(NearestWhileHeld,
				FVector2D::Distance(Van->LastMotion.Position, CrossingAt));
		}

		if (Plane != nullptr)
		{
			Closest = FMath::Min(Closest,
				FVector2D::Distance(Plane->LastMotion.Position, Van->LastMotion.Position));

			// A YIELD IS THE TRUCK STOPPED WITH ROAD STILL LEFT, not the truck having
			// finished. WaitingOn names who refused it, so this cannot be satisfied by the
			// truck stopping for any other reason.
			bTruckWaited |= Van->Phase == EAgentPhase::Taxiing
				&& Van->LastMotion.GroundSpeed <= 0.0
				&& Van->WaitingOn == Aircraft;
		}
	}

	// MIN SEPARATION, MEASURED (spec §8) and reported rather than asserted against a
	// Euclidean threshold.
	//
	// A threshold of half-footprint-plus-half-footprint was tried and is WRONG PHYSICS for
	// this model: FTrafficRules footprints are lengths ALONG A ROUTE, not radii, and two
	// agents on perpendicular lines legitimately pass with their centres closer than the sum
	// of their half-lengths - measured at 698 uu against a 750 uu "touching" figure, with the
	// truck correctly waiting throughout. Asserting it would have failed a build that was
	// behaving exactly as designed.
	AddInfo(FString::Printf(
		TEXT("min separation %.0f uu; truck came no nearer than %.0f uu to the crossing while "
			 "the aircraft held it; truck stopped for %d step(s)"),
		Closest, NearestWhileHeld, TruckStoppedSteps));

	TestTrue(TEXT("the truck gave way to the aircraft at the crossing"), bTruckWaited);

	// AND IT STAYED OFF THE CROSSING WHILE THE AIRCRAFT OWNED IT. This is the property the
	// arbiter actually promises, and the one a truck driving straight through would break:
	// the truck's centre never came within its own half-footprint of the node while the
	// table said the aircraft held it.
	TestTrue(*FString::Printf(
		TEXT("the truck stayed off the crossing while the aircraft held it: nearest %.0f uu"),
		NearestWhileHeld),
		NearestWhileHeld > Traffic->Rules.VehicleFootprint * 0.5);

	// AND THE TRUCK STILL GETS THERE. A yield that never released would satisfy every
	// assertion above and be exactly the starvation the resolver exists to prevent.
	TestTrue(TEXT("the truck reaches the far side"), bTruckArrived);
	return true;
}

#endif
