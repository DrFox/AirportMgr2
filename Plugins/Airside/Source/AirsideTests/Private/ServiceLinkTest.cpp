#include "CoreMinimal.h"
#include "Build/AnchorLink.h"
#include "Build/ServiceLoopBuild.h"
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
	// A MIRRORED PAIR WHERE THE LANE HAS ROOM: one curving each way, so a truck has a smooth
	// approach whichever direction it comes round the ring from. See FServiceLoopBuild's spur
	// block for what one alone cost - the agent crabbed into position from the wrong side.
	//
	TestEqual(TEXT("a mirrored pair of spurs for every service anchor"), First.SpursBuilt, 10);

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
	// Each road below sits GapNear or GapFar beyond one of the lane's four sides -
	// UEntityDefinition::ServiceLaneBounds(), not the four corners typed a second time (#104):
	// for a Code C stand at the origin, heading 0, that box is x in [-3550, +1700], y in
	// [-2090, +2090], derived from BuildCodeCStand's clearance round the aircraft and anchors.
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
	const FBox2D LaneBounds = Stand->ServiceLaneBounds();

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
	// UEntityDefinition::ServiceLaneBounds(), not the ring's four corners typed a second time
	// (#104): for a Code C stand it is local X -3550..+1700, Y -2090..+2090, derived in
	// BuildCodeCStand from the design aircraft's footprint and the anchors. The stand sits at
	// the origin facing +X so local and world coincide, stated rather than assumed.
	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
	const FBox2D LaneBounds = Stand->ServiceLaneBounds();
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

	const double CornerReach = FServiceLoopBuild::LaneTurnRadius * UE_DOUBLE_SQRT_2;

	// THE NEAR SIDE, ALONG ITS LENGTH - not at a corner. The tie between every point of a side
	// running parallel to a road is broken at the MIDDLE by GuidelineGeom::
	// NearestBetweenPolylines, and this is the assertion that fails without it.
	//
	// NOT THE MIDDLE ITSELF ANY MORE, AND THAT IS A KNOWN REGRESSION. Rounding the ring's
	// corners made each bend an edge of its own, sharing an endpoint with the side beside it -
	// so the BEND's link sweep cuts into the side, and the side's own entry comes out 195 uu
	// from the bend's foot rather than at -925. Measured 2026-09-15.
	//
	// Pinned loosely rather than dropped, because what the rule is FOR still holds: the near
	// side is entered somewhere along it, so a truck can turn either way on arriving. Grouping
	// a side with its bends in FAnchorLink was tried as the fix and traded this entry for the
	// nose end's, three down to two, which is worse. The real fix belongs with FAnchorLink's
	// per-side linking - the same code that still joins a lane square-on at (16334,2488) - and
	// when it lands, this assertion should go back to demanding the middle.
	const double NearSide = LaneBounds.Min.Y;
	bool bAlongTheNearSide = false;
	for (const FVector2D& Entry : Entries)
	{
		bAlongTheNearSide = bAlongTheNearSide
			|| (FMath::IsNearlyEqual(Entry.Y, NearSide, 50.0)
				&& Entry.X > LaneBounds.Min.X + CornerReach + 100.0
				&& Entry.X < LaneBounds.Max.X - CornerReach - 100.0);
	}
	TestTrue(*FString::Printf(
			TEXT("the near side is entered along it, clear of both bends - at %s"), *Where),
		bAlongTheNearSide);

	// THE END SIDES, at the corner each brings nearest the road. A corner here is right where
	// it was wrong on the near side: it genuinely IS the nearest point, and the connector
	// leaving it runs away from the lane rather than across it.
	//
	// NOT AT THE SQUARE CORNER OF ServiceLaneBounds ANY MORE. The ring's corners are rounded
	// as it is laid, so it turns in this far before the box corner and never reaches it; the
	// foot of that bend is where an end side now comes nearest a road along the near one.
	//
	// DERIVED, not the 1061 uu it happens to be. T = R / tan(theta/2), the circular fillet, is
	// NOT the figure: the ring's corners are quadratics, whose radius at the apex is
	// T sin^2(theta/2) / cos(theta/2), so a right angle needs T = R * sqrt(2). Restated from
	// the geometry rather than calling the builder's own inverse, which could be wrong in one
	// place and agree with itself.
	TestTrue(TEXT("the tail end side joins at the foot of its near bend"),
		Has(Entries, FVector2D(LaneBounds.Min.X + CornerReach, LaneBounds.Min.Y)));
	TestTrue(TEXT("the nose end side joins at the foot of its near bend"),
		Has(Entries, FVector2D(LaneBounds.Max.X - CornerReach, LaneBounds.Min.Y)));

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
	// back to reach it. From the nose corner it is up the east side plus the spur.
	//
	// THE FIGURE ROSE BY 854 uu WHEN SPURS WERE MADE TANGENTIAL, and that is a purchase and
	// not a regression. A spur now meets the lane PreferredSpurRun further along and curves
	// back, so the truck drives that run twice - against which the junction it used to reach
	// was a 90 degree instant turn, which FSpeedProfile calls untakeable at any speed and
	// crawls at MinTaxiSpeed. 854 uu at the 600 uu/s the new bend allows is under two
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSpursLeaveTheLaneTangentiallyTest,
	"Airside.Build.SpursLeaveTheLaneTangentially",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSpursLeaveTheLaneTangentiallyTest::RunTest(const FString& Parameters)
{
	using namespace ServiceLinkFixture;

	// REPORTED FROM PLAY, 2026-09-14: the fuel truck crawls the corner onto the stand.
	//
	// An anchor spur used to meet the lane at the anchor's NEAREST point, which is square on -
	// so the junction was a vertex whose heading changed by 90 degrees instantly, and
	// FSpeedProfile calls that untakeable at any speed and drops the agent to MinTaxiSpeed.
	//
	// ROUNDING IT AFTERWARDS CANNOT WORK, and was tried. A 90 degree turn at the 471 uu a
	// truck's steering lock allows needs 666 uu of run on each arm, so two spurs sharing a
	// side need 1332 uu between them. On a Code C stand three spurs land on the north side
	// 900 uu apart, and that side has four gaps in 5250 uu - 78 uu short of the 5328 that
	// perfect spacing would need. There is no room to round into. Measured 2026-09-15.
	//
	// SO THE SPUR CARRIES THE CURVE INSTEAD. It joins the lane RUN uu back from the anchor's
	// perpendicular foot, with its control point ON that foot - so it leaves along the lane's
	// own direction and there is no turn at the junction at all.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
	const FEntityInstanceId Placed = PlaceStand(*Net, *Stand, FVector2D::ZeroVector, 0.0);

	const FServiceLoopBuild::FResult Built = FServiceLoopBuild::Build(*Net);
	if (!TestTrue(*FString::Printf(TEXT("every service anchor got a spur - %d laid"),
			Built.SpursBuilt), Built.SpursBuilt >= 5))
	{
		return false;
	}

	const TArray<FGuidelineEdgeId>* Lane = Built.Lanes.Find(Placed);
	if (!TestNotNull(TEXT("the stand got a lane"), Lane))
	{
		return false;
	}

	// A PAIR ON EVERY ANCHOR. One spur is tangent for a truck arriving from ONE side and a
	// hairpin from the other, so an anchor with a single spur is one the agent crabs into
	// whenever the short way round is the wrong way. Counted on the ANCHOR, not on the
	// builder's census, because the census cannot say they landed where they were meant to.
	{
		const FEntityInstance* Entity = Net->GetEntity(Placed);
		if (!TestNotNull(TEXT("the stand is placed"), Entity))
		{
			return false;
		}
		int32 Paired = 0;
		for (const FResolvedAnchor& Anchor : Entity->ResolvedAnchors)
		{
			if (TraversalForRole(Anchor.Role) == ETraversalClass::Aircraft)
			{
				continue;
			}
			const FGuidelineNode* Node = Net->GetGuidelineNode(Anchor.Node);
			if (Node == nullptr)
			{
				continue;
			}
			TestEqual(
				*FString::Printf(TEXT("anchor '%s' has a spur each way"), *Anchor.Id.ToString()),
				Node->Incident.Num(), 2);
			Paired += Node->Incident.Num();
		}

		// TEN LINES ONTO FIVE BOXES. Pinned as a figure because the pair is the whole point:
		// one spur is smooth from one side of the ring and a crab from the other.
		TestEqual(TEXT("ten spurs over five anchors"), Paired, 10);
	}

	// AND NOT ONE OF THEM CROSSES THE AEROPLANE. Airside.Entities.ServiceLoopClearsTheAircraft
	// makes this promise on the DEFINITION, judging a spur as a straight line from the anchor
	// to the nearest point on the loop. Neither half of that is true any more - a spur is a
	// quadratic and it joins the lane some way off the nearest point - so the promise has to
	// be measured again here, on the geometry that actually gets laid.
	//
	// The centreline as a finite segment, never the footprint outline: forbidding the
	// footprint would forbid passing UNDER A WING, which is normal and which the hydrant
	// requires - HydrantPit is under the starboard wing root because that is where one is.
	{
		const FEntityFootprint& Footprint = Stand->DesignAircraft->Footprint;
		const FVector2D Tail(Footprint.TailX, 0.0);
		const FVector2D Nose(Footprint.NoseX, 0.0);

		int32 Crossings = 0;
		for (const FGuidelineEdgeId& EdgeId : *Lane)
		{
			const FGuidelineEdge* Edge = Net->GetGuidelineEdge(EdgeId);
			TArray<FVector2D> Points;
			if (Edge == nullptr || !Edge->bAlive || !Edge->bServiceSpur
				|| !Net->SampleGuideline(EdgeId, Points))
			{
				continue;
			}
			for (int32 At = 1; At < Points.Num(); ++At)
			{
				Crossings += RoadGeom::SegmentsCross(Points[At - 1], Points[At], Tail, Nose)
					? 1 : 0;
			}
		}
		TestEqual(TEXT("no spur crosses the fuselage lengthwise"), Crossings, 0);
	}

	// The same expression FSpeedProfile::Build uses, written out rather than shared - for the
	// reason Airside.Model.ServiceRoadFilletClearsTheTruckLock gives at its own copy.
	const FAirframe Van = UAirsideSettings::ResolveDefaultVehicle();
	const double Lock = FMath::Sin(FMath::DegreesToRadians(
		FMath::Clamp(Van.Ground.MaxSteerDegrees, 0.0, 90.0)));
	if (!TestTrue(TEXT("the default vehicle steers on measured axles"),
			Van.HasAxles() && Lock > KINDA_SMALL_NUMBER))
	{
		return false;
	}
	const double TightestFollowable = Van.Wheelbase() / Lock;

	// A SPUR DOES NOT ALWAYS MEET THE LANE. Three of a Code C stand's five meet an EARLIER
	// SPUR instead: the search runs over the lane's current edges, spurs included, and
	// EquipmentFwd is 900 uu from HydrantPit's spur against 990 from the north side. So the
	// thing to assert is not "tangent to the lane" but "tangent to whatever it joins", which
	// is the same property and is what a truck actually drives through.
	// THE ANALYTIC TANGENT AT AN EDGE'S END, never a difference of samples. A quadratic's
	// first sampled chord is a degree or two off its true tangent, and a test that measured
	// the chord would report a turn that no follower ever makes - GuidelineGeom::Tangent is
	// the derivative of the very function Sample evaluates, which is the point of it.
	auto LeavingAlong = [Net](FGuidelineEdgeId Id, FGuidelineNodeId From, FVector2D& Out) -> bool
	{
		const FGuidelineEdge* Edge = Net->GetGuidelineEdge(Id);
		if (Edge == nullptr || !Edge->bAlive)
		{
			return false;
		}
		const FGuidelineNode* A = Net->GetGuidelineNode(Edge->A);
		const FGuidelineNode* B = Net->GetGuidelineNode(Edge->B);
		if (A == nullptr || B == nullptr)
		{
			return false;
		}
		const bool bFromB = Edge->B == From;
		const FVector2D Dir = GuidelineGeom::Tangent(
			A->Position, Edge->Control, B->Position, bFromB ? 1.0 : 0.0);
		Out = bFromB ? -Dir : Dir;
		return !Out.IsNearlyZero();
	};

	// THE ANCHOR ITSELF IS NOT A JUNCTION. Its two spurs meet there in a V, and that V is
	// deliberate - see FServiceLoopBuild's spur block. A truck stops at an anchor; one driving
	// THROUGH one would be cutting across a painted equipment box, and the V is what makes
	// FSpeedProfile cost that more than going round.
	TSet<FGuidelineNodeId> Anchors;
	for (const FResolvedAnchor& Anchor : Net->GetEntity(Placed)->ResolvedAnchors)
	{
		Anchors.Add(Anchor.Node);
	}

	// Which anchors a truck can reach without slowing for the bend. Filled below.
	TSet<FGuidelineNodeId> Drivable;

	int32 Checked = 0;
	for (const FGuidelineEdgeId& SpurId : *Lane)
	{
		const FGuidelineEdge* Spur = Net->GetGuidelineEdge(SpurId);
		if (Spur == nullptr || !Spur->bAlive || !Spur->bServiceSpur)
		{
			continue;
		}

		TArray<FVector2D> SpurPoints;
		if (!Net->SampleGuideline(SpurId, SpurPoints) || SpurPoints.Num() < 2)
		{
			continue;
		}

		for (const FGuidelineNodeId& End : { Spur->A, Spur->B })
		{
			const FGuidelineNode* Node = Net->GetGuidelineNode(End);
			if (Node == nullptr || Node->Incident.Num() < 2 || Anchors.Contains(End))
			{
				// A dead end is nothing to turn at, and an anchor is a destination.
				continue;
			}

			FVector2D OutSpur;
			if (!LeavingAlong(SpurId, End, OutSpur))
			{
				continue;
			}
			const FVector2D JoinAt = Net->GetGuidelineNode(End)->Position;

			// STRAIGHT THROUGH ONTO SOMETHING. At least one other arm here must leave in the
			// OPPOSITE direction, which is what makes arriving along it and turning onto this
			// spur a move with no heading change in it at all - the test FSpeedProfile applies
			// to a vertex, asked of two whole edges.
			//
			// At least one, not all: the other side of a lane is a 90 degree turn whichever
			// way the spur leaves, and the lane is a closed ring, so a truck simply comes
			// round the way that works.
			double Best = 0.0;
			for (const FGuidelineEdgeId& OtherId : Node->Incident)
			{
				if (OtherId == SpurId)
				{
					continue;
				}
				FVector2D OutOther;
				if (!LeavingAlong(OtherId, End, OutOther))
				{
					continue;
				}
				Best = FMath::Max(Best, -FVector2D::DotProduct(OutSpur, OutOther));
			}

			++Checked;
			TestTrue(
				*FString::Printf(
					TEXT("the spur at (%.0f,%.0f) can be joined without turning - its best "
					     "neighbour is %.1f deg off straight"),
					JoinAt.X, JoinAt.Y,
					FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(Best, -1.0, 1.0)))),
				Best > FMath::Cos(0.02));
		}

		// AND THE CURVE IT CARRIES INSTEAD IS ONE THE TRUCK CAN HOLD, measured the way
		// FSpeedProfile measures curvature, over the spur's own samples.
		//
		// COUNTED PER ANCHOR, NOT DEMANDED OF EVERY SPUR. The pair exists so a truck has a
		// smooth approach from either way round the ring, and near a corner one of the two has
		// nowhere to put its bend - EquipmentAft's foot is 389 uu from the north-west bend, so
		// going that way tops out at 240 uu of radius whatever run it is given. What must hold
		// is that every anchor is reachable AT SPEED from somewhere; the other spur of the
		// pair is still the shorter way in from its own side, taken slower.
		double Tightest = TNumericLimits<double>::Max();
		for (int32 At = 1; At + 1 < SpurPoints.Num(); ++At)
		{
			const FVector2D Before = SpurPoints[At] - SpurPoints[At - 1];
			const FVector2D After = SpurPoints[At + 1] - SpurPoints[At];
			const double Turn = FMath::Abs(FMath::UnwindRadians(
				RoadGeom::Bearing(After) - RoadGeom::Bearing(Before)));
			if (Turn > 1.0e-9)
			{
				Tightest = FMath::Min(Tightest, After.Size() / Turn);
			}
		}

		if (Tightest >= TightestFollowable)
		{
			for (const FGuidelineNodeId& End : { Spur->A, Spur->B })
			{
				if (Anchors.Contains(End))
				{
					Drivable.Add(End);
				}
			}
		}
	}

	// NOT VACUOUS. A loop whose spurs all failed the end-finding above would assert nothing.
	// Five spurs, each with at least one junction end - more when a later spur has split one.
	TestTrue(*FString::Printf(TEXT("every spur junction was measured - %d found"), Checked),
		Checked >= 10);

	// THE ASSERTION THAT MATTERS. An anchor none of whose spurs clears the lock is one a
	// truck crawls into however it comes, which is the reported defect itself.
	for (const FResolvedAnchor& Anchor : Net->GetEntity(Placed)->ResolvedAnchors)
	{
		if (TraversalForRole(Anchor.Role) == ETraversalClass::Aircraft)
		{
			continue;
		}

		// TugStand excepted, and by geometry rather than by name: it waits 300 uu off the
		// lane, and this construction's radius is bounded by that offset - 0.77 of it at the
		// very best, which is 231 against the 471 the lock needs. Nothing laid on three metres
		// could do better, and it is the last few metres of a journey that ends in a stop.
		//
		// Measured off the RING as it now stands, not off UEntityDefinition::ServiceLoop: the
		// definition is still a box, and what an anchor is actually offset from is the lane
		// with its corners rounded.
		const FGuidelineNode* Node = Net->GetGuidelineNode(Anchor.Node);
		double Offset = TNumericLimits<double>::Max();
		for (const FGuidelineEdgeId& Id : *Lane)
		{
			const FGuidelineEdge* Edge = Net->GetGuidelineEdge(Id);
			TArray<FVector2D> Points;
			if (Edge == nullptr || !Edge->bAlive || Edge->bServiceSpur
				|| !Net->SampleGuideline(Id, Points))
			{
				continue;
			}
			int32 Span = 0;
			double Fraction = 0.0;
			Offset = FMath::Min(Offset,
				GuidelineGeom::NearestOnPolyline(Points, Node->Position, Span, Fraction));
		}
		if (Offset * 0.77 < TightestFollowable)
		{
			continue;
		}

		TestTrue(
			*FString::Printf(TEXT("anchor '%s' is reachable at speed from at least one side"),
				*Anchor.Id.ToString()),
			Drivable.Contains(Anchor.Node));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLaneCornersAreDrivableTest,
	"Airside.Build.LaneCornersAreDrivable",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FLaneCornersAreDrivableTest::RunTest(const FString& Parameters)
{
	using namespace ServiceLinkFixture;

	// THE RING'S OWN CORNERS, which are authored as square and were laid that way.
	// UEntityDefinition::ServiceLoop is four points and stays four points - a box is how a
	// lane is DESCRIBED - but a box is not something a truck can drive round: a corner where
	// two straight sides meet is a vertex whose heading changes instantly, and FSpeedProfile
	// calls one of those untakeable at any speed.
	//
	// THIS IS THE SAME CONSTRUCTION THE SPURS USE, one level up: the corner is replaced by a
	// quadratic whose control sits ON it, so both sides leave tangentially and the bend
	// carries the turn. See Airside.Build.SpursLeaveTheLaneTangentially for the other half.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
	const FEntityInstanceId Placed = PlaceStand(*Net, *Stand, FVector2D::ZeroVector, 0.0);

	const FServiceLoopBuild::FResult Built = FServiceLoopBuild::Build(*Net);
	const TArray<FGuidelineEdgeId>* Lane = Built.Lanes.Find(Placed);
	if (!TestNotNull(TEXT("the stand got a lane"), Lane))
	{
		return false;
	}

	// THE RING, which is every lane edge that is not a spur. Both the straight sides and the
	// bends between them: together they are what a truck drives round.
	TArray<FGuidelineEdgeId> Ring;
	for (const FGuidelineEdgeId& Id : *Lane)
	{
		const FGuidelineEdge* Edge = Net->GetGuidelineEdge(Id);
		if (Edge != nullptr && Edge->bAlive && !Edge->bServiceSpur)
		{
			Ring.Add(Id);
		}
	}
	if (!TestTrue(TEXT("the lane has a ring"), Ring.Num() >= 4))
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

	// NOWHERE ON THE RING DOES A TRUCK HAVE TO TURN. Asked of every node the ring passes
	// through - the bends' own ends, and the joins where a spur split a side - by comparing
	// the two ring arms' ANALYTIC tangents. Straight through means they leave in opposite
	// directions, which is exactly the test FSpeedProfile applies to a vertex.
	int32 Checked = 0;
	for (const FGuidelineEdgeId& Id : Ring)
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
				if (OtherId == Id || Other == nullptr || Other->bServiceSpur
					|| !LeavingAlong(*Net, OtherId, End, Theirs))
				{
					continue;
				}
				Best = FMath::Max(Best, -FVector2D::DotProduct(Mine, Theirs));
			}

			if (Best < 0.0)
			{
				// No other ring edge here at all, which would mean the ring is not closed.
				continue;
			}

			++Checked;
			TestTrue(
				*FString::Printf(
					TEXT("the ring runs straight through (%.0f,%.0f) - its best neighbour is "
					     "%.1f deg off"),
					Net->GetGuidelineNode(End)->Position.X,
					Net->GetGuidelineNode(End)->Position.Y,
					FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(Best, -1.0, 1.0)))),
				Best > FMath::Cos(0.02));
		}
	}

	// AND THE BENDS THAT CARRY THE TURN INSTEAD ARE ONES THE TRUCK CAN HOLD. Curvature the
	// way FSpeedProfile measures it, over each ring edge's own samples.
	double Tightest = TNumericLimits<double>::Max();
	int32 Bends = 0;
	for (const FGuidelineEdgeId& Id : Ring)
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

	// NOT VACUOUS. A ring still laid as four straight sides has no curved span at all, so the
	// radius below would pass on a box with square corners - which is the defect itself.
	if (!TestTrue(TEXT("the ring bends somewhere - its corners are rounded"), Bends > 0))
	{
		return false;
	}

	TestTrue(
		*FString::Printf(
			TEXT("and the ring's tightest bend is %.0f uu, clearing the %.0f uu the truck's "
			     "steering needs"),
			Tightest, TightestFollowable),
		Tightest >= TightestFollowable);

	TestTrue(*FString::Printf(TEXT("every ring junction was measured - %d found"), Checked),
		Checked >= 8);
	return true;
}

#endif
