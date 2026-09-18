#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Present/RoadNetworkActor.h"
#include "Profiles/RoadProfile.h"
#include "Solve/GuideArbiter.h"
#include "Tool/RoadEditTarget.h"
#include "Tool/RoadNaming.h"
#include "Tool/SnapGuideChain.h"
#include "Tool/SnapGuideSettings.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** A road of the given kind between two points, through the facade so it gets a profile. */
	void Lay(ARoadNetworkActor* Actor, const FVector2D& From, const FVector2D& To, ERoadKind Kind)
	{
		IRoadEditTarget* Target = Actor;
		const int32 A = Target->PlaceNode(From);
		const int32 B = Target->PlaceNode(To);
		Target->ConnectNodes(A, B, Kind, INDEX_NONE);
	}

	/** An anchor with no reference and no points, so ONLY the network sources answer. */
	FGuideAnchor BareAnchor(const FVector2D& Origin)
	{
		FGuideAnchor Anchor;
		Anchor.Origin = Origin;
		return Anchor;
	}

	/**
	 * A runway strip. NOT ConnectNodes: ERoadKind has only Taxiway and ServiceRoad, because a
	 * runway is not a road kind - it is a segment placed through PlaceRunway with a runway
	 * profile, which is what URoadNetwork::IsRunwaySegment then recognises.
	 *
	 * MinimumRunwayLength is dropped first: it defaults to 50000 uu and PlaceRunway refuses
	 * anything under it, so a test strip either lowers the bar or is half a kilometre long.
	 * MeshFreshnessTest does exactly this, for exactly this reason.
	 */
	bool LayRunway(ARoadNetworkActor* Actor, const FVector2D& From, const FVector2D& To)
	{
		// A NODE FIRST, PURELY TO BRING THE NETWORK INTO BEING. The facade creates URoadNetwork
		// lazily inside PlaceNode and PlaceRunway does NOT - so a test whose first call is
		// PlaceRunway leaves Actor->Network null, and dereferencing it reads offset 0x60 off a
		// null pointer. That is not hypothetical: it crashed this very test, and a crash hides
		// its cause where a failure would have named it. MeshFreshnessTest places a node first
		// for the same reason and says so.
		Actor->PlaceNode(FVector2D(-100000.0, -100000.0));

		URoadProfile* Profile = URoadProfile::MakeTransient(4500.0, 1500.0, 450.0);
		Profile->bContinuousThroughJunctions = true;

		// Defaults to 50000 uu, and PlaceRunway refuses anything under it.
		Actor->MinimumRunwayLength = 100.0;
		return Actor->PlaceRunway(From, To, Profile);
	}

	/** Every candidate ONE source proposes, with the rest of the chain kept out of it. */
	TArray<SnapGuide::FCandidate> ProposedBy(const IGuideSource& Source,
		const URoadNetwork& Network, const FGuideAnchor& Anchor)
	{
		TArray<SnapGuide::FCandidate> Out;
		Source.Propose(Network, Anchor, Out);
		return Out;
	}
}

