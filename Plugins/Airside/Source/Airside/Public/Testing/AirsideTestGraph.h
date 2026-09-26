#pragma once

// TEST-ONLY, and guarded because of it: WITH_DEV_AUTOMATION_TESTS is 0 in a shipping build,
// so a fixture header with no consumer left in that config must not even try to compile -
// see AirsideTestWorld.h's own top comment, which this mirrors.
//
// ISSUE #311 (regression of #101, itself following #189's move of FAirsideTestWorld here):
// TestGraph, TestProfiles and FTestAirport lived in AirsideTests/Private/AirsideTestFixtures.h,
// Private to that module - so AirportOpsTests (OfferGeneratorTest's FieldWith) and the game
// module's RigTestCourseTest could not reach them and re-drew the same runway/taxiway/guideline
// setup from AddGuidelineNode by hand instead (#310's own "Related" note). Public here for the
// same reason #189 moved FAirsideTestWorld: AirportMgr, AirportOpsTests and AirsideEditor all
// already depend on Airside (never the reverse), and a Public header behind
// WITH_DEV_AUTOMATION_TESTS costs nothing in a shipping build. AirsideTestFixtures.h forwards
// to this header rather than declaring its own copy, so every existing AirsideTests includer
// keeps compiling unchanged.
#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Build/RoadNetworkSolver.h"
#include "Model/RoadEntity.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RouteSearch.h"

class URoadProfile;
struct FAirframe;
struct FVehicle;

/** Runway and taxiway profiles authored by hand, MakeTransient so no asset is touched. */
namespace TestProfiles
{
	/** 4500 wide / 1500 / 450, continuous through junctions - the runway every fixture in
	 *  the module lands a Piper on. */
	AIRSIDE_API URoadProfile* Runway();

	/** 1800 wide / 1500 / 180, continuous through junctions - the width TrafficHeadOnReplan
	 *  and HoldingPositionFullWidth need to own, where a wider strip's own geometry would
	 *  hide the thing under test. */
	AIRSIDE_API URoadProfile* NarrowRunway();

	/** 2300 wide / 1500 / 230 - the taxiway every fixture in the module uses. NOT continuous
	 *  through junctions: a taxiway is not a runway chain. */
	AIRSIDE_API URoadProfile* Taxiway();

	/**
	 * The content set's three service-road tiers (narrow first), or empty when the set does
	 * not have exactly three - #310: byte-identical in BendLaneTest, WidthTaperTest and
	 * DesignVehicleTest (blame cbe729d6) before this, each with its own `!= 3` literal that a
	 * fourth tier would have had to fix in three places at once. Guarded on
	 * UAirsideSettings::WideServiceTier + 1, not a bare 3, so the guard NAMES why three: Wide
	 * is index 2, and Resolve*() below it reads ServiceRoadProfiles[WideServiceTier].
	 */
	AIRSIDE_API TArray<URoadProfile*> ServiceTiers();
}

/** Options for FTestAirport::Build, defaulted to the single-exit, single-stand shape every
 *  site but the two named on FTestAirport itself used before #101. */
struct FTestAirportOptions
{
	int32 StandCount = 1;
	/** 1: one exit, taxiway and stand(s) beside it. 2: two exits (ArrivalPlannerTest's
	 *  "earliest exit wins" shape) joined by a crossbar; the stand(s) sit beside the SECOND
	 *  exit's taxiway, which both exits can reach. */
	int32 ExitCount = 1;
	/** Derive the guideline graph (and, once a stand exists, its anchor link) before
	 *  returning. False for a fixture built onto a network something else - an actor's
	 *  RebuildMesh - will derive itself; see ArrivalDispatchTest's world variant. */
	bool bDerived = true;
};

/**
 * The arrival-airport fixture repeated, with small unnamed drifts, at every one of the sites
 * #101 names: a runway split at its exit(s) so a guideline node lands on the centreline for
 * RunwayExitNodes to find, a taxiway south from the (last) exit, and StandCount stand(s)
 * beside it facing east so their lead-ins cast west and meet the taxiway - see FAnchorLink's
 * own comment on why a stand must FACE the guideline it joins.
 */
struct AIRSIDE_API FTestAirport
{
	URoadNetwork* Net = nullptr;
	FVector2D Threshold = FVector2D::ZeroVector;
	/** The exit stands sit beside - the only one there is, or the second of two. Exits.Last(). */
	FVector2D ExitAt = FVector2D::ZeroVector;
	/** Every exit, threshold-first: one entry for ExitCount=1, two (the earlier, longer-taxi
	 *  exit then the one stands sit beside) for ExitCount=2. ArrivalPlannerTest's
	 *  EarliestExitWinsTest reads Exits[0] to check WHICH junction the chosen exit sits at,
	 *  rather than recomputing Needed * 1.2 itself - the drift #101 exists to end. */
	TArray<FVector2D> Exits;
	/** The runway's own threshold segment - a valid Seed for RunwayExitNodes/RunwayChain,
	 *  since either walks the whole chain from any member. */
	FRoadSegmentId ThresholdSegment;
	TArray<FEntityInstanceId> Stands;

