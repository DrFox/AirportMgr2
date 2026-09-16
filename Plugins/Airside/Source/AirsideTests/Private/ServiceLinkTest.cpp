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
	 * The nodes a stand's declared entries became.
	 *
	 * ASKED OF THE BUILDER, which is the only thing that knows: nothing marks a node as "an
	 * entry waypoint became this" once it is laid - see FStandLaneBuild::FResult::Entries.
	 *
	 * SAFE AFTER FAnchorLink::Build, and that is the point of calling it that way round here.
	 * The lane is already in the graph by then, so this takes the builder's idempotent skip
	 * path and RECOVERS the entries rather than laying a second lane - which is the same path
	 * the linking pass itself took, so a recovery that disagreed with the laying would show up
	 * as a test that cannot find the node the road is plainly joined to.
	 */
	TArray<FGuidelineNodeId> EntriesOf(URoadNetwork& Net, FEntityInstanceId Entity)
	{
		const FStandLaneBuild::FResult Built = FStandLaneBuild::Build(Net);
		const TArray<FGuidelineNodeId>* Found = Built.Entries.Find(Entity);
		return Found != nullptr ? *Found : TArray<FGuidelineNodeId>();
	}

	/**
	 * Where each declared entry that a road actually joined is.
	 *
	 * THREE EDGES ON A NODE THE LANE GAVE TWO. Every entry node sits mid-cycle with a lane edge
	 * either side of it, so counting edges is the whole test - and counting them on the ENTRY
	 * rather than looking for unowned edges near the lane is what makes this measure the claim
	 * "a road joined THIS declared entry" instead of "something unowned is lying about".
	 */
	TArray<FVector2D> LinkedEntries(const URoadNetwork& Net, const TArray<FGuidelineNodeId>& Entries)
	{
		TArray<FVector2D> At;
		for (const FGuidelineNodeId& Id : Entries)
		{
			const FGuidelineNode* Node = Net.GetGuidelineNode(Id);
			if (Node != nullptr && Node->Incident.Num() > 2)
			{
				At.Add(Node->Position);
			}
		}
		return At;
	}

	/** The linked entries as a string, so a failure names WHICH ones rather than how many. */
	FString Where(const TArray<FVector2D>& Points)
	{
		FString Out;
		for (const FVector2D& Point : Points)
		{
			Out += FString::Printf(TEXT("(%.0f,%.0f) "), Point.X, Point.Y);
		}
		return Out;
	}

	/** A stand at the origin facing +X with one straight service road laid beside it. */
	struct FStandBesideARoad
	{
		URoadNetwork* Net = nullptr;
		FEntityInstanceId Placed;
		FGuidelineNodeId RoadNear, RoadFar;
	};

	FStandBesideARoad StandBesideARoad(UEntityDefinition& Stand,
		const FVector2D& RoadFrom, const FVector2D& RoadTo)
	{
		FStandBesideARoad Built;
		Built.Net = NewObject<URoadNetwork>(GetTransientPackage());
		Built.RoadNear = Lay(*Built.Net, RoadFrom, RoadTo, ETraversalClass::GroundVehicle, Built.RoadFar);
		Built.Placed = PlaceStand(*Built.Net, Stand, FVector2D::ZeroVector, 0.0);
		FAnchorLink::Build(*Built.Net);
		return Built;
	}
}