/**
 * A LAID TAXIWAY PRODUCES A PARALLEL CANDIDATE with its direction and a ReferenceAt ON it -
 * spec section 9's Airside.Tool.GuideChainProposesFromTheNetwork, which stage 1 could not
 * write because no source read the network.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FParallelGuideFollowsTheNearestRoadTest,
	"Airside.Tool.ParallelGuideFollowsTheNearestRoad",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FParallelGuideFollowsTheNearestRoadTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("a network actor"), Actor)) { return false; }

	// An east-west taxiway through the origin, and a north-south one far to the east.
	Lay(Actor, FVector2D(-10000.0, 0.0), FVector2D(10000.0, 0.0), ERoadKind::Taxiway);
	Lay(Actor, FVector2D(50000.0, -10000.0), FVector2D(50000.0, 10000.0), ERoadKind::Taxiway);

	const FParallelGuideSource Source;
	const FGuideAnchor Anchor = BareAnchor(FVector2D(0.0, 2000.0));
	const TArray<SnapGuide::FCandidate> Candidates =
		ProposedBy(Source, *Actor->Network, Anchor);

	if (!TestEqual(TEXT("the nearest road proposes its direction and its perpendicular"),
		Candidates.Num(), 2))
	{
		return false;
	}

	// THE NEAR ROAD, NOT THE FAR ONE. Both are taxiways; only the reach tells them apart, and
	// without it the far one would be in the race on equal terms.
	TestTrue(TEXT("the candidate runs along the near road"),
		FMath::IsNearlyZero(Candidates[0].Direction.Y, 1.0e-6));
	TestTrue(TEXT("and the dashed line points at a spot ON that road"),
		FMath::IsNearlyZero(Candidates[0].ReferenceAt.Y, 1.0e-6));
	TestTrue(TEXT("beneath the drag, not at the road's far end"),
		FMath::IsNearlyZero(Candidates[0].ReferenceAt.X, 1.0e-6));

	TestEqual(TEXT("angular, because it answers which way from here"),
		static_cast<int32>(Candidates[0].Fit), static_cast<int32>(SnapGuide::EFit::Angular));
	TestTrue(TEXT("through the drag's own origin"),
		Candidates[0].Through.Equals(Anchor.Origin, 1.0e-6));
	TestEqual(TEXT("and named by what the road admits"),
		Candidates[0].Description, FString(TEXT("parallel to the taxiway")));
	TestEqual(TEXT("with the perpendicular named too"),
		Candidates[1].Description, FString(TEXT("square to the taxiway")));

	// CONTROL LEG: the reach is real. Drag beyond it and the near road stops answering, so
	// the assertions above are measuring the search and not merely the first segment laid.
	const TArray<SnapGuide::FCandidate> FarAway =
		ProposedBy(Source, *Actor->Network, BareAnchor(FVector2D(0.0, 30000.0)));
	TestEqual(TEXT("a drag beyond the search reach gets nothing from this source"),
		FarAway.Num(), 0);

	return true;
}

/**
 * A SERVICE ROAD IS NOT A TAXIWAY, and a label that called it one would be the kind of wrong
 * that survives review because each reader assumes the other's definition.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadNamingSaysWhatARoadAdmitsTest,
	"Airside.Tool.RoadNamingSaysWhatARoadAdmits",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadNamingSaysWhatARoadAdmitsTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("a network actor"), Actor)) { return false; }

	Lay(Actor, FVector2D(-10000.0, 0.0), FVector2D(10000.0, 0.0), ERoadKind::Taxiway);
	Lay(Actor, FVector2D(-10000.0, 5000.0), FVector2D(10000.0, 5000.0), ERoadKind::ServiceRoad);

	TestEqual(TEXT("a taxiway is called one"),
		RoadNaming::Describe(*Actor->Network, Actor->Network->SegmentIdAt(0)),
		FString(TEXT("the taxiway")));
	TestEqual(TEXT("and a service road is not called a taxiway"),
		RoadNaming::Describe(*Actor->Network, Actor->Network->SegmentIdAt(1)),
		FString(TEXT("the service road")));

	return true;
}

/**
 * IN LINE WITH A ROAD IS NOT THE SAME AS PARALLEL TO IT. A cursor past the end of a taxiway,
 * dead on its centreline, is collinear with it; a cursor the same distance to the SIDE is
 * parallel and not collinear. The two sources must disagree there, or one is redundant.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCollinearGuideIsNotParallelTest,
	"Airside.Tool.CollinearGuideIsNotParallel",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FCollinearGuideIsNotParallelTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("a network actor"), Actor)) { return false; }

	// One east-west taxiway, from the origin eastwards.
	Lay(Actor, FVector2D(0.0, 0.0), FVector2D(6000.0, 0.0), ERoadKind::Taxiway);

	const FCollinearGuideSource Source;
	const TArray<SnapGuide::FCandidate> Candidates =
		ProposedBy(Source, *Actor->Network, BareAnchor(FVector2D(7000.0, 0.0)));

	if (!TestEqual(TEXT("the one road in reach proposes its own line"), Candidates.Num(), 1))
	{
		return false;
	}

	TestEqual(TEXT("perpendicular, because it is about where the cursor ended up"),
		static_cast<int32>(Candidates[0].Fit),
		static_cast<int32>(SnapGuide::EFit::Perpendicular));
	TestTrue(TEXT("the line passes through the road, not through the drag"),
		FMath::IsNearlyZero(Candidates[0].Through.Y, 1.0e-6));
	TestEqual(TEXT("named as being in line with it"),
		Candidates[0].Description, FString(TEXT("in line with the taxiway")));

	// THE LINE IS THE ROAD'S, so the arbiter finds the cursor ON it however far past the end
	// the drag has gone - which is the case Parallel cannot express.
	const SnapGuide::FResult Result = SnapGuide::Arbitrate(
		Candidates, FVector2D(7000.0, 0.0), FVector2D(9000.0, 40.0), SnapGuide::FResult());
	TestTrue(TEXT("a cursor on the road's extension is offered the guide"), Result.bActive);
	TestTrue(TEXT("and is pulled exactly onto the centreline"),
		FMath::IsNearlyZero(Result.Point.Y, 1.0e-6));

	// CONTROL LEG: a cursor well to the SIDE of the road is not in line with it, however
	// parallel it may be. Without this the test would pass on a source that proposed a line
	// through the drag instead of through the road.
	const SnapGuide::FResult Beside = SnapGuide::Arbitrate(
		Candidates, FVector2D(7000.0, 0.0), FVector2D(9000.0, 3000.0), SnapGuide::FResult());
	TestFalse(TEXT("a cursor 30 m to the side is not in line with anything"), Beside.bActive);

	return true;
}

/**
 * A RUNWAY IS OFFERED FROM ANYWHERE ON THE FIELD, which is the one way this source differs
 * from Parallel - and that difference is the point of it: an airport squares to its runways.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRunwayGuideReachesTheWholeFieldTest,
	"Airside.Tool.RunwayGuideReachesTheWholeField",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRunwayGuideReachesTheWholeFieldTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("a network actor"), Actor)) { return false; }

	// A runway along +X through the origin. NORTH IS +X in this project (see RunwayDesignator's
	// own header comment), so this strip is 18/36 and NOT 09/27 - the first draft of this test
	// asserted 09/27 and would have failed against a correct source.
	if (!TestTrue(TEXT("the runway is placed"),
		LayRunway(Actor, FVector2D(-40000.0, 0.0), FVector2D(40000.0, 0.0))))
	{
		return false;
	}

	// HONOURED, NOT ASSUMED. PlaceRunway can refuse - too short, no profile - and every
	// assertion below would then be measuring an empty field while looking like a source bug.
	if (!TestTrue(TEXT("and the network exists to be searched"), Actor->Network != nullptr))
	{
		return false;
	}

	const FRunwayGuideSource Source;

	// FAR BEYOND SearchRadiusUu - 500 m out, where every other network source has given up.
	const TArray<SnapGuide::FCandidate> Candidates =
		ProposedBy(Source, *Actor->Network, BareAnchor(FVector2D(0.0, 50000.0)));

	if (!TestEqual(TEXT("the runway proposes its heading and its perpendicular"),
		Candidates.Num(), 2))
	{
		return false;
	}

	TestTrue(TEXT("offered from right across the field, unlike every other network source"),
		FMath::IsNearlyZero(Candidates[0].Direction.Y, 1.0e-6));
	TestEqual(TEXT("angular, through the drag's own origin"),
		static_cast<int32>(Candidates[0].Fit), static_cast<int32>(SnapGuide::EFit::Angular));

	// NAMED AS A RUNWAY IS SPOKEN OF, both ends, low first - a label saying "the taxiway" over
	// a runway would be the classification quietly disagreeing with itself.
	TestTrue(*FString::Printf(TEXT("named by its designators, got '%s'"),
		*Candidates[0].Description),
		Candidates[0].Description.Contains(TEXT("runway 18/36")));

	// CONTROL LEG: the source is selective. A taxiway laid beside it must NOT be offered here,
	// or this test would pass on a source that proposed every segment on the field.
	Lay(Actor, FVector2D(-10000.0, 20000.0), FVector2D(10000.0, 20000.0), ERoadKind::Taxiway);
	const TArray<SnapGuide::FCandidate> Again =
		ProposedBy(Source, *Actor->Network, BareAnchor(FVector2D(0.0, 50000.0)));
	TestEqual(TEXT("and a taxiway is not mistaken for a runway"), Again.Num(), 2);

	return true;
}

/**
 * A STAND'S POSE SETS A DIRECTION, and the guide must take it from the pose rather than from
 * anything the stand happens to sit beside.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAlignedGuideTakesThePoseDirectionTest,
	"Airside.Tool.AlignedGuideTakesThePoseDirection",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAlignedGuideTakesThePoseDirectionTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("a network actor"), Actor)) { return false; }

	// A stand at the origin facing 45 degrees - deliberately NOT a world axis, so a source that
	// quietly proposed an axis instead of the pose would be caught here.
	IRoadEditTarget* Target = Actor;
	Target->PlaceStand(FVector2D(0.0, 0.0), FMath::DegreesToRadians(45.0));

	if (!TestTrue(TEXT("the network exists to be searched"), Actor->Network != nullptr))
	{
		return false;
	}

	const FAlignedGuideSource Source;
	const TArray<SnapGuide::FCandidate> Candidates =
		ProposedBy(Source, *Actor->Network, BareAnchor(FVector2D(2000.0, 2000.0)));

	if (!TestEqual(TEXT("the stand proposes its facing and its perpendicular"),
		Candidates.Num(), 2))
	{
		return false;
	}

	// RADIANS, NOT DEGREES. cos(45 deg) and sin(45 deg) are the same number, so this leg also
	// pins that the source did not read Heading as degrees - 45 radians points somewhere else
	// entirely, and plausible-but-wrong is the worst kind of wrong.
	const double Root2Over2 = FMath::Sin(FMath::DegreesToRadians(45.0));
	TestTrue(TEXT("the candidate points the way the stand faces, in radians not degrees"),
		FMath::IsNearlyEqual(Candidates[0].Direction.X, Root2Over2, 1.0e-6)
			&& FMath::IsNearlyEqual(Candidates[0].Direction.Y, Root2Over2, 1.0e-6));
	TestTrue(TEXT("and the dashed line points at the stand itself"),
		Candidates[0].ReferenceAt.Equals(FVector2D::ZeroVector, 1.0e-6));
	TestEqual(TEXT("angular, through the drag's own origin"),
		static_cast<int32>(Candidates[0].Fit), static_cast<int32>(SnapGuide::EFit::Angular));

	// NAMED, AND NOT EMPTY. The definition carries no authored DisplayName in a test, so this
	// is the asset-name fallback doing its job - an empty label would read as a bug on screen.
	TestFalse(TEXT("the label names the thing rather than reading blank"),
		Candidates[0].Description.IsEmpty());
	TestTrue(TEXT("and says what the relationship is"),
		Candidates[0].Description.StartsWith(TEXT("aligned with ")));

	// CONTROL LEG: the reach applies here too, so this source cannot quietly become global.
	const TArray<SnapGuide::FCandidate> FarAway =
		ProposedBy(Source, *Actor->Network, BareAnchor(FVector2D(0.0, 40000.0)));
	TestEqual(TEXT("a stand beyond the search reach proposes nothing"), FarAway.Num(), 0);

	return true;
}

/**
 * SEVEN SOURCES, ONE ANGULAR SLOT. With a taxiway, a runway and the world grid all offering
 * the same direction, the tiebreak must go to the most specific - and "most specific" is the
 * design's section 3 order, not the order the chain happens to ask in.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGuideChainPrefersTheLocalOverTheGlobalTest,
	"Airside.Tool.GuideChainPrefersTheLocalOverTheGlobal",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGuideChainPrefersTheLocalOverTheGlobalTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("a network actor"), Actor)) { return false; }

	// A taxiway and a runway both running along +X, which is also a world axis: three sources
	// offering one direction, every one of them in tolerance at once.
	Lay(Actor, FVector2D(-10000.0, 0.0), FVector2D(10000.0, 0.0), ERoadKind::Taxiway);
	if (!TestTrue(TEXT("the runway is placed"),
		LayRunway(Actor, FVector2D(-40000.0, 8000.0), FVector2D(40000.0, 8000.0))))
	{
		return false;
	}

	const FSnapGuideChain Chain;
	const FGuideAnchor Anchor = BareAnchor(FVector2D(0.0, 2000.0));

	// EVERY SOURCE THIS TEST IS ABOUT, STATED RATHER THAN INHERITED. Since stage 3 the chain
	// skips whatever is switched off, and Runway defaults OFF - a test about PRIORITY that took
	// the defaults would silently be testing which sources happen to be on instead.
	FSnapGuideSettings Live;
	Live.bParallel = true;
	Live.bRunway = true;
	Live.bWorld = true;

	const SnapGuide::FResult Result = Chain.Resolve(
		*Actor->Network, Anchor, FVector2D(3000.0, 2100.0), SnapGuide::FResult(), Live);

	if (!TestTrue(TEXT("something answers"), Result.bActive)) { return false; }

	// PARALLEL BEATS RUNWAY BEATS WORLD. An airport squares to its runways, but not in
	// preference to the taxiway the player is actually working beside.
	TestEqual(TEXT("the nearest road wins over the runway and the world grid"),
		static_cast<int32>(Result.Winners[0].Source),
		static_cast<int32>(SnapGuide::ESource::Parallel));

	// CONTROL LEG: the runway was a live competitor, not one the reach quietly excluded. Take
	// the taxiway out of range and the runway takes the slot - which also pins that Runway is
	// exempt from SearchRadiusUu, since the drag is 80 m from it.
	const FGuideAnchor FarFromTheRoad = BareAnchor(FVector2D(0.0, 30000.0));
	const SnapGuide::FResult WithoutTheTaxiway = Chain.Resolve(
		*Actor->Network, FarFromTheRoad, FVector2D(3000.0, 30100.0), SnapGuide::FResult(), Live);
	if (!TestTrue(TEXT("the runway still answers from across the field"),
		WithoutTheTaxiway.bActive))
	{
		return false;
	}
	TestEqual(TEXT("and takes the slot once no road is in reach"),
		static_cast<int32>(WithoutTheTaxiway.Winners[0].Source),
		static_cast<int32>(SnapGuide::ESource::Runway));

	return true;
}

#endif
