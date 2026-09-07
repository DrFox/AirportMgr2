#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Model/RunwayFacts.h"
#include "Present/RoadEditFacade.h"
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
	URoadProfile* Runway = URoadProfile::MakeTransient(4500.0, 1500.0, 450.0);
	Runway->bContinuousThroughJunctions = true;
	URoadProfile* Taxiway = URoadProfile::MakeTransient(2300.0, 1500.0, 230.0);

	const FRoadNodeId T = Net->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId E = Net->AddNode(FVector2D(60000.0, 0.0));
	const FRoadNodeId F = Net->AddNode(FVector2D(100000.0, 0.0));
	const FRoadSegmentId RW1 = Net->AddStraightSegment(T, E, Runway);
	const FRoadSegmentId RW2 = Net->AddStraightSegment(E, F, Runway);
	const FRoadNodeId X = Net->AddNode(FVector2D(60000.0, -20000.0));
	const FRoadSegmentId Tx = Net->AddStraightSegment(E, X, Taxiway);

	// Defaults: an existing level loads as tarmac / visual (spec §5).
	const FRunwayFacts Loaded = Net->RunwayFactsFor(RW1);
	TestEqual(TEXT("a fresh runway is tarmac by default"), Loaded.Surface, ERunwaySurface::Tarmac);
	TestEqual(TEXT("and visual by default"), Loaded.Approach, ERunwayApproach::Visual);

	FRunwayFacts Facts;
	Facts.Surface = ERunwaySurface::Concrete;
	Facts.Approach = ERunwayApproach::Precision;
	TestTrue(TEXT("setting facts on a runway segment succeeds"), Net->SetRunwayFacts(RW1, Facts));

	const FRunwayFacts Other = Net->RunwayFactsFor(RW2);
	TestEqual(TEXT("the OTHER half of the strip carries the surface"), Other.Surface, ERunwaySurface::Concrete);
	TestEqual(TEXT("and the approach"), Other.Approach, ERunwayApproach::Precision);

	// A taxiway has no runway facts to set; refusing is how the tool learns its click missed.
	FRunwayFacts Grass;
	Grass.Surface = ERunwaySurface::Grass;
	TestFalse(TEXT("a taxiway refuses runway facts"), Net->SetRunwayFacts(Tx, Grass));
	TestEqual(TEXT("and nothing on the runway changed"), Net->RunwayFactsFor(RW1).Surface, ERunwaySurface::Concrete);
	TestEqual(TEXT("a non-runway reads back the struct default"), Net->RunwayFactsFor(Tx).Surface, ERunwaySurface::Tarmac);

	// The split surgery replaces a segment with two: both halves must inherit the facts, or
	// adding an exit to a precision runway would silently demote the half past the exit.
	const FRoadNodeId Middle = URoadEditFacade::SplitSegmentIn(*Net, RW2, FVector2D(80000.0, 0.0));
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
		TestEqual(TEXT("every runway segment after the split carries the surface"), Segment.Runway.Surface, ERunwaySurface::Concrete);
		TestEqual(TEXT("and the approach"), Segment.Runway.Approach, ERunwayApproach::Precision);
	}
	TestEqual(TEXT("the split made three runway segments"), RunwaySegments, 3);

	// The PIE path: play duplicates the level, and a UPROPERTY on the segment must come
	// through it. A transient or non-UPROPERTY field would land in play as the default.
	URoadNetwork* Dup = DuplicateObject<URoadNetwork>(Net, GetTransientPackage());
	if (!TestNotNull(TEXT("the network duplicates"), Dup)) { return false; }
	TestEqual(TEXT("the duplicate reads the same surface"), Dup->RunwayFactsFor(RW1).Surface, ERunwaySurface::Concrete);
	TestEqual(TEXT("and the same approach"), Dup->RunwayFactsFor(RW1).Approach, ERunwayApproach::Precision);
	return true;
}

#endif
