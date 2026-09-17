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

	// THE FLaneLinkFinder BLOCK IS DELETED, 2026-09-16, with the finder it measured.
	//
	// It asserted that a stand's whole LANE was measured against a road polyline to polyline -
	// which existed because a ring declared no entrance, so the pass had to find a point on the
	// lane to join at. A stand declares its entries now (UEntityDefinition::ServiceLane), so
	// the FROM end of the link is a node and FProximityLinkFinder above is the whole rule. The
	// property that block really protected - a road drawn PARALLEL to a lane being measured
	// side to side rather than corner to corner - moved to
	// Airside.Build.ServiceLaneEntersOnEverySideWithinReach, which measures it on the real
	// stand: a road alongside enters at BOTH near corners, not once at whichever end came
	// first. GuidelineGeom::NearestBetweenPolylines keeps its own tests in
	// Airside.Solve.GuidelineGeom.

	// LinkFinderFor: dispatches to the strategy matching Kind. Proved by BEHAVIOUR that only
	// the right strategy can produce, not by a result a wrong one could match by accident:
	// Ray fails facing away and succeeds facing toward the SAME guideline, which Proximity
	// (which succeeds either way) cannot reproduce. Testing only the "away" half was the bug:
	// a switch that wrongly returned some other finder for Ray would ALSO report false there,
	// for the wrong reason, and the test would not have noticed.
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

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
