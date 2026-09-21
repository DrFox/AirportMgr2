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
 * THE GRID IS A LIST, AND THIS IS ITS FIRST CONSUMER. Twenty-five of the forty-two pairs are
 * legal; a hole is a statement, not an omission, so the count is asserted rather than the shape.
 * See the 2026-09-20 guide-grid design section 3 for each hole's reason.
 *
 * IT WAS NINETEEN OF THIRTY-SIX until the Road column became Taxiway and ServiceRoad later that
 * same day. Five cells arrived by that doubling and one by argument: LevelWith x Runway, whose
 * hole had rested on a runway's thresholds being "ordinary nodes already served by Road" - a
 * sentence the split made false. See SnapGuide::IsLegalCell's LevelWith row.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGuideGridDeclaresItsCellsTest,
	"Airside.Solve.GuideGridDeclaresItsCells",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGuideGridDeclaresItsCellsTest::RunTest(const FString& Parameters)
{
	const int32 Relations = static_cast<int32>(SnapGuide::ERelation::MatchingGap) + 1;
	const int32 References = static_cast<int32>(SnapGuide::EReference::World) + 1;
	TestEqual(TEXT("six relations"), Relations, 6);
	TestEqual(TEXT("seven references"), References, 7);

	int32 Legal = 0;
	for (int32 R = 0; R < Relations; ++R)
	{
		for (int32 F = 0; F < References; ++F)
		{
			if (SnapGuide::IsLegalCell(
				static_cast<SnapGuide::ERelation>(R), static_cast<SnapGuide::EReference>(F)))
			{
				++Legal;
			}
		}
	}
	TestEqual(TEXT("twenty-five of the forty-two pairs are legal"), Legal, 25);

	// THE TWO ROAD COLUMNS AGREE, ROW BY ROW. A taxiway and a service road are the same SHAPE
	// on the field - a centreline with pavement either side and ends to radiate from - so
	// every geometric argument that admits one admits the other, and a row that took only one
	// would be a rule about the toggle rather than about the geometry. Walked rather than
	// listed, because the point is that no row may differ.
	for (int32 R = 0; R < Relations; ++R)
	{
		const SnapGuide::ERelation Relation = static_cast<SnapGuide::ERelation>(R);
		TestEqual(
			*FString::Printf(TEXT("relation %d admits a service road exactly as it admits a taxiway"), R),
			SnapGuide::IsLegalCell(Relation, SnapGuide::EReference::ServiceRoad),
			SnapGuide::IsLegalCell(Relation, SnapGuide::EReference::Taxiway));
	}

	// THREE NAMED CELLS, not a re-listing of the table: a test that restated the whole grid
	// would be a second copy of it, and the two would drift. These three are the ones whose
	// reasoning the design argues hardest, so they are the ones worth pinning by name.
	TestTrue(TEXT("Extending is about the shape being drawn"),
		SnapGuide::IsLegalCell(SnapGuide::ERelation::Extending, SnapGuide::EReference::ThisGesture));
	TestFalse(TEXT("and Extending means nothing against a road"),
		SnapGuide::IsLegalCell(SnapGuide::ERelation::Extending, SnapGuide::EReference::Taxiway));
	TestFalse(TEXT("a world axis has no position, so nothing can be in line with it"),
		SnapGuide::IsLegalCell(SnapGuide::ERelation::Collinear, SnapGuide::EReference::World));

	// A RUNWAY'S THRESHOLD IS A POINT TO BE LEVEL WITH, since 2026-09-20. Pinned by name
	// because it is the one cell the Road split OPENED rather than doubled, and the one a
	// later reader would be most tempted to close again on the old reasoning.
	TestTrue(TEXT("a runway threshold is something to be level with"),
		SnapGuide::IsLegalCell(SnapGuide::ERelation::LevelWith, SnapGuide::EReference::Runway));

	// THE ONE HOLE THAT IS NOT ABOUT GEOMETRY, and so the one most likely to be "fixed" by a
	// later reader: LevelWith x ThisGesture already proposes the 0 and 90 degree lines through
	// every pinned corner, off the same reference direction. AngledFrom's 90 degree spoke would
	// be that identical line under a second name.
	TestFalse(TEXT("your own corners are LevelWith's, not AngledFrom's"),
		SnapGuide::IsLegalCell(SnapGuide::ERelation::AngledFrom, SnapGuide::EReference::ThisGesture));
	TestTrue(TEXT("but another road's end does throw spokes"),
		SnapGuide::IsLegalCell(SnapGuide::ERelation::AngledFrom, SnapGuide::EReference::Taxiway));

	return true;
}

