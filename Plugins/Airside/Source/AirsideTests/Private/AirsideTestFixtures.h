#pragma once

// The tests module is a UNITY build: two anonymous-namespace helpers of one name in two
// .cpp files compile alone and collide together. Before this header the fix was a per-file
// name prefix (M2*, StandOcc2*, ...) - 12 different prefixes for the same handful of
// fixtures, ~1,500 lines of pasted setup. One header shared by every .cpp in AirsideTests
// needs no prefix at all: it is included once per translation unit like any other header,
// not pasted into an anonymous namespace per file. See #99. NOT YET exposed to
// AirportOpsTests - that needs a Public/ move, AIRSIDETESTS_API on the non-template members,
// and an AirportOpsTests.Build.cs dependency; tracked as a #99 follow-up rather than done
// here, given this PR's size.

#include "CoreMinimal.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadEntity.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Tool/RoadBuildTool.h"
#include "Tool/RoadSnap.h"

class ARoadNetworkActor;
class UWorld;
class URoadProfile;
struct FRunwayRequirements;

/**
 * A world and a network actor to test through the composition root rather than the model
 * alone - RAII so a test that returns early (TestTrue(...) { return false; }, this
 * codebase's idiom throughout) still tears the world down. Mirrors the
 * CreateWorld / CreateNewWorldContext / SpawnActor<ARoadNetworkActor> / DestroyWorldContext
 * sequence every present-layer test wrote out by hand (HoldingPointToolTest, RunwayToolTest,
 * TrafficForwardersTest before #99).
 */
struct FAirsideTestWorld
{
	UWorld* World = nullptr;
	ARoadNetworkActor* Actor = nullptr;

	FAirsideTestWorld();
	~FAirsideTestWorld();

	FAirsideTestWorld(const FAirsideTestWorld&) = delete;
	FAirsideTestWorld& operator=(const FAirsideTestWorld&) = delete;
};

/** FToolContext builders shared by every tool test - see ContextAt's own comment (#104). */
namespace TestTool
{
	/**
	 * A context whose snap is exactly what a click at Where would report, at 150uu radius
	 * (the value every tool test used) unless SnapRadius says otherwise (GuidelineDrawToolTest
	 * needs 400uu to reach across the gap it tests). Cursor and Snap.Position both land on
	 * Where, matching every one of the eight per-file builders this replaces - they never
	 * differed. A Node or Segment Kind still needs its own handle filled in by the caller
	 * (Snap.Node / Snap.Segment): this only sets what every kind has in common.
	 */
	FToolContext ContextAt(IRoadEditTarget& Target, const FVector2D& Where,
		ERoadSnapKind Kind = ERoadSnapKind::Free, double SnapRadius = 150.0);
}

/**
 * Ticks Traffic in fixed steps of Dt until Seconds elapse or Callback returns false for a
 * tick. Callback takes the tick index (0-based) and returns whether to keep going. Returns
 * the number of ticks actually run. Generalises GroundTrafficTest's M2TrafficRun, called
 * about thirty times in that one file alone - the shape FFuelFixture::AdvanceUntil
 * (AirportOpsTests/FuelServiceTest.cpp) also uses, on its own day-compressed clock.
 */
template <typename F>
int32 TickUntil(UGroundTraffic& Traffic, const URoadNetwork& Net, double Seconds, F Callback, double Dt = 0.05)
{
	int32 Ticks = 0;
	for (double Clock = 0.0; Clock < Seconds; Clock += Dt, ++Ticks)
	{
		Traffic.Advance(Dt, &Net);
		if (!Callback(Ticks)) { break; }
	}
	return Ticks;
}

/**
 * Ticks Traffic in fixed steps of Dt until Pred() is true or Seconds elapse; returns
 * whether Pred was true when it stopped (checked once more even past the deadline, so a
 * predicate that only turns true on the final tick is not missed by one Dt). Generalises
 * the StandOcc/StandOcc2/StandOcc3 RunUntil triplet (StandClaimTest, StandChoiceTest,
 * StandRetargetTest) - a bare predicate rather than TickUntil's per-tick callback, so kept
 * as its own helper instead of forcing one shape onto the other.
 */
template <typename P>
bool RunUntil(UGroundTraffic& Traffic, const URoadNetwork& Net, double Seconds, P Pred, double Dt = 0.05)
{
	for (double Clock = 0.0; Clock < Seconds; Clock += Dt)
	{
		Traffic.Advance(Dt, &Net);
		if (Pred()) { return true; }
	}
	return Pred();
}

/**
 * Airframes built by hand for tests that need one without going through content. THE RAW
 * FALLBACK, not UAirsideSettings::ResolveDefaultAirframe(), because these tests need a
 * fixed, known airframe (an aircraft that can land, or one that deliberately cannot) rather
 * than whatever DefaultAirside.ini currently names as the default - a content change should
 * not silently change what these tests measure. Check-Architecture rule 4 enforces that
 * PiperMeridian*() is called from nowhere else in a test module.
 */
namespace TestAirframes
{
	/** A Piper Meridian: Ground, Climb, Approach and Engine, so it can both land and taxi. */
	FAirframe Piper();

	/** Ground defaults (Accel 100, Decel 200, cap 1000), a nimble nosewheel so corners do
	 *  not dominate the clock, and nothing that could arm a departure. */
	FAirframe Van();