namespace ServiceLinkFixture
{
	/**
	 * The edges one link laid, from the entry outwards: the lead-in, then the MERGE sweep, then
	 * the turn-back. Empty when this entry took no link.
	 *
	 * OFF THE GRAPH, not off the builder's word for it. A link is three edges - a lead-in from
	 * the declared entry to a node short of the road, and a sweep onto the road each way - and
	 * all three are UNOWNED, which is what tells them from the lane they leave (see
	 * FGuidelineEdge's StandGeometryOwner, and IsServiceNodeConnected, which lives on the same
	 * distinction).
	 *
	 * THE MERGE BEFORE THE TURN-BACK, and told apart by geometry rather than by index: both
	 * sweeps leave the lead-in's far end along the same tangent and both meet the road
	 * tangentially, so what differs is WHICH WAY along the road they go. The one on the side the
	 * lead-in was already pointing is the merge, and it turns by the transition's deflection;
	 * the other turns by 180 minus it. Their radii differ by an order of magnitude at a close
	 * gap and the test would be measuring a coin toss without this.
	 */
	TArray<FGuidelineEdgeId> LinkEdgesAt(const URoadNetwork& Net, FGuidelineNodeId Entry)
	{
		auto Unowned = [&Net](FGuidelineEdgeId Id)
		{
			const FGuidelineEdge* Edge = Net.GetGuidelineEdge(Id);
			return Edge != nullptr && Edge->bAlive && !Edge->StandGeometryOwner.IsSet();
		};

		TArray<FGuidelineEdgeId> Found;
		const FGuidelineNode* Node = Net.GetGuidelineNode(Entry);
		if (Node == nullptr)
		{
			return Found;
		}

		FGuidelineEdgeId LeadId;
		for (const FGuidelineEdgeId& Id : Node->Incident)
		{
			if (Unowned(Id)) { LeadId = Id; }
		}
		if (!LeadId.IsSet())
		{
			return Found;
		}
		Found.Add(LeadId);

		const FGuidelineEdge* Lead = Net.GetGuidelineEdge(LeadId);
		const FGuidelineNodeId LeadEndId = Lead->A == Entry ? Lead->B : Lead->A;
		const FGuidelineNode* LeadEnd = Net.GetGuidelineNode(LeadEndId);
		if (LeadEnd == nullptr)
		{
			return Found;
		}

		// The sweeps share the lead-in's far end and take their control from the point on the
		// road the transition aims at - the same point for both, which is why comparing against
		// it separates them.
		FVector2D Arriving = FVector2D::ZeroVector;
		if (!LeavingAlong(Net, LeadId, LeadEndId, Arriving))
		{
			return Found;
		}
		Arriving = -Arriving;   // The direction the lead-in ARRIVES in, not the way back up it.

		// AHEAD OF THE MEETING POINT, measured from the sweep's own control - which IS that
		// point, for both of them. A sweep whose road end lies further along the way the lead-in
		// was heading is the merge; the one behind it is the turn-back.
		auto Ahead = [&Net, &Arriving, LeadEndId](const FGuidelineEdgeId& Id)
		{
			const FGuidelineEdge* Edge = Net.GetGuidelineEdge(Id);
			const FGuidelineNode* Far =
				Net.GetGuidelineNode(Edge->A == LeadEndId ? Edge->B : Edge->A);
			return Far != nullptr
				? FVector2D::DotProduct(Far->Position - Edge->Control, Arriving)
				: 0.0;
		};

		TArray<FGuidelineEdgeId> Sweeps;
		for (const FGuidelineEdgeId& Id : LeadEnd->Incident)
		{
			if (Id != LeadId && Unowned(Id)) { Sweeps.Add(Id); }
		}
		Sweeps.Sort([&Ahead](const FGuidelineEdgeId& L, const FGuidelineEdgeId& R)
		{
			return Ahead(L) > Ahead(R);
		});
		Found.Append(Sweeps);
		return Found;
	}

	/**
	 * A delivered radius as a reader can take it in.
	 *
	 * A STRAIGHT EDGE HAS NO RADIUS AT ALL and TightestRadius says so with a numeric maximum, so
	 * a diagnostic that printed the number would be three hundred digits of nothing. Infinity is
	 * also the RIGHT answer for a lead-in on the crossing branch, which leaves the lane along
	 * the lane, so this case is common rather than exotic.
	 */
	FString Curvature(double Radius)
	{
		return Radius > 1.0e6 ? TEXT("straight") : FString::Printf(TEXT("%.0f"), Radius);
	}