/**
 * THE TIEBREAK IS A PAIR, RELATION FIRST. Two candidates equally close must not be separated
 * by the order the network happened to be walked in - that changes with an unrelated edit.
 * A stand's pose losing to the nearest road is the ONE ranking this split changes, and
 * FAlignedGuideSource's own header already argued for it: "the weakest of the four network
 * sources", which its old rank of third-of-eight contradicted.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGuideGridBreaksTiesByRelationThenReferenceTest,
	"Airside.Solve.GuideGridBreaksTiesByRelationThenReference",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGuideGridBreaksTiesByRelationThenReferenceTest::RunTest(const FString& Parameters)
{
	SnapGuide::FCandidate Road;
	Road.Direction = FVector2D(1.0, 0.0);
	Road.Through = FVector2D::ZeroVector;
	Road.Fit = SnapGuide::EFit::Angular;
	Road.Relation = SnapGuide::ERelation::Parallel;
	Road.Reference = SnapGuide::EReference::Taxiway;

	// THE SAME DIRECTION, so the two are exactly tied on error and only the rank can separate
	// them. Listed AFTER the stand, so a first-wins bug shows up as the stand winning.
	SnapGuide::FCandidate Stand = Road;
	Stand.Reference = SnapGuide::EReference::Stand;

	const SnapGuide::FCandidate Candidates[] = { Stand, Road };
	const SnapGuide::FResult Result = SnapGuide::Arbitrate(
		Candidates, FVector2D::ZeroVector, FVector2D(1000.0, 0.0), SnapGuide::FResult());

	if (!TestTrue(TEXT("a guide holds"), Result.bActive)) { return false; }
	TestEqual(TEXT("the nearest road beats a stand's pose on a tie"),
		static_cast<int32>(Result.Winners[0].Reference),
		static_cast<int32>(SnapGuide::EReference::Taxiway));

	// AND RELATION OUTRANKS REFERENCE. Extending x ThisGesture is a worse reference rank than
	// Parallel x Road, and must still win - otherwise the pair is being compared the wrong way
	// round, which a reference-only test could not tell apart.
	SnapGuide::FCandidate Extending = Road;
	Extending.Relation = SnapGuide::ERelation::Extending;
	Extending.Reference = SnapGuide::EReference::ThisGesture;

	const SnapGuide::FCandidate Both[] = { Road, Extending };
	const SnapGuide::FResult Second = SnapGuide::Arbitrate(
		Both, FVector2D::ZeroVector, FVector2D(1000.0, 0.0), SnapGuide::FResult());

	if (!TestTrue(TEXT("a guide holds"), Second.bActive)) { return false; }
	TestEqual(TEXT("what you are extending beats the road you are beside"),
		static_cast<int32>(Second.Winners[0].Relation),
		static_cast<int32>(SnapGuide::ERelation::Extending));

	return true;
}

/**
 * NOTHING MAY PROPOSE A PAIR THE GRID DOES NOT DECLARE.
 *
 * THE TEST THE 2026-09-20 REPORT NEEDED. The old registry test walked the enum against the
 * button list and could never have caught it: Collinear x Runway was firing while nothing
 * declared that cell existed, because there was no notion of a cell. This walks the other way -
 * from what the chain actually produces, back to the list.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGuideGridHasNoCellOutsideTheListTest,
	"Airside.Tool.GuideGridHasNoCellOutsideTheList",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGuideGridHasNoCellOutsideTheListTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("a network actor"), Actor)) { return false; }

	// ONE OF EACH THING A SOURCE CAN LOOK AT, all within reach of one origin, so every source
	// has something to answer with. A field holding only a taxiway would let a source proposing
	// into a hole pass simply for having nothing to propose about.
	//
	// AN APRON TOO, since 2026-09-20 - four sources answer for that column now, and this test is
	// the only thing standing between them and a cell the grid does not declare.
	//
	// NO STAND: placing an entity needs a UEntityDefinition this fixture has no business
	// authoring, so the Stand column stays unexercised here. Said out loud because a test whose
	// coverage is narrower than its name is how a green run comes to mean nothing.
	if (!TestTrue(TEXT("the runway is laid"),
		TestGuide::LayRunway(Actor, FVector2D(-30000.0, 9000.0), FVector2D(30000.0, 9000.0))))
	{
		return false;
	}
	IRoadEditTarget* Target = Actor;
	Target->AddApron({ FVector2D(2000.0, 4000.0), FVector2D(8000.0, 4000.0),
		FVector2D(8000.0, 8000.0), FVector2D(2000.0, 8000.0) });
	const int32 West = Target->PlaceNode(FVector2D(-10000.0, 0.0));
	const int32 East = Target->PlaceNode(FVector2D(10000.0, 0.0));
	Target->ConnectNodes(West, East, ERoadKind::Taxiway, INDEX_NONE);
	const int32 SouthWest = Target->PlaceNode(FVector2D(-10000.0, -6000.0));
	const int32 SouthEast = Target->PlaceNode(FVector2D(10000.0, -6000.0));
	Target->ConnectNodes(SouthWest, SouthEast, ERoadKind::Taxiway, INDEX_NONE);

	// A SERVICE ROAD TOO, since 2026-09-20: with only taxiways on the field the ServiceRoad
	// column could propose nothing, and a source tagging it with a cell the grid rejects would
	// go unseen - the exact shape of failure this test exists to catch, in the newest column.
	const int32 VanWest = Target->PlaceNode(FVector2D(-10000.0, -3000.0));
	const int32 VanEast = Target->PlaceNode(FVector2D(10000.0, -3000.0));
	Target->ConnectNodes(VanWest, VanEast, ERoadKind::ServiceRoad, INDEX_NONE);
	if (!TestTrue(TEXT("the network exists"), Actor->Network != nullptr)) { return false; }

	FGuideAnchor Anchor;
	Anchor.Origin = FVector2D(0.0, 3000.0);
	Anchor.Reference = FVector2D(1.0, 0.0);
	Anchor.ReferenceAt = FVector2D(-4000.0, 3000.0);
	Anchor.ReferenceName = TEXT("this road");
	Anchor.AlignTo.Add({ FVector2D(6000.0, 3000.0), TEXT("that node") });

	// EVERY ROW AND EVERY COLUMN ON, which is the only setting under which a source proposing
	// into a hole is visible at all: with anything switched off, the gate would remove the
	// illegal candidate for the wrong reason and the test would pass while the bug stood.
	FSnapGuideSettings Settings;
	Settings.bExtending = true;
	Settings.bLevelWith = true;
	Settings.bParallel = true;
	Settings.bCollinear = true;
	// ANGLEDFROM WAS MISSING FROM THIS LIST until 2026-09-20, so the spoke sources proposed
	// nothing and this test never saw the cells they tag. It says "every row and every column
	// on"; a row left out is exactly the coverage gap its own comment warns about.
	Settings.bAngledFrom = true;
	Settings.bMatchingGap = true;
	Settings.bTaxiway = true;
	Settings.bServiceRoad = true;
	Settings.bRunway = true;
	Settings.bApron = true;
	Settings.bStand = true;
	Settings.bWorld = true;

	// THE CHAIN'S SOURCES, ASKED WITHOUT ARBITRATION. Resolve returns only the winners, and a
	// winner is at most two candidates - an illegal cell that lost its race would never be seen.
	const FSnapGuideChain Chain;
	TArray<SnapGuide::FCandidate> Everything;
	Chain.ProposeAll(*Actor->Network, Anchor, Anchor.Origin, Settings, Everything);

	if (!TestTrue(TEXT("the sources proposed something to check"), Everything.Num() > 0))
	{
		return false;
	}

	for (const SnapGuide::FCandidate& Candidate : Everything)
	{
		// Candidate.Description ITSELF IS EMPTY - #183 moved its formatting out of ProposeAll and
		// into FSnapGuideChain::Resolve, which this test bypasses on purpose (see the comment
		// above). SnapGuide::Describe is called here only so a failure still names the guide.
		TestTrue(*FString::Printf(TEXT("relation %d against reference %d is a declared cell ('%s')"),
				static_cast<int32>(Candidate.Relation),
				static_cast<int32>(Candidate.Reference),
				*SnapGuide::Describe(*Actor->Network, Anchor, Candidate.Label)),
			SnapGuide::IsLegalCell(Candidate.Relation, Candidate.Reference));
	}

	return true;
}

#endif
