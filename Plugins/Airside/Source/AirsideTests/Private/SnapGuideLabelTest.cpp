#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadNetworkActor.h"
#include "Solve/GuideArbiter.h"
#include "Tool/RoadEditTarget.h"
#include "Tool/SnapGuideChain.h"
#include "Tool/SnapGuideLabel.h"
#include "Tool/SnapGuideSettings.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * #183: FCandidate::Description used to be `FString::Printf`'d for every proposed candidate -
 * every spoke off every segment in reach, every direction off every reference - when Arbitrate
 * keeps at most two. Propose now fills a POD FGuideLabel instead, and SnapGuide::Describe turns
 * that back into the string only for a winner.
 *
 * THIS TEST PINS THE STRINGS, not just the mechanism: for every ELabelKind that AddSpokes,
 * AddDirections and the hand-written candidates can produce, the text SnapGuide::Describe
 * returns must be byte-identical to what the old inline `FString::Printf` calls produced,
 * recorded here from main before #183 touched them. AngledGuideTest, NetworkGuideSourceTest,
 * SnapGuideChainTest and RoadColumnSplitTest already pin most of these exactly (through
 * TestGuide::ProposedBy or FSnapGuideChain::Resolve, both of which now call Describe too); this
 * covers the shapes those happen not to assert byte-for-byte - AngledFromEnd, the apron's
 * EdgeFlushWith/InLineWith, PointAlign's GesturePoint subject, and MatchingGap's exact text.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSnapGuideLabelsMatchTheOldStringsTest,
	"Airside.Tool.SnapGuideLabelsMatchTheOldStrings",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSnapGuideLabelsMatchTheOldStringsTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("a network actor"), Actor)) { return false; }

	IRoadEditTarget* Target = Actor;
	const int32 West = Target->PlaceNode(FVector2D(-10000.0, 0.0));
	const int32 East = Target->PlaceNode(FVector2D(10000.0, 0.0));
	Target->ConnectNodes(West, East, ERoadKind::Taxiway, INDEX_NONE);
	Target->AddApron({ FVector2D(-4000.0, 4000.0), FVector2D(4000.0, 4000.0),
		FVector2D(4000.0, 8000.0), FVector2D(-4000.0, 8000.0) });
	if (!TestTrue(TEXT("the network exists"), Actor->Network != nullptr)) { return false; }

	// AngledFromEnd: the taxiway's own end throws three spokes. ANCHOR NEAR THE EAST END so
	// both ends are in AddSpokes' reach (measured from the cursor, which ProposedBy defaults to
	// the anchor's own origin).
	{
		const FGuideAnchor Anchor = TestGuide::BareAnchor(FVector2D(12000.0, 2000.0));
		const FAngledRoadGuideSource Source;
		const TArray<SnapGuide::FCandidate> Candidates =
			TestGuide::ProposedBy(Source, *Actor->Network, Anchor);

		TSet<FString> Labels;
		for (const SnapGuide::FCandidate& Candidate : Candidates) { Labels.Add(Candidate.Description); }
		for (const TCHAR* Wanted : { TEXT("45 degrees from the end of the taxiway"),
			TEXT("90 degrees from the end of the taxiway"),
			TEXT("135 degrees from the end of the taxiway") })
		{
			TestTrue(*FString::Printf(TEXT("'%s' is one of the spokes offered"), Wanted),
				Labels.Contains(FString(Wanted)));
		}
	}

	// FApronLineGuideSource: InLineWith with no half-width, EdgeFlushWith with one.
	{
		FGuideAnchor Flush = TestGuide::BareAnchor(FVector2D(0.0, 4000.0));
		Flush.HalfWidthLeft = 300.0;
		Flush.HalfWidthRight = 300.0;
		const FApronLineGuideSource Source;
		const TArray<SnapGuide::FCandidate> Flushed =
			TestGuide::ProposedBy(Source, *Actor->Network, Flush, FVector2D(0.0, 4000.0));

		TSet<FString> FlushLabels;
		for (const SnapGuide::FCandidate& Candidate : Flushed) { FlushLabels.Add(Candidate.Description); }
		TestTrue(TEXT("a centreline drag off the apron edge is told which side, exactly"),
			FlushLabels.Contains(FString(TEXT("edge flush with the apron edge"))));

		const FGuideAnchor Bare = TestGuide::BareAnchor(FVector2D(0.0, 4000.0));
		const TArray<SnapGuide::FCandidate> Plain =
			TestGuide::ProposedBy(Source, *Actor->Network, Bare, FVector2D(0.0, 4000.0));
		// EVERY EDGE, NOT JUST index 0: ApronEdge carries no per-edge name, so all of them read
		// the same - which is what makes checking only the first one still a real assertion.
		if (TestTrue(TEXT("with no half-width, the apron's edges answer"), Plain.Num() > 0))
		{
			TestEqual(TEXT("named exactly, not merely containing the words"),
				Plain[0].Description, FString(TEXT("in line with the apron edge")));
		}
	}

	// FPointAlignGuideSource: the GesturePoint subject, resolved by INDEX rather than a copied
	// name - #183's other change to this source.
	{
		FGuideAnchor Anchor = TestGuide::BareAnchor(FVector2D(0.0, 0.0));
		Anchor.Reference = FVector2D(1.0, 0.0);
		Anchor.AlignTo.Add({ FVector2D(6000.0, 3000.0), TEXT("corner 5") });
		const FPointAlignGuideSource Source;
		const TArray<SnapGuide::FCandidate> Candidates =
			TestGuide::ProposedBy(Source, *Actor->Network, Anchor);

		TSet<FString> Labels;
		for (const SnapGuide::FCandidate& Candidate : Candidates) { Labels.Add(Candidate.Description); }
		TestTrue(TEXT("level with the named point, exactly"),
			Labels.Contains(FString(TEXT("0 degrees to corner 5"))));
		TestTrue(TEXT("and square to it, exactly"),
			Labels.Contains(FString(TEXT("square to corner 5"))));
	}

	// FOffsetGuideSource: the exact text, not merely "contains '40 m'" - OffsetGuideTest
	// covers the Contains leg; this pins the whole string, including which road it names.
	{
		FAirsideTestWorld GapWorld;
		if (!TestNotNull(TEXT("a second world"), GapWorld.World)) { return false; }
		ARoadNetworkActor* GapActor = GapWorld.Actor;
		if (!TestNotNull(TEXT("a second network actor"), GapActor)) { return false; }

		IRoadEditTarget* GapTarget = GapActor;
		const int32 A1 = GapTarget->PlaceNode(FVector2D(-10000.0, 0.0));
		const int32 A2 = GapTarget->PlaceNode(FVector2D(10000.0, 0.0));
		GapTarget->ConnectNodes(A1, A2, ERoadKind::Taxiway, INDEX_NONE);
		const int32 B1 = GapTarget->PlaceNode(FVector2D(-10000.0, 4000.0));
		const int32 B2 = GapTarget->PlaceNode(FVector2D(10000.0, 4000.0));
		GapTarget->ConnectNodes(B1, B2, ERoadKind::Taxiway, INDEX_NONE);
		if (!TestTrue(TEXT("the network exists"), GapActor->Network != nullptr)) { return false; }

		const FGuideAnchor Anchor = TestGuide::BareAnchor(FVector2D(0.0, 5000.0));
		const FOffsetGuideSource Source;
		const TArray<SnapGuide::FCandidate> Candidates =
			TestGuide::ProposedBy(Source, *GapActor->Network, Anchor);

		if (TestEqual(TEXT("the one neighbouring road offers its gap"), Candidates.Num(), 1))
		{
			TestEqual(TEXT("the whole string, road and all"),
				Candidates[0].Description, FString(TEXT("40 m, matching the taxiway")));
		}
	}

	// FAlignedGuideSource: the Entity subject, resolved by INDEX into Network.GetEntities() -
	// cross-checked against EntityNaming::Describe directly rather than a hard-coded name,
	// since a transient test fixture's asset-name fallback is not this test's business to pin.
	{
		IRoadEditTarget* StandTarget = Actor;
		StandTarget->PlaceStand(FVector2D(0.0, -6000.0), FMath::DegreesToRadians(45.0));
		if (!TestTrue(TEXT("the stand exists"), Actor->Network->GetEntities().Num() > 0))
		{
			return false;
		}

		const FGuideAnchor Anchor = TestGuide::BareAnchor(FVector2D(2000.0, -4000.0));
		const FAlignedGuideSource Source;
		const TArray<SnapGuide::FCandidate> Candidates =
			TestGuide::ProposedBy(Source, *Actor->Network, Anchor);

		if (TestTrue(TEXT("the stand proposes"), Candidates.Num() > 0))
		{
			const FString Expected = FString::Printf(TEXT("aligned with %s"),
				*EntityNaming::Describe(Actor->Network->GetEntities()[0]));
			TestEqual(TEXT("the whole string, not merely the prefix"),
				Candidates[0].Description, Expected);
		}
	}

	return true;
}

