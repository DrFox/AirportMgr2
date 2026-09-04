#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Build/AnchorLink.h"
#include "Entities/AircraftType.h"
#include "Entities/EntityDefinition.h"
#include "Model/RoadEntity.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RouteSearch.h"
#include "Solve/GuidelineGeom.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** A straight east-west taxiway across y = 0, admitting everything. */
	void LayTaxiway(URoadNetwork& Net, FGuidelineNodeId& OutWest, FGuidelineNodeId& OutEast)
	{
		OutWest = Net.AddGuidelineNode(FVector2D(-10000.0, 0.0));
		OutEast = Net.AddGuidelineNode(FVector2D(10000.0, 0.0));

		FGuidelineEdge Edge;
		Edge.A = OutWest;
		Edge.B = OutEast;
		Edge.Control = FVector2D(0.0, 0.0);
		Edge.AllowedTraffic = FTrafficMask::All();
		Edge.Direction = EGuidelineDir::Bidirectional;
		Edge.Width = 2300.0;
		Edge.bDerived = true;
		Net.AddGuidelineEdge(MoveTemp(Edge));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAnchorLinkTest,
	"Airside.Build.AnchorLink",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAnchorLinkTest::RunTest(const FString& Parameters)
{
	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();

	// The defect this whole file exists for: PlaceEntity resolves anchors to guideline
	// nodes and connects NONE of them, so before FAnchorLink runs a stand is an island and
	// no route can reach it. Asserted directly, because "the search found nothing" would
	// otherwise be indistinguishable from a broken search.
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		FGuidelineNodeId West;
		FGuidelineNodeId East;
		LayTaxiway(*Net, West, East);

		// North of the taxiway, rotated +90 degrees.
		//
		// The heading arithmetic is why this fixture is spelled out. A stand's lead-in
		// leaves along its heading PLUS 180, because +X faces the terminal and the lead-in
		// runs back out to the movement area. So +90 puts the ray at 270 degrees - straight
		// down -Y, at the taxiway. Placing at -90 aims it at +Y instead and joins nothing,
		// which is the case asserted further down.
		const FEntityInstanceId Placed =
			Net->PlaceEntity(Stand, FVector2D(0.0, 4000.0), UE_DOUBLE_PI * 0.5);

		const FEntityInstance* Instance = Net->GetEntity(Placed);
		if (!TestNotNull(TEXT("the stand resolves"), Instance))
		{
			return false;
		}
		const FGuidelineNodeId PoseNode = Instance->PoseNode;

		const FGuidelineNode* Stop = Net->GetGuidelineNode(PoseNode);
		if (!TestNotNull(TEXT("the stop position resolved to a node"), Stop))
		{
			return false;
		}
		TestEqual(TEXT("and it starts an island - nothing joins it"), Stop->Incident.Num(), 0);

		const int32 Joined = FAnchorLink::Build(*Net);
		TestTrue(TEXT("at least the stop position joins"), Joined >= 1);

		// Re-read: joining reallocates the node array.
		Stop = Net->GetGuidelineNode(PoseNode);
		if (!TestNotNull(TEXT("the stop position still resolves"), Stop))
		{
			return false;
		}
		TestEqual(TEXT("the stop position now has exactly one lead-in"), Stop->Incident.Num(), 1);

		// The point of the whole exercise: an aircraft can now be routed to the stand.
		FRouteQuery Query;
		Query.Start = West;
		Query.Goal = PoseNode;
		Query.Class = ETraversalClass::Aircraft;

		const FRoutePlan Plan = RouteSearch::Find(*Net, Query);
		if (!TestTrue(TEXT("an aircraft can now route to the stand"), Plan.IsValid())
			|| Plan.Steps.Num() == 0)
		{
			// Returning rather than reading on. A refused plan has an EMPTY polyline, and
			// Last() on it is an out-of-bounds read that takes the whole suite down with
			// it - which is exactly how this test first failed.
			return false;
		}

		TestTrue(TEXT("and the route ends at the nose stop"),
			FVector2D::Distance(Plan.Polyline.Last(), Stop->Position) < 1.0);

		// The lead-in ends where the SWEEP takes over, not on the taxiway.
		//
		// It used to end on the centreline, and that was the defect: the ray meets the
		// taxiway square-on, so joining there made a 90 degree corner. The straight painted
		// line now stops short and two arcs carry the turn - see Airside.Build.LeadInSweep,
		// which measures the angle rather than the topology.
		const FGuidelineNode* LeadEnd = Net->GetGuidelineNode(Plan.Steps.Last().bReversed
			? Net->GetGuidelineEdge(Plan.Steps.Last().Edge)->B
			: Net->GetGuidelineEdge(Plan.Steps.Last().Edge)->A);
		if (TestNotNull(TEXT("the lead-in has a far end"), LeadEnd))
		{
			TestEqual(TEXT("still directly above the stand, on the painted line"),
				LeadEnd->Position.X, 0.0);
			TestTrue(TEXT("but set BACK from the taxiway, leaving room for the turn"),
				LeadEnd->Position.Y > 1.0);

			// The lead-in plus one sweep each way.
			TestEqual(TEXT("three edges meet where the sweeps begin"), LeadEnd->Incident.Num(), 3);
		}

		// The taxiway was SPLIT rather than merely touched, and to BOTH sides: an aircraft
		// taxiing past the stand must still get through. Asserted as a route rather than
		// inferred from a node's degree, because through-traffic is the thing that actually
		// matters and a correct-looking degree can still be a severed taxiway.
		{
			FRouteQuery Through;
			Through.Start = West;
			Through.Goal = East;
			Through.Class = ETraversalClass::Aircraft;

			TestTrue(TEXT("traffic still passes the stand from end to end"),
				RouteSearch::Find(*Net, Through).IsValid());
		}
	}

	// Facing AWAY. A ray, not a line - the reason the rule is "along the heading" and not
	// "nearest guideline". A stand backing onto a taxiway must not silently connect to it.
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		FGuidelineNodeId West;
		FGuidelineNodeId East;
		LayTaxiway(*Net, West, East);

		// Rotated -90, so the nose stop's lead-in casts along world +Y - directly away from
		// the taxiway it is sitting beside.
		const FEntityInstanceId Placed =
			Net->PlaceEntity(Stand, FVector2D(0.0, 4000.0), -UE_DOUBLE_PI * 0.5);

		FAnchorLink::Build(*Net);

		const FGuidelineNode* Stop = Net->GetGuidelineNode(Net->GetEntity(Placed)->PoseNode);
		if (TestNotNull(TEXT("the stop position resolves"), Stop))
		{
			TestEqual(TEXT("a stand facing away joins nothing"), Stop->Incident.Num(), 0);
		}
	}

	// Out of range. An anchor nowhere near a taxiway stays unjoined rather than trailing a
	// kilometre-long lead-in across the airport.
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		FGuidelineNodeId West;
		FGuidelineNodeId East;
		LayTaxiway(*Net, West, East);

		const FEntityInstanceId Placed = Net->PlaceEntity(
			Stand, FVector2D(0.0, FAnchorLink::DefaultMaxLeadIn * 2.0), UE_DOUBLE_PI * 0.5);

		FAnchorLink::Build(*Net);

		const FGuidelineNode* Stop = Net->GetGuidelineNode(Net->GetEntity(Placed)->PoseNode);
		if (TestNotNull(TEXT("the stop position resolves"), Stop))
		{
			TestEqual(TEXT("a distant stand joins nothing"), Stop->Incident.Num(), 0);
		}
	}

	// Running twice must not double up. The graph is rebuilt on every edit, so this is the
	// ordinary case rather than an unusual one.
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		FGuidelineNodeId West;
		FGuidelineNodeId East;
		LayTaxiway(*Net, West, East);

		const FEntityInstanceId Placed =
			Net->PlaceEntity(Stand, FVector2D(0.0, 4000.0), UE_DOUBLE_PI * 0.5);

		FAnchorLink::Build(*Net);
		const int32 SecondPass = FAnchorLink::Build(*Net);

		TestEqual(TEXT("a second pass joins nothing new"), SecondPass, 0);

		const FGuidelineNode* Stop = Net->GetGuidelineNode(Net->GetEntity(Placed)->PoseNode);
		if (TestNotNull(TEXT("the stop position resolves"), Stop))
		{
			TestEqual(TEXT("and leaves one lead-in, not two"), Stop->Incident.Num(), 1);
		}
	}

	// The stand's design aircraft limits its lead-in, so a widebody aimed at a Code C stand
	// is told it is TOO WIDE rather than that the airport is disconnected.
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		FGuidelineNodeId West;
		FGuidelineNodeId East;
		LayTaxiway(*Net, West, East);

		const FEntityInstanceId Placed =
			Net->PlaceEntity(Stand, FVector2D(0.0, 4000.0), UE_DOUBLE_PI * 0.5);
		FAnchorLink::Build(*Net);

		FRouteQuery Query;
		Query.Start = West;
		Query.Goal = Net->GetEntity(Placed)->PoseNode;
		Query.Class = ETraversalClass::Aircraft;
		Query.Wingspan = Stand->DesignAircraft->Footprint.Wingspan;

		TestTrue(TEXT("the stand's own design aircraft fits"), RouteSearch::Find(*Net, Query).IsValid());

		Query.Wingspan = Stand->DesignAircraft->Footprint.Wingspan * 2.0;
		const FRoutePlan Refused = RouteSearch::Find(*Net, Query);
		TestFalse(TEXT("twice that does not"), Refused.IsValid());
		TestEqual(TEXT("and is reported as too wide, not unreachable"),
			Refused.Result, ERouteResult::TooWide);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGuidelineGeomTest,
	"Airside.Solve.GuidelineGeom",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGuidelineGeomTest::RunTest(const FString& Parameters)
{
	const FVector2D A(0.0, 0.0);
	const FVector2D B(1000.0, 0.0);

	// A straight guideline costs two points, not sixteen. The common case by a long way,
	// and fifteen interior points identical to the line through them is work spent to
	// produce the same answer.
	{
		TArray<FVector2D> Points;
		GuidelineGeom::Sample(A, (A + B) * 0.5, B, Points);
		TestEqual(TEXT("a straight guideline samples to two points"), Points.Num(), 2);
		TestEqual(TEXT("starting at A"), Points[0], A);
		TestEqual(TEXT("ending at B"), Points.Last(), B);
	}

	// Endpoints must be EXACT, not near. This is what lets a route weld consecutive edges
	// by dropping one point - if the ends were merely close, every join would leave a
	// sub-uu gap that no length assertion would ever notice.
	{
		const FVector2D Control(500.0, 900.0);
		TArray<FVector2D> Points;
		GuidelineGeom::Sample(A, Control, B, Points);

		TestEqual(TEXT("a curve samples to the full count"), Points.Num(), GuidelineGeom::DefaultSamples);
		TestEqual(TEXT("its first point IS A"), Points[0], A);
		TestEqual(TEXT("its last point IS B"), Points.Last(), B);
		TestTrue(TEXT("a curve is longer than its chord"),
			GuidelineGeom::PolylineLength(Points) > FVector2D::Distance(A, B));
	}

	// The split is what joining a lead-in does to a taxiway. Both halves must lie ON the
	// original curve, or every stand would visibly kink the taxiway it joins.
	{
		const FVector2D Control(500.0, 900.0);
		const double At = 0.35;

		FVector2D Mid;
		FVector2D Left;
		FVector2D Right;
		GuidelineGeom::Split(A, Control, B, At, Mid, Left, Right);

		TestTrue(TEXT("the split point is on the original curve"),
			FVector2D::Distance(Mid, GuidelineGeom::Eval(A, Control, B, At)) < 1e-9);

		// A point midway along the first half must equal the original at half of At.
		TestTrue(TEXT("the near half traces the original"),
			FVector2D::Distance(
				GuidelineGeom::Eval(A, Left, Mid, 0.5),
				GuidelineGeom::Eval(A, Control, B, At * 0.5)) < 1e-9);

		TestTrue(TEXT("the far half traces the original"),
			FVector2D::Distance(
				GuidelineGeom::Eval(Mid, Right, B, 0.5),
				GuidelineGeom::Eval(A, Control, B, At + (1.0 - At) * 0.5)) < 1e-9);
	}

	// Walking a polyline, which is what a follower does every tick.
	{
		const TArray<FVector2D> Line = { FVector2D(0.0, 0.0), FVector2D(100.0, 0.0), FVector2D(100.0, 100.0) };

		FVector2D At;
		double Heading = 0.0;

		TestTrue(TEXT("distance zero is the start"), GuidelineGeom::PointAtDistance(Line, 0.0, At, Heading));
		TestEqual(TEXT("at the first point"), At, FVector2D(0.0, 0.0));

		GuidelineGeom::PointAtDistance(Line, 150.0, At, Heading);
		TestEqual(TEXT("past the corner, half way up the second leg"), At, FVector2D(100.0, 50.0));
		TestTrue(TEXT("heading turned with it"),
			FMath::IsNearlyEqual(Heading, UE_DOUBLE_PI * 0.5, 1e-9));

		// Overshoot must CLAMP and keep the last real heading - an agent at its destination
		// facing the way it arrived, rather than one that flies off or snaps to due east.
		GuidelineGeom::PointAtDistance(Line, 1e6, At, Heading);
		TestEqual(TEXT("an overshoot stops at the end"), At, FVector2D(100.0, 100.0));
		TestTrue(TEXT("still facing the way it arrived"),
			FMath::IsNearlyEqual(Heading, UE_DOUBLE_PI * 0.5, 1e-9));

		// A degenerate polyline has no direction, and saying so is what stops a caller
		// writing an unset position through to an actor.
		const TArray<FVector2D> Single = { FVector2D(7.0, 7.0) };
		TestFalse(TEXT("one point has no direction"),
			GuidelineGeom::PointAtDistance(Single, 0.0, At, Heading));
	}

	return true;
}

#endif
