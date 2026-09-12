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

/** Airframes built by hand for tests that need one without going through content. */
namespace TestAirframes
{
}

/** Runway and taxiway profiles authored by hand, MakeTransient so no asset is touched. */
namespace TestProfiles
{
}

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