	/** The radius the edge AS LAID delivers, which is the one a truck has to follow. */
	double DeliveredRadius(const URoadNetwork& Net, FGuidelineEdgeId Id)
	{
		const FGuidelineEdge* Edge = Net.GetGuidelineEdge(Id);
		const FGuidelineNode* A = Edge != nullptr ? Net.GetGuidelineNode(Edge->A) : nullptr;
		const FGuidelineNode* B = Edge != nullptr ? Net.GetGuidelineNode(Edge->B) : nullptr;
		if (A == nullptr || B == nullptr)
		{
			return 0.0;
		}
		return GuidelineGeom::TightestRadius(A->Position, Edge->Control, B->Position);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FServiceLaneEntersOnEverySideWithinReachTest,
	"Airside.Build.ServiceLaneEntersOnEverySideWithinReach",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FServiceLaneEntersOnEverySideWithinReachTest::RunTest(const FString& Parameters)
{
	using namespace ServiceLinkFixture;

	// A ROAD JOINS A DECLARED ENTRY, and this measures the declaration rather than a
	// heuristic. The ring this replaces had to DISCOVER which sides of itself a road was
	// beside - IsLaneBend, WholeSide and three thresholds tuned against one another - because
	// it declared no entrances at all. A stand now authors four, at the corners where a
	// crossing meets a run (see UEntityDefinition::BuildCodeCStandFor), and what is left to
	// measure is which of them a given road should have.
	//
	// EIGHT NODES, NOT FOUR, and that is the shape of the thing rather than a defect. An entry
	// is authored AT a corner and a rounded corner carries no node of its own: the bend's
	// control sits on the corner and its two ends sit back along the two legs. So each declared
	// entry offers a PAIR - one node on the run, one on the crossing, each with a clean heading
	// along its own straight - and the linking pass takes whichever is nearer the road.
	//
	// ServiceLinkFixture::LaneBoundsOf, not the lane's corners typed a second time (#104):
	// derived in BuildCodeCStand from the design aircraft's footprint and the anchors. The
	// stand sits at the origin facing +X so local and world coincide, stated rather than
	// assumed.
	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
	const FBox2D LaneBounds = LaneBoundsOf(*Stand);

	// 4 m clear of the lane, which is how close a player draws a service road to a stand. The
	// figure is only ever a clearance here: nothing in the rule under test is tuned to it.
	constexpr double RoadClearance = 400.0;

	// A ROAD ALONGSIDE, past the port run. The whole case: a service road running past a row of
	// stands, which is how a player builds one.
	FStandBesideARoad Alongside;
	{
		const double RoadY = LaneBounds.Min.Y - RoadClearance;
		Alongside = StandBesideARoad(*Stand,
			FVector2D(-30000.0, RoadY), FVector2D(30000.0, RoadY));

		const TArray<FGuidelineNodeId> Entries = EntriesOf(*Alongside.Net, Alongside.Placed);
		TestEqual(TEXT("four declared entries, each a pair of nodes round its rounded corner"),
			Entries.Num(), 8);

		const TArray<FVector2D> Linked = LinkedEntries(*Alongside.Net, Entries);
		const FString At = Where(Linked);

		// TWO, ONE AT EACH END OF THE SIDE THE ROAD RUNS PAST - which is what makes the lane a
		// drive-through rather than a cul-de-sac. With one entrance a truck drove up to half
		// the lane's perimeter to reach an anchor a few metres from where it came in.
		//
		// THE POSITIONS ARE IN THE MESSAGE, not just the count. A bare "expected 2, got 4" says
		// nothing about WHICH entries linked wrongly, and every failure this test found while
		// it was being written was diagnosed from exactly this list.
		TestEqual(*FString::Printf(
				TEXT("a road alongside is joined at both ends of the side it passes - at %s"), *At),
			Linked.Num(), 2);

		for (const FVector2D& Entry : Linked)
		{
			// ON THE NEAR RUN ITSELF, which says both which SIDE was chosen and which node of
			// each pair: the run's node lies exactly on the run, the crossing's sits back up
			// the crossing. A road parallel to the run is nearer the first.
			TestEqual(*FString::Printf(TEXT("and on the near run itself - at %s"), *At),
				Entry.Y, LaneBounds.Min.Y, 1.0);
		}

		// AND ONE AT EACH END rather than two side by side. The two entries are at opposite
		// ends of a lane 62 m long; anything less than half of that is two entrances at one
		// corner, which is the defect the pair rule exists to prevent.
		if (Linked.Num() == 2)
		{
			TestTrue(*FString::Printf(TEXT("one at each end of the run, not two at one - at %s"), *At),
				FMath::Abs(Linked[0].X - Linked[1].X) > LaneBounds.GetSize().X * 0.5);
		}

		// AND THE FAR SIDE IS NOT JOINED, though it is 2100 uu from the road and the service
		// radius is 62 m. Its connector would run the whole depth of the stand, across both of
		// the lane's crossings, to reach a road the near run is 4 m from.
		//
		// REFUSED BY MEASURING, NOT BY A THRESHOLD. The rule is that the entry NEAREST a point
		// of road is the one that gets it - see FAnchorLink::Gather - which is the same
		// sentence that refuses the far END of a stand for a road at one end of it. The
		// service radius is the player's knob for how far a stand may sit from its road and
		// must not silently double as this rule.
		for (const FVector2D& Entry : Linked)
		{
			TestTrue(*FString::Printf(TEXT("no entry on the far side of the aeroplane - at %s"), *At),
				Entry.Y < LaneBounds.GetCenter().Y);
		}

		// IDEMPOTENT. The graph is rebuilt on every road edit and this runs each time; a pass
		// that could not see its own previous links would stack an entrance per rebuild.
		//
		// ASKED OF THE WHOLE CORNER, which is what makes this more than a formality now that an
		// entry is a PAIR: the joined node reads as taken, and its sibling 300 uu away reads as
		// free unless the pass looks at both.
		FAnchorLink::Build(*Alongside.Net);
		const TArray<FVector2D> Again = LinkedEntries(*Alongside.Net,
			EntriesOf(*Alongside.Net, Alongside.Placed));
		TestEqual(*FString::Printf(TEXT("a second pass adds no further entrances - at %s"),
				*Where(Again)),
			Again.Num(), 2);
	}

	// WHICH NODE OF THE PAIR, measured by putting the road on the two sides that disagree.
	//
	// A road OUTBOARD of a run arrives square to it and the run's own node is nearest; a road
	// across the END of the stand arrives along the crossing, and the crossing's node - which
	// is the one whose heading points at it - is nearest. The pair exists precisely so that
	// both of those have an entrance with a heading a vehicle can leave on.
	{
		const double RoadY = LaneBounds.Max.Y + RoadClearance;
		const FStandBesideARoad Outboard = StandBesideARoad(*Stand,
			FVector2D(-30000.0, RoadY), FVector2D(30000.0, RoadY));

		const TArray<FVector2D> Linked =
			LinkedEntries(*Outboard.Net, EntriesOf(*Outboard.Net, Outboard.Placed));
		const FString At = Where(Linked);

		TestEqual(*FString::Printf(TEXT("a road outboard of the box row joins two entries - at %s"), *At),
			Linked.Num(), 2);
		for (const FVector2D& Entry : Linked)
		{
			TestEqual(*FString::Printf(TEXT("and takes the node ON the row - at %s"), *At),
				Entry.Y, LaneBounds.Max.Y, 1.0);
		}
	}

	{
		const double RoadX = LaneBounds.Max.X + RoadClearance;
		const FStandBesideARoad Ahead = StandBesideARoad(*Stand,
			FVector2D(RoadX, -30000.0), FVector2D(RoadX, 30000.0));

		const TArray<FVector2D> Linked =
			LinkedEntries(*Ahead.Net, EntriesOf(*Ahead.Net, Ahead.Placed));
		const FString At = Where(Linked);

		TestEqual(*FString::Printf(TEXT("a road across the nose joins two entries - at %s"), *At),
			Linked.Num(), 2);
		for (const FVector2D& Entry : Linked)
		{
			// NEITHER RUN. Both runs are at a fixed Y - the box row and the port line the
			// bridge sits on - so "off both of them" is exactly "on the crossing", and the
			// crossing's node is the one that faces a road ahead of the aeroplane.
			TestTrue(*FString::Printf(TEXT("and takes the node back along the crossing - at %s"), *At),
				Entry.Y > LaneBounds.Min.Y + 1.0 && Entry.Y < LaneBounds.Max.Y - 1.0);
		}

		// AND THE CURVE ONTO THE ROAD IS ONE A TRUCK CAN FOLLOW, which is the property this
		// fixture is uniquely placed to measure: it is the only one in the suite that reaches
		// FAnchorLink::Join's CROSSING branch - a road across the end of a stand meets the
		// lane's own heading at 45 degrees rather than lying beside it, so the connector runs
		// on to where they meet and rounds the corner instead of changing lanes.
		//
		// EVERY ONE OF THEM, which is what the figures allow: the nose crossing's two entries
		// face the road along converging diagonals and aim at nearly the same point of it, so
		// the second to be joined meets the road inside the fillet the first already cut - and
		// what that costs it is a SHALLOWER merge rather than a tighter one. Both clear.
		// Airside.Build.StandLinkClearsTheTruckLock prints both figures and pins the lead-in as
		// straight, which is what tells this branch from the lane change.
		const FAirframe Truck = UAirsideSettings::ResolveLargestServiceVehicle();
		const double Lock = FMath::Sin(FMath::DegreesToRadians(
			FMath::Clamp(Truck.Ground.MaxSteerDegrees, 0.0, 90.0)));
		const double Followable = Lock > KINDA_SMALL_NUMBER ? Truck.Wheelbase() / Lock : 0.0;

		FString Merges;
		for (const FGuidelineNodeId& Entry : EntriesOf(*Ahead.Net, Ahead.Placed))
		{
			const TArray<FGuidelineEdgeId> Link = LinkEdgesAt(*Ahead.Net, Entry);
			if (Link.Num() < 2)
			{
				continue;
			}
			const double Merge = DeliveredRadius(*Ahead.Net, Link[1]);
			Merges += Curvature(Merge) + TEXT(" ");
			TestTrue(*FString::Printf(
					TEXT("and the merge onto it clears a %.0f uu lock - %s"),
					Followable, *Curvature(Merge)),
				Merge >= Followable);

			// THE LEAD-IN IS STRAIGHT on this branch, by construction: it runs along the lane's
			// own heading to the corner, so there is no curve on it to be tight. Measuring it
			// is what would catch the S being taken here by mistake, which is how this fixture
			// failed when the crossing branch did not yet exist.
			TestTrue(*FString::Printf(
					TEXT("the lead-in onto a crossing road is straight - %s uu"),
					*Curvature(DeliveredRadius(*Ahead.Net, Link[0]))),
				DeliveredRadius(*Ahead.Net, Link[0]) > Followable);
		}
		AddInfo(FString::Printf(TEXT("NOSE merges: %s"), *Merges));
	}

	// WHAT IT BUYS, MEASURED ON A JOURNEY. The GPU sits at local (300, -600), on the port run;
	// a truck coming down the road should turn in at the near end of that run rather than tour
	// the lane to reach it.
	//
	// STILL NOWHERE NEAR A TOUR, which is what this bound exists to catch: half this lane's
	// perimeter is another 9400 uu on top. Measured from the ROAD's east end, so the figure is
	// the journey and not an arbitrary start.
	{
		FRouteQuery Query;
		Query.Start = Alongside.RoadFar;
		Query.Goal = AnchorNode(*Alongside.Net, Alongside.Placed, TEXT("FixedGPU"));
		Query.Class = ETraversalClass::GroundVehicle;

		const FRoutePlan Plan = RouteSearch::Find(*Alongside.Net, Query);
		if (TestTrue(TEXT("a truck routes from the road to the ground power"), Plan.IsValid()))
		{
			TestTrue(*FString::Printf(
					TEXT("and turns in at the nearest entry rather than touring the lane "
					     "- %.0f uu"),
					GuidelineGeom::PolylineLength(Plan.Polyline)),
				GuidelineGeom::PolylineLength(Plan.Polyline) < 28300.0 + 4700.0);

			// THE ROUTE'S OWN CORNERS, REPORTED AND NOT ASSERTED. The entrance is where the
			// stand says it is and it leaves ALONG the lane, on a run sized from the radius it
			// owes rather than from a constant - GuidelineGeom::ShiftDeflectionFor, which is
			// what makes the curves on this route ones a truck can actually follow. The RADII
			// are asserted by Airside.Build.StandLinkClearsTheTruckLock, which measures the
			// edges as laid; this prints what the ROUTER hands a driver, end to end, because a
			// route can still be poor while every edge on it is fine - a detour, or a turn at a
			// junction the link never made. What would show here is an instant heading change,
			// and there is none left on it.
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
	FStandLinkClearsTheTruckLockTest,
	"Airside.Build.StandLinkClearsTheTruckLock",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandLinkClearsTheTruckLockTest::RunTest(const FString& Parameters)
{
	using namespace ServiceLinkFixture;

	// THE ONE CURVE NOBODY MEASURED. Every corner INSIDE the lane is held against the truck's
	// steering lock - Airside.Build.LaneCornersAreDrivable, PlacedStandLaneIsOneDrivableCycle,
	// Airside.Entities.StandLaneCornersClearTheTruckLock - and the whole reason the lane was
	// redesigned on 2026-09-16 is that the ring's corners were tighter than a real 8.5 m
	// dispenser can follow. The LINK from the road onto that lane had no such test, and it was
	// delivering 40 uu where 699 is needed: a curve tighter than the lock is untakeable at any
	// speed, not merely slow, so a truck could not drive onto the stand at all.
	//
	// THE RADIUS AS LAID, NEVER THE RUN REQUESTED. Commit 8be494c cost this project three
	// sessions because a test checked the figure a builder asked for while the follower drove
	// the figure it got, and ServiceRoadFilletClearsTheTruckLock was green throughout. So this
	// measures GuidelineGeom::TightestRadius on the edge that is in the graph.
	const FAirframe Truck = UAirsideSettings::ResolveLargestServiceVehicle();
	const double Lock = FMath::Sin(FMath::DegreesToRadians(
		FMath::Clamp(Truck.Ground.MaxSteerDegrees, 0.0, 90.0)));
	if (!TestTrue(TEXT("the largest service vehicle steers on measured axles"),
			Truck.HasAxles() && Lock > KINDA_SMALL_NUMBER))
	{
		return false;
	}

	// Written out rather than calling TightestFollowableRadius, for the reason that function's
	// own header gives: a helper both the production code and its test called could be wrong in
	// one place and agree with itself.
	const double Followable = Truck.Wheelbase() / Lock;

	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
	const FBox2D LaneBounds = LaneBoundsOf(*Stand);

	// TWO GAPS, because the answer depends on the gap and one fixture could pass by luck. 4 m is
	// as close as a player can draw a road to a stand; 54 m is what the rest of the suite uses
	// (FuelServiceTest, RoadAlongsideARowOfStands and StandFuelAnchorJoinsRoad all put their
	// road at y = -6000 with the stand at the origin), and it is past the point where the
	// transition stops being constrained at all - see GuidelineGeom::ShiftDeflectionFor.
	for (const double Gap : { 400.0, 5400.0 })
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		const double RoadY = LaneBounds.Min.Y - Gap;

		// LONG, so the fillet is never clamped by the road running out - that is a different
		// failure with a different fix, and mixing the two would leave neither measured.
		FGuidelineNodeId East;
		Lay(*Net, FVector2D(-60000.0, RoadY), FVector2D(60000.0, RoadY),
			ETraversalClass::GroundVehicle, East);

		const FEntityInstanceId Placed = PlaceStand(*Net, *Stand, FVector2D::ZeroVector, 0.0);
		FAnchorLink::Build(*Net);

		int32 Measured = 0;
		for (const FGuidelineNodeId& Entry : EntriesOf(*Net, Placed))
		{
			const TArray<FGuidelineEdgeId> Link = LinkEdgesAt(*Net, Entry);
			if (Link.Num() < 3)
			{
				continue;
			}
			++Measured;

			const FGuidelineNode* At = Net->GetGuidelineNode(Entry);
			const FString Where = FString::Printf(TEXT("(%.0f,%.0f) at a %.0f uu gap"),
				At != nullptr ? At->Position.X : 0.0, At != nullptr ? At->Position.Y : 0.0, Gap);

			// THE LEAD-IN, which every truck entering this stand drives whichever way it came.
			const double Lead = DeliveredRadius(*Net, Link[0]);
			TestTrue(*FString::Printf(
					TEXT("the lead-in from %s delivers %.0f uu, and the truck's lock wants %.0f"),
					*Where, Lead, Followable),
				Lead >= Followable);

			// AND THE MERGE ONTO THE ROAD, which is the other half of the same transition.
			const double Merge = DeliveredRadius(*Net, Link[1]);
			TestTrue(*FString::Printf(
					TEXT("the merge onto the road at %s delivers %.0f uu, and the truck's lock "
					     "wants %.0f"),
					*Where, Merge, Followable),
				Merge >= Followable);

			// THE TURN-BACK IS THE ACHIEVABLE BOUND, NOT THE IDEAL, and the difference is a fact
			// about the ground rather than about this builder. Merging onto a road 4 m away has
			// to be done at a slant - there is no room for a square turn - and turning the OTHER
			// way out of a slant is a U-turn, which needs about twice the lock of lateral room
			// and has four metres. So it is laid (without it a truck arriving from the far side
			// has no way in at all), the builder WARNS naming the gap, and the router costs it.
			//
			// AT A WIDER GAP THERE IS NOTHING TO EXCUSE: the deflection reaches its right-angle
			// cap, both sweeps become the same shape, and both clear. That is the assertion that
			// would fail if the transition ever stopped being sized from the radius.
			const double TurnBack = DeliveredRadius(*Net, Link[2]);
			AddInfo(FString::Printf(TEXT("LINK %s: lead %.0f, merge %.0f, turn-back %.0f uu"),
				*Where, Lead, Merge, TurnBack));
			if (Gap > Followable * 2.83)
			{
				TestTrue(*FString::Printf(
						TEXT("and past 2.83 times the lock the turn-back clears too - %s "
						     "delivers %.0f against %.0f"),
						*Where, TurnBack, Followable),
					TurnBack >= Followable);
			}
		}

		// NOT VACUOUS. Every loop above runs zero times on a stand that joined nothing, and the
		// test would report success on a builder that had stopped linking altogether.
		TestEqual(*FString::Printf(
				TEXT("a road %.0f uu off the lane is joined at both ends of the near side"), Gap),
			Measured, 2);
	}

	// THE CROSSING BRANCH, WHICH IS A DIFFERENT CONSTRUCTION AND WAS UNMEASURED. A road across
	// the END of a stand meets the lane's own heading rather than lying beside it, so
	// FAnchorLink::Join runs on to where the two meet and rounds the corner - sized by
	// CornerRunFor rather than ShiftDeflectionFor. Until this fixture nothing measured what it
	// delivered, which is the same defect as the one this whole test exists for, one branch over.
	//
	// A STRAIGHT LEAD-IN IS THE BRANCH'S SIGNATURE, and asserting it on BOTH links is what makes
	// this a test of the branch rather than of one link: the lane change always curves its
	// lead-in, so a curve here would mean the S had been taken by mistake. That is exactly how
	// this fixture failed while the crossing branch did not yet exist.
	//
	// BOTH ENTRIES, AND BOTH MERGES, because the nose crossing's two face the road along
	// CONVERGING diagonals and their aims land 75 uu apart on the shipping stand - so the second
	// to be joined meets the road inside the fillet the first already cut and has to work with
	// what is left. Measured here: 769 uu on the first and a merge that barely turns at all on
	// the second, both of which a truck can follow. They cannot be separated by moving the road
	// to measure one alone, either - any segment near enough for one is nearer still to the
	// other, which is what the "nearest entry wins the contact point" rule then acts on - so the
	// pair IS the case, and both are asserted.
	//
	// AND THE TURN-BACK IS ASSERTED HERE, NOT MERELY PRINTED, which is the correction of the
	// final review. It is NOT asserted against the lock, because it does not clear it and no
	// layout makes it: on this branch the deflection is the fixed angle at which the lane's
	// crossing meets the road, so BOTH sweeps are cut back the same Offset and the sharper one
	// gets Offset*sin^2(t/2)/cos(t/2) at the supplement of the angle the merge got. What is
	// asserted instead is that RELATION - the turn-back is the merge's own construction read at
	// the other angle - and that it does not move with the gap, which is the claim the builder's
	// comment now rests on. See there for why nothing warns.
	//
	// TWO GAPS, for exactly that: the lane-change branch above clears at a wide gap and the
	// reader could carry that expectation over. 2000 rather than the 5400 used above because a
	// crossing further off than the link may reach is not a crossing this branch takes at all -
	// the meeting point is 1.414 times the gap on a 45-degree crossing, and past Link.Reach
	// FAnchorLink::Join falls to the lane change - so 5400 would silently measure the other
	// branch twice.
	{
		double LastSharper = 0.0;
		for (const double Gap : { 400.0, 2000.0 })
		{
			URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
			const double RoadX = LaneBounds.Max.X + Gap;

			FGuidelineNodeId North;
			Lay(*Net, FVector2D(RoadX, -30000.0), FVector2D(RoadX, 30000.0),
				ETraversalClass::GroundVehicle, North);

			const FEntityInstanceId Placed = PlaceStand(*Net, *Stand, FVector2D::ZeroVector, 0.0);
			FAnchorLink::Build(*Net);

			int32 Measured = 0;
			FString Merges;
			double Sharpest = TNumericLimits<double>::Max();
			for (const FGuidelineNodeId& Entry : EntriesOf(*Net, Placed))
			{
				const TArray<FGuidelineEdgeId> Link = LinkEdgesAt(*Net, Entry);
				if (Link.Num() < 3)
				{
					continue;
				}
				++Measured;

				const double Lead = DeliveredRadius(*Net, Link[0]);
				const double Merge = DeliveredRadius(*Net, Link[1]);
				const double TurnBack = DeliveredRadius(*Net, Link[2]);
				Merges += Curvature(Merge) + TEXT(" ");
				Sharpest = FMath::Min(Sharpest, FMath::Min(Merge, TurnBack));

				// THE TWO SWEEPS' OWN ANGLES, off the graph rather than off the spec. A
				// quadratic cut back Run on each leg of an interior angle t delivers
				// Run*sin^2(t/2)/cos(t/2), so an interior and a leg length are the whole of
				// what either sweep is - and reading them from the edges as laid is what makes
				// the relation below a measurement of the construction rather than a restatement
				// of it.
				const auto Interior = [Net](FGuidelineEdgeId Id)
				{
					const FGuidelineEdge* Edge = Net->GetGuidelineEdge(Id);
					const FGuidelineNode* A = Net->GetGuidelineNode(Edge->A);
					const FGuidelineNode* B = Net->GetGuidelineNode(Edge->B);
					return FMath::Acos(FMath::Clamp(FVector2D::DotProduct(
						(A->Position - Edge->Control).GetSafeNormal(),
						(B->Position - Edge->Control).GetSafeNormal()), -1.0, 1.0));
				};
				const auto Leg = [Net](FGuidelineEdgeId Id)
				{
					const FGuidelineEdge* Edge = Net->GetGuidelineEdge(Id);
					return FVector2D::Distance(
						Net->GetGuidelineNode(Edge->A)->Position, Edge->Control);
				};

				AddInfo(FString::Printf(
					TEXT("CROSSING at a %.0f uu gap: lead %s, merge %s (%.1f deg, leg %.0f), "
					     "turn-back %s uu (%.1f deg, leg %.0f)"),
					Gap, *Curvature(Lead),
					*Curvature(Merge), FMath::RadiansToDegrees(Interior(Link[1])), Leg(Link[1]),
					*Curvature(TurnBack), FMath::RadiansToDegrees(Interior(Link[2])), Leg(Link[2])));

				TestTrue(*FString::Printf(
						TEXT("a link onto a crossing road leaves straight - %s uu"), *Curvature(Lead)),
					Lead > Followable);
				TestTrue(*FString::Printf(
						TEXT("and rounds onto it at %s uu, against a lock of %.0f"),
						*Curvature(Merge), Followable),
					Merge >= Followable);

				// THE TURN-BACK, ASSERTED AND NOT ONLY PRINTED, and asserted as the CONSTRUCTION
				// rather than against the lock - which it does not clear and, on this branch,
				// cannot be made to. Both sweeps are cut back the same leg from the same corner,
				// so each delivers leg*sin^2(t/2)/cos(t/2) at its own interior angle t: the
				// merge gets the gentle one, the turn-back its supplement. Holding BOTH to that
				// identity says the sharp sweep is the same fillet read at the other angle,
				// which is the fact the builder's comment now rests on - and it fails if either
				// sweep ever stops being sized from the corner it shares.
				for (int32 Which : { 1, 2 })
				{
					const double Angle = Interior(Link[Which]);

					// A SWEEP THAT DOES NOT TURN HAS NO RADIUS TO PREDICT, and that is a real
					// case rather than a guard against arithmetic: at a 4 m gap the second entry
					// to be joined meets the road INSIDE the fillet the first already cut, so
					// its merge comes out dead straight (180.0 deg, measured) - and both
					// sin^2(t/2)/cos(t/2) and TightestRadius run away there.
					if (Angle > UE_DOUBLE_PI - 0.05)
					{
						continue;
					}

					const double Predicted = Leg(Link[Which])
						* FMath::Square(FMath::Sin(Angle * 0.5)) / FMath::Cos(Angle * 0.5);
					TestEqual(
						*FString::Printf(
							TEXT("sweep %d delivers what a %.1f deg corner cut back %.0f uu can "
							     "- %s uu"),
							Which, FMath::RadiansToDegrees(Angle), Leg(Link[Which]),
							*Curvature(DeliveredRadius(*Net, Link[Which]))),
						DeliveredRadius(*Net, Link[Which]), Predicted,
						FMath::Max(1.0, Predicted * 0.01));
				}
			}

			TestEqual(*FString::Printf(
					TEXT("a road %.0f uu across the nose is joined at both of its entries"), Gap),
				Measured, 2);
			AddInfo(FString::Printf(TEXT("CROSSING merges at a %.0f uu gap: %s"), Gap, *Merges));

			// THE GAP IS NOT THE LEVER ON THIS BRANCH, and that is the whole of why the builder
			// does not warn here - a warning naming the gap would be false advice. Asserted as
			// an equality across the two gaps rather than as a bound, because a bound would pass
			// on a figure that had merely got worse.
			if (LastSharper > 0.0)
			{
				TestEqual(
					*FString::Printf(
						TEXT("the sharpest sweep on a crossing is the same %.0f uu at every gap"),
						Sharpest),
					Sharpest, LastSharper, 1.0);
			}
			LastSharper = Sharpest;
		}
	}

	// AND WHERE THE GROUND CANNOT GIVE IT, THE BUILDER SAYS SO. A road drawn as a short stub
	// has no room either side of the join for the fillet, so the clamp in FAnchorLink::Join
	// takes what is left and the delivered radius falls under the lock however it was sized.
	// That is a fact about the layout rather than about the construction - the player can move
	// or lengthen the road, and nothing else will fix it - so the builder warns naming the gap
	// and this exercises the path rather than leaving it to a reader.
	//
	// NO ASSERTION ON THE RADIUS HERE, deliberately: the figure is whatever the stub allows, and
	// pinning it would be pinning the clamp rather than the rule. What is asserted is that the
	// stand is still JOINED - a cramped road is worse service, not no service.
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		const double RoadY = LaneBounds.Min.Y - 400.0;

		// 900 uu of road, which is shorter than the fillet a 4 m gap asks for at either end.
		FGuidelineNodeId East;
		Lay(*Net, FVector2D(1000.0, RoadY), FVector2D(1900.0, RoadY),
			ETraversalClass::GroundVehicle, East);

		const FEntityInstanceId Placed = PlaceStand(*Net, *Stand, FVector2D::ZeroVector, 0.0);
		FAnchorLink::Build(*Net);

		// ONE EDGE IS ENOUGH TO COUNT HERE, and that is the shape of the answer rather than a
		// looser test: with no room for a fillet FAnchorLink::Join falls back to the hard join -
		// the lead-in runs to a node cut straight into the road and there are no sweeps at all -
		// which is deliberate (see the comment at its Offset test: an ugly corner is
		// recoverable, an inverted arc is not). So the stub gets a one-edge link, the builder
		// warns, and the stand is still served.
		int32 Joined = 0;
		for (const FGuidelineNodeId& Entry : EntriesOf(*Net, Placed))
		{
			const TArray<FGuidelineEdgeId> Link = LinkEdgesAt(*Net, Entry);
			if (Link.Num() == 0)
			{
				continue;
			}
			++Joined;

			FString Radii;
			for (const FGuidelineEdgeId& Id : Link)
			{
				Radii += Curvature(DeliveredRadius(*Net, Id)) + TEXT(" ");
			}
			AddInfo(FString::Printf(TEXT("STUB: %d edge(s), radii %s uu"), Link.Num(), *Radii));
		}
		TestTrue(TEXT("a stand beside a short stub of road is still joined to it"), Joined >= 1);
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
	// EVERY EDGE *Lane HOLDS IS THE CYCLE, and that needs no filter of its own: FStandLaneBuild
	// puts its owner on the lane and on nothing else, and the road link a declared entry casts is
	// deliberately unowned - which is what lets IsServiceNodeConnected tell a lane that reaches a
	// road from one that only reaches itself. A bStandApproach flag stood here until 2026-09-16
	// to tell an owned APPROACH from an owned cycle edge; there is no owned approach any more,
	// and the flag is deleted rather than left as a mark nothing writes.
	TArray<FGuidelineEdgeId> Cycle;
	for (const FGuidelineEdgeId& Id : *Lane)
	{
		const FGuidelineEdge* Edge = Net->GetGuidelineEdge(Id);
		if (Edge != nullptr && Edge->bAlive)
		{
			Cycle.Add(Id);
		}
	}
	if (!TestTrue(TEXT("the lane has a cycle to drive round"), Cycle.Num() >= 4))
	{
		return false;
	}

	// THE LARGEST VEHICLE ADMITTED, and not the van this measured until the final review of
	// 2026-09-16. FStandLaneBuild rounds these very corners to
	// ResolveLargestServiceVehicle().TightestFollowableRadius(), so measuring them against
	// ResolveDefaultVehicle() (471 uu, against 699.4) left a 228 uu band in which the lane could
	// shrink with this test still green. Airside.Build.PlacedStandLaneIsOneDrivableCycle already
	// held the same edges to the right bar, so two tests were using two vehicles as the bar for
	// one property - a pair that drifts, and the rule is that ground geometry is sized for the
	// largest vehicle ADMITTED, never the one driving now.
	const FAirframe Truck = UAirsideSettings::ResolveLargestServiceVehicle();
	const double Lock = FMath::Sin(FMath::DegreesToRadians(
		FMath::Clamp(Truck.Ground.MaxSteerDegrees, 0.0, 90.0)));
	if (!TestTrue(TEXT("the largest service vehicle steers on measured axles"),
			Truck.HasAxles() && Lock > KINDA_SMALL_NUMBER))
	{
		return false;
	}

	// The same expression FSpeedProfile::Build uses, written out rather than shared - for the
	// reason Airside.Model.ServiceRoadFilletClearsTheTruckLock gives at its own copy: a helper
	// both the production code and its test called could be wrong in one place and agree with
	// itself.
	const double TightestFollowable = Truck.Wheelbase() / Lock;

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
				if (OtherId == Id || Other == nullptr
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
