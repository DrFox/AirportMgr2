#include "CoreMinimal.h"
#include "Build/AnchorLink.h"
#include "Build/StandLaneBuild.h"
#include "Content/AirsideSettings.h"
#include "Entities/AircraftType.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadEntity.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadTraffic.h"
#include "Model/RouteSearch.h"
#include "Solve/GuidelineGeom.h"
#include "Solve/RoadGeom.h"
#include "StandFixture.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace ServiceLinkFixture
{
	/** A straight guideline admitting exactly one class, plus Emergency as derived ones do.
	 *  Returns the node at From; OutFar is the one at To. */
	FGuidelineNodeId Lay(URoadNetwork& Net, const FVector2D& From, const FVector2D& To,
		ETraversalClass Class, FGuidelineNodeId& OutFar)
	{
		const FGuidelineNodeId Near = Net.AddGuidelineNode(From);
		OutFar = Net.AddGuidelineNode(To);

		FGuidelineEdge Edge;
		Edge.A = Near;
		Edge.B = OutFar;
		Edge.Control = (From + To) * 0.5;
		Edge.AllowedTraffic = FTrafficMask::Only(Class);
		Edge.AllowedTraffic.Add(ETraversalClass::Emergency);
		Edge.Direction = EGuidelineDir::Bidirectional;
		Edge.Width = 600.0;
		Edge.bDerived = true;
		Net.AddGuidelineEdge(MoveTemp(Edge));
		return Near;
	}

	/**
	 * The axis-aligned box the definition's lane occupies, in its own local space.
	 *
	 * HERE RATHER THAN ON UEntityDefinition, where it was UEntityDefinition::ServiceLaneBounds()
	 * until 2026-09-16. That accessor existed so a test could ask "where is the lane" without
	 * typing the ring's four corners a second time (#104), and the reason it earned its place on
	 * the asset - the lane is a rectangle by construction, so its bounds ARE its shape - stopped
	 * being true the moment the lane became a seventeen-point cycle with a dip in it. A bounding
	 * box of that is a TEST'S convenience and nothing else, so it lives with the tests.
	 */
	FBox2D LaneBoundsOf(const UEntityDefinition& Definition)
	{
		FBox2D Bounds(ForceInit);
		for (const FStandWaypoint& Point : Definition.ServiceLane)
		{
			Bounds += Point.Local;
		}
		return Bounds;
	}

	// ServiceLinkFixture::PlaceStand MOVED to StandFixture.h, 2026-09-16: StandLaneTest.cpp
	// needs the same placement, and a copied one is a second statement of which of
	// PlaceEntity's arguments come off the definition. Included above, same namespace.

	/**
	 * The direction an edge LEAVES From in, taken analytically.
	 *
	 * Never a difference of samples: a quadratic's first sampled chord is a degree or two off
	 * its true tangent, and a test that measured the chord would report a turn no follower
	 * ever makes. GuidelineGeom::Tangent is the derivative of the very function Sample
	 * evaluates, which is the point of it.
	 */
	bool LeavingAlong(const URoadNetwork& Net, FGuidelineEdgeId Id, FGuidelineNodeId From,
		FVector2D& Out)
	{
		const FGuidelineEdge* Edge = Net.GetGuidelineEdge(Id);
		if (Edge == nullptr || !Edge->bAlive)
		{
			return false;
		}
		const FGuidelineNode* A = Net.GetGuidelineNode(Edge->A);
		const FGuidelineNode* B = Net.GetGuidelineNode(Edge->B);
		if (A == nullptr || B == nullptr)
		{
			return false;
		}
		const bool bFromB = Edge->B == From;
		const FVector2D Dir = GuidelineGeom::Tangent(
			A->Position, Edge->Control, B->Position, bFromB ? 1.0 : 0.0);
		Out = bFromB ? -Dir : Dir;
		return !Out.IsNearlyZero();
	}

	/** The node a named anchor resolved to, or an unset handle. */
	FGuidelineNodeId AnchorNode(const URoadNetwork& Net, FEntityInstanceId Entity, const TCHAR* Id)
	{
		const FResolvedAnchor* Found = Net.FindResolvedAnchor(Entity, FName(Id));
		return Found != nullptr ? Found->Node : FGuidelineNodeId();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandLaneReachesTheGraphTest,
	"Airside.Build.StandLaneReachesTheGraph",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandLaneReachesTheGraphTest::RunTest(const FString& Parameters)
{
	using namespace ServiceLinkFixture;

	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
	const FEntityInstanceId Placed = PlaceStand(*Net, *Stand, FVector2D::ZeroVector, 0.0);

	const FStandLaneBuild::FResult First = FStandLaneBuild::Build(*Net);
	TestEqual(TEXT("one stand, one lane"), First.LanesBuilt, 1);

	// EVERY SERVICE ANCHOR NOW HAS LINE ON IT, and the AIRCRAFT stop mark still does not -
	// the lane is for vehicles, and a painted lead-in is not this builder's business.
	//
	// TWO EDGES, NOT MERELY ONE. A spurred anchor had a stub into the ring and one edge would
	// have been the whole story; the lane runs THROUGH the box now, so an anchor with one edge
	// on it is a dead end - and reverse does not exist, so a dead end is an anchor no truck can
	// leave. Airside.Build.PlacedStandLaneIsOneDrivableCycle measures the same property over
	// the whole cycle.
	for (const FResolvedAnchor& Anchor : Net->GetEntity(Placed)->ResolvedAnchors)
	{
		if (TraversalForRole(Anchor.Role) == ETraversalClass::Aircraft)
		{
			continue;
		}
		const FGuidelineNode* Node = Net->GetGuidelineNode(Anchor.Node);
		if (TestNotNull(TEXT("the anchor resolves"), Node))
		{
			TestEqual(
				*FString::Printf(TEXT("and the lane runs through '%s'"), *Anchor.Id.ToString()),
				Node->Incident.Num(), 2);
		}
	}
	TestEqual(TEXT("the aircraft stop mark is untouched"),
		Net->GetGuidelineNode(Net->GetEntity(Placed)->PoseNode)->Incident.Num(), 0);

	// BUT IT IS NOT ON A ROAD. The lane is itself a vehicle guideline, so a check that only
	// counted edges would call this stand connected and no truck would ever route to it.
	TestFalse(TEXT("a lane with no road near it is not a connection"),
		Net->IsServiceNodeConnected(AnchorNode(*Net, Placed, TEXT("HydrantPit"))));

	// AND IT IS A CYCLE, not a set of unjoined runs: every node has two lane edges on it, so a
	// truck can go round either way.
	{
		int32 CornersWithTwoWaysRound = 0;
		for (const FGuidelineNodeId& Node : First.Nodes)
		{
			const FGuidelineNode* Found = Net->GetGuidelineNode(Node);
			CornersWithTwoWaysRound += (Found != nullptr && Found->Incident.Num() >= 2) ? 1 : 0;
		}
		TestTrue(TEXT("the lane closes - every lane node has a way round both sides"),
			CornersWithTwoWaysRound >= 4);
	}

	// A SECOND PASS ADDS NOTHING. The graph is rebuilt on every road edit and this runs each
	// time; a builder that could not see its own previous output would stack a lane per pass.
	const FStandLaneBuild::FResult Second = FStandLaneBuild::Build(*Net);
	TestEqual(TEXT("a second pass builds no second lane"), Second.LanesBuilt, 0);
	TestEqual(TEXT("but it still reports the lane that is there"), Second.Lanes.Num(), 1);

	// AND IT REPORTS THE SAME ENTRIES, NODE FOR NODE.
	//
	// THE WHOLE RESULT OR NONE OF IT. Lanes and Nodes are re-gathered from the graph on the
	// skip path and Entries is RECOVERED there - by FStandLaneBuild's own RecoverEntries,
	// through the very measurement the laying path lays by. Filled only by the laying path, as
	// it was when this builder was written, the second pass would hand back a correct Lanes, a
	// correct Nodes and an EMPTY Entries: not merely incomplete but inconsistent, and silent,
	// because the census line is gated on LanesBuilt and says nothing on a pass that laid none.
	//
	// THE CONSEQUENCE IS REMOTE FROM THE CAUSE, which is why it is pinned here rather than
	// left to the pass that suffers it: the linking pass reads Entries, so a stand laid on one
	// pass and linked on the next would simply never be joined to a road, with nothing anywhere
	// to say why. FAnchorLink::Build is the production caller and this file already calls it
	// twice, so the path is real.
	{
		const TArray<FGuidelineNodeId>* Laid = First.Entries.Find(Placed);
		const TArray<FGuidelineNodeId>* Recovered = Second.Entries.Find(Placed);
		if (TestNotNull(TEXT("the first pass recorded the stand's entries"), Laid)
			&& TestNotNull(TEXT("and the second pass recovered them"), Recovered))
		{
			TestEqual(
				*FString::Printf(TEXT("the same number of entry nodes - %d laid, %d recovered"),
					Laid->Num(), Recovered->Num()),
				Recovered->Num(), Laid->Num());

			// BY HANDLE, not by count. A recovery that matched the wrong end of every bend
			// would agree on the count and hand the linking pass four nodes on the inside of
			// the turns - which is the failure a count alone would wave through.
			for (const FGuidelineNodeId& Node : *Laid)
			{
				const FGuidelineNode* At = Net->GetGuidelineNode(Node);
				TestTrue(
					*FString::Printf(TEXT("entry node at (%.0f,%.0f) is recovered by handle"),
						At != nullptr ? At->Position.X : 0.0,
						At != nullptr ? At->Position.Y : 0.0),
					Recovered->Contains(Node));
			}
		}
	}

	// A DEPOT HAS NO LANE, so nothing is built for it at all - one pose, nothing parked.
	{
		URoadNetwork* Bare = NewObject<URoadNetwork>(GetTransientPackage());
		UEntityDefinition* Depot = UEntityDefinition::MakeFuelDepotTransient();
		Bare->PlaceEntity(Depot, Depot->Anchors, FVector2D::ZeroVector, 0.0, 0.0,
			Depot->PoseRole, Depot->Trucks);
		TestEqual(TEXT("a depot gets no lane"), FStandLaneBuild::Build(*Bare).LanesBuilt, 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAircraftLeadInStillCastsARayTest,
	"Airside.Build.AircraftLeadInStillCastsARay",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAircraftLeadInStillCastsARayTest::RunTest(const FString& Parameters)
{
	using namespace ServiceLinkFixture;

	// THE TEST THAT FAILS IF THE PROXIMITY RULE LEAKS INTO THE AIRCRAFT ONE.
	//
	// A stand's lead-in IS the painted line, so it is cast along the stand's own heading and
	// a taxiway BEHIND the stand is behind the aircraft's tail. Nearest-guideline was rejected
	// for aircraft precisely because nearest is regularly the taxiway on the far side of the
	// terminal - see FAnchorLink's header. Ten metres behind is as near as it gets, and it
	// must still not join.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	FGuidelineNodeId East;
	Lay(*Net, FVector2D(-10000.0, 1000.0), FVector2D(10000.0, 1000.0),
		ETraversalClass::Aircraft, East);

	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();

	// The stand sits at y = 2000 with the taxiway ten metres below it at y = 1000. Heading
	// -90 aims the stand at -Y, so its pose ray leaves along heading + 180 - straight up +Y,
	// directly AWAY from the taxiway it is sitting beside.
	const FEntityInstanceId Placed =
		PlaceStand(*Net, *Stand, FVector2D(0.0, 2000.0), -UE_DOUBLE_PI * 0.5);

	FAnchorLink::Build(*Net);

	const FGuidelineNode* Pose = Net->GetGuidelineNode(Net->GetEntity(Placed)->PoseNode);
	if (TestNotNull(TEXT("the stop position resolves"), Pose))
	{
		TestEqual(TEXT("a taxiway 10 m behind a stand is still not joined"),
			Pose->Incident.Num(), 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FServiceLinkJoinsFromAnyDirectionTest,
	"Airside.Build.ServiceLinkJoinsFromAnyDirection",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FServiceLinkJoinsFromAnyDirectionTest::RunTest(const FString& Parameters)
{
	using namespace ServiceLinkFixture;

	// THE WHOLE POINT OF THE CHANGE. A stand's five service anchors nearly all cast the same
	// way - three at -90, one at +90, one at 180 - so a road drawn the way a player draws
	// one, ALONGSIDE the stands, was parallel to every ray and served none of them. A vehicle
	// may genuinely arrive from any side, so a service link measures distance, not direction.
	//
	// Each road below sits GapNear or GapFar beyond one of the lane's four sides -
	// ServiceLinkFixture::LaneBoundsOf, not the lane's corners typed a second time (#104), and
	// derived from BuildCodeCStand's own arithmetic round the aircraft and anchors.
	//
	// 4500 rather than exactly the 5000 uu radius: a boundary case measures the comparison
	// operator rather than the rule, and would flip on a rounding error.
	constexpr double GapNear = 4500.0;
	constexpr double GapFar = 20000.0;

	struct FSide
	{
		const TCHAR* Name;
		FVector2D From;
		FVector2D To;
	};

	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
	const FBox2D LaneBounds = LaneBoundsOf(*Stand);

	auto RoadsAt = [&LaneBounds](double Gap) -> TArray<FSide>
	{
		return {
			{ TEXT("south"), FVector2D(-20000.0, LaneBounds.Min.Y - Gap), FVector2D(20000.0, LaneBounds.Min.Y - Gap) },
			{ TEXT("north"), FVector2D(-20000.0, LaneBounds.Max.Y + Gap), FVector2D(20000.0, LaneBounds.Max.Y + Gap) },
			{ TEXT("west"),  FVector2D(LaneBounds.Min.X - Gap, -20000.0), FVector2D(LaneBounds.Min.X - Gap, 20000.0) },
			{ TEXT("east"),  FVector2D(LaneBounds.Max.X + Gap, -20000.0), FVector2D(LaneBounds.Max.X + Gap, 20000.0) },
		};
	};

	for (const FSide& Side : RoadsAt(GapNear))
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		FGuidelineNodeId Far;
		const FGuidelineNodeId Near =
			Lay(*Net, Side.From, Side.To, ETraversalClass::GroundVehicle, Far);

		const FEntityInstanceId Placed = PlaceStand(*Net, *Stand, FVector2D::ZeroVector, 0.0);
		FAnchorLink::Build(*Net);

		const FGuidelineNodeId Hydrant = AnchorNode(*Net, Placed, TEXT("HydrantPit"));
		TestTrue(*FString::Printf(TEXT("a road to the %s reaches the lane"), Side.Name),
			Net->IsServiceNodeConnected(Hydrant));

		// AND A TRUCK CAN ACTUALLY GET THERE. Connectivity is the claim; a route is the proof,
		// and "the search found nothing" would otherwise read like a broken search.
		FRouteQuery Query;
		Query.Start = Near;
		Query.Goal = Hydrant;
		Query.Class = ETraversalClass::GroundVehicle;
		TestTrue(*FString::Printf(TEXT("and a truck routes from the %s to the hydrant"), Side.Name),
			RouteSearch::Find(*Net, Query).IsValid());
	}

	for (const FSide& Side : RoadsAt(GapFar))
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		FGuidelineNodeId Far;
		Lay(*Net, Side.From, Side.To, ETraversalClass::GroundVehicle, Far);

		const FEntityInstanceId Placed = PlaceStand(*Net, *Stand, FVector2D::ZeroVector, 0.0);
		FAnchorLink::Build(*Net);

		// THE SHORT RADIUS IS WHAT KEEPS THE REJECTED CASE REJECTED: at 200 m the nearest
		// vehicle line is regularly the service road on the far side of a terminal, and the
		// link would run straight through the building with nothing to report it.
		TestFalse(*FString::Printf(TEXT("a road 200 m to the %s does not"), Side.Name),
			Net->IsServiceNodeConnected(AnchorNode(*Net, Placed, TEXT("HydrantPit"))));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandLaneDoesNotJoinItselfTest,
	"Airside.Build.StandLaneDoesNotJoinItself",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandLaneDoesNotJoinItselfTest::RunTest(const FString& Parameters)
{
	using namespace ServiceLinkFixture;

	// THE LANE IS ITSELF A VEHICLE GUIDELINE. A search that failed to exclude the searcher's
	// own geometry would find the nearest vehicle line a few metres away - the other side of
	// its own box - report every stand connected, and route no truck anywhere.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());

	// A TAXIWAY within reach, so this is not merely an empty graph: the stand's own POSE must
	// still join it, and the lane must still join nothing. The stand faces +X (heading 0) and
	// its pose ray leaves along heading + 180, so it casts west at the line laid there.
	//
	// NORTH-SOUTH, and that matters: an east-west line at y = 0 would be COLLINEAR with the
	// ray, which RayHitsSegment deliberately refuses - there is no single point to join and
	// picking one would be arbitrary. Written the other way round first, and the test then
	// failed for a reason that had nothing to do with service loops.
	FGuidelineNodeId TaxiNorth;
	Lay(*Net, FVector2D(-10000.0, -10000.0), FVector2D(-10000.0, 10000.0),
		ETraversalClass::Aircraft, TaxiNorth);

	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
	const FEntityInstanceId Placed = PlaceStand(*Net, *Stand, FVector2D::ZeroVector, 0.0);

	FAnchorLink::Build(*Net);

	TestTrue(TEXT("the aircraft half still works - the pose joins the taxiway"),
		Net->GetGuidelineNode(Net->GetEntity(Placed)->PoseNode)->Incident.Num() > 0);

	for (const FResolvedAnchor& Anchor : Net->GetEntity(Placed)->ResolvedAnchors)
	{
		TestFalse(TEXT("no service anchor is on a road, because there is no road"),
			Net->IsServiceNodeConnected(Anchor.Node));
	}

	// A SECOND PASS JOINS NOTHING NEW, which is the ordinary case rather than an unusual one:
	// the graph is rebuilt on every road edit and lane and links are laid again each time.
	TestEqual(TEXT("a second pass joins nothing new"), FAnchorLink::Build(*Net), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadAlongsideARowOfStandsTest,
	"Airside.Build.RoadAlongsideARowOfStands",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadAlongsideARowOfStandsTest::RunTest(const FString& Parameters)
{
	using namespace ServiceLinkFixture;

	// THE CASE THAT FAILED IN PIE ON 2026-09-07, and the one this whole design exists for:
	// one service road drawn alongside a row of stands, which is how a player draws one.
	// Measured then: 9 of 25 lead-ins joined, and the only service anchor that joined at any
	// stand was TugStand - the one anchor that casts ACROSS the road instead of along it.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());

	constexpr double RoadY = -6000.0;
	FGuidelineNodeId RoadEast;
	const FGuidelineNodeId RoadWest =
		Lay(*Net, FVector2D(-40000.0, RoadY), FVector2D(40000.0, RoadY),
			ETraversalClass::GroundVehicle, RoadEast);

	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();

	TArray<FEntityInstanceId> Row;
	for (int32 At = 0; At < 4; ++At)
	{
		// 8000 uu apart: a lane is 5250 uu wide, so that leaves 27 m of clear ground between
		// neighbours and no lane is nearer to another lane than it is to the road.
		Row.Add(PlaceStand(*Net, *Stand, FVector2D(-15000.0 + At * 8000.0, 0.0), 0.0));
	}

	FAnchorLink::Build(*Net);

	for (int32 At = 0; At < Row.Num(); ++At)
	{
		const FGuidelineNodeId Hydrant = AnchorNode(*Net, Row[At], TEXT("HydrantPit"));
		TestTrue(*FString::Printf(TEXT("stand %d's hydrant is on the road"), At),
			Net->IsServiceNodeConnected(Hydrant));

		FRouteQuery Query;
		Query.Start = RoadWest;
		Query.Goal = Hydrant;
		Query.Class = ETraversalClass::GroundVehicle;
		TestTrue(*FString::Printf(TEXT("and a truck routes to stand %d"), At),
			RouteSearch::Find(*Net, Query).IsValid());
	}

	// EACH STAND GETS ITS OWN CONNECTION, which is what makes one road serving a row the
	// normal case rather than a conflict.
	FRouteQuery BetweenStands;
	BetweenStands.Start = AnchorNode(*Net, Row[0], TEXT("HydrantPit"));
	BetweenStands.Goal = AnchorNode(*Net, Row.Last(), TEXT("HydrantPit"));
	BetweenStands.Class = ETraversalClass::GroundVehicle;
	TestTrue(TEXT("and a truck can go from the first stand to the last down the road"),
		RouteSearch::Find(*Net, BetweenStands).IsValid());
	return true;
}

namespace ServiceLinkFixture
{
	/**
	 * Every lane node that carries a link to a road - an incident edge no stand lane owns.
	 *
	 * The link edge's OTHER end is on the road, and the road's own splits and fillets are
	 * unowned too, so the test has to start from the LANE side. Counting nodes rather than
	 * edges is what makes an entry welded onto a shared corner count once.
	 */
	TArray<FVector2D> EntryPoints(const URoadNetwork& Net)
	{
		TSet<FGuidelineNodeId> LaneNodes;
		for (const FGuidelineEdge& Edge : Net.GetGuidelineEdges())
		{
			if (Edge.bAlive && Edge.StandGeometryOwner.IsSet())
			{
				LaneNodes.Add(Edge.A);
				LaneNodes.Add(Edge.B);
			}
		}

		TArray<FVector2D> Entries;
		for (const FGuidelineNodeId& Id : LaneNodes)
		{
			const FGuidelineNode* Node = Net.GetGuidelineNode(Id);
			if (Node == nullptr) { continue; }
			for (const FGuidelineEdgeId& Incident : Node->Incident)
			{
				const FGuidelineEdge* Edge = Net.GetGuidelineEdge(Incident);
				if (Edge != nullptr && Edge->bAlive && !Edge->StandGeometryOwner.IsSet())
				{
					Entries.AddUnique(Node->Position);
					break;
				}
			}
		}
		return Entries;
	}

	/** Is one of these points within Tolerance of At? */
	bool Has(const TArray<FVector2D>& Points, const FVector2D& At, double Tolerance = 50.0)
	{
		for (const FVector2D& Point : Points)
		{
			if (FVector2D::Distance(Point, At) <= Tolerance) { return true; }
		}
		return false;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FServiceLaneEntersOnEverySideWithinReachTest,
	"Airside.Build.ServiceLaneEntersOnEverySideWithinReach",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FServiceLaneEntersOnEverySideWithinReachTest::RunTest(const FString& Parameters)
{
	using namespace ServiceLinkFixture;

	// A ROAD ALONGSIDE, 4 m clear of the lane's south side. The whole case: a service road
	// running past a row of stands, which is how a player builds one.
	//
	// ServiceLinkFixture::LaneBoundsOf, not the lane's corners typed a second time (#104):
	// derived in BuildCodeCStand from the design aircraft's footprint and the anchors. The
	// stand sits at the origin facing +X so local and world coincide, stated rather than
	// assumed.
	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
	const FBox2D LaneBounds = LaneBoundsOf(*Stand);
	constexpr double RoadClearance = 400.0;

	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	FGuidelineNodeId East;
	const double RoadY = LaneBounds.Min.Y - RoadClearance;
	const FGuidelineNodeId West = Lay(*Net, FVector2D(-30000.0, RoadY), FVector2D(30000.0, RoadY),
		ETraversalClass::GroundVehicle, East);

	const FEntityInstanceId Placed = PlaceStand(*Net, *Stand, FVector2D::ZeroVector, 0.0);
	FAnchorLink::Build(*Net);

	const TArray<FVector2D> Entries = EntryPoints(*Net);

	// THREE, not one. The near side reaches the road along its whole length; the two END
	// sides reach it at the corner each shares with the near side. One entry made the ring a
	// cul-de-sac a truck had to drive half way round; three make it a drive-through.
	// THE POSITIONS ARE IN THE MESSAGE, not just the count. A bare "expected 3, got 5" says
	// nothing about WHICH two sides linked wrongly, and every failure this test found while
	// it was being written was diagnosed from exactly this list.
	FString Where;
	for (const FVector2D& Entry : Entries)
	{
		Where += FString::Printf(TEXT("(%.0f,%.0f) "), Entry.X, Entry.Y);
	}
	TestEqual(*FString::Printf(TEXT("the lane joins the road on all three sides within reach - at %s"), *Where),
		Entries.Num(), 3);

	// HOW FAR BACK A ROUNDED CORNER REACHES, derived exactly as the builder derives it: the
	// radius the largest admitted service vehicle needs, cut at a right angle, which is
	// R*sqrt(2) and not R. FStandLaneBuild::LaneTurnRadius was a typed 750 until 2026-09-16
	// and this restated it; both read the same one function now.
	const double CornerReach =
		UAirsideSettings::ResolveLargestServiceVehicle().TightestFollowableRadius()
			* UE_DOUBLE_SQRT_2;

	// THE NEAR SIDE, IN THE MIDDLE OF ITS OVERLAP WITH THE ROAD - not at the first point that
	// tied. This is the assertion that fails without GuidelineGeom::NearestBetweenPolylines
	// breaking its tie at the middle.
	//
	// EXACT AGAIN. It was loosened while a bend could take this side's entrance and leave it
	// 195 uu from the bend's foot; a bend is no longer a candidate, so the side gets its own
	// entrance back and the middle is the middle.
	TestTrue(*FString::Printf(TEXT("the near side is entered at its middle - at %s"), *Where),
		Has(Entries, FVector2D(LaneBounds.GetCenter().X, LaneBounds.Min.Y)));

	// THE END SIDES, each at the nearest point of ITS OWN STRAIGHT.
	//
	// NOT AT A BEND, and that is the rule rather than an accident of this layout. A bend is
	// the turn between two sides and belongs to both, so an entrance on one is an entrance
	// neither side can call its own - which left a stand with three connections clustered
	// along its bottom edge, of three different qualities, instead of one per side. Reported
	// from play 2026-09-15 with a picture.
	//
	// So an end side is entered where its own straight comes closest to the road, and its
	// straight starts CornerReach up from the near side because that is how far the bend
	// reaches back. Derived rather than the 1029 uu it happens to be.
	TestTrue(*FString::Printf(TEXT("the tail end side joins at the near end of its own straight - at %s"), *Where),
		Has(Entries, FVector2D(LaneBounds.Min.X, LaneBounds.Min.Y + CornerReach)));
	TestTrue(*FString::Printf(TEXT("the nose end side joins at the near end of its own straight - at %s"), *Where),
		Has(Entries, FVector2D(LaneBounds.Max.X, LaneBounds.Min.Y + CornerReach)));

	// AND THE FAR SIDE DOES NOT, though it is 4580 uu from the road and the service radius is
	// 5000. Its connector would run the whole depth of the stand, through the parked
	// aircraft, to reach a road the near side already touches. Refused by measuring the
	// crossing, not by shortening the radius - the radius is the player's knob for how far a
	// stand may sit from its road and must not silently double as this rule.
	for (const FVector2D& Entry : Entries)
	{
		TestTrue(TEXT("no entry is on the far side of the lane"), Entry.Y < 0.0);
	}

	// IDEMPOTENT. The graph is rebuilt on every road edit and this runs each time; a pass
	// that could not see its own previous links would stack an entry per side per rebuild.
	FAnchorLink::Build(*Net);
	{
		FString Again;
		for (const FVector2D& Entry : EntryPoints(*Net))
		{
			Again += FString::Printf(TEXT("(%.0f,%.0f) "), Entry.X, Entry.Y);
		}
		TestEqual(*FString::Printf(TEXT("a second pass adds no further entries - at %s"), *Again),
			EntryPoints(*Net).Num(), 3);
	}

	// WHAT IT BUYS, MEASURED ON A JOURNEY. The GPU sits at local (300, -600) and spurs to the
	// EAST side; with one entry at the tail corner a truck drove the length of the stand and
	// back to reach it. From the nose corner it is up the east side plus the spur.
	//
	// THE FIGURE ROSE BY 854 uu WHEN SPURS WERE MADE TANGENTIAL, and that is a purchase and
	// not a regression. A spur now meets the lane PreferredSpurRun further along and curves
	// back, so the truck drives that run twice - against which the junction it used to reach
	// was a 90 degree instant turn, which FSpeedProfile calls untakeable at any speed and
	// crawls at MinSteeringSpeed. 854 uu at the 600 uu/s the new bend allows is under two
	// seconds; the crawl it replaces was tens.
	//
	// STILL NOWHERE NEAR A TOUR, which is what this bound exists to catch: half this ring's
	// perimeter is another 9400 uu on top.
	{
		FRouteQuery Query;
		Query.Start = East;
		Query.Goal = AnchorNode(*Net, Placed, TEXT("FixedGPU"));
		Query.Class = ETraversalClass::GroundVehicle;

		const FRoutePlan Plan = RouteSearch::Find(*Net, Query);
		if (TestTrue(TEXT("a truck routes from the road to the ground power"), Plan.IsValid()))
		{
			// Measured from the ROAD's east end, so the figure is the journey and not an
			// arbitrary start: 28300 of road, then under 4700 inside the stand.
			TestTrue(*FString::Printf(
					TEXT("and turns in at the nearest corner rather than touring the lane "
					     "- %.0f uu"),
					GuidelineGeom::PolylineLength(Plan.Polyline)),
				GuidelineGeom::PolylineLength(Plan.Polyline) < 28300.0 + 4700.0);

			// TEMPORARY, 2026-09-15. The entrance is on the right side now and there is still
			// a tight bend onto the stand. Measured the way FSpeedProfile measures it, over
			// the route the truck actually drives.
			{
				const TArray<FVector2D>& P = Plan.Polyline;
				int32 Sharp = 0;
				double Worst = 0.0;
				FString SharpAt;
				double Tightest = TNumericLimits<double>::Max();
				FVector2D TightAt = FVector2D::ZeroVector;
				for (int32 At = 0; At < P.Num(); ++At)
				{
					double In = 0.0, Out = 0.0;
					GuidelineGeom::VertexHeadings(P, At, In, Out);
					const double Instant = FMath::Abs(FMath::UnwindRadians(Out - In));
					if (Instant > 1.0e-6)
					{
						++Sharp;
						Worst = FMath::Max(Worst, FMath::RadiansToDegrees(Instant));
						SharpAt += FString::Printf(TEXT("%.0fdeg@(%.0f,%.0f) "),
							FMath::RadiansToDegrees(Instant), P[At].X, P[At].Y);
					}
					if (At + 1 < P.Num())
					{
						double NextIn = 0.0, NextOut = 0.0;
						GuidelineGeom::VertexHeadings(P, At + 1, NextIn, NextOut);
						const double Turn = FMath::Abs(FMath::UnwindRadians(NextIn - Out));
						const double Len = FVector2D::Distance(P[At], P[At + 1]);
						if (Turn > 1.0e-6 && Len > 0.0 && Len / Turn < Tightest)
						{
							Tightest = Len / Turn;
							TightAt = P[At];
						}
					}
				}
				AddInfo(FString::Printf(
					TEXT("ROUTE: %d sharp [%s], tightest %.0f uu at (%.0f,%.0f), start (%.0f,%.0f) end (%.0f,%.0f)"),
					Sharp, *SharpAt, Tightest == TNumericLimits<double>::Max() ? 0.0 : Tightest,
					TightAt.X, TightAt.Y, P[0].X, P[0].Y, P.Last().X, P.Last().Y));
			}
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTruckReachesHydrantWithoutCrossingTheAircraftTest,
	"Airside.Model.Traffic.TruckReachesHydrantWithoutCrossingTheAircraft",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTruckReachesHydrantWithoutCrossingTheAircraftTest::RunTest(const FString& Parameters)
{
	using namespace ServiceLinkFixture;

	// THE INVARIANT, MEASURED ON A ROUTE. Airside.Entities.StandLaneClearsTheAircraft
	// measures the definition; this measures what the SEARCH will actually hand a driver,
	// which is the thing the player watches. A lane that cleared the aeroplane and a link
	// that did not would pass the first test and fail here.
	//
	// The road is on the PORT side and the hydrant is under the STARBOARD wing, which is the
	// arrangement that makes a straight spur cross 37 m of fuselage. That is exactly why the
	// lane exists, and why joining each anchor directly to the road was rejected.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());

	// A SHORT road, so the journey's length is about the STAND rather than about how far
	// down the road the start node happens to sit.
	constexpr double RoadY = -6000.0;
	FGuidelineNodeId RoadEast;
	const FGuidelineNodeId RoadWest =
		Lay(*Net, FVector2D(-9000.0, RoadY), FVector2D(9000.0, RoadY),
			ETraversalClass::GroundVehicle, RoadEast);

	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
	if (!TestNotNull(TEXT("a design aircraft to clear"), Stand->DesignAircraft.Get())) { return false; }

	const FEntityInstanceId Placed = PlaceStand(*Net, *Stand, FVector2D::ZeroVector, 0.0);
	FAnchorLink::Build(*Net);

	FRouteQuery Query;
	Query.Start = RoadWest;
	Query.Goal = AnchorNode(*Net, Placed, TEXT("HydrantPit"));
	Query.Class = ETraversalClass::GroundVehicle;

	const FRoutePlan Plan = RouteSearch::Find(*Net, Query);
	if (!TestTrue(TEXT("a truck routes from the road to the hydrant"), Plan.IsValid())
		|| Plan.Polyline.Num() < 2)
	{
		// Returning rather than reading on: a refused plan has an EMPTY polyline and the loop
		// below would measure nothing while reporting success.
		return false;
	}

	// The parked aircraft's centreline in WORLD space. The stand is at the origin facing +X,
	// so local and world coincide - stated rather than assumed, because a fixture that
	// rotated the stand and forgot to rotate this would measure the wrong line.
	const FEntityFootprint& Footprint = Stand->DesignAircraft->Footprint;
	const FVector2D Tail(Footprint.TailX, 0.0);
	const FVector2D Nose(Footprint.NoseX, 0.0);

	int32 Crossings = 0;
	for (int32 At = 1; At < Plan.Polyline.Num(); ++At)
	{
		Crossings += RoadGeom::SegmentsCross(Plan.Polyline[At - 1], Plan.Polyline[At], Tail, Nose)
			? 1 : 0;
	}

	// NOT "does not intersect the footprint", deliberately: that would forbid passing under a
	// wing, which is normal and which the hydrant requires - the pit is under the starboard
	// wing root because that is where a hydrant pit is.
	TestEqual(TEXT("the truck's route never crosses the fuselage lengthwise"), Crossings, 0);

	// AND THE ROUTE IS NOT ABSURD. A plan that went round the airport would cross nothing
	// either, so the crossing count above passes vacuously on a truck that never came. Twice
	// the straight-line distance is generous - the lane is a detour by construction - and far
	// short of anything that could be called a tour.
	const FGuidelineNode* Start = Net->GetGuidelineNode(RoadWest);
	const FGuidelineNode* Goal = Net->GetGuidelineNode(Query.Goal);
	if (Start != nullptr && Goal != nullptr)
	{
		TestTrue(TEXT("and it is a short journey, not a tour of the airport"),
			GuidelineGeom::PolylineLength(Plan.Polyline)
				< FVector2D::Distance(Start->Position, Goal->Position) * 2.0);
	}
	return true;
}

// Airside.Build.SpursLeaveTheLaneTangentially IS DELETED, 2026-09-16, and a deleted test is a
// claim that needs an argument. Every assertion it made was about the SPUR PAIR - two lines
// from each anchor into the ring, meeting at the anchor in a V - and there is no spur any
// more: an anchor is a waypoint ON the lane, reached by driving along it. The properties the
// test actually protected have all moved, none of them dropped:
//
//   "an anchor has line on it"        -> Airside.Build.StandLaneReachesTheGraph, which now
//                                        demands TWO edges rather than at least one
//   "the junction needs no turning"   -> Airside.Build.PlacedStandLaneIsOneDrivableCycle,
//                                        which measures the delivered radius of every lane
//                                        curve against the vehicle's own lock
//   "nothing crosses the aeroplane"   -> the same test, on the sampled curve and against the
//                                        fuselage RECTANGLE rather than a zero-width axis
//
// What it can no longer say is anything at all: with the anchors on the lane its spur search
// finds nothing, and every loop in it would run zero times while the test reported success.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLaneCornersAreDrivableTest,
	"Airside.Build.LaneCornersAreDrivable",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FLaneCornersAreDrivableTest::RunTest(const FString& Parameters)
{
	using namespace ServiceLinkFixture;

	// THE LANE'S OWN CORNERS, which are authored square and must not be laid that way.
	// UEntityDefinition::ServiceLane is a polyline of straights - a polyline is how a lane is
	// DESCRIBED - but it is not something a truck can drive round: a corner where two straight
	// sides meet is a vertex whose heading changes instantly, and FSpeedProfile calls one of
	// those untakeable at any speed.
	//
	// THE CORNER IS REPLACED BY A QUADRATIC whose control sits ON it, so both sides leave
	// tangentially and the bend carries the turn. See
	// Airside.Build.PlacedStandLaneIsOneDrivableCycle for the other half of the same claim,
	// measured as a delivered radius per edge rather than as a turn per junction.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
	const FEntityInstanceId Placed = PlaceStand(*Net, *Stand, FVector2D::ZeroVector, 0.0);

	const FStandLaneBuild::FResult Built = FStandLaneBuild::Build(*Net);
	const TArray<FGuidelineEdgeId>* Lane = Built.Lanes.Find(Placed);
	if (!TestNotNull(TEXT("the stand got a lane"), Lane))
	{
		return false;
	}

	// THE WHOLE CYCLE: every lane edge, straight runs and the bends between them alike, which
	// together are what a truck drives round.
	//
	// THE bStandApproach FILTER IS A NO-OP TODAY and is kept as the statement of what this
	// measures rather than as live selection. Nothing writes the mark true since the anchor
	// spurs went - see FGuidelineEdge::bStandApproach - but Task 5's road approach will, and an
	// approach is not part of the cycle: it arrives at it.
	TArray<FGuidelineEdgeId> Cycle;
	for (const FGuidelineEdgeId& Id : *Lane)
	{
		const FGuidelineEdge* Edge = Net->GetGuidelineEdge(Id);
		if (Edge != nullptr && Edge->bAlive && !Edge->bStandApproach)
		{
			Cycle.Add(Id);
		}
	}
	if (!TestTrue(TEXT("the lane has a cycle to drive round"), Cycle.Num() >= 4))
	{
		return false;
	}

	const FAirframe Van = UAirsideSettings::ResolveDefaultVehicle();
	const double Lock = FMath::Sin(FMath::DegreesToRadians(
		FMath::Clamp(Van.Ground.MaxSteerDegrees, 0.0, 90.0)));
	if (!TestTrue(TEXT("the default vehicle steers on measured axles"),
			Van.HasAxles() && Lock > KINDA_SMALL_NUMBER))
	{
		return false;
	}

	// The same expression FSpeedProfile::Build uses, written out rather than shared - for the
	// reason Airside.Model.ServiceRoadFilletClearsTheTruckLock gives at its own copy: a helper
	// both the production code and its test called could be wrong in one place and agree with
	// itself.
	const double TightestFollowable = Van.Wheelbase() / Lock;

	// NOWHERE ON THE CYCLE DOES A TRUCK HAVE TO TURN. Asked of every node the cycle passes
	// through - the bends' own ends, the anchors it runs through, and any join a later split
	// left behind - by comparing the two arms' ANALYTIC tangents. Straight through means they
	// leave in opposite directions, which is exactly the test FSpeedProfile applies to a
	// vertex.
	int32 Checked = 0;
	for (const FGuidelineEdgeId& Id : Cycle)
	{
		const FGuidelineEdge* Edge = Net->GetGuidelineEdge(Id);
		for (const FGuidelineNodeId& End : { Edge->A, Edge->B })
		{
			FVector2D Mine;
			if (!LeavingAlong(*Net, Id, End, Mine))
			{
				continue;
			}

			double Best = -1.0;
			const FGuidelineNode* Node = Net->GetGuidelineNode(End);
			for (const FGuidelineEdgeId& OtherId : Node->Incident)
			{
				const FGuidelineEdge* Other = Net->GetGuidelineEdge(OtherId);
				FVector2D Theirs;
				if (OtherId == Id || Other == nullptr || Other->bStandApproach
					|| !LeavingAlong(*Net, OtherId, End, Theirs))
				{
					continue;
				}
				Best = FMath::Max(Best, -FVector2D::DotProduct(Mine, Theirs));
			}

			if (Best < 0.0)
			{
				// No other lane edge here at all, which would mean the cycle is not closed.
				continue;
			}

			++Checked;
			TestTrue(
				*FString::Printf(
					TEXT("the lane runs straight through (%.0f,%.0f) - its best neighbour is "
					     "%.1f deg off"),
					Net->GetGuidelineNode(End)->Position.X,
					Net->GetGuidelineNode(End)->Position.Y,
					FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(Best, -1.0, 1.0)))),
				Best > FMath::Cos(0.02));
		}
	}

	// AND THE BENDS THAT CARRY THE TURN INSTEAD ARE ONES THE TRUCK CAN HOLD. Curvature the
	// way FSpeedProfile measures it, over each lane edge's own samples.
	double Tightest = TNumericLimits<double>::Max();
	int32 Bends = 0;
	for (const FGuidelineEdgeId& Id : Cycle)
	{
		TArray<FVector2D> Points;
		if (!Net->SampleGuideline(Id, Points) || Points.Num() < 3)
		{
			continue;
		}
		for (int32 At = 1; At + 1 < Points.Num(); ++At)
		{
			const FVector2D Before = Points[At] - Points[At - 1];
			const FVector2D After = Points[At + 1] - Points[At];
			const double Turn = FMath::Abs(FMath::UnwindRadians(
				RoadGeom::Bearing(After) - RoadGeom::Bearing(Before)));
			if (Turn > 1.0e-9)
			{
				++Bends;
				Tightest = FMath::Min(Tightest, After.Size() / Turn);
			}
		}
	}

	// NOT VACUOUS. A lane still laid as bare straight runs has no curved span at all, so the
	// radius below would pass on a cycle with square corners - which is the defect itself.
	if (!TestTrue(TEXT("the lane bends somewhere - its corners are rounded"), Bends > 0))
	{
		return false;
	}

	TestTrue(
		*FString::Printf(
			TEXT("and the lane's tightest bend is %.0f uu, clearing the %.0f uu the truck's "
			     "steering needs"),
			Tightest, TightestFollowable),
		Tightest >= TightestFollowable);

	TestTrue(*FString::Printf(TEXT("every lane junction was measured - %d found"), Checked),
		Checked >= 8);
	return true;
}

#endif