	/** Fuel depots BuildScale places - empty for the plain Build() above, which places none.
	 *  Same FEntityInstanceId a stand is, so Pose() below reads either kind. */
	TArray<FEntityInstanceId> Depots;

	/** An interior TAXIWAY grid node BuildScale places - unset for the plain Build() above.
	 *  Unset (not Threshold or one of Exits) on purpose: a drag-frame test wants an ordinary
	 *  junction, not a runway endpoint that MoveNode's own length/facts validation may treat
	 *  differently - see Airside.Perf.Scale.DragFrameStaysGeometryOnly for the one caller. */
	FRoadNodeId SampleGridNode;

	/** Builds onto ExistingNet if given, else a fresh transient URoadNetwork. */
	static FTestAirport Build(const FAirframe& Airframe, const FTestAirportOptions& Options = FTestAirportOptions(),
		URoadNetwork* ExistingNet = nullptr);

	/**
	 * Issue #256: the ten-node shape above proves correctness; nothing in this module ever
	 * measured COST at the scale a real airport reaches. Two long runways (each split at two
	 * exits, matching Build's own single-exit shape doubled), an 8x20 taxiway grid south of
	 * them (~300 road segments between the two - see the .cpp for the exact count), 30 stands
	 * and 4 fuel depots hung off the grid's own rows.
	 *
	 * SEEDED, NOT RANDOM: Stream picks which of several plausible stand/depot slots and agent
	 * routes a given Seed uses, so two calls with the same Seed build bit-identical networks
	 * (a budget assertion that flakes on machine noise is worse than none) while still
	 * spreading load across the grid rather than piling every stand onto one taxiway the way
	 * a hand-picked fixture would. The GRID ITSELF is not randomised - its row/column count
	 * and spacing are fixed - only which grid nodes get a stand, a depot, or an agent's route
	 * endpoints varies with Seed.
	 *
	 * EXTENDS Build() rather than standing up a second builder file (see the issue's own
	 * brief): reuses TestGraph::Lay, TestProfiles::Runway/Taxiway and TestAirframes exactly as
	 * Build() does, and returns the SAME FTestAirport struct so Pose() below reads a scale
	 * fixture's stands and depots with no second accessor. Does NOT dispatch agents itself,
	 * matching Build()'s own division of labour: a caller that wants traffic on the fixture
	 * calls Pose() for a depot and a stand, routes between them with RouteSearch::Find, and
	 * dispatches - directly on a UGroundTraffic if no actor is involved, or on
	 * Actor->GetTraffic()->GetModel() when one is, the same choice every existing fixture
	 * caller already makes for itself.
	 *
	 * bDerived MATCHES FTestAirportOptions::bDerived above, kept as its own bool rather than
	 * folded into a options struct of one field: false for a fixture built onto an actor's own
	 * network (Actor->RebuildMesh does the derive, the real path an edit takes - see
	 * ArrivalDispatchTest's own "world variant" for the established shape); true for a bare
	 * network with nothing else to derive it.
	 */
	static FTestAirport BuildScale(const FAirframe& Airframe, int32 Seed, bool bDerived = true,
		URoadNetwork* ExistingNet = nullptr);

	/** One of Stands' or Depots' own pose node, or unset. Generalises the StandOcc/StandOcc2/
	 *  StandOcc3 Pose triplet (StandClaimTest, StandChoiceTest, StandRetargetTest) - and now
	 *  BuildScale's depots too, since a depot's placed FEntityInstance carries a PoseNode the
	 *  same way a stand's does. */
	FGuidelineNodeId Pose(FEntityInstanceId Stand) const;
};

/** Guideline-graph and road-graph builders shared by every fixture in the module. */
namespace TestGraph
{
	/** An AUTHORED guideline node (bDerived=false) at (X, Y). */
	AIRSIDE_API FGuidelineNodeId Node(URoadNetwork& Net, double X, double Y);

	/** Options for Join, defaulted to what a plain two-node line wants. */
	struct FJoinOptions
	{
		EGuidelineDir Direction = EGuidelineDir::Bidirectional;
		/** Overrides the control point; null takes the midpoint of A and B. */
		const FVector2D* Control = nullptr;
		bool bDerived = true;
	};

