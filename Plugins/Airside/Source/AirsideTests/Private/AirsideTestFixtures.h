#pragma once

// The tests module is a UNITY build: two anonymous-namespace helpers of one name in two
// .cpp files compile alone and collide together. Before this header the fix was a per-file
// name prefix (M2*, StandOcc2*, ...) - 12 different prefixes for the same handful of
// fixtures, ~1,500 lines of pasted setup. One header shared by every .cpp in AirsideTests
// needs no prefix at all: it is included once per translation unit like any other header,
// not pasted into an anonymous namespace per file. See #99.
//
// #189 EXPOSED FAirsideTestWorld AND FNullEditTarget to AirportMgr, AirportOpsTests and
// AirsideEditor - the Public/ move this comment used to defer - by putting them in
// Airside/Public/Testing/AirsideTestWorld.h instead of here (AirsideTests is Private to this
// module, so nothing outside it may include this header; Airside is a dependency of all four
// test-hosting modules already). This header #includes and forwards rather than declaring
// its own copy, so every one of the 64 existing FAirsideTestWorld users in this module needed
// no change.

#include "CoreMinimal.h"
#include "Build/RoadNetworkSolver.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadEntity.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Testing/AirsideTestGraph.h"
#include "Testing/AirsideTestWorld.h"
#include "Tool/RoadBuildTool.h"
#include "Tool/RoadSnap.h"
#include "Tool/SnapGuideChain.h"

class ARoadNetworkActor;
class UAircraftType;
class URoadProfile;
struct FRunwayRequirements;

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
 * Guide-source builders shared by every guide test.
 *
 * MOVED OUT OF NetworkGuideSourceTest.cpp's anonymous namespace on 2026-09-20, when a second
 * and third file needed LayRunway. The tests module is a UNITY build: two helpers of one name
 * in two anonymous namespaces compile alone and collide together, which is the whole reason
 * this header exists - see its own top comment.
 */
namespace TestGuide
{
	/** An anchor with no reference and no points, so ONLY the network sources answer. */
	FGuideAnchor BareAnchor(const FVector2D& Origin);

	/**
	 * A runway strip. NOT ConnectNodes: ERoadKind has only Taxiway and ServiceRoad, because a
	 * runway is not a road kind - it is a segment placed through PlaceRunway with a runway
	 * profile, which is what URoadNetwork::IsRunwaySegment then recognises.
	 *
	 * Minimum is dropped first: MinimumRunwayLength defaults to 50000 uu and PlaceRunway
	 * refuses anything under it, so a test strip either lowers the bar or is half a kilometre
	 * long. MeshFreshnessTest does exactly this, for exactly this reason. Defaulted to 100.0,
	 * the figure every existing caller wanted, so #310's migration needed no call-site change;
	 * a caller that wants PlaceRunway's own refusal passes the real 50000 through explicitly.
	 */
	bool LayRunway(ARoadNetworkActor* Actor, const FVector2D& From, const FVector2D& To, double Minimum = 100.0);

	/**
	 * Every candidate ONE source proposes, with the rest of the chain kept out of it.
	 *
	 * CURSOR DEFAULTS TO THE ANCHOR'S ORIGIN, which is what every caller written before
	 * IGuideSource::Propose took one meant: the drag had not moved. A test about where the FAR
	 * END lands passes its own - see Airside.Tool.OffsetGuideReachesWhatTheCursorIsNear.
	 */
	TArray<SnapGuide::FCandidate> ProposedBy(const IGuideSource& Source,
		const URoadNetwork& Network, const FGuideAnchor& Anchor,
		const TOptional<FVector2D>& Cursor = TOptional<FVector2D>());
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
	 *  not dominate the clock, and nothing that could arm a departure - an FVehicle since
	 *  2026-09-23, so there is no climb to arm one with. */
	FVehicle Van();

	/** The content-set default airframe with Climb cleared, so it taxis but never lands or
	 *  departs - a ground vehicle in everything but name. */
	FAirframe GroundOnly();

	/** The Piper's own published field-length requirements, independent of Piper() above:
	 *  Airside.Model.ArrivalPlanner.NotAdmitted tests admission against the PUBLISHED
	 *  figures on their own, not bundled into a flyable airframe. */
	FRunwayRequirements PiperRequirements();

	/**
	 * A fresh, transient UAircraftType already run through BuildPiperMeridian - not merely
	 * the FAirframe fields Piper() above gives, but the AUTHORED TYPE itself, for a test that
	 * needs to mutate a footprint or an axle figure before reading it back through
	 * Type->Airframe() (issue #194: AirframeAxlesTest.cpp, FieldLengthTest.cpp and
	 * GearCycleTest.cpp each built one by hand with NewObject<UAircraftType>() plus their own
	 * BuildPiperMeridian call - three copies of the same two lines with no fixture between
	 * them). Check-Architecture rule 4 exempts this file as the one allowed PiperMeridian*()
	 * caller in the test modules; every other test goes through this instead.
	 */
	UAircraftType* PiperType();
}

// TestProfiles, FTestAirportOptions, FTestAirport and TestGraph now live in
// Airside/Public/Testing/AirsideTestGraph.h (#311, a regression of #101, following #189's own
// move of FAirsideTestWorld here) - #included above rather than declared again in this file,
// so every one of the 60+ existing AirsideTests includers keeps compiling unchanged.

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
