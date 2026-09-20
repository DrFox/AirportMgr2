#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadNetworkActor.h"
#include "Solve/GuideArbiter.h"
#include "Tool/RoadEditTarget.h"
#include "Tool/SnapGuideChain.h"
#include "Tool/SnapGuideSettings.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	void LayTaxiway(ARoadNetworkActor* Actor, const FVector2D& From, const FVector2D& To)
	{
		IRoadEditTarget* Target = Actor;
		const int32 A = Target->PlaceNode(From);
		const int32 B = Target->PlaceNode(To);
		Target->ConnectNodes(A, B, ERoadKind::Taxiway, INDEX_NONE);
	}

	FGuideAnchor AnchorAt(const FVector2D& Origin)
	{
		FGuideAnchor Anchor;
		Anchor.Origin = Origin;
		return Anchor;
	}
}

/**
 * TWO ROADS 40 m APART OFFER A THIRD AT 40 m. That is the whole of Offset: a field laid out by
 * hand comes out regular because the spacing already there is what is proposed.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOffsetGuideMatchesTheExistingGapTest,
	"Airside.Tool.OffsetGuideMatchesTheExistingGap",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FOffsetGuideMatchesTheExistingGapTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("a network actor"), Actor)) { return false; }

	// Two east-west taxiways, 4000 uu (40 m) apart.
	LayTaxiway(Actor, FVector2D(-10000.0, 0.0), FVector2D(10000.0, 0.0));
	LayTaxiway(Actor, FVector2D(-10000.0, 4000.0), FVector2D(10000.0, 4000.0));
	if (!TestTrue(TEXT("the network exists"), Actor->Network != nullptr)) { return false; }

	// A drag just north of the SECOND road: its nearest road is the one at y = 4000, and the
	// neighbour 4000 below it is the gap to copy.
	const FOffsetGuideSource Source;
	TArray<SnapGuide::FCandidate> Candidates;
	Source.Propose(*Actor->Network, AnchorAt(FVector2D(0.0, 5000.0)),
		FVector2D(0.0, 5000.0), Candidates);

	if (!TestEqual(TEXT("the one neighbouring road offers its gap"), Candidates.Num(), 1))
	{
		return false;
	}

	TestEqual(TEXT("perpendicular, because it is a line the new road must sit on"),
		static_cast<int32>(Candidates[0].Fit),
		static_cast<int32>(SnapGuide::EFit::Perpendicular));
	TestTrue(TEXT("running parallel to the road it is measured from"),
		FMath::IsNearlyZero(Candidates[0].Direction.Y, 1.0e-6));

	// AWAY FROM THE NEIGHBOUR: 4000 above the reference at y = 4000, not below it where the
	// neighbour already is.
	TestTrue(TEXT("the line sits one gap beyond the reference, away from the neighbour"),
		FMath::IsNearlyEqual(Candidates[0].Through.Y, 8000.0, 1.0));

	// THE NUMBER IS IN THE LABEL, or the player cannot tell 40 m from 45 m.
	TestTrue(*FString::Printf(TEXT("the label carries the gap, got '%s'"),
		*Candidates[0].Description),
		Candidates[0].Description.Contains(TEXT("40 m")));

	// AND IT PULLS A NEAR CURSOR EXACTLY ONTO THE SPACING.
	const SnapGuide::FResult Result = SnapGuide::Arbitrate(
		Candidates, FVector2D(0.0, 5000.0), FVector2D(3000.0, 7900.0), SnapGuide::FResult());
	TestTrue(TEXT("a drag within tolerance of the matching gap is offered it"), Result.bActive);
	TestTrue(TEXT("and lands exactly on it"),
		FMath::IsNearlyEqual(Result.Point.Y, 8000.0, 1.0e-6));

	return true;
}

/**
 * A ROAD THAT CROSSES THE REFERENCE HAS NO GAP TO COPY. The distance between two crossing roads
 * depends where you measure, so there is no number to offer - and offering one anyway would be
 * a figure the player could not account for.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOffsetGuideIgnoresACrossingRoadTest,
	"Airside.Tool.OffsetGuideIgnoresACrossingRoad",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FOffsetGuideIgnoresACrossingRoadTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("a network actor"), Actor)) { return false; }

	// One east-west reference, and one road crossing it at 45 degrees.
	//
	// THE CROSSING IS DIAGONAL, AND AWAY FROM WHERE THE GAP IS MEASURED. A north-south road
	// through x = 2000 also crosses, but its nearest point to the reference's measuring point
	// lies ON the reference - a perpendicular gap of exactly zero - so the near-zero guard
	// rejected it and this test passed with the parallel filter deleted. It was measuring the
	// wrong rule, which only a deliberate mutation showed. This road's nearest point is
	// (1000, -1000): a gap of 1000, so nothing but the parallel test can reject it.
	LayTaxiway(Actor, FVector2D(-10000.0, 0.0), FVector2D(10000.0, 0.0));
	LayTaxiway(Actor, FVector2D(0.0, -2000.0), FVector2D(4000.0, 2000.0));
	if (!TestTrue(TEXT("the network exists"), Actor->Network != nullptr)) { return false; }

	const FOffsetGuideSource Source;
	TArray<SnapGuide::FCandidate> Crossing;
	Source.Propose(*Actor->Network, AnchorAt(FVector2D(0.0, 1500.0)),
		FVector2D(0.0, 1500.0), Crossing);
	TestEqual(TEXT("a crossing road offers no gap"), Crossing.Num(), 0);

	// CONTROL LEG: add a PARALLEL neighbour and a drag beside the pair does get an offer - so
	// the assertion above is measuring the parallel test, not an empty search.
	//
	// THE DRAG SITS OUTSIDE THE PAIR, at y = 3500, rather than between them at y = 1500. Not
	// fussiness: at 1500 the two roads are EQUIDISTANT, so which one becomes the reference is
	// decided by the network's iteration order, and the leg would be asserting against an
	// accident. At 3500 the reference is the road at 3000 and there is one answer.
	LayTaxiway(Actor, FVector2D(-10000.0, 3000.0), FVector2D(10000.0, 3000.0));
	TArray<SnapGuide::FCandidate> WithNeighbour;
	Source.Propose(*Actor->Network, AnchorAt(FVector2D(0.0, 3500.0)),
		FVector2D(0.0, 3500.0), WithNeighbour);
	if (!TestEqual(TEXT("but a parallel one does"), WithNeighbour.Num(), 1))
	{
		return false;
	}
	TestTrue(TEXT("one 3000 gap beyond the reference, away from the neighbour"),
		FMath::IsNearlyEqual(WithNeighbour[0].Through.Y, 6000.0, 1.0));

	return true;
}

/**
 * A DRAG BETWEEN THE PAIR IS NEVER OFFERED THE NEIGHBOUR'S OWN LINE.
 *
 * The first draft of this source put the line on the CURSOR's side of the reference, which is
 * the same side as the NEIGHBOUR whenever the drag starts between the two - so the gap it
 * offered to keep was the one already occupied, and the guide proposed drawing a road on top of
 * an existing one. Offsetting away from the neighbour instead is what this pins.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOffsetGuideNeverProposesTheNeighboursLineTest,
	"Airside.Tool.OffsetGuideNeverProposesTheNeighboursLine",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FOffsetGuideNeverProposesTheNeighboursLineTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("a network actor"), Actor)) { return false; }

	LayTaxiway(Actor, FVector2D(-10000.0, 0.0), FVector2D(10000.0, 0.0));
	LayTaxiway(Actor, FVector2D(-10000.0, 3000.0), FVector2D(10000.0, 3000.0));
	if (!TestTrue(TEXT("the network exists"), Actor->Network != nullptr)) { return false; }

	// BETWEEN THEM, and nearer the northern one, so the reference is unambiguous and the
	// neighbour lies on the same side as the drag.
	const FOffsetGuideSource Source;
	TArray<SnapGuide::FCandidate> Candidates;
	Source.Propose(*Actor->Network, AnchorAt(FVector2D(0.0, 2000.0)),
		FVector2D(0.0, 2000.0), Candidates);

	for (const SnapGuide::FCandidate& Candidate : Candidates)
	{
		TestTrue(*FString::Printf(
			TEXT("no guide is offered along an existing road, got y = %.1f"),
			Candidate.Through.Y),
			!FMath::IsNearlyEqual(Candidate.Through.Y, 0.0, 1.0)
				&& !FMath::IsNearlyEqual(Candidate.Through.Y, 3000.0, 1.0));
	}

	// AND THE CURSOR IS OFFERED NOTHING while it is in there: the only line on the drag's own
	// side is the neighbour's, and that gap is taken.
	const SnapGuide::FResult Result = SnapGuide::Arbitrate(
		Candidates, FVector2D(0.0, 2000.0), FVector2D(2000.0, 2050.0), SnapGuide::FResult());
	TestFalse(TEXT("a drag between two roads is offered no gap to match"), Result.bActive);

	return true;
}

/**
 * A LONG ROAD STARTS FAR AWAY, AND THE GAP IT IS AIMING AT IS UNDER THE CURSOR.
 *
 * Reported from PIE 2026-09-20 with a diagram (samples/matching1.png): "if you build a road that
 * had a matching gap suggestion, the next road you try to build has to have a SHORTER segment
 * than the previous one otherwise it wont get the suggestion".
 *
 * LENGTH IS THE SYMPTOM, NOT THE CAUSE. FOffsetGuideSource picks its reference as the nearest
 * segment to the ANCHOR'S ORIGIN - the player's FIRST click - and applies SearchRadiusUu from
 * there. A longer road starts further from the pair it is being matched against, so the search
 * finds nothing and the source returns before proposing anything at all. The cursor, which is
 * right beside the roads in question, is never consulted: Propose is not given it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOffsetGuideReachesWhatTheCursorIsNearTest,
	"Airside.Tool.OffsetGuideReachesWhatTheCursorIsNear",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FOffsetGuideReachesWhatTheCursorIsNearTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("a network actor"), Actor)) { return false; }

	// The pair to match: two east-west taxiways 4000 uu apart, the reference at y = 0.
	IRoadEditTarget* Target = Actor;
	const int32 NearWest = Target->PlaceNode(FVector2D(-20000.0, 0.0));
	const int32 NearEast = Target->PlaceNode(FVector2D(20000.0, 0.0));
	Target->ConnectNodes(NearWest, NearEast, ERoadKind::Taxiway, INDEX_NONE);
	const int32 FarWest = Target->PlaceNode(FVector2D(-20000.0, -4000.0));
	const int32 FarEast = Target->PlaceNode(FVector2D(20000.0, -4000.0));
	Target->ConnectNodes(FarWest, FarEast, ERoadKind::Taxiway, INDEX_NONE);
	if (!TestTrue(TEXT("the network exists"), Actor->Network != nullptr)) { return false; }

	const FOffsetGuideSource Source;

	// THE SHORT ROAD - road C in the diagram. Its first click is 2000 uu from the reference,
	// well inside the 100 m reach, and it gets the suggestion today.
	FGuideAnchor Close;
	Close.Origin = FVector2D(0.0, 2000.0);

	TArray<SnapGuide::FCandidate> FromClose;
	Source.Propose(*Actor->Network, Close, Close.Origin, FromClose);
	if (!TestTrue(TEXT("a road starting beside the pair is offered their gap"),
		FromClose.Num() > 0))
	{
		return false;
	}

	// THE LONG ROAD - road B. Its first click is 300 m away, outside SearchRadiusUu, but the
	// player is dragging its far end down to exactly the same place road C ended: one gap north
	// of the reference, which is the line they are trying to land on.
	//
	// THE CURSOR IS WHAT THEY ARE AIMING WITH, and the source never sees it.
	FGuideAnchor Far;
	Far.Origin = FVector2D(-30000.0, 30000.0);

	TArray<SnapGuide::FCandidate> FromFar;
	// THE FAR END, one gap north of the reference - the line road C landed on, and the place
	// this drag is being aimed at. The origin is 300 m away and irrelevant to what it is near.
	Source.Propose(*Actor->Network, Far, FVector2D(0.0, 4000.0), FromFar);
	TestTrue(TEXT("and so is a road whose FAR END reaches them, however far off it began"),
		FromFar.Num() > 0);

	return true;
}

#endif
