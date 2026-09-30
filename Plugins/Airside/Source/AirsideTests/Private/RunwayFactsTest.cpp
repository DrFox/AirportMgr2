#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/ReverseTurn.h"
#include "Model/RoadApron.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"
#include "Model/RunwayFacts.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRunwayFactsTest,
	"Airside.Model.RunwayFacts",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRunwayFactsTest::RunTest(const FString& Parameters)
{
	// A runway split by a taxiway into two segments. The facts are ONE runway's, so every
	// segment of the chain must carry them - a strip whose halves disagreed would paint one
	// end as concrete and land aircraft on the other as grass.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Runway = TestProfiles::Runway();
	URoadProfile* Taxiway = TestProfiles::Taxiway();

	const FRoadNodeId T = Net->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId E = Net->AddNode(FVector2D(60000.0, 0.0));
	const FRoadNodeId F = Net->AddNode(FVector2D(100000.0, 0.0));
	const FRoadSegmentId RW1 = Net->AddStraightSegment(T, E, Runway);
	const FRoadSegmentId RW2 = Net->AddStraightSegment(E, F, Runway);
	const FRoadNodeId X = Net->AddNode(FVector2D(60000.0, -20000.0));
	const FRoadSegmentId Tx = Net->AddStraightSegment(E, X, Taxiway);

	// Defaults: an existing level loads as tarmac / visual (spec §5).
	const FRunwayFacts Loaded = Net->RunwayFactsFor(RW1);
	TestEqual(TEXT("a fresh runway is tarmac by default"), Loaded.Surface, EPavement::Tarmac);
	TestEqual(TEXT("and visual by default"), Loaded.Approach, ERunwayApproach::Visual);

	FRunwayFacts Facts;
	Facts.Surface = EPavement::Concrete;
	Facts.Approach = ERunwayApproach::Precision;
	TestTrue(TEXT("setting facts on a runway segment succeeds"), Net->SetRunwayFacts(RW1, Facts));

	const FRunwayFacts Other = Net->RunwayFactsFor(RW2);
	TestEqual(TEXT("the OTHER half of the strip carries the surface"), Other.Surface, EPavement::Concrete);
	TestEqual(TEXT("and the approach"), Other.Approach, ERunwayApproach::Precision);

	// A taxiway has no runway facts to set; refusing is how the tool learns its click missed.
	FRunwayFacts Grass;
	Grass.Surface = EPavement::Grass;
	TestFalse(TEXT("a taxiway refuses runway facts"), Net->SetRunwayFacts(Tx, Grass));
	TestEqual(TEXT("and nothing on the runway changed"), Net->RunwayFactsFor(RW1).Surface, EPavement::Concrete);
	TestEqual(TEXT("a non-runway reads back the struct default"), Net->RunwayFactsFor(Tx).Surface, EPavement::Tarmac);

	// The split surgery replaces a segment with two: both halves must inherit the facts, or
	// adding an exit to a precision runway would silently demote the half past the exit.
	const FRoadNodeId Middle = Net->SplitSegment(RW2, FVector2D(80000.0, 0.0));
	if (!TestTrue(TEXT("the split produced a node"), Middle.IsSet())) { return false; }
	int32 RunwaySegments = 0;
	for (int32 Index = 0; Index < Net->GetSegments().Num(); ++Index)
	{
		const FRoadSegment& Segment = Net->GetSegments()[Index];
		if (!Segment.bAlive || !Segment.Profile->bContinuousThroughJunctions)
		{
			continue;
		}
		++RunwaySegments;
		TestEqual(TEXT("every runway segment after the split carries the surface"), Segment.Runway.Surface, EPavement::Concrete);
		TestEqual(TEXT("and the approach"), Segment.Runway.Approach, ERunwayApproach::Precision);
	}
	TestEqual(TEXT("the split made three runway segments"), RunwaySegments, 3);

	// The PIE path: play duplicates the level, and a UPROPERTY on the segment must come
	// through it. A transient or non-UPROPERTY field would land in play as the default.
	URoadNetwork* Dup = DuplicateObject<URoadNetwork>(Net, GetTransientPackage());
	if (!TestNotNull(TEXT("the network duplicates"), Dup)) { return false; }
	TestEqual(TEXT("the duplicate reads the same surface"), Dup->RunwayFactsFor(RW1).Surface, EPavement::Concrete);
	TestEqual(TEXT("and the same approach"), Dup->RunwayFactsFor(RW1).Approach, ERunwayApproach::Precision);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPavementOrderIsStrengthTest, "Airside.Model.Pavement.OrderIsStrength",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FPavementOrderIsStrengthTest::RunTest(const FString&)
{
	// ALL SIXTEEN PAIRS, because the rule is the ordering itself: a hand-picked pair or two
	// would pass against a Judge that special-cased grass.
	for (uint8 Have = 0; Have < static_cast<uint8>(EPavement::Count); ++Have)
	{
		for (uint8 Need = 0; Need < static_cast<uint8>(EPavement::Count); ++Need)
		{
			const FPavementCheck Check = Pavement::Judge(static_cast<EPavement>(Have), static_cast<EPavement>(Need));
			TestEqual(FString::Printf(TEXT("%s ground, %s needed: passes iff strong enough"),
				Pavement::Name(static_cast<EPavement>(Have)), Pavement::Name(static_cast<EPavement>(Need))),
				Check.Passes(), Have >= Need);
			TestEqual(TEXT("and a passing check has no sentence"), Check.Passes(), Pavement::Describe(Check).IsEmpty());
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPavementDescribeMatchesRunwaySentenceTest, "Airside.Model.Pavement.DescribeMatchesRunwaySentence",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FPavementDescribeMatchesRunwaySentenceTest::RunTest(const FString&)
{
	// BYTE-IDENTICAL to the sentence RunwayAdmission::Describe printed before the check moved
	// here (captured from main 2026-09-27) - the move must not change what the player reads.
	TestEqual(TEXT("the runway's refusal sentence survives the move"),
		Pavement::Describe(Pavement::Judge(EPavement::Grass, EPavement::Tarmac)),
		FString(TEXT("the surface is grass; this aircraft needs tarmac")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPavementRateFactorTest, "Airside.Model.Pavement.RateFactor",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FPavementRateFactorTest::RunTest(const FString&)
{
	// TARMAC IS 1 so every price authored before pavement mattered is unchanged; the others
	// are the spec's first guesses and ordered like the scale - stronger costs more.
	TestEqual(TEXT("grass"), Pavement::RateFactor(EPavement::Grass), 0.4);
	TestEqual(TEXT("tarmac is the authored rate itself"), Pavement::RateFactor(EPavement::Tarmac), 1.0);
	TestEqual(TEXT("concrete"), Pavement::RateFactor(EPavement::Concrete), 1.4);
	TestEqual(TEXT("reinforced"), Pavement::RateFactor(EPavement::Reinforced), 1.8);
	return true;
}

/**
 * #446: EVERY FACT A PLANNER READS MOVES GuidelineRevision - the clock offer admission, the job board's re-bid and re-offer,
 * the flight board's verdicts, the held taxi-out, the Land panel and the inspector card key on. Until #446 these writes
 * moved nothing, and the caches saw a runway flip or a shed purchase only because the facade escalated it to a Topology
 * rebuild; a runway flip is a Facts edit now, which re-derives nothing, so the model has to say it changed. One row per
 * mutator, so a new fact that forgets NoteFactChanged has a row here to be added to - and a refusal must move nothing.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEveryFactMovesTheGuidelineRevisionTest, "Airside.Model.EveryFactMovesTheGuidelineRevision",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FEveryFactMovesTheGuidelineRevisionTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FRoadNodeId T = Net->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId E = Net->AddNode(FVector2D(60000.0, 0.0));
	const FRoadSegmentId Strip = Net->AddStraightSegment(T, E, TestProfiles::Runway());
	// A PLAIN TRANSIENT PROFILE offers every pavement (the taxiway fixture's does not), so the road can be put to grass.
	const FRoadNodeId R = Net->AddNode(FVector2D(0.0, -30000.0));
	const FRoadSegmentId Road = TestGraph::Lay(*Net, T, R, URoadProfile::MakeTransient(2300.0, 1500.0));

	auto Moves = [this, Net](const TCHAR* What, TFunctionRef<bool()> Write)
	{
		const uint32 Before = Net->GetGuidelineRevision();
		const bool bWrote = Write();
		TestTrue(FString::Printf(TEXT("setup: %s is accepted"), What), bWrote);
		TestNotEqual(FString::Printf(TEXT("%s moves GuidelineRevision - a planner reads it, and no rebuild may follow (#446)"), What),
			Net->GetGuidelineRevision(), Before);
	};

	FRunwayFacts Grass;
	Grass.Surface = EPavement::Grass;
	Moves(TEXT("a runway's facts"), [Net, Strip, &Grass]() { return Net->SetRunwayFacts(Strip, Grass); });
	Moves(TEXT("the drive side"), [Net]() { return Net->SetDriveSide(Net->GetDriveSide() == EDriveSide::Left ? EDriveSide::Right : EDriveSide::Left); });
	Moves(TEXT("a road's pavement"), [Net, Road]() { return Net->SetSegmentSurface(Road, EPavement::Grass); });

	const TArray<FVector2D> Plot = { FVector2D(0.0, 20000.0), FVector2D(5000.0, 20000.0), FVector2D(5000.0, 22400.0), FVector2D(0.0, 22400.0) };
	UEntityDefinition* DepotDef = UEntityDefinition::MakeFuelDepotTransient();
	FEntityPlacement Placement;
	Placement.Definition = DepotDef;
	Placement.Anchors = DepotDef->Anchors;
	Placement.Position = (Plot[0] + Plot[1]) * 0.5;
	Placement.PoseRole = EServiceRole::Fuel;
	Placement.Outline = Plot;
	Placement.Modules = { EDepotModule::Shed, EDepotModule::Tank, EDepotModule::Pump };
	FEntityInstanceId Depot;
	Moves(TEXT("a depot placed"), [Net, &Placement, &Depot]() { Depot = Net->PlaceEntity(Placement); return Depot.IsSet(); });
	Moves(TEXT("a module bought"), [Net, &Depot]() { return Net->AddEntityModule(Depot, EDepotModule::Shed); });
	Moves(TEXT("a module removed"), [Net, &Depot]() { return Net->RemoveEntityModules(Depot, EDepotModule::Shed, 1) == 1; });
	Moves(TEXT("an entity re-pointed at a definition"), [Net, &Depot, DepotDef]() { return Net->SetEntityDefinition(Depot, DepotDef); });

	UEntityDefinition* StandDef = UEntityDefinition::MakeStandTransient();
	FEntityInstanceId Stand;
	Moves(TEXT("a stand placed"), [Net, StandDef, &Stand]()
	{
		Stand = Net->PlaceEntity(StandDef, StandDef->Anchors, FVector2D(30000.0, 20000.0), 0.0, 0.0, StandDef->PoseRole, 0);
		return Stand.IsSet();
	});
	Moves(TEXT("a stand's design wingspan"), [Net, &Stand]() { return Net->SetStandDesignWingspan(Stand, 4000.0); });
	Moves(TEXT("an entity removed"), [Net, &Stand]() { return Net->RemoveEntity(Stand); });

	FApronSurface Apron;
	Apron.Outline = { FVector2D(0.0, 40000.0), FVector2D(9000.0, 40000.0), FVector2D(9000.0, 49000.0) };
	FApronId ApronId;
	Moves(TEXT("an apron added"), [Net, &Apron, &ApronId]() { ApronId = Net->AddApron(MoveTemp(Apron)); return ApronId.IsSet(); });
	Moves(TEXT("an apron removed"), [Net, &ApronId]() { return Net->RemoveApron(ApronId); });

	FReverseTurn Turn;
	Moves(TEXT("a reverse turn recorded"), [Net, &Turn]() { return Net->AddReverseTurn(Turn) != INDEX_NONE; });
	const FGuidelineNodeId Bar = Net->AddGuidelineNode(FVector2D(90000.0, 90000.0), /*bDerived*/ false);
	Moves(TEXT("a holding bar set"), [Net, Bar]() { return Net->SetIntermediateHoldingPosition(Bar, true); });

	// A REFUSAL CHANGED NOTHING, so it moves nothing: a stamp moved for nothing makes every cache keyed here redo its work.
	const uint32 BeforeRefusals = Net->GetGuidelineRevision();
	TestFalse(TEXT("setup: a module on a dead depot is refused"), Net->AddEntityModule(FEntityInstanceId(), EDepotModule::Shed));
	TestEqual(TEXT("setup: removing none removes none"), Net->RemoveEntityModules(Depot, EDepotModule::Pump, 0), 0);
	TestFalse(TEXT("setup: the drive side it already has is refused"), Net->SetDriveSide(Net->GetDriveSide()));
	TestEqual(TEXT("and none of the refusals moved the clock"), Net->GetGuidelineRevision(), BeforeRefusals);
	return true;
}

#endif
