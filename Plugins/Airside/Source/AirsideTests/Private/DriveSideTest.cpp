#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Content/AirsideSettings.h"
#include "Misc/AutomationTest.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadAgent.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadTraffic.h"
#include "Model/RoutePolicy.h"
#include "Model/RouteSearch.h"
#include "Present/AirsideTraffic.h"
#include "Present/RoadNetworkActor.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

// The drive side is ONE airport-wide setting (spec 2026-09-23 §2), stored on the network so
// it saves and undoes with everything else the player built.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDriveSideDefaultTest,
	"Airside.Model.DriveSide.Default",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDriveSideDefaultTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	TestEqual(TEXT("right-hand traffic until the player says otherwise"),
		static_cast<int32>(Net->GetDriveSide()), static_cast<int32>(EDriveSide::Right));

	const uint32 Before = Net->GetEditRevision();
	TestTrue(TEXT("a change reports itself"), Net->SetDriveSide(EDriveSide::Left));
	TestEqual(TEXT("but moves no node or segment, so the road-graph clock (ghost, deletion plan) stands"),
		static_cast<int64>(Net->GetEditRevision()), static_cast<int64>(Before));
	TestFalse(TEXT("setting the side it already has is a no-op, so no undo step"),
		Net->SetDriveSide(EDriveSide::Left));

	// Profiles are authored for right-hand traffic; OffsetFor is where the side is applied.
	FProfileGuideline Lane;
	Lane.CentreOffset = -150.0;
	TestEqual(TEXT("right drive keeps the authored offset"), Lane.OffsetFor(EDriveSide::Right), -150.0);
	TestEqual(TEXT("left drive mirrors it across the centreline"), Lane.OffsetFor(EDriveSide::Left), 150.0);

	FProfileGuideline Centre;
	TestEqual(TEXT("a centreline guideline does not move under either side"), Centre.OffsetFor(EDriveSide::Left), 0.0);
	return true;
}

namespace DriveSideTest
{
	/** Y of the A->B lane of Seg's first sample, or NaN. */
	double ForwardLaneY(const URoadNetwork& Net, FRoadSegmentId Seg, FGuidelineNodeId* OutA = nullptr, FGuidelineNodeId* OutB = nullptr)
	{
		for (const FGuidelineEdge& Edge : Net.GetGuidelineEdges())
		{
			if (Edge.bAlive && Edge.DerivedFrom == Seg && Edge.Direction == EGuidelineDir::AToB)
			{
				if (OutA) { *OutA = Edge.A; }
				if (OutB) { *OutB = Edge.B; }
				return Net.GetGuidelineNode(Edge.A)->Position.Y;
			}
		}
		return NAN;
	}
}

