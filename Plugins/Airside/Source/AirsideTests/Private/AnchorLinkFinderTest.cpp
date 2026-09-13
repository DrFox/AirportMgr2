#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Build/AnchorLink.h"
#include "Build/AnchorLinkFinder.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Solve/GuidelineGeom.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** A straight east-west guideline across Y, admitting Class. Returns its handle. */
	FGuidelineEdgeId LayStraight(URoadNetwork& Net, double Y, ETraversalClass Class,
		FGuidelineNodeId& OutWest, FGuidelineNodeId& OutEast)
	{
		OutWest = Net.AddGuidelineNode(FVector2D(-10000.0, Y));
		OutEast = Net.AddGuidelineNode(FVector2D(10000.0, Y));

		FGuidelineEdge Edge;
		Edge.A = OutWest;
		Edge.B = OutEast;
		Edge.Control = FVector2D(0.0, Y);
		Edge.AllowedTraffic = FTrafficMask::Only(Class);
		Edge.Direction = EGuidelineDir::Bidirectional;
		Edge.bDerived = true;
		return Net.AddGuidelineEdge(MoveTemp(Edge));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnchorLinkFinderTest,
	"Airside.Build.AnchorLinkFinder",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAnchorLinkFinderTest::RunTest(const FString& Parameters)
{
	// FRayLinkFinder: the first guideline strictly ahead along Dir, never behind it, never
	// past Reach, and never one incident to an already-anchored node - see ILinkFinder's own
	// doc comment for why that last exclusion needs no separate mark on the edge.
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		FGuidelineNodeId West, East;
		const FGuidelineEdgeId RoadId =
			LayStraight(*Net, 0.0, ETraversalClass::Aircraft, West, East);

		FPendingLink Link;
		Link.Kind = ELinkKind::Ray;
		Link.At = FVector2D(0.0, 4000.0);
		Link.Dir = FVector2D(0.0, -1.0);
		Link.Class = ETraversalClass::Aircraft;
		Link.Reach = 20000.0;

		const FRayLinkFinder Finder;
		TSet<FGuidelineNodeId> AnchorNodes;

		FLinkHit Hit;
		TestTrue(TEXT("the ray strikes the guideline ahead of it"),
			Finder.Find(*Net, Link, AnchorNodes, Hit));
		TestEqual(TEXT("and reports that guideline"), Hit.Edge, RoadId);
		TestEqual(TEXT("at its midpoint, straight down from the anchor"), Hit.Param, 0.5, 0.01);

		FLinkHit Behind;
		FPendingLink Reversed = Link;
		Reversed.Dir = FVector2D(0.0, 1.0);
		TestFalse(TEXT("a ray pointed away from the guideline finds nothing"),
			Finder.Find(*Net, Reversed, AnchorNodes, Behind));

		FLinkHit TooFar;
		FPendingLink ShortReach = Link;
		ShortReach.Reach = 1000.0;
		TestFalse(TEXT("a ray capped short of the guideline finds nothing"),
			Finder.Find(*Net, ShortReach, AnchorNodes, TooFar));

		FLinkHit Excluded;
		TSet<FGuidelineNodeId> Anchored = { West };
		TestFalse(TEXT("a guideline with an anchored endpoint is never a target"),
			Finder.Find(*Net, Link, Anchored, Excluded));
	}

	// FProximityLinkFinder: nearest guideline of the link's class, in any direction - so,
	// unlike the ray, a target BEHIND the link's own Dir still counts.
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		FGuidelineNodeId West, East;
		const FGuidelineEdgeId RoadId =
			LayStraight(*Net, 0.0, ETraversalClass::GroundVehicle, West, East);

		FPendingLink Link;
		Link.Kind = ELinkKind::Proximity;
		Link.At = FVector2D(0.0, 4000.0);
		Link.Dir = FVector2D(0.0, 1.0); // Facing AWAY - proximity must not care.
		Link.Class = ETraversalClass::GroundVehicle;
		Link.Reach = 20000.0;

		const FProximityLinkFinder Finder;
		TSet<FGuidelineNodeId> AnchorNodes;

		FLinkHit Hit;
		TestTrue(TEXT("proximity finds the guideline despite facing away from it"),
			Finder.Find(*Net, Link, AnchorNodes, Hit));
		TestEqual(TEXT("and reports it"), Hit.Edge, RoadId);

		// Within LeadInWeldTolerance of the guideline: a "hit" this close would only ever be
		// welded back to nothing, so it is refused rather than reported.
		FPendingLink AlmostOn = Link;
		AlmostOn.At = FVector2D(0.0, FAnchorLink::LeadInWeldTolerance * 0.5);
		FLinkHit TooClose;
		TestFalse(TEXT("a point already on the guideline is refused, not reported"),
			Finder.Find(*Net, AlmostOn, AnchorNodes, TooClose));

		FPendingLink ShortReach = Link;
		ShortReach.Reach = 1000.0;
		FLinkHit TooFar;
		TestFalse(TEXT("a reach capped short of the guideline finds nothing"),
			Finder.Find(*Net, ShortReach, AnchorNodes, TooFar));
	}

	// FLaneLinkFinder: nearest approach between the LANE (Link.Lane) and a road, not between
	// the anchor point and the road - the lane has no single point to link from.
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());

		// The lane: a short side of a stand's service ring, well clear of the road.
		const FGuidelineNodeId LaneA = Net->AddGuidelineNode(FVector2D(0.0, 0.0));
		const FGuidelineNodeId LaneB = Net->AddGuidelineNode(FVector2D(1000.0, 0.0));
		FGuidelineEdge LaneEdge;
		LaneEdge.A = LaneA;
		LaneEdge.B = LaneB;
		LaneEdge.Control = FVector2D(500.0, 0.0);
		LaneEdge.AllowedTraffic = FTrafficMask::Only(ETraversalClass::GroundVehicle);
		LaneEdge.bDerived = true;
		const FGuidelineEdgeId LaneId = Net->AddGuidelineEdge(MoveTemp(LaneEdge));

		// The road: drawn PARALLEL to the lane, 5000 uu away - the case this finder exists
		// for, where a corner-to-corner measure would answer wrong.
		FGuidelineNodeId RoadWest, RoadEast;
		const FGuidelineEdgeId RoadId =
			LayStraight(*Net, 5000.0, ETraversalClass::GroundVehicle, RoadWest, RoadEast);

		FPendingLink Link;
		Link.Kind = ELinkKind::Lane;
		Link.Lane = { LaneId };
		Link.Class = ETraversalClass::GroundVehicle;
		Link.Reach = 20000.0;

		const FLaneLinkFinder Finder;
		// The lane's own nodes are excluded, exactly as FAnchorLink::Gather excludes every
		// node of every service lane and spur - otherwise the lane would be a candidate for
		// itself at zero distance.
		TSet<FGuidelineNodeId> AnchorNodes = { LaneA, LaneB };

		FLinkHit Hit;
		TestTrue(TEXT("the lane finds the parallel road, not itself"),
			Finder.Find(*Net, Link, AnchorNodes, Hit));
		TestEqual(TEXT("the road is reported as the target edge"), Hit.Edge, RoadId);
		TestEqual(TEXT("and the lane side as the source edge"), Hit.LaneEdge, LaneId);

		const FGuidelineNode* RoadWestNode = Net->GetGuidelineNode(RoadWest);
		const FGuidelineNode* RoadEastNode = Net->GetGuidelineNode(RoadEast);
		if (TestNotNull(TEXT("road west resolves"), RoadWestNode) &&
			TestNotNull(TEXT("road east resolves"), RoadEastNode))
		{
			const FVector2D RoadPoint = GuidelineGeom::Eval(
				RoadWestNode->Position, FVector2D(0.0, 5000.0), RoadEastNode->Position, Hit.Param);
			TestEqual(TEXT("the road point measured is on the road's line"), RoadPoint.Y, 5000.0, 0.01);
		}

		// The lane itself is not a candidate road: excluding it is what AnchorNodes is for,
		// and without that exclusion this would report a zero-distance hit on itself.
		FPendingLink NoRoad;
		NoRoad.Kind = ELinkKind::Lane;
		NoRoad.Lane = { LaneId };
		NoRoad.Class = ETraversalClass::GroundVehicle;
		NoRoad.Reach = 20000.0;

		FLinkHit ShortOfIt;
		TSet<FGuidelineNodeId> ExcludeRoad = { LaneA, LaneB, RoadWest, RoadEast };
		TestFalse(TEXT("with the road itself excluded, nothing is left to join"),
			Finder.Find(*Net, NoRoad, ExcludeRoad, ShortOfIt));

		FPendingLink ShortReach = Link;
		ShortReach.Reach = 1000.0;
		FLinkHit TooFar;
		TestFalse(TEXT("a reach capped short of the road finds nothing"),
			Finder.Find(*Net, ShortReach, AnchorNodes, TooFar));

		// An empty Lane array is the tag for "this is not actually a lane link" - the finder
		// must not silently fall back to measuring the anchor point itself.
		FPendingLink EmptyLane = Link;
		EmptyLane.Lane.Empty();
		FLinkHit Nothing;
		TestFalse(TEXT("a lane link with no sides to measure from finds nothing"),
			Finder.Find(*Net, EmptyLane, AnchorNodes, Nothing));
	}

	// LinkFinderFor: dispatches to the strategy matching Kind. Proved by BEHAVIOUR that only
	// the right strategy can produce, not by a result a wrong one could match by accident:
	//
	// - Ray fails facing away and succeeds facing toward the SAME guideline - a fingerprint
	//   neither Proximity (succeeds either way) nor Lane (Link.Lane is empty here, so it
	//   always fails) can reproduce. Testing only the "away" half was the bug: a switch that
	//   wrongly returned the Lane finder for Ray would ALSO report false there, for the
	//   wrong reason, and the test would not have noticed.
	// - Lane's hit alone is not proof either, since a wrongly-dispatched Ray or Proximity
	//   finder can still land on the same edge by its own rule. FLinkHit::LaneEdge is the
	//   fingerprint instead: only FLaneLinkFinder ever writes it.
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		FGuidelineNodeId West, East;
		LayStraight(*Net, 0.0, ETraversalClass::GroundVehicle, West, East);

		FPendingLink RayLink;
		RayLink.Kind = ELinkKind::Ray;
		RayLink.At = FVector2D(0.0, 4000.0);
		RayLink.Dir = FVector2D(0.0, 1.0); // Away from the guideline.
		RayLink.Class = ETraversalClass::GroundVehicle;
		RayLink.Reach = 20000.0;

		FPendingLink ProximityLink = RayLink;
		ProximityLink.Kind = ELinkKind::Proximity;

		TSet<FGuidelineNodeId> AnchorNodes;
		FLinkHit RayHit, ProximityHit;
		TestFalse(TEXT("LinkFinderFor(Ray) behaves like a ray: nothing behind it"),
			LinkFinderFor(RayLink.Kind).Find(*Net, RayLink, AnchorNodes, RayHit));
		TestTrue(TEXT("LinkFinderFor(Proximity) behaves omnidirectionally"),
			LinkFinderFor(ProximityLink.Kind).Find(*Net, ProximityLink, AnchorNodes, ProximityHit));

		FPendingLink RayTowardLink = RayLink;
		RayTowardLink.Dir = FVector2D(0.0, -1.0); // Toward the guideline.
		FLinkHit RayTowardHit;
		TestTrue(TEXT("LinkFinderFor(Ray) still finds the same guideline when facing it"),
			LinkFinderFor(RayTowardLink.Kind).Find(*Net, RayTowardLink, AnchorNodes, RayTowardHit));
	}

	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());

		const FGuidelineNodeId LaneA = Net->AddGuidelineNode(FVector2D(0.0, 0.0));
		const FGuidelineNodeId LaneB = Net->AddGuidelineNode(FVector2D(1000.0, 0.0));
		FGuidelineEdge LaneEdge;
		LaneEdge.A = LaneA;
		LaneEdge.B = LaneB;
		LaneEdge.Control = FVector2D(500.0, 0.0);
		LaneEdge.AllowedTraffic = FTrafficMask::Only(ETraversalClass::GroundVehicle);
		LaneEdge.bDerived = true;
		const FGuidelineEdgeId LaneId = Net->AddGuidelineEdge(MoveTemp(LaneEdge));

		FGuidelineNodeId RoadWest, RoadEast;
		LayStraight(*Net, 5000.0, ETraversalClass::GroundVehicle, RoadWest, RoadEast);

		FPendingLink LaneLink;
		LaneLink.Kind = ELinkKind::Lane;
		LaneLink.Lane = { LaneId };
		LaneLink.Class = ETraversalClass::GroundVehicle;
		LaneLink.Reach = 20000.0;

		TSet<FGuidelineNodeId> AnchorNodes = { LaneA, LaneB };
		FLinkHit LaneHit;
		TestTrue(TEXT("LinkFinderFor(Lane) finds the road"),
			LinkFinderFor(LaneLink.Kind).Find(*Net, LaneLink, AnchorNodes, LaneHit));
		TestTrue(TEXT("and only the Lane strategy ever sets LaneEdge"), LaneHit.LaneEdge.IsSet());
		TestEqual(TEXT("naming the lane side it measured from"), LaneHit.LaneEdge, LaneId);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
