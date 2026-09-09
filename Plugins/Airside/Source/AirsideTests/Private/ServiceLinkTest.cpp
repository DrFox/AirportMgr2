#include "CoreMinimal.h"
#include "Build/AnchorLink.h"
#include "Build/ServiceLoopBuild.h"
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

	FEntityInstanceId PlaceStand(URoadNetwork& Net, UEntityDefinition& Stand,
		const FVector2D& At, double Heading)
	{
		return Net.PlaceEntity(&Stand, Stand.Anchors, At, Heading,
			Stand.DesignAircraft != nullptr ? Stand.DesignAircraft->Footprint.Wingspan : 0.0,
			Stand.PoseRole, Stand.Trucks);
	}

	/** The node a named anchor resolved to, or an unset handle. */
	FGuidelineNodeId AnchorNode(const URoadNetwork& Net, FEntityInstanceId Entity, const TCHAR* Id)
	{
		const FResolvedAnchor* Found = Net.FindResolvedAnchor(Entity, FName(Id));
		return Found != nullptr ? Found->Node : FGuidelineNodeId();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FServiceLoopReachesTheGraphTest,
	"Airside.Build.ServiceLoopReachesTheGraph",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FServiceLoopReachesTheGraphTest::RunTest(const FString& Parameters)
{
	using namespace ServiceLinkFixture;

	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
	const FEntityInstanceId Placed = PlaceStand(*Net, *Stand, FVector2D::ZeroVector, 0.0);

	const FServiceLoopBuild::FResult First = FServiceLoopBuild::Build(*Net);
	TestEqual(TEXT("one stand, one lane"), First.LoopsBuilt, 1);
	TestEqual(TEXT("a spur for every service anchor"), First.SpursBuilt, 5);

	// EVERY SERVICE ANCHOR NOW HAS LINE ON IT, and the AIRCRAFT stop mark still does not -
	// the lane is for vehicles, and a painted lead-in is not this builder's business.
	for (const FResolvedAnchor& Anchor : Net->GetEntity(Placed)->ResolvedAnchors)
	{
		const FGuidelineNode* Node = Net->GetGuidelineNode(Anchor.Node);
		if (TestNotNull(TEXT("the anchor resolves"), Node))
		{
			TestTrue(TEXT("and is spurred to the lane"), Node->Incident.Num() > 0);
		}
	}
	TestEqual(TEXT("the aircraft stop mark is untouched"),
		Net->GetGuidelineNode(Net->GetEntity(Placed)->PoseNode)->Incident.Num(), 0);

	// BUT IT IS NOT ON A ROAD. The lane is itself a vehicle guideline, so a check that only
	// counted edges would call this stand connected and no truck would ever route to it.
	TestFalse(TEXT("a lane with no road near it is not a connection"),
		Net->IsServiceNodeConnected(AnchorNode(*Net, Placed, TEXT("HydrantPit"))));

	// AND IT IS A LOOP, not four unjoined sides: every corner has two lane edges on it, so a
	// truck can go round either way. Counted on the anchor-free corners, since a corner that
	// took a spur has three.
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
	const FServiceLoopBuild::FResult Second = FServiceLoopBuild::Build(*Net);
	TestEqual(TEXT("a second pass builds no second lane"), Second.LoopsBuilt, 0);
	TestEqual(TEXT("and no second spur"), Second.SpursBuilt, 0);
	TestEqual(TEXT("but it still reports the lane that is there"), Second.Lanes.Num(), 1);

	// A DEPOT HAS NO LANE, so nothing is built for it at all - one pose, nothing parked.
	{
		URoadNetwork* Bare = NewObject<URoadNetwork>(GetTransientPackage());
		UEntityDefinition* Depot = UEntityDefinition::MakeFuelDepotTransient();
		Bare->PlaceEntity(Depot, Depot->Anchors, FVector2D::ZeroVector, 0.0, 0.0,
			Depot->PoseRole, Depot->Trucks);
		TestEqual(TEXT("a depot gets no lane"), FServiceLoopBuild::Build(*Bare).LoopsBuilt, 0);
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
	// The lane round a Code C stand at the origin, heading 0, is x in [-3550, +1700] and y in
	// [-2090, +2090]. Each road below sits GapNear or GapFar beyond one of those four sides.
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

	auto RoadsAt = [](double Gap) -> TArray<FSide>
	{
		return {
			{ TEXT("south"), FVector2D(-20000.0, -2090.0 - Gap), FVector2D(20000.0, -2090.0 - Gap) },
			{ TEXT("north"), FVector2D(-20000.0,  2090.0 + Gap), FVector2D(20000.0,  2090.0 + Gap) },
			{ TEXT("west"),  FVector2D(-3550.0 - Gap, -20000.0), FVector2D(-3550.0 - Gap, 20000.0) },
			{ TEXT("east"),  FVector2D( 1700.0 + Gap, -20000.0), FVector2D( 1700.0 + Gap, 20000.0) },
		};
	};

	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();

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
	FServiceLoopDoesNotJoinItselfTest,
	"Airside.Build.ServiceLoopDoesNotJoinItself",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FServiceLoopDoesNotJoinItselfTest::RunTest(const FString& Parameters)
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
	 * Every lane node that carries a link to a road - an incident edge no service loop owns.
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
			if (Edge.bAlive && Edge.ServiceLoopOwner.IsSet())
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
				if (Edge != nullptr && Edge->bAlive && !Edge->ServiceLoopOwner.IsSet())
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
	// The Code C ring is local X -3550..+1700, Y -2090..+2090 - see
	// UEntityDefinition::BuildCodeCStand, where it is derived from the design aircraft's
	// footprint and the anchors rather than typed. The stand sits at the origin facing +X so
	// local and world coincide, stated rather than assumed.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	FGuidelineNodeId East;
	const FGuidelineNodeId West = Lay(*Net, FVector2D(-30000.0, -2490.0), FVector2D(30000.0, -2490.0),
		ETraversalClass::GroundVehicle, East);

	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
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

	// THE NEAR SIDE, IN THE MIDDLE OF ITS OVERLAP WITH THE ROAD - not at the first corner
	// that tied. Halfway between -3550 and +1700 is -925. This is the assertion that fails
	// without GuidelineGeom::NearestBetweenPolylines breaking its tie at the middle.
	TestTrue(TEXT("the near side is entered at its middle"),
		Has(Entries, FVector2D(-925.0, -2090.0)));

	// THE END SIDES, at the corner each brings nearest the road. A corner here is right where
	// it was wrong on the near side: it genuinely IS the nearest point, and the connector
	// leaving it runs away from the lane rather than across it.
	TestTrue(TEXT("the tail end side joins at its near corner"),
		Has(Entries, FVector2D(-3550.0, -2090.0)));
	TestTrue(TEXT("the nose end side joins at its near corner"),
		Has(Entries, FVector2D(1700.0, -2090.0)));

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
	TestEqual(TEXT("a second pass adds no further entries"), EntryPoints(*Net).Num(), 3);

	// WHAT IT BUYS, MEASURED ON A JOURNEY. The GPU sits at local (300, -600) and spurs to the
	// EAST side; with one entry at the tail corner a truck drove the length of the stand and
	// back to reach it. From the nose corner it is 1490 up the east side plus a 1400 spur.
	{
		FRouteQuery Query;
		Query.Start = East;
		Query.Goal = AnchorNode(*Net, Placed, TEXT("FixedGPU"));
		Query.Class = ETraversalClass::GroundVehicle;

		const FRoutePlan Plan = RouteSearch::Find(*Net, Query);
		if (TestTrue(TEXT("a truck routes from the road to the ground power"), Plan.IsValid()))
		{
			// Measured from the ROAD's east end, so the figure is the journey and not an
			// arbitrary start: 28300 of road, then under 3500 inside the stand.
			TestTrue(TEXT("and turns in at the nearest corner rather than touring the lane"),
				GuidelineGeom::PolylineLength(Plan.Polyline) < 28300.0 + 3500.0);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTruckReachesHydrantWithoutCrossingTheAircraftTest,
	"Airside.Traffic.TruckReachesHydrantWithoutCrossingTheAircraft",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTruckReachesHydrantWithoutCrossingTheAircraftTest::RunTest(const FString& Parameters)
{
	using namespace ServiceLinkFixture;

	// THE INVARIANT, MEASURED ON A ROUTE. Airside.Entities.ServiceLoopClearsTheAircraft
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

#endif
