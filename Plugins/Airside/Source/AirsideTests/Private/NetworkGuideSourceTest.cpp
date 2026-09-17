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

#endif
