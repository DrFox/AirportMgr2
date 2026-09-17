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

#endif