// Spec test 9: the SEAM. The model's SetDriveSide is tested above; this is the actor's
// forwarder, the facade's undo step and rebuild, and an agent already on the road when the
// side flips - the composition, not the struct.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDriveSideCompositionTest,
	"Airside.Present.DriveSide.Composition",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDriveSideCompositionTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("the actor"), Actor)) { return false; }
	Actor->ServiceRoadProfile = URoadProfile::MakeServiceRoadTransient();

	const int32 A = Actor->PlaceNode(FVector2D(0.0, 0.0));
	const int32 B = Actor->PlaceNode(FVector2D(40000.0, 0.0));
	if (!TestTrue(TEXT("a service road connects"), Actor->ConnectNodes(A, B, ERoadKind::ServiceRoad))) { return false; }
	URoadNetwork& Net = *Actor->Network;
	const FRoadSegmentId Seg = Net.SegmentIdAt(0);

	FGuidelineNodeId Start, Goal;
	TestTrue(TEXT("right-hand: the A->B lane is at +Y, screen-right of +X"),
		DriveSideTest::ForwardLaneY(Net, Seg, &Start, &Goal) > 0.0);

	// #312: was a hand-built FRouteQuery that skipped AvoidRunways.
	const FRoutePlan Plan = TestGraph::Probe(Net, Start, Goal, ETraversalClass::GroundVehicle);
	if (!TestTrue(TEXT("a route down the road"), Plan.IsValid())) { return false; }
	if (!TestTrue(TEXT("a truck is dispatched"),
		Actor->DispatchAgent(Plan, UAirsideSettings::ResolveDefaultVehicle(), ETraversalClass::GroundVehicle))) { return false; }
	// WELL INTO THE ROAD before the flip, far from any lane node. A straight lane is ONE edge
	// with nodes only at its cut ends, and the first version of the rejoin searched NODES
	// near the truck - it passed a test that flipped 60 ticks in, beside the start node, and
	// stranded every truck anywhere else (review of 2026-09-23).
	for (int32 Tick = 0; Tick < 20000 && Actor->GetTraffic()->LastAgentPositionForTest().X < 15000.0; ++Tick)
	{
		Actor->Tick(1.0f / 30.0f);
	}
	TestTrue(TEXT("the truck is mid-road, tens of metres from any lane node"),
		Actor->GetTraffic()->LastAgentPositionForTest().X >= 15000.0);

	TestTrue(TEXT("the flip is an edit"), Actor->SetDriveSide(EDriveSide::Left));
	TestFalse(TEXT("flipping to the side it has is refused, so no empty undo step"), Actor->SetDriveSide(EDriveSide::Left));
	TestTrue(TEXT("the network re-derived its lanes: A->B now at -Y"), DriveSideTest::ForwardLaneY(Net, Seg) < 0.0);

	// The truck that was mid-road when the side flipped. Its plan named nodes that no longer
	// exist; wherever it ends up, it must be on the lane the network now has - not driving the
	// old right-hand line through the new left-hand road.
	bool bParked = false;
	for (int32 Tick = 0; Tick < 20000 && !bParked; ++Tick)
	{
		Actor->Tick(1.0f / 30.0f);
		bParked = Actor->GetTraffic()->LastAgentPhaseForTest() == EAgentPhase::Parked
			|| Actor->GetTraffic()->GetAgentCount() == 0;
	}
	const FVector2D Final = Actor->GetTraffic()->LastAgentPositionForTest();
	UE_LOG(LogTemp, Display, TEXT("DriveSide.Composition: after the flip the truck ended at (%.0f, %.0f), phase %d, %d agent(s)"),
		Final.X, Final.Y, static_cast<int32>(Actor->GetTraffic()->LastAgentPhaseForTest()), Actor->GetTraffic()->GetAgentCount());
	TestTrue(TEXT("the truck stopped or was retired rather than driving on for ever"), bParked);
	TestTrue(TEXT("and it is not on the old right-hand line"), Final.Y < 100.0);
	TestTrue(TEXT("and it carried on to the far end rather than stopping where it was"), Final.X > 35000.0);

	TestTrue(TEXT("undo takes the flip back"), Actor->Undo());
	TestEqual(TEXT("right-hand again"),
		static_cast<int32>(Actor->Network->GetDriveSide()), static_cast<int32>(EDriveSide::Right));
	TestTrue(TEXT("and the lanes with it"), DriveSideTest::ForwardLaneY(*Actor->Network, Seg) > 0.0);
	return true;
}

