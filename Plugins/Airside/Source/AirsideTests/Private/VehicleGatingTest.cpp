#include "CoreMinimal.h"
#include "Build/RoadGuidelineBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Content/AirsideSettings.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RoutePolicy.h"
#include "Model/RouteSearch.h"
#include "Model/SpeedProfile.h"
#include "Model/Vehicle.h"
#include "Model/VehicleFit.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

// Spec 2026-09-23 §6, test 6: a vehicle is routed only where it fits. NOT by width alone - a
// 2.54 m rig fits a 3 m straight lane, as real lorries do - but through the corners, where its
// trailer's swept path is what the pavement has to hold.

namespace VehicleGating
{
	void Derive(URoadNetwork& Net)
	{
		FRoadGuidelineBuilder::Build(Net, FRoadNetworkSolver::SolveAll(Net),
			UAirsideSettings::ResolveLargestServiceVehicle());
	}

	/** Straight road (0,0)->(30000,0); returns its A->B lane's ends. */
	URoadNetwork* Straight(URoadProfile* Profile, FGuidelineNodeId& OutFrom, FGuidelineNodeId& OutTo)
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		const FRoadSegmentId Seg = Net->AddStraightSegment(
			Net->AddNode(FVector2D(0.0, 0.0)), Net->AddNode(FVector2D(30000.0, 0.0)), Profile);
		Derive(*Net);
		for (const FGuidelineEdge& Edge : Net->GetGuidelineEdges())
		{
			if (Edge.bAlive && Edge.DerivedFrom == Seg && Edge.Direction == EGuidelineDir::AToB)
			{
				OutFrom = Edge.A;
				OutTo = Edge.B;
			}
		}
		return Net;
	}

	/** A T at the origin, arms west, east and north; from the west arm's far end onto the north arm. */
	URoadNetwork* TurnNorth(URoadProfile* Profile, FGuidelineNodeId& OutFrom, FGuidelineNodeId& OutTo)
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		const FRoadNodeId Hub = Net->AddNode(FVector2D(0.0, 0.0));
		const FRoadSegmentId West = Net->AddStraightSegment(Hub, Net->AddNode(FVector2D(-30000.0, 0.0)), Profile);
		Net->AddStraightSegment(Hub, Net->AddNode(FVector2D(30000.0, 0.0)), Profile);
		const FRoadSegmentId North = Net->AddStraightSegment(Hub, Net->AddNode(FVector2D(0.0, 30000.0)), Profile);
		Derive(*Net);
		for (const FGuidelineEdge& Edge : Net->GetGuidelineEdges())
		{
			if (!Edge.bAlive) { continue; }
			// Arms were added FROM the hub: B->A arrives, A->B leaves.
			if (Edge.DerivedFrom == West && Edge.Direction == EGuidelineDir::BToA) { OutFrom = Edge.B; }
			if (Edge.DerivedFrom == North && Edge.Direction == EGuidelineDir::AToB) { OutTo = Edge.B; }
		}
		return Net;
	}

	FRoutePlan Route(const URoadNetwork& Net, FGuidelineNodeId From, FGuidelineNodeId To, const FVehicle& Vehicle)
	{
		FRouteQuery Query = FRouteQuery::For(ERouteErrand::GraphProbe, From, To, 0.0, ETraversalClass::GroundVehicle);
		Query.WithVehicle(Vehicle);
		return RouteSearch::Find(Net, Query);
	}
}

using namespace VehicleGating;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVehicleGatingTest, "Airside.Model.VehicleGating",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVehicleGatingTest::RunTest(const FString& Parameters)
{
	const FVehicle Bowser = UAirsideSettings::ResolveDefaultVehicle();
	const FVehicle Rig = UAirsideSettings::ResolveRigVehicle();
	FGuidelineNodeId From, To;

	// STRAIGHT: width only.
	{
		URoadNetwork* Net = Straight(URoadProfile::MakeServiceRoadTransient(300.0), From, To);
		TestTrue(TEXT("the rig drives a straight Narrow road - 2.54 m in a 3 m lane, as lorries do"),
			Route(*Net, From, To, Rig).IsValid());
	}
	{
		URoadNetwork* Net = Straight(URoadProfile::MakeServiceRoadTransient(250.0), From, To);
		const FRoutePlan Plan = Route(*Net, From, To, Bowser);
		TestEqual(TEXT("a 2.5 m lane is too narrow for a 2.37 m bowser with its margins"),
			static_cast<int32>(Plan.Result), static_cast<int32>(ERouteResult::TooNarrow));
		TestTrue(TEXT("and the refusal names the edge that refused it"), Plan.RejectedEdge.IsSet());
	}

	// THE CORNER: the swept path.
	{
		URoadNetwork* Net = TurnNorth(URoadProfile::MakeServiceRoadTransient(300.0), From, To);
		const FRoutePlan BowserPlan = Route(*Net, From, To, Bowser);
		if (TestTrue(TEXT("the bowser turns at a Narrow junction"), BowserPlan.IsValid()))
		{
			FSpeedProfile Profile;
			Profile.Build(BowserPlan.Polyline, Bowser.Chassis);
			// The drivability AUTHORITY over the whole admitted route, not a per-edge restatement
			// of its rule (memory: FSpeedProfile is the drivability authority).
			TestFalse(TEXT("and every metre of the route it was given is inside its lock"), Profile.WasTighterThanLock());
		}
		const FRoutePlan RigPlan = Route(*Net, From, To, Rig);
		TestEqual(TEXT("the rig's trailer cannot get round a Narrow corner"),
			static_cast<int32>(RigPlan.Result), static_cast<int32>(ERouteResult::TooNarrow));
		TestTrue(TEXT("and it says which edge"), RigPlan.RejectedEdge.IsSet());
		const FGuidelineEdge* Rejected = Net->GetGuidelineEdge(RigPlan.RejectedEdge);
		TestTrue(TEXT("which is the corner, not a lane"), Rejected != nullptr && !Rejected->DerivedFrom.IsSet());
	}

	// UNMEASURED EDGES GATE NOTHING (review focus 1): hand-made and saved edges keep routing.
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		const FGuidelineNodeId A = Net->AddGuidelineNode(FVector2D(0.0, 0.0), false);
		const FGuidelineNodeId B = Net->AddGuidelineNode(FVector2D(1000.0, 0.0), false);
		FGuidelineEdge Edge;
		Edge.A = A;
		Edge.B = B;
		Edge.Control = FVector2D(500.0, 200.0);
		Edge.AllowedTraffic = FTrafficMask::Only(ETraversalClass::GroundVehicle);
		Net->AddGuidelineEdge(MoveTemp(Edge));
		TestTrue(TEXT("an edge nobody measured admits even the rig"), Route(*Net, A, B, Rig).IsValid());
	}

	// NO VEHICLE, NO GATE: the query every caller made before this stays exactly as it was.
	{
		URoadNetwork* Net = TurnNorth(URoadProfile::MakeServiceRoadTransient(300.0), From, To);
		FRouteQuery Query = FRouteQuery::For(ERouteErrand::GraphProbe, From, To, 0.0, ETraversalClass::GroundVehicle);
		TestTrue(TEXT("a query with no vehicle is not gated"), RouteSearch::Find(*Net, Query).IsValid());
	}
	return true;
}

#endif