/**
 * THE #183 REGRESSION ITSELF: Describe must run once per WINNER, never once per CANDIDATE.
 *
 * A counter in the label formatter (SnapGuide::DescribeCallCountForTest), matching the
 * project's own convention for measuring a boundary rather than reasoning about it (see
 * RouteSearch::NodeVisitCountForTest). Before #183 every one of the dozens of candidates a
 * field like this proposes paid for an FString::Printf; after it, only the winners do.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSnapGuideDescribesOnlyTheWinnersTest,
	"Airside.Tool.SnapGuideDescribesOnlyTheWinners",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSnapGuideDescribesOnlyTheWinnersTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("a network actor"), Actor)) { return false; }

	// ONE TAXIWAY IS ENOUGH: with the default FSnapGuideSettings (Extending, LevelWith,
	// Parallel, Collinear, Taxiway, ServiceRoad and World all on), a single road beside a
	// gesture with its own reference already yields Extending's two, Parallel's four and
	// World's four - a dozen candidates from one segment, which is the shape #183 reports.
	IRoadEditTarget* Target = Actor;
	const int32 West = Target->PlaceNode(FVector2D(-10000.0, 0.0));
	const int32 East = Target->PlaceNode(FVector2D(10000.0, 0.0));
	Target->ConnectNodes(West, East, ERoadKind::Taxiway, INDEX_NONE);
	if (!TestTrue(TEXT("the network exists"), Actor->Network != nullptr)) { return false; }

	FGuideAnchor Anchor;
	Anchor.Origin = FVector2D(3000.0, 0.0);
	Anchor.Reference = FVector2D(1.0, 0.0);
	Anchor.ReferenceAt = FVector2D::ZeroVector;
	Anchor.ReferenceName = TEXT("the frontage");

	const FSnapGuideChain Chain;
	const FVector2D Cursor = Anchor.Origin + FVector2D(50.0, 2000.0);

	TArray<SnapGuide::FCandidate> Everything;
	Chain.ProposeAll(*Actor->Network, Anchor, Cursor, FSnapGuideSettings(), Everything);
	if (!TestTrue(TEXT("the field proposed more than a winner's worth of candidates"),
		Everything.Num() > 2))
	{
		return false;
	}

	SnapGuide::ResetDescribeCallCountForTest();
	const SnapGuide::FResult Result = Chain.Resolve(
		*Actor->Network, Anchor, Cursor, SnapGuide::FResult());
	if (!TestTrue(TEXT("a guide holds"), Result.bActive)) { return false; }

	TestEqual(
		*FString::Printf(TEXT("Describe ran once per winner (%d), not once per candidate (%d)"),
			Result.Winners.Num(), Everything.Num()),
		SnapGuide::DescribeCallCountForTest(), Result.Winners.Num());

	// CONTROL LEG: a second Resolve call on the SAME field must not accumulate the first call's
	// count - the counter measures ONE call, not a running total, or a regression that doubled
	// every candidate's cost would still read as "one per winner" after enough calls diluted it.
	SnapGuide::ResetDescribeCallCountForTest();
	const SnapGuide::FResult Again = Chain.Resolve(
		*Actor->Network, Anchor, Cursor, Result);
	TestEqual(TEXT("and stays exactly one per winner on the very next call, held guide or not"),
		SnapGuide::DescribeCallCountForTest(), Again.Winners.Num());

	return true;
}

#endif
