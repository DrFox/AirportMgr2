#pragma once

// The tests module is a UNITY build: two anonymous-namespace helpers of one name in two
// .cpp files compile alone and collide together. Before this header the fix was a per-file
// name prefix (M2*, StandOcc2*, ...) - 12 different prefixes for the same handful of
// fixtures, ~1,500 lines of pasted setup. One header shared by every .cpp in the module (and
// forwarded to AirportOpsTests) needs no prefix at all: it is included once per translation
// unit like any other header, not pasted into an anonymous namespace per file. See #99.

#include "CoreMinimal.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadEntity.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"

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
	double TaxiwayLength = 20000.0;
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
	/** The exit stands sit beside - the only one there is, or the second of two. */
	FVector2D ExitAt = FVector2D::ZeroVector;
	/** The runway's own threshold segment - a valid Seed for RunwayExitNodes/RunwayChain,
	 *  since either walks the whole chain from any member. */
	FRoadSegmentId ThresholdSegment;
	TArray<FEntityInstanceId> Stands;

	/** Builds onto ExistingNet if given, else a fresh transient URoadNetwork. */
	static FTestAirport Build(const FAirframe& Airframe, const FTestAirportOptions& Options = FTestAirportOptions(),
		URoadNetwork* ExistingNet = nullptr);
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

	/** An authored guideline edge from A to B. */
	FGuidelineEdgeId Join(URoadNetwork& Net, FGuidelineNodeId A, FGuidelineNodeId B, const FJoinOptions& Options = FJoinOptions());

	/** A straight road segment between two EXISTING road nodes, carrying Profile. */
	FRoadSegmentId Lay(URoadNetwork& Net, FRoadNodeId A, FRoadNodeId B, URoadProfile* Profile);

	/** The guideline node the builder derived for one end of Segment, or unset. */
	FGuidelineNodeId NodeFor(const URoadNetwork& Net, FRoadSegmentId Segment, bool bEndA);

	/** Solve, derive guidelines and re-link every entity: what the facade's RebuildMesh does. */
	void Rebuild(URoadNetwork& Net);
}