	/** A guideline edge from A to B. Options.bDerived defaults true, matching FGuidelineEdge's
	 *  own default - so a plain Join() is NOT authored and will not survive a guideline
	 *  sweep. Pass bDerived=false for one that does (see TrafficForwardersTest's own comment
	 *  on why AgentRedirectTest's fixture needs that). */
	AIRSIDE_API FGuidelineEdgeId Join(URoadNetwork& Net, FGuidelineNodeId A, FGuidelineNodeId B, const FJoinOptions& Options = FJoinOptions());

	/** A straight road segment between two EXISTING road nodes, carrying Profile. */
	AIRSIDE_API FRoadSegmentId Lay(URoadNetwork& Net, FRoadNodeId A, FRoadNodeId B, URoadProfile* Profile);

	/** The guideline node the builder derived for one end of Segment, or unset. */
	AIRSIDE_API FGuidelineNodeId NodeFor(const URoadNetwork& Net, FRoadSegmentId Segment, bool bEndA);

	/**
	 * Solve and derive the guideline graph the PRODUCTION way: one FRoadDesignVehicles
	 * resolved (or DesignVehicles, if a caller already has one) and passed to both SolveAll
	 * and FRoadGuidelineBuilder::Build, the same "resolve once, hand it down" URoadSurfacePresenter
	 * itself follows (#190) - not SolveAll(nullptr) then a second, independent
	 * ResolveRoadDesignVehicles() call for Build, which is what every one of the ~45 inline
	 * copies this replaces did instead (issue #311, regression of #101). Returns the solve
	 * result for a caller that still reads it (BendOuters, NodeResults, FailedNodes, ...);
	 * a caller that does not just discards it. Does NOT link stands - see Rebuild below,
	 * built on this, for the fixture-wide facade sequence.
	 */
	AIRSIDE_API FRoadSolveResult Derive(URoadNetwork& Net, const FRoadDesignVehicles* DesignVehicles = nullptr,
		EWideningTrace Widening = EWideningTrace::Trace);

	/** Solve, derive guidelines and re-link every entity: what the facade's RebuildMesh does. */
	AIRSIDE_API void Rebuild(URoadNetwork& Net);

	/** A Corner() fixture's own network, solve result and the three handles a caller needs -
	 *  the node between the two arms, and each arm's segment. */
	struct FCornerFixture
	{
		URoadNetwork* Net = nullptr;
		FRoadSolveResult Solved;
		FRoadNodeId Corner;
		FRoadSegmentId First;
		FRoadSegmentId Second;
	};

	/**
	 * A 3-node road fixture, solved and guideline-built the production way
	 * (ResolveRoadDesignVehicles, then the solver, then the builder): West(0,0) -> Corner ->
	 * Far, First carrying Profile and Second carrying SecondProfile (or Profile again if
	 * null). #310: BendLaneTest's right-angle bend (CornerAt (8000,0), FarAt (8000,8000)) and
	 * WidthTaperTest's straight width step (CornerAt (6000,0), FarAt (12000,0)) were the SAME
	 * fixture typed twice - only the two points differ, not the shape.
	 */
	AIRSIDE_API FCornerFixture Corner(URoadProfile* Profile, URoadProfile* SecondProfile = nullptr,
		const FVector2D& CornerAt = FVector2D(8000.0, 0.0), const FVector2D& FarAt = FVector2D(8000.0, 8000.0));

	/**
	 * A GraphProbe route from A to B, through FRouteQuery::For rather than a hand-built
	 * FRouteQuery - issue #312. FRouteQuery::For is the ONLY place that copies the resolved
	 * FRoutePolicy::Avoidance into Query.AvoidRunways (RouteSearch.cpp's own comment on why:
	 * "the table overwrites the field, rather than being set beside it"), and RunSearch's hot
	 * loop reads AvoidRunways, never Policy. A caller that fills Start/Goal/Class/Errand by
	 * hand and skips For() gets a query that always resolves the permissive
	 * ERunwayAvoidance::None, silently, for every errand - the six-helper, 48-site bug this
	 * function replaces. Vehicle null and Wingspan 0 match every GraphProbe call site's own
	 * defaults before #312 (no gating, no span limit) - not new behaviour, the SAME query
	 * those sites were already asking for, this time with AvoidRunways actually set.
	 */
	AIRSIDE_API FRoutePlan Probe(const URoadNetwork& Net, FGuidelineNodeId A, FGuidelineNodeId B,
		ETraversalClass Class, const FVehicle* Vehicle = nullptr, double Wingspan = 0.0);
}

#endif // WITH_DEV_AUTOMATION_TESTS
