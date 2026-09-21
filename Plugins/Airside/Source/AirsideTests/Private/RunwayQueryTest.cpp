#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Model/RunwayQuery.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * RunwayQuery:: itself, not through URoadNetwork's forwarders every other test asks (issue
 * #194): the forwarders prove a caller reaches URoadNetwork::RunwayChain and friends, never
 * that the two IsPointOnRunway overloads - Seed, which walks, and Chain, which does not -
 * agree with each other, or that FRunwayChainCache answers with the SAME chain the plain
 * walk gives. Both are the "one evaluator" promise #87 and #170 make; nothing measured it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRunwayQueryOneEvaluatorTest,
	"Airside.Model.RunwayQuery.OneEvaluator",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRunwayQueryOneEvaluatorTest::RunTest(const FString& Parameters)
{
	// The same shape RunwayChainTest builds: a runway split at an exit into R1/R2, a
	// taxiway off the split, and a second, disjoint runway that must never be swept in.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Runway = URoadProfile::MakeTransient(4500.0, 1500.0, 450.0);
	Runway->bContinuousThroughJunctions = true;
	URoadProfile* Taxiway = URoadProfile::MakeTransient(2300.0, 1500.0, 230.0);

	const FRoadNodeId T = Net->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId E = Net->AddNode(FVector2D(60000.0, 0.0));
	const FRoadNodeId F = Net->AddNode(FVector2D(100000.0, 0.0));
	const FRoadSegmentId R1 = Net->AddStraightSegment(T, E, Runway);
	const FRoadSegmentId R2 = Net->AddStraightSegment(E, F, Runway);
	const FRoadNodeId X = Net->AddNode(FVector2D(60000.0, -20000.0));
	const FRoadSegmentId Tx = Net->AddStraightSegment(E, X, Taxiway);

	const FRoadNodeId T2 = Net->AddNode(FVector2D(0.0, 500000.0));
	const FRoadNodeId F2 = Net->AddNode(FVector2D(100000.0, 500000.0));
	Net->AddStraightSegment(T2, F2, Runway);

	// RunwayQuery::RunwayChain called DIRECTLY - not Net->RunwayChain - so this is a test of
	// the free function itself rather than of the forwarder that already has its own test.
	const TArray<FRoadSegmentId> Chain = RunwayQuery::RunwayChain(*Net, R2);
	TestEqual(TEXT("the walk crosses the exit split"), Chain.Num(), 2);
	TestTrue(TEXT("both halves are in it"), Chain.Contains(R1) && Chain.Contains(R2));

	// EVERY PROBE ASKED BOTH WAYS: the Seed overload re-walks RunwayChain(Seed) internally
	// and then calls the Chain overload (see RunwayQuery.cpp) - so if that internal call
	// were ever to drift (a copy-paste that walked a DIFFERENT seed, say), only comparing
	// the two live results here would catch it. A bare bool would not: both could happen to
	// agree on "false" while disagreeing on WHY.
	auto CheckAgree = [this, Net, R2, &Chain](const TCHAR* Name, const FVector2D& Probe)
	{
		double HalfWidthBySeed = -1.0;
		double HalfWidthByChain = -1.0;
		const bool bBySeed = RunwayQuery::IsPointOnRunway(*Net, Probe, R2, &HalfWidthBySeed);
		const bool bByChain = RunwayQuery::IsPointOnRunway(*Net, Probe, Chain, &HalfWidthByChain);
		TestEqual(*FString::Printf(TEXT("%s: Seed and Chain overloads agree on membership"), Name),
			bBySeed, bByChain);
		TestEqual(*FString::Printf(TEXT("%s: and on the chain's half width"), Name),
			HalfWidthBySeed, HalfWidthByChain, 0.01);
	};

	// On the near half of the chain (R1), on the far half (R2), and squarely off the strip
	// (the taxiway's own end, and a point beyond the strip's half width) - membership must
	// differ across these, but the TWO EVALUATORS must never differ from EACH OTHER.
	CheckAgree(TEXT("on R1"), FVector2D(30000.0, 0.0));
	CheckAgree(TEXT("on R2"), FVector2D(80000.0, 0.0));
	CheckAgree(TEXT("at the exit node"), FVector2D(60000.0, 0.0));
	CheckAgree(TEXT("on the taxiway, off the strip"), FVector2D(60000.0, -20000.0));
	CheckAgree(TEXT("laterally beyond the strip's half width"), FVector2D(30000.0, 5000.0));

	// And the one true positive is actually true, not merely "the two agree on false":
	// agreement alone cannot catch a shared bug that makes both overloads say no everywhere.
	TestTrue(TEXT("the centreline of R1 reads as on-strip"),
		RunwayQuery::IsPointOnRunway(*Net, FVector2D(30000.0, 0.0), R2));
	TestFalse(TEXT("the taxiway end reads as off-strip"),
		RunwayQuery::IsPointOnRunway(*Net, FVector2D(60000.0, -20000.0), R2));

	// --- FRunwayChainCache answers with the SAME chain the walk gives (issue #170) ---------
	FRunwayChainCache Cache;
	const TArray<FRoadSegmentId>& CachedChain = Cache.Get(*Net, R2);
	TestTrue(TEXT("the cache's chain is exactly the walk's, element for element"),
		CachedChain == Chain);
	TestEqual(TEXT("one walk paid for the first ask"), Cache.GetWalksForTest(), 1);

	// A second ask of the SAME seed, same revision, must be a cache HIT: no second walk,
	// which is the entire performance claim #170 exists to make. Asked through GetOrSeed
	// too - EntryFor's comment says both questions about one seed share the one walk.
	const TArray<FRoadSegmentId>& CachedOrSeed = Cache.GetOrSeed(*Net, R2);
	TestTrue(TEXT("GetOrSeed agrees with RunwayChainOrSeed on a live runway"),
		CachedOrSeed == RunwayQuery::RunwayChainOrSeed(*Net, R2));
	TestEqual(TEXT("answering GetOrSeed for the same seed cost no second walk"),
		Cache.GetWalksForTest(), 1);

	// Seeded from a taxiway (never a runway), the cache's OrSeed answer must fall back to
	// just the seed, exactly as the free function does - RunwayChainOrSeed's own contract.
	const TArray<FRoadSegmentId>& TaxiwayOrSeed = Cache.GetOrSeed(*Net, Tx);
	TestTrue(TEXT("cached OrSeed on a taxiway falls back to the seed alone"),
		TaxiwayOrSeed == RunwayQuery::RunwayChainOrSeed(*Net, Tx));
	TestEqual(TEXT("and that seed is the one asked for"), TaxiwayOrSeed.Num(), 1);
	TestTrue(TEXT("holding the taxiway itself"), TaxiwayOrSeed.Contains(Tx));
	return true;
}

#endif
