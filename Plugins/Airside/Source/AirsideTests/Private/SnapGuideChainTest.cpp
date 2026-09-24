#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadNetworkActor.h"
#include "Tool/RoadEditTarget.h"
#include "Tool/SnapGuideChain.h"
#include "Tool/SnapGuideSettings.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/**
	 * An EMPTY network, because none of the three sources installed so far reads one -
	 * Extending and PointAlign are handed their geometry by the tool, and World is absolute.
	 * A network with roads in it would suggest these sources consult it, which is exactly
	 * what stage 2 changes.
	 */
	URoadNetwork* EmptyNetwork()
	{
		return NewObject<URoadNetwork>(GetTransientPackage());
	}

	/** A plot's back corner: swinging around the far end of an east-west frontage. */
	FGuideAnchor Frontage()
	{
		FGuideAnchor Anchor;
		Anchor.Origin = FVector2D(3000.0, 0.0);
		Anchor.Reference = FVector2D(1.0, 0.0);
		Anchor.ReferenceAt = FVector2D::ZeroVector;
		Anchor.ReferenceName = TEXT("the frontage");
		return Anchor;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGuideChainProposesTheFrontageAndItsPerpendicularTest,
	"Airside.Tool.GuideChainProposesTheFrontageAndItsPerpendicular",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGuideChainProposesTheFrontageAndItsPerpendicularTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Network = EmptyNetwork();
	if (!TestNotNull(TEXT("a network"), Network)) { return false; }

	const FSnapGuideChain Chain;
	// THE NUMBER MOVES WITH EVERY SOURCE ADDED, and that is exactly why it is asserted: a
	// source written, declared and never installed in the constructor would be invisible
	// otherwise - its candidates simply never appear, and every other test of the chain still
	// passes. Stage 2 takes this from 3 to 7, one at a time, and stage 5 to 8.
	//
	// FIFTEEN SINCE 2026-09-20, and the number climbs in GROUPS because the chain skips a source
	// by its declared Relation() before it walks anything - so a column needs one source per
	// relation it serves, not one source that declares several. FRunwayLineGuideSource split off
	// FRunwayGuideSource for exactly that reason; AngledFrom then arrived as road and runway
	// separately; and the Apron column arrived as four at once.
	//
	// NINETEEN SINCE 2026-09-24: the same four again, each registered a second time over plotted
	// stands' and depots' outlines (EGuideOutlines::Plots) - see FOutlineGuideSource.
	TestEqual(TEXT("the chain installs every source it declares"), Chain.NumSources(), 19);

	const FGuideAnchor Anchor = Frontage();

	// A CORNER DRAGGED ALMOST SQUARE: 2000 uu out and 50 uu along, which is about 1.4
	// degrees off the perpendicular - inside the 7-degree tolerance.
	const FVector2D Cursor = Anchor.Origin + FVector2D(50.0, 2000.0);

	const SnapGuide::FResult Result = Chain.Resolve(
		*Network, Anchor, Cursor, SnapGuide::FResult());

	if (!TestTrue(TEXT("a corner dragged near square is offered a guide"), Result.bActive))
	{
		return false;
	}

	TestEqual(TEXT("the perpendicular of the tool's own reference is what wins"),
		static_cast<int32>(Result.Winners[0].Relation), static_cast<int32>(SnapGuide::ERelation::Extending));
	TestEqual(TEXT("and it is described by the name the TOOL gave its reference"),
		Result.Winners[0].Description, FString(TEXT("square to the frontage")));

	// THE DASHED LINE POINTS AT THE EDGE, not along the guide - design section 6. This is the
	// field the overlay draws to, so an anchor whose ReferenceAt did not travel would draw a
	// line to the world origin and look like a bug in the gesture.
	TestTrue(TEXT("the reference point the tool supplied travels to the winner"),
		Result.Winners[0].ReferenceAt.Equals(Anchor.ReferenceAt, 1.0e-6));

	// EXACTLY SQUARE, not nearly: the constrained point is what the corner becomes.
	TestTrue(TEXT("the constrained point is exactly square to the frontage"),
		FMath::IsNearlyEqual(Result.Point.X, Anchor.Origin.X, 1.0e-6));

	// AND THE PARALLEL IS PROPOSED TOO. A corner dragged back along the frontage gets the
	// other of Extending's two candidates - without this leg the source could be proposing
	// one direction and the test above would not notice.
	const SnapGuide::FResult AlongIt = Chain.Resolve(
		*Network, Anchor, Anchor.Origin + FVector2D(2000.0, 30.0), SnapGuide::FResult());
	TestEqual(TEXT("dragging along the frontage gets the frontage's own direction"),
		AlongIt.Winners[0].Description, FString(TEXT("along the frontage")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGuideChainPrefersTheFrontageOverTheWorldGridTest,
	"Airside.Tool.GuideChainPrefersTheFrontageOverTheWorldGrid",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGuideChainPrefersTheFrontageOverTheWorldGridTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Network = EmptyNetwork();
	if (!TestNotNull(TEXT("a network"), Network)) { return false; }

	const FSnapGuideChain Chain;

	// A FRONTAGE ON A WORLD AXIS - every plot off an east-west service road. Extending
	// proposes 0 and 90; World proposes 0, 45, 90, 135. The 90s tie exactly.
	FGuideAnchor Anchor = Frontage();
	const FVector2D Cursor = Anchor.Origin + FVector2D(60.0, 2000.0);

	const SnapGuide::FResult Result = Chain.Resolve(
		*Network, Anchor, Cursor, SnapGuide::FResult());
	TestEqual(TEXT("a tie between the frontage and a world axis goes to the frontage"),
		static_cast<int32>(Result.Winners[0].Relation), static_cast<int32>(SnapGuide::ERelation::Extending));

	// CONTROL LEG: World was a live competitor, not an absent one. With no reference the
	// Extending source proposes nothing and the same cursor gets the world axis instead - so
	// the assertion above is measuring the tiebreak rather than an empty list.
	Anchor.Reference = FVector2D::ZeroVector;
	const SnapGuide::FResult WorldOnly = Chain.Resolve(
		*Network, Anchor, Cursor, SnapGuide::FResult());
	TestTrue(TEXT("with no reference the world grid still answers"), WorldOnly.bActive);
	TestEqual(TEXT("and it is the world axis that does"),
		static_cast<int32>(WorldOnly.Winners[0].Reference), static_cast<int32>(SnapGuide::EReference::World));
	// NAMED AS AN AXIS, BOTH ENDS, since the grid has nothing on the map to point at. It read
	// "90 degrees" until 2026-09-20 - a number that was already east's compass bearing, but
	// said so nowhere, and sat beside "45 degrees to the taxiway", which is measured from that
	// road rather than from north.
	TestEqual(TEXT("named by the axis it lies on, since the grid has nothing to point at"),
		WorldOnly.Winners[0].Description, FString(TEXT("east-west")));
	TestTrue(TEXT("and its line points back at the corner it swings around"),
		WorldOnly.Winners[0].ReferenceAt.Equals(Anchor.Origin, 1.0e-6));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGuideChainOffersNothingBetweenCandidatesTest,
	"Airside.Tool.GuideChainOffersNothingBetweenCandidates",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGuideChainOffersNothingBetweenCandidatesTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Network = EmptyNetwork();
	if (!TestNotNull(TEXT("a network"), Network)) { return false; }

	const FSnapGuideChain Chain;

	FGuideAnchor Anchor = Frontage();
	Anchor.Reference = FVector2D::ZeroVector;   // world axes only, 45 degrees apart

	// 22 degrees off +X: 22 from one axis, 23 from the next, both past the 7-degree
	// tolerance. THE GUIDE MUST BE OFF MOST OF THE TIME, or it is a constraint the player
	// never asked for rather than an aid.
	const double Radians = FMath::DegreesToRadians(22.0);
	const FVector2D Cursor = Anchor.Origin
		+ FVector2D(FMath::Cos(Radians), FMath::Sin(Radians)) * 2000.0;

	const SnapGuide::FResult Result = Chain.Resolve(
		*Network, Anchor, Cursor, SnapGuide::FResult());
	TestFalse(TEXT("a cursor between two world axes is offered neither"), Result.bActive);

	return true;
}

/**
 * #192 item 1: SnapGuideChain.cpp's eight per-segment walks became one FSegmentGuideSource base
 * plus a per-segment emit hook. This pins the candidate COUNT the six segment-walking sources
 * produce for a fixture with one taxiway and one runway, so a refactor that changes what any of
 * them emits - not just whether they compile - fails loudly rather than quietly.
 *
 * ONLY THOSE SIX RELATIONS/COLUMNS ARE ENABLED, so the total is the six sources' own arithmetic
 * and nothing else's: Parallel x (Taxiway, Runway) is FParallelGuideSource (nearest taxiway, 4
 * candidates) plus FRunwayGuideSource (every runway, 4); Collinear x (Taxiway, Runway) is
 * FCollinearGuideSource (1) plus FRunwayLineGuideSource (1, unbounded); AngledFrom x (Taxiway,
 * Runway) is FAngledRoadGuideSource (both ends x 3 spokes = 6) plus FAngledRunwayGuideSource (6,
 * unbounded). 8 + 2 + 12 = 22. ServiceRoad/Apron/Stand/World are switched off, and there are no
 * aprons or entities in the fixture, so the sources that also answer to Parallel/Collinear/
 * AngledFrom (World, Apron x3, Aligned) contribute nothing to add or hide from that number.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSegmentWalkersProposeTheSameCandidateCountTest,
	"Airside.Tool.SegmentWalkersProposeTheSameCandidateCount",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSegmentWalkersProposeTheSameCandidateCountTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("a network actor"), Actor)) { return false; }

	if (!TestTrue(TEXT("the runway is laid"),
		TestGuide::LayRunway(Actor, FVector2D(-5000.0, 8000.0), FVector2D(5000.0, 8000.0))))
	{
		return false;
	}
	IRoadEditTarget* Target = Actor;
	const int32 West = Target->PlaceNode(FVector2D(-5000.0, 0.0));
	const int32 East = Target->PlaceNode(FVector2D(5000.0, 0.0));
	Target->ConnectNodes(West, East, ERoadKind::Taxiway, INDEX_NONE);
	if (!TestTrue(TEXT("the network exists"), Actor->Network != nullptr)) { return false; }

	FGuideAnchor Anchor;
	Anchor.Origin = FVector2D::ZeroVector;
	const FVector2D Cursor(0.0, 3000.0);

	FSnapGuideSettings Settings;
	Settings.bExtending = false;
	Settings.bLevelWith = false;
	Settings.bParallel = true;
	Settings.bCollinear = true;
	Settings.bAngledFrom = true;
	Settings.bMatchingGap = false;
	Settings.bTaxiway = true;
	Settings.bServiceRoad = false;
	Settings.bRunway = true;
	Settings.bApron = false;
	Settings.bStand = false;
	Settings.bWorld = false;

	const FSnapGuideChain Chain;
	TArray<SnapGuide::FCandidate> Candidates;
	Chain.ProposeAll(*Actor->Network, Anchor, Cursor, Settings, Candidates);

	int32 Parallel = 0;
	int32 Collinear = 0;
	int32 AngledFrom = 0;
	for (const SnapGuide::FCandidate& Candidate : Candidates)
	{
		switch (Candidate.Relation)
		{
		case SnapGuide::ERelation::Parallel:   ++Parallel; break;
		case SnapGuide::ERelation::Collinear:  ++Collinear; break;
		case SnapGuide::ERelation::AngledFrom: ++AngledFrom; break;
		default: break;
		}
	}

	TestEqual(TEXT("Parallel: FParallelGuideSource's 4 plus FRunwayGuideSource's 4"), Parallel, 8);
	TestEqual(TEXT("Collinear: FCollinearGuideSource's 1 plus FRunwayLineGuideSource's 1"),
		Collinear, 2);
	TestEqual(TEXT("AngledFrom: FAngledRoadGuideSource's 6 plus FAngledRunwayGuideSource's 6"),
		AngledFrom, 12);
	TestEqual(TEXT("the six segment walkers propose 22 candidates between them"),
		Candidates.Num(), 22);

	return true;
}

/**
 * #192 item 2: IGuideSource::Propose now takes the FTuning FSnapGuideChain::Resolve was handed,
 * instead of every network source reading a fresh default SnapGuide::FTuning() for its own
 * reach test. Shrinking SearchRadiusUu below a segment's distance must drop it from the
 * candidates - which is false on main, where the Tuning argument to ProposeAll/Resolve changed
 * nothing any source actually measured against.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGuideSourceReachIsThreadedFromTuningTest,
	"Airside.Tool.GuideSourceReachIsThreadedFromTuning",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGuideSourceReachIsThreadedFromTuningTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("a network actor"), Actor)) { return false; }

	// THE NEAR END SITS 3000uu FROM THE ORIGIN - inside the default SearchRadiusUu (10000) and
	// outside a tightened one (2000), so the same fixture answers both halves of the comparison.
	// FParallelGuideSource measures from the closest point on the segment, which clamps to this
	// end since the origin is off the segment's far side.
	IRoadEditTarget* Target = Actor;
	const int32 West = Target->PlaceNode(FVector2D(3000.0, 0.0));
	const int32 East = Target->PlaceNode(FVector2D(13000.0, 0.0));
	Target->ConnectNodes(West, East, ERoadKind::Taxiway, INDEX_NONE);
	if (!TestTrue(TEXT("the network exists"), Actor->Network != nullptr)) { return false; }

	FGuideAnchor Anchor;
	Anchor.Origin = FVector2D::ZeroVector;

	FSnapGuideSettings Settings;
	Settings.bExtending = false;
	Settings.bLevelWith = false;
	Settings.bParallel = true;
	Settings.bCollinear = false;
	Settings.bAngledFrom = false;
	Settings.bMatchingGap = false;
	Settings.bTaxiway = true;
	Settings.bServiceRoad = false;
	Settings.bRunway = false;
	Settings.bApron = false;
	Settings.bStand = false;
	Settings.bWorld = false;

	const FSnapGuideChain Chain;

	TArray<SnapGuide::FCandidate> Default;
	Chain.ProposeAll(*Actor->Network, Anchor, Anchor.Origin, Settings, Default);
	TestEqual(TEXT("the default 10000uu reach finds the taxiway 3000uu away"), Default.Num(), 4);

	SnapGuide::FTuning Tight;
	Tight.SearchRadiusUu = 2000.0;
	TArray<SnapGuide::FCandidate> Shrunk;
	Chain.ProposeAll(*Actor->Network, Anchor, Anchor.Origin, Settings, Shrunk, Tight);
	TestEqual(TEXT("a 2000uu reach does not - the source measured against IT, not a fresh default"),
		Shrunk.Num(), 0);

	return true;
}

#endif
