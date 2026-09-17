#include "CoreMinimal.h"
#include "Build/AnchorLink.h"
#include "Build/StandLayoutBuild.h"
#include "Solve/IcaoCode.h"
#include "Content/AirsideSettings.h"
#include "Entities/AircraftType.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadEntity.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadTraffic.h"
#include "Model/RouteSearch.h"
#include "Model/SpeedProfile.h"
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
	 * The axis-aligned box the definition's SERVICE LAYOUT occupies, in its own local space.
	 *
	 * HERE RATHER THAN ON UEntityDefinition, where it was UEntityDefinition::ServiceLaneBounds()
	 * until 2026-09-16. That accessor existed so a test could ask "where is the layout" without
	 * typing its corners a second time (#104), and the reason it earned its place on the asset -
	 * the lane was a rectangle by construction, so its bounds WERE its shape - stopped being
	 * true the moment it gained a dip, and is no truer of four legs per bay. A bounding box of
	 * that is a TEST'S convenience and nothing else, so it lives with the tests.
	 *
	 * OFF THE SAMPLED LEGS, not off the poses. A bay's poses sit well inside the curve that
	 * reaches them, so a box drawn round the poses alone would report a layout smaller than the
	 * ground it actually uses - which is the figure every caller here wants.
	 */
	FBox2D LaneBoundsOf(const UEntityDefinition& Definition)
	{
		FBox2D Bounds(ForceInit);
		for (const FServiceBay& Bay : Definition.ServiceBays)
		{
			for (const FStandLeg* Leg :
				{ &Bay.ArriveLeg, &Bay.ServeLeg, &Bay.ReverseLeg, &Bay.DepartLeg })
			{
				TArray<FVector2D> Sampled;
				Leg->Sample(Sampled);
				for (const FVector2D& At : Sampled)
				{
					Bounds += At;
				}
			}
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

	const FStandLayoutBuild::FResult First = FStandLayoutBuild::Build(*Net);
	TestEqual(TEXT("one stand, one lane"), First.LayoutsBuilt, 1);

	// EVERY SERVICE POINT HAS A WAY IN AND A WAY OUT, and the AIRCRAFT stop mark still has
	// neither - the layout is for vehicles, and a painted lead-in is not this builder's
	// business.
	//
	// TWO EDGES, AND WHICH TWO IS THE POINT: the SERVE leg arrives and the REVERSE leg leaves.
	// One edge would be a dead end, and a service point a vehicle cannot leave is the pile-up
	// this whole design exists to prevent. The two are not the same curve - the vehicle does
	// not retrace what it drove in on, which is what lets the reverse be judged by the reverse
	// limit alone.
	//
	// TWO ANCHORS HAVE NO BAY AND BOTH ARE ASSERTED AS SUCH, rather than skipped - a skip is
	// indistinguishable from a bay this builder silently failed to lay. The TUG has none
	// because pushback couples at the nose gear and is FPushbackRun's manoeuvre; the PASSENGER
	// DOOR has none because an air bridge is a structure and drives nowhere.
	for (const FResolvedAnchor& Anchor : Net->GetEntity(Placed)->ResolvedAnchors)
	{
		if (TraversalForRole(Anchor.Role) == ETraversalClass::Aircraft)
		{
			continue;
		}

		const bool bDrives = TraversalForRole(Anchor.Role) == ETraversalClass::GroundVehicle
			&& Anchor.Role != EServiceRole::Tug;

		const FGuidelineNode* Node = Net->GetGuidelineNode(Anchor.Node);
		if (TestNotNull(TEXT("the anchor resolves"), Node))
		{
			TestEqual(
				*FString::Printf(TEXT("'%s' is %s"), *Anchor.Id.ToString(),
					bDrives ? TEXT("served by a leg in and a leg out")
					        : TEXT("a position no vehicle parks at")),
				Node->Incident.Num(), bDrives ? 2 : 0);
		}
	}
	TestEqual(TEXT("the aircraft stop mark is untouched"),
		Net->GetGuidelineNode(Net->GetEntity(Placed)->PoseNode)->Incident.Num(), 0);

	// BUT IT IS NOT ON A ROAD. The lane is itself a vehicle guideline, so a check that only
	// counted edges would call this stand connected and no truck would ever route to it.
	TestFalse(TEXT("a lane with no road near it is not a connection"),
		Net->IsServiceNodeConnected(AnchorNode(*Net, Placed, TEXT("HydrantPit"))));

	// AND IT IS CONNECTED, which is a weaker claim than the cycle this replaces and a truer
	// one. A layout is a TREE per side, not a ring: nothing may pass under a wing, so the two
	// sides never meet, and each bay is a dead end whose turn-round is the reverse leg. What
	// must hold is that no laid node is ISOLATED - a node with nothing on it is a leg the
	// builder made and then failed to connect, which would read as a working layout in the
	// overlay and route nothing.
	{
		int32 Isolated = 0;
		for (const FGuidelineNodeId& Node : First.Nodes)
		{
			const FGuidelineNode* Found = Net->GetGuidelineNode(Node);
			Isolated += (Found == nullptr || Found->Incident.Num() == 0) ? 1 : 0;
		}

		// THE TUG'S NODE IS THE ONE ALLOWED ISOLATE, and it is not in First.Nodes at all - the
		// builder never touches it - so the figure here is zero rather than one.
		TestEqual(TEXT("no laid node is left with nothing on it"), Isolated, 0);
	}

	// A SECOND PASS ADDS NOTHING. The graph is rebuilt on every road edit and this runs each
	// time; a builder that could not see its own previous output would stack a lane per pass.
	const FStandLayoutBuild::FResult Second = FStandLayoutBuild::Build(*Net);
	TestEqual(TEXT("a second pass builds no second lane"), Second.LayoutsBuilt, 0);
	TestEqual(TEXT("but it still reports the lane that is there"), Second.Layouts.Num(), 1);

	// AND IT REPORTS THE SAME ENTRIES, NODE FOR NODE.
	//
	// THE WHOLE RESULT OR NONE OF IT. Lanes and Nodes are re-gathered from the graph on the
	// skip path and Entries is RECOVERED there - by FStandLayoutBuild's own RecoverEntries,
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
		TestEqual(TEXT("a depot gets no lane"), FStandLayoutBuild::Build(*Bare).LayoutsBuilt, 0);
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
	FStandIsEnteredWhereItDeclaresTest,
	"Airside.Build.StandIsEnteredWhereItDeclares",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandIsEnteredWhereItDeclaresTest::RunTest(const FString& Parameters)
{
	using namespace ServiceLinkFixture;

	// THIS REPLACES ServiceLinkJoinsFromAnyDirection, AND REVERSES ITS CLAIM. That test said
	// "a service link measures distance, not direction", which was the right rule for a lane
	// that was a CYCLE: a truck could join it anywhere and drive round to any anchor, so
	// refusing a road for being on the wrong side refused a stand a player had every reason to
	// expect would work.
	//
	// THE LAYOUT IS NOT A CYCLE. Nothing may pass under a wing, so each side of a stand is its
	// own dead end, and every way in is DECLARED - on the aft edge, where a GSE road runs
	// behind a row of stands. Direction is now the whole of the question, and the old claim
	// would pass only by finding a connection the design says should not exist.
	//
	// SO WHAT IS MEASURED IS THE RULE THE PLAYER MEETS: put a road where the stand says it may
	// be entered and every service is reachable; put one anywhere else and the stand is
	// refused, which is a refusal they can act on.
	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();

	// 4500 rather than exactly the reach: a boundary case measures the comparison operator
	// rather than the rule, and would flip on a rounding error.
	constexpr double GapNear = 4500.0;
	constexpr double GapFar = 20000.0;

	// THE AFT EDGE IS READ OFF THE TEMPLATE, never typed. Every entry is authored there, so
	// asking the bays where they are is the same question placement asks - and moving the
	// entries moves this fixture with them.
	double AftX = TNumericLimits<double>::Max();
	for (const FServiceBay& Bay : Stand->ServiceBays)
	{
		AftX = FMath::Min(AftX, Bay.EntryLocal.X);
	}

	// BEHIND THE STAND, which is where a road belongs.
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		FGuidelineNodeId Far;
		const FGuidelineNodeId Near =
			Lay(*Net, FVector2D(AftX - GapNear, -20000.0), FVector2D(AftX - GapNear, 20000.0),
				ETraversalClass::GroundVehicle, Far);

		const FEntityInstanceId Placed = PlaceStand(*Net, *Stand, FVector2D::ZeroVector, 0.0);
		FAnchorLink::Build(*Net);

		// EVERY SERVICE, not just the fuel one. The two sides never meet, so a test that asked
		// only about the hydrant would pass with the whole port side orphaned - which is
		// exactly the state this suite sat in before the road moved behind the stand.
		for (const FServiceBay& Bay : Stand->ServiceBays)
		{
			const FGuidelineNodeId Node = AnchorNode(*Net, Placed, *Bay.AnchorId.ToString());
			if (!TestTrue(*FString::Printf(TEXT("'%s' is connected"), *Bay.AnchorId.ToString()),
				Net->IsServiceNodeConnected(Node)))
			{
				continue;
			}

			// AND A TRUCK CAN ACTUALLY GET THERE. Connectivity is the claim; a route is the
			// proof, and "the search found nothing" would otherwise read like a broken search.
			FRouteQuery Query;
			Query.Start = Near;
			Query.Goal = Node;
			Query.Class = ETraversalClass::GroundVehicle;
			TestTrue(
				*FString::Printf(TEXT("and a truck routes to '%s'"), *Bay.AnchorId.ToString()),
				RouteSearch::Find(*Net, Query).IsValid());
		}
	}

	// IN FRONT OF THE NOSE, at the same gap, WHERE A ROAD CANNOT SERVE IT. This is the case
	// the old test would have called a pass: the road is near the stand, just not near
	// anything the stand declared. A player who draws one there gets a refusal naming the
	// entry rather than a stand that half works.
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		FGuidelineNodeId Far;
		const double NoseX = IcaoCode::MaxNoseFwdForLetter(TEXT("C"));
		Lay(*Net, FVector2D(NoseX + GapNear, -20000.0), FVector2D(NoseX + GapNear, 20000.0),
			ETraversalClass::GroundVehicle, Far);

		const FEntityInstanceId Placed = PlaceStand(*Net, *Stand, FVector2D::ZeroVector, 0.0);
		FAnchorLink::Build(*Net);

		TestFalse(TEXT("a road across the nose enters nothing - the entries are all aft"),
			Net->IsServiceNodeConnected(AnchorNode(*Net, Placed, TEXT("HydrantPit"))));
	}

	// AND BEHIND, BUT TOO FAR. The short radius is what keeps the rejected case rejected: at
	// 200 m the nearest vehicle line is regularly the service road on the far side of a
	// terminal, and the link would run straight through the building with nothing to report it.
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		FGuidelineNodeId Far;
		Lay(*Net, FVector2D(AftX - GapFar, -20000.0), FVector2D(AftX - GapFar, 20000.0),
			ETraversalClass::GroundVehicle, Far);

		const FEntityInstanceId Placed = PlaceStand(*Net, *Stand, FVector2D::ZeroVector, 0.0);
		FAnchorLink::Build(*Net);

		TestFalse(TEXT("a road 200 m behind the stand does not reach it"),
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
	// one service road serving a whole row of stands, which is how a player builds one.
	// Measured then: 9 of 25 lead-ins joined, and the only service anchor that joined at any
	// stand was TugStand - the one anchor that cast ACROSS the road instead of along it.
	//
	// THE ROW IS SIDE BY SIDE AND THE ROAD RUNS BEHIND IT, since 2026-09-17. It used to be
	// four stands nose-to-tail along X with the road ALONGSIDE at y = -6000, which is not a row
	// of stands at all - it is four stands parked one behind another - and it only ever worked
	// because a lane was a cycle a road could join from any side. Stands sit shoulder to
	// shoulder facing the terminal, and the GSE road runs along the back of the row past every
	// one of their aft edges. That is also the only arrangement the layout admits: nothing may
	// pass under a wing, so a stand is entered from behind or not at all.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());

	// 420 uu behind the aft edge, which every stand in the row shares because they all face
	// the same way.
	constexpr double RoadX = -5400.0;
	FGuidelineNodeId RoadNorth;
	const FGuidelineNodeId RoadSouth =
		Lay(*Net, FVector2D(RoadX, -40000.0), FVector2D(RoadX, 40000.0),
			ETraversalClass::GroundVehicle, RoadNorth);

	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();

	TArray<FEntityInstanceId> Row;
	for (int32 At = 0; At < 4; ++At)
	{
		// 8000 uu apart: a Code C stand is 5300 wide, so that leaves 27 m of clear ground
		// between neighbours and no layout is nearer to another layout than it is to the road.
		Row.Add(PlaceStand(*Net, *Stand, FVector2D(0.0, -12000.0 + At * 8000.0), 0.0));
	}

	FAnchorLink::Build(*Net);

	for (int32 At = 0; At < Row.Num(); ++At)
	{
		const FGuidelineNodeId Hydrant = AnchorNode(*Net, Row[At], TEXT("HydrantPit"));
		TestTrue(*FString::Printf(TEXT("stand %d's hydrant is on the road"), At),
			Net->IsServiceNodeConnected(Hydrant));

		FRouteQuery Query;
		Query.Start = RoadSouth;
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
	 * entry waypoint became this" once it is laid - see FStandLayoutBuild::FResult::Entries.
	 *
	 * SAFE AFTER FAnchorLink::Build, and that is the point of calling it that way round here.
	 * The lane is already in the graph by then, so this takes the builder's idempotent skip
	 * path and RECOVERS the entries rather than laying a second lane - which is the same path
	 * the linking pass itself took, so a recovery that disagreed with the laying would show up
	 * as a test that cannot find the node the road is plainly joined to.
	 */
	TArray<FGuidelineNodeId> EntriesOf(URoadNetwork& Net, FEntityInstanceId Entity)
	{
		const FStandLayoutBuild::FResult Built = FStandLayoutBuild::Build(Net);
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

// ServiceLaneEntersOnEverySideWithinReach IS DELETED, 2026-09-17, and what it covered is
// named here rather than left to be rediscovered.
//
// EVERY PROPERTY IT ASSERTED WAS THE CYCLE'S. "Four declared entries, each a PAIR of nodes
// round its rounded corner" - a layout entry is authored on a straight and is one node.
// "A road alongside is joined at both ends of the side it passes" - there are no sides to
// pass, and a road alongside joins nothing at all now. "Turns in at the nearest entry rather
// than touring the lane" - the nearest-entry rule is deleted, because the user's ruling of
// 2026-09-17 is that every bay gets its own way in.
//
// ITS TWO CLAIMS THAT OUTLIVED THE LANE HAVE HOMES: that a stand is entered only where it
// declares is Airside.Build.StandIsEnteredWhereItDeclares, and that the transition onto the
// road clears the truck's lock is the test below.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandLinkClearsTheTruckLockTest,
	"Airside.Build.StandLinkClearsTheTruckLock",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandLinkClearsTheTruckLockTest::RunTest(const FString& Parameters)
{
	using namespace ServiceLinkFixture;

	// THE ONE CURVE NOBODY MEASURED. Every corner inside the layout is held against the truck's
	// steering lock - Airside.Entities.EveryTemplateLegIsDrivableByEveryVehicle asks the oracle
	// itself - and the LINK from the road onto it had no such test, and was delivering 40 uu
	// where 699 is needed: a curve tighter than the lock is untakeable at any speed, not merely
	// slow, so a truck could not drive onto the stand at all.
	//
	// THE RADIUS AS LAID, NEVER THE RUN REQUESTED. Commit 8be494c cost this project three
	// sessions because a test checked the figure a builder asked for while the follower drove
	// the figure it got. So this measures GuidelineGeom::TightestRadius on the edge that is in
	// the graph.
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
	// as close as a player can draw a road to a stand; 54 m is a comfortable one.
	//
	// BEHIND THE AFT EDGE, which is the only place a road can serve a stand: every entry and
	// every exit is authored there, and nothing may pass under a wing to reach the far side.
	for (const double Gap : { 400.0, 5400.0 })
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		const double RoadX = LaneBounds.Min.X - Gap;

		// LONG, so the fillet is never clamped by the road running out - that is a different
		// failure with a different fix, and mixing the two would leave neither measured.
		FGuidelineNodeId North;
		Lay(*Net, FVector2D(RoadX, -60000.0), FVector2D(RoadX, 60000.0),
			ETraversalClass::GroundVehicle, North);

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
			// This one has no excuse at any gap: it leaves the entry along the layout's own
			// heading, so it is a straight or very nearly one.
			const double Lead = DeliveredRadius(*Net, Link[0]);
			TestTrue(*FString::Printf(
					TEXT("the lead-in from %s delivers %.0f uu, and the truck's lock wants %.0f"),
					*Where, Lead, Followable),
				Lead >= Followable);

			// AND THE BETTER OF THE TWO SWEEPS ONTO THE ROAD, which is the one a truck coming
			// from the favourable direction takes.
			//
			// THE BETTER, NOT BOTH, AND THAT IS A RULING RATHER THAN A CONCESSION. An entry is
			// authored at 45 degrees so that the turn off the road costs CornerRunFor(R, 135)
			// = 345 uu instead of the 1088 a square one would - which is what lets a road sit
			// 4 m behind a stand at all. A vehicle arriving from the OTHER direction meets the
			// mirror of that corner, about 135 degrees, and no radius fixes it: the user's
			// ruling of 2026-09-17 is that such a vehicle MANOEUVRES in - a shunt, forward
			// then back then forward - and never crabs. Measured here at 89 and 101 uu, which
			// is a U-turn wearing a fillet, exactly as expected.
			//
			// WHICH OF THE PAIR IS WHICH IS NOT FIXED, so taking the better of the two is
			// direction-agnostic; asking for Link[1] by index would pass or fail on the order
			// a node happens to list its edges in.
			const double SweepA = DeliveredRadius(*Net, Link[1]);
			const double SweepB = DeliveredRadius(*Net, Link[2]);
			AddInfo(FString::Printf(TEXT("LINK %s: lead %.0f, sweeps %.0f and %.0f uu"),
				*Where, Lead, SweepA, SweepB));

			TestTrue(*FString::Printf(
					TEXT("one way onto the road at %s clears the lock - sweeps %.0f and %.0f "
					     "against %.0f"),
					*Where, SweepA, SweepB, Followable),
				FMath::Max(SweepA, SweepB) >= Followable);
		}

		// NOT VACUOUS. Every loop above runs zero times on a stand that joined nothing, and the
		// whole test would then be green on an airport where no truck can move.
		TestTrue(*FString::Printf(TEXT("links were measured at a %.0f uu gap - %d found"),
				Gap, Measured),
			Measured > 0);
	}

	// A KNOWN GAP, RECORDED HERE BECAUSE THIS IS WHERE IT WOULD BITE. Nothing stops the SEARCH
	// choosing the unfavourable sweep: RouteSearch::EdgeCost is sampled length plus congestion,
	// with no curvature and no heading term - see AnchorLink.cpp, which says the same thing at
	// the sweep it lays. That is the identical root cause as the 175 degree on-the-spot
	// reversal piece A found, and it is not fixed by the layout: the layout made the wrong
	// ENTRY unrepresentable, not the wrong APPROACH to a right one. What will catch it is
	// FSpeedProfile at the moment a route is driven, which is where the acceptance test looks.
	AddInfo(TEXT("EdgeCost has no curvature term, so a route may still choose the sweep that "
	             "needs a shunt - see Airside.Model.Traffic.TruckDrivesTheWholeRouteToTheHydrant"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTruckDrivesTheWholeRouteToTheHydrantTest,
	"Airside.Model.Traffic.TruckDrivesTheWholeRouteToTheHydrant",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTruckDrivesTheWholeRouteToTheHydrantTest::RunTest(const FString& Parameters)
{
	using namespace ServiceLinkFixture;

	// THE TEST THIS SUITE NEVER HAD, and the reason four attempts at the stand's routing
	// shipped green and produced a truck that crabbed.
	//
	// FSpeedProfile::Build is the ONLY authority on whether a vehicle can drive a line. It
	// measures Length/Turn across every span of the WHOLE ROUTE - a concatenation of edges,
	// including the spans that straddle the join where two of them meet. Every other test in
	// this file measures ONE EDGE, analytically, with the rule hand-copied into it; grep for
	// "the same expression FSpeedProfile::Build uses, written out rather than shared" and
	// every hit is a site that should have called it instead.
	//
	// A ROUTE OF INDIVIDUALLY-LEGAL EDGES CAN STILL BE ILLEGAL WHERE TWO MEET, and that is
	// precisely what no per-edge test can see. Reported from PIE 2026-09-16 as a truck that
	// crabbed twice, with four warnings on one journey - R=97, R=290, and three at 653-670
	// against the 699 the lock allows, the last three 87 uu apart, which is the signature of
	// a join and not of a corner anyone sized.
	//
	// So this asks the authority, about the journey the player actually watches.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());

	// THE ROAD RUNS BEHIND THE STAND, along its aft edge, and that moved on 2026-09-17 with
	// the layout it serves. It used to run ALONGSIDE at y = -6000, which worked while the lane
	// was a cycle reachable from any side. It is not one any more: nothing may pass under a
	// wing, so each side of the stand is its own dead end reached only from the aft edge, and
	// a road alongside leaves the far side orphaned. MEASURED rather than assumed - with the
	// road at y = -6000 the starboard entries sit 6650 to 8450 uu away against a
	// DefaultServiceLinkRadius of 6500, so not one of them linked and the hydrant, which is a
	// starboard service, had no route at all.
	//
	// THE ASSERTIONS BELOW ARE UNCHANGED. This is the fixture put where the design says a road
	// must be, not the question made easier.
	constexpr double RoadX = -5400.0;
	FGuidelineNodeId RoadNorth;
	const FGuidelineNodeId RoadSouth =
		Lay(*Net, FVector2D(RoadX, -6000.0), FVector2D(RoadX, 6000.0),
			ETraversalClass::GroundVehicle, RoadNorth);

	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
	const FEntityInstanceId Placed = PlaceStand(*Net, *Stand, FVector2D::ZeroVector, 0.0);
	FAnchorLink::Build(*Net);

	FRouteQuery Query;
	Query.Start = RoadSouth;
	Query.Goal = AnchorNode(*Net, Placed, TEXT("HydrantPit"));
	Query.Class = ETraversalClass::GroundVehicle;

	const FRoutePlan Plan = RouteSearch::Find(*Net, Query);
	if (!TestTrue(TEXT("a truck routes from the road to the hydrant"), Plan.IsValid())
		|| Plan.Polyline.Num() < 2)
	{
		return false;
	}

	// THE LARGEST VEHICLE ADMITTED, not the one that happens to be driving - the same rule
	// the ground geometry is sized by. A road a big dispenser cannot take is a defect whether
	// or not a small van could have managed it.
	const FAirframe Truck = UAirsideSettings::ResolveLargestServiceVehicle();

	FSpeedProfile Profile;
	Profile.Build(Plan.Polyline, Truck);

	AddInfo(FString::Printf(
		TEXT("route %.0f uu, %d point(s); tightest R=%.0f uu at %.0f against the %.0f the lock ")
		TEXT("allows; %d sharp vertex/vertices, sharpest %.0f deg at %.0f"),
		GuidelineGeom::PolylineLength(Plan.Polyline), Plan.Polyline.Num(),
		Profile.GetTightestRadius(), Profile.GetTightestAt(), Truck.TightestFollowableRadius(),
		Profile.GetSharpVertexCount(), Profile.GetSharpestDegrees(), Profile.GetSharpestAt()));

	// BOTH RULES, because asking only the first is how this test passed while its route
	// contained a 175 degree instantaneous reversal. A sharp vertex is a corner with no curve
	// in it at all - the radius rule cannot see it, since a zero-length turn has no Length to
	// divide by - and no vehicle takes one at any speed.
	TestFalse(
		*FString::Printf(
			TEXT("no vertex of the route turns instantly (%d found, sharpest %.0f deg at %.0f)"),
			Profile.GetSharpVertexCount(), Profile.GetSharpestDegrees(),
			Profile.GetSharpestAt()),
		Profile.HasSharpVertex());

	// NOT a re-derivation of the rule. This is the profile's OWN verdict, which is what the
	// follower acts on and what the warning in the log reports. A copy of the rule can agree
	// with itself while disagreeing with the original, which is how this went wrong before.
	TestFalse(
		*FString::Printf(
			TEXT("no span of the route asks for a radius the steering lock cannot hold ")
			TEXT("(tightest R=%.0f uu at %.0f, lock allows R>=%.0f)"),
			Profile.GetTightestRadius(), Profile.GetTightestAt(),
			Truck.TightestFollowableRadius()),
		Profile.WasTighterThanLock());

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
	// The road is BEHIND the stand and the hydrant is on the far side of the aeroplane from
	// where the route enters, which is the arrangement that makes a straight spur cross 37 m
	// of fuselage. That is exactly why the layout exists, and why joining each anchor directly
	// to the road was rejected.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());

	// A SHORT road, so the journey's length is about the STAND rather than about how far
	// down the road the start node happens to sit.
	//
	// ALONG THE AFT EDGE since 2026-09-17, for the reason
	// Airside.Model.Traffic.TruckDrivesTheWholeRouteToTheHydrant gives at its own fixture: with
	// no way under a wing, a road alongside the stand can only reach the side it is on.
	constexpr double RoadX = -5400.0;
	FGuidelineNodeId RoadNorth;
	const FGuidelineNodeId RoadWest =
		Lay(*Net, FVector2D(RoadX, -6000.0), FVector2D(RoadX, 6000.0),
			ETraversalClass::GroundVehicle, RoadNorth);

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

	const FStandLayoutBuild::FResult Built = FStandLayoutBuild::Build(*Net);
	const TArray<FGuidelineEdgeId>* Lane = Built.Layouts.Find(Placed);
	if (!TestNotNull(TEXT("the stand got a lane"), Lane))
	{
		return false;
	}

	// THE WHOLE CYCLE: every lane edge, straight runs and the bends between them alike, which
	// together are what a truck drives round.
	//
	// EVERY EDGE *Lane HOLDS IS THE CYCLE, and that needs no filter of its own: FStandLayoutBuild
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
	// 2026-09-16. FStandLayoutBuild rounds these very corners to
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
	// way FSpeedProfile measures it, over each laid edge's own samples.
	//
	// FORWARD EDGES ONLY, and skipping the rest is the whole reason FGuidelineEdge::bReverseLeg
	// exists. A reverse leg is LEGITIMATELY tighter than the forward limit - a reversing
	// vehicle pivots about its FIXED axle and holds L/tan(lock) where forward driving needs
	// L/sin(lock), 494.5 against 699.3 - so sweeping every laid edge past the forward rule
	// refuses the one manoeuvre the layout is designed around. Measured: it reported 547 uu,
	// which is a reverse leg comfortably inside its own limit and 152 uu outside a limit that
	// does not apply to it.
	//
	// THE REVERSE LEGS ARE NOT UNJUDGED. FReverseRun::Start arms every one of them at
	// template-build time and refuses a curve it cannot hold -
	// Airside.Entities.EveryTemplateLegIsDrivableByEveryVehicle is where that is measured, and
	// re-judging them here by the wrong rule would be a second evaluator disagreeing with it.
	double Tightest = TNumericLimits<double>::Max();
	int32 Bends = 0;
	for (const FGuidelineEdgeId& Id : Cycle)
	{
		const FGuidelineEdge* Edge = Net->GetGuidelineEdge(Id);
		if (Edge != nullptr && Edge->bReverseLeg)
		{
			continue;
		}

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