	/** The content-set default airframe with Climb cleared, so it taxis but never lands or
	 *  departs - a ground vehicle in everything but name. */
	FAirframe GroundOnly();

	/** The Piper's own published field-length requirements, independent of Piper() above:
	 *  Airside.Model.ArrivalPlanner.NotAdmitted tests admission against the PUBLISHED
	 *  figures on their own, not bundled into a flyable airframe. */
	FRunwayRequirements PiperRequirements();
}

/** Runway and taxiway profiles authored by hand, MakeTransient so no asset is touched. */
namespace TestProfiles
{
	/** 4500 wide / 1500 / 450, continuous through junctions - the runway every fixture in
	 *  the module lands a Piper on. */
	URoadProfile* Runway();

	/** 1800 wide / 1500 / 180, continuous through junctions - the width TrafficHeadOnReplan
	 *  and HoldingPositionFullWidth need to own, where a wider strip's own geometry would
	 *  hide the thing under test. */
	URoadProfile* NarrowRunway();

	/** 2300 wide / 1500 / 230 - the taxiway every fixture in the module uses. NOT continuous
	 *  through junctions: a taxiway is not a runway chain. */
	URoadProfile* Taxiway();
}

/**
 * A taxiway crossing a runway: S -> H (near bar) -> X (on the strip's centreline) -> N, hand-
 * built with no DerivedFrom - the holding position at H is the only thing protecting the
 * runway, which is the point of every test that builds one. bFarBar adds a second bar past X
 * protecting the SAME strip, the way a real crossing is painted (one bar each side) -
 * Airside.Model.Traffic.CrossingHoldsRunway needs it to measure that the far bar does not
 * re-arm the crossing once passed. The far node itself is not returned: nothing downstream
 * of Build needs its handle, only that it exists.
 */
struct FCrossingFixture
{
	FRoadSegmentId Strip;
	FGuidelineNodeId S, H, X, N;

	static FCrossingFixture Build(URoadNetwork& Net, bool bFarBar = false);
};

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
struct FTestAirport
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

	/** Builds onto ExistingNet if given, else a fresh transient URoadNetwork. */
	static FTestAirport Build(const FAirframe& Airframe, const FTestAirportOptions& Options = FTestAirportOptions(),
		URoadNetwork* ExistingNet = nullptr);

	/** One of Stands' own pose node, or unset. Generalises the StandOcc/StandOcc2/StandOcc3
	 *  Pose triplet (StandClaimTest, StandChoiceTest, StandRetargetTest). */
	FGuidelineNodeId Pose(FEntityInstanceId Stand) const;
};

/** Guideline-graph and road-graph builders shared by every fixture in the module. */
namespace TestGraph
{
	/** An AUTHORED guideline node (bDerived=false) at (X, Y). */
	FGuidelineNodeId Node(URoadNetwork& Net, double X, double Y);

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
	FGuidelineEdgeId Join(URoadNetwork& Net, FGuidelineNodeId A, FGuidelineNodeId B, const FJoinOptions& Options = FJoinOptions());

	/** A straight road segment between two EXISTING road nodes, carrying Profile. */
	FRoadSegmentId Lay(URoadNetwork& Net, FRoadNodeId A, FRoadNodeId B, URoadProfile* Profile);

	/** The guideline node the builder derived for one end of Segment, or unset. */
	FGuidelineNodeId NodeFor(const URoadNetwork& Net, FRoadSegmentId Segment, bool bEndA);

	/** Solve, derive guidelines and re-link every entity: what the facade's RebuildMesh does. */
	void Rebuild(URoadNetwork& Net);
}

/**
 * A runway long enough for the Piper to stop before the exit, one 45 degree taxiway, and a
 * stand beside it. Shared by RunwayExitArcTest.cpp's Build tests and ArrivalExitArcTest.cpp's
 * Model tests (issue #105 item 13 split the one file into those two, and hoisted this and
 * ExitArcNodeFor/ExitArcNodeNear/ExitArcTurnBetween here so both still argue about the same
 * junction) - X sits 60000 uu from the W threshold because the exit arc begins ExitLength
 * before it, and that start must be past the landing distance (about 37000) or the planner
 * would rightly skip it for a later node.
 */
struct FExitArcAirport
{
	URoadNetwork* Net = nullptr;
	FRoadSegmentId RW1, RW2, XT;
	FVector2D XAt = FVector2D(20000.0, 0.0);
	double ExitLength = 6000.0;
	FVector2D Threshold = FVector2D(-40000.0, 0.0);
};

FExitArcAirport ExitArcBuildAirport(UObject* Outer, bool bWithStand, double XDistance = 60000.0);

/** The guideline node a segment's derived guideline ENDS on, found by identity. */
FGuidelineNodeId ExitArcNodeFor(const URoadNetwork& Net, FRoadSegmentId Segment, bool bEndA);

/** The alive guideline node nearest a position, and how far off it is. */
FGuidelineNodeId ExitArcNodeNear(const URoadNetwork& Net, const FVector2D& At, double& OutMiss);

/** An alive derived TURN PATH (no DerivedFrom) joining two nodes, either way round. */
const FGuidelineEdge* ExitArcTurnBetween(const URoadNetwork& Net, FGuidelineNodeId P, FGuidelineNodeId Q);