// ISSUE #429 (and #314's second item): THE FLIP IS A REJOIN, LIKE ITS TWO SIBLINGS. It was RestartTaxi on the new lane
// with the pose's position re-seated by hand - so every truck on the road stopped dead on the flip's frame, and until
// its next Advance was posed facing EAST (RestartTaxi's fallback pose is a bare FAgentMotion). A paused game showed it
// so for as long as the pause lasted. Measured with NO tick after the flip, which is the paused frame, and on a road
// running NORTH: on Composition's east-running road heading 0 is the right answer by accident, and the bug hid there.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDriveSideFlipKeepsSpeedAndHeadingTest,
	"Airside.Present.DriveSide.FlipKeepsSpeedAndHeading",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDriveSideFlipKeepsSpeedAndHeadingTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("the actor"), Actor)) { return false; }
	Actor->ServiceRoadProfile = URoadProfile::MakeServiceRoadTransient();

	const int32 A = Actor->PlaceNode(FVector2D(0.0, 0.0));
	const int32 B = Actor->PlaceNode(FVector2D(0.0, 40000.0));
	if (!TestTrue(TEXT("a service road north connects"), Actor->ConnectNodes(A, B, ERoadKind::ServiceRoad))) { return false; }
	URoadNetwork& Net = *Actor->Network;
	const FRoadSegmentId Seg = Net.SegmentIdAt(0);

	// THE NORTHBOUND LANE: the A->B guideline derived from the road, whichever side it is on.
	FGuidelineNodeId Start, Goal;
	for (const FGuidelineEdge& Edge : Net.GetGuidelineEdges())
	{
		if (Edge.bAlive && Edge.DerivedFrom == Seg && Edge.Direction == EGuidelineDir::AToB)
		{
			Start = Edge.A;
			Goal = Edge.B;
			break;
		}
	}
	if (!TestTrue(TEXT("the road derived a one-way lane"), Start.IsSet() && Goal.IsSet())) { return false; }
	const bool bNorthbound = Net.GetGuidelineNode(Goal)->Position.Y > Net.GetGuidelineNode(Start)->Position.Y;
	const FRoutePlan Plan = TestGraph::Probe(Net, bNorthbound ? Start : Goal, bNorthbound ? Goal : Start,
		ETraversalClass::GroundVehicle);
	if (!TestTrue(TEXT("a route north up the road"), Plan.IsValid())) { return false; }
	if (!TestTrue(TEXT("a truck is dispatched"),
		Actor->DispatchAgent(Plan, UAirsideSettings::ResolveDefaultVehicle(), ETraversalClass::GroundVehicle))) { return false; }

	// WELL INTO THE ROAD AND ROLLING, as Composition's truck is, far from any lane node.
	for (int32 Tick = 0; Tick < 20000 && Actor->GetTraffic()->LastAgentPositionForTest().Y < 15000.0; ++Tick)
	{
		Actor->Tick(1.0f / 30.0f);
	}
	const UGroundTraffic* Model = Actor->GetTraffic()->GetModel();
	if (!TestTrue(TEXT("one truck on the road"), Model != nullptr && Model->GetAgentCount() == 1)) { return false; }
	const FRoadAgent& Was = Model->GetAgents()[0];
	const double SpeedWas = Was.Follower.Speed;
	const double HeadingWas = Was.LastMotion.Heading;
	const FVector2D WasAt = Was.LastMotion.Position;
	if (!TestTrue(FString::Printf(TEXT("mid-road and rolling north (%.0f uu/s, heading %.0f deg)"),
		SpeedWas, FMath::RadiansToDegrees(HeadingWas)),
		WasAt.Y >= 15000.0 && SpeedWas > 100.0
		&& FMath::Abs(FMath::UnwindRadians(HeadingWas - UE_DOUBLE_HALF_PI)) < FMath::DegreesToRadians(5.0))) { return false; }

	if (!TestTrue(TEXT("the flip is an edit"), Actor->SetDriveSide(EDriveSide::Left))) { return false; }

	// NO TICK: the paused frame.
	const FRoadAgent& Now = Model->GetAgents()[0];
	TestEqual(TEXT("still taxiing - rejoined, not stranded"), Now.Phase, EAgentPhase::Taxiing);
	TestTrue(FString::Printf(TEXT("the truck carries on at its speed (%.0f uu/s, was %.0f) - not stopped dead"),
		Now.Follower.Speed, SpeedWas), FMath::IsNearlyEqual(Now.Follower.Speed, SpeedWas, 1.0));
	TestTrue(FString::Printf(TEXT("and is posed facing north as it was (%.0f deg), not east"),
		FMath::RadiansToDegrees(Now.LastMotion.Heading)),
		FMath::Abs(FMath::UnwindRadians(Now.LastMotion.Heading - HeadingWas)) < FMath::DegreesToRadians(1.0));
	TestTrue(FString::Printf(TEXT("on the other lane now: moved %.0f uu across the road, %.0f uu along it"),
		FMath::Abs(Now.LastMotion.Position.X - WasAt.X), FMath::Abs(Now.LastMotion.Position.Y - WasAt.Y)),
		FMath::Abs(Now.LastMotion.Position.X - WasAt.X) > 100.0 && FMath::Abs(Now.LastMotion.Position.Y - WasAt.Y) < 100.0);
	return true;
}

#endif
