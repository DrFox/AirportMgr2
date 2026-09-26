#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Solve/GuidelineGeom.h"
#include "Solve/LinkGeom.h"

#if WITH_DEV_AUTOMATION_TESTS

// WORLD-FREE, deliberately: no NewObject, no URoadNetwork, no FGuidelineEdge - LinkGeom::Plan
// takes plain points because Solve/ may see nothing else (Check-Architecture's solve-purity
// rule), and these tests prove that holds by never reaching for a world to make one pass.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLinkGeomChooseDepartureTest,
	"Airside.Solve.LinkGeom.ChooseDeparture",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FLinkGeomChooseDepartureTest::RunTest(const FString& Parameters)
{
	using namespace LinkGeom;

	// NO CANDIDATES: false, OutAlong untouched - the caller's own fallback to a straight
	// lead-in, exactly as EntryDeparture's header promises.
	{
		FVector2D OutAlong = FVector2D(7.0, 7.0);
		TestFalse(TEXT("no candidates chooses nothing"),
			ChooseDeparture(TArray<FDepartureCandidate>(), FVector2D(1.0, 0.0), OutAlong));
		TestEqual(TEXT("and leaves OutAlong alone"), OutAlong, FVector2D(7.0, 7.0));
	}

	// TOWARD THE ROAD WINS FIRST, over both bend and room: a candidate facing away is refused
	// even though it has more room and is the bend, since a curve leaving backwards swings out
	// over the lane's own crossing before it gets anywhere.
	{
		TArray<FDepartureCandidate> Candidates;
		Candidates.Add({ FVector2D(1.0, 0.0), /*bBend=*/false, /*Room=*/100.0 });
		Candidates.Add({ FVector2D(-1.0, 0.0), /*bBend=*/true, /*Room=*/9999.0 });

		FVector2D OutAlong;
		TestTrue(TEXT("a candidate exists"),
			ChooseDeparture(Candidates, FVector2D(1.0, 0.0), OutAlong));
		TestEqual(TEXT("the one facing the road wins, not the roomier bend facing away"),
			OutAlong, FVector2D(1.0, 0.0));
	}

	// A TIE ON "TOWARD" GOES TO THE BEND, not the roomier straight - the corner-side rule: a
	// tangential join is smooth in one direction only, and putting the control on the bend's
	// side keeps the STRAIGHT leg - typically the long run to a stand's plant - free of it.
	{
		TArray<FDepartureCandidate> Candidates;
		Candidates.Add({ FVector2D(0.0, 1.0), /*bBend=*/false, /*Room=*/4700.0 });
		Candidates.Add({ FVector2D(0.0, -1.0), /*bBend=*/true, /*Room=*/300.0 });

		FVector2D OutAlong;
		TestTrue(TEXT("a candidate exists"),
			ChooseDeparture(Candidates, FVector2D(1.0, 0.0), OutAlong));
		TestEqual(TEXT("the bend wins the tie despite less room"),
			OutAlong, FVector2D(0.0, -1.0));

		// ORDER-INDEPENDENT: the same tie, offered the other way round, still picks the bend -
		// proving the rule is the comparison, not which candidate happened to arrive first.
		Candidates.Swap(0, 1);
		FVector2D OutAlongReversed;
		ChooseDeparture(Candidates, FVector2D(1.0, 0.0), OutAlongReversed);
		TestEqual(TEXT("and the order it was offered in does not matter"),
			OutAlongReversed, FVector2D(0.0, -1.0));
	}

	// A TIE ON BOTH "TOWARD" AND "BEND" GOES TO MORE ROOM - the third and last test, which
	// nothing above it needs to fall through to unless the first two are themselves tied.
	{
		TArray<FDepartureCandidate> Candidates;
		Candidates.Add({ FVector2D(0.0, 1.0), /*bBend=*/true, /*Room=*/50.0 });
		Candidates.Add({ FVector2D(0.0, -1.0), /*bBend=*/true, /*Room=*/500.0 });

		FVector2D OutAlong;
		ChooseDeparture(Candidates, FVector2D(1.0, 0.0), OutAlong);
		TestEqual(TEXT("the roomier of two tied bends wins"), OutAlong, FVector2D(0.0, -1.0));
	}

	return true;
}

namespace
{
	/** A straight road along X at y = 0, from (0,0) to (2000,0) - LinkGeom::Plan's Curve is the
	 *  sampled polyline (URoadNetwork::SampleGuideline's own domain); GuidelineGeom::Sample
	 *  short-circuits a straight guideline to its two endpoints, so this is that shortcut typed
	 *  out rather than reached for through a network. */
	LinkGeom::FLinkApproach StraightRoadApproach()
	{
		LinkGeom::FLinkApproach Approach;
		Approach.PositionA = FVector2D(0.0, 0.0);
		Approach.Control = FVector2D(1000.0, 0.0);
		Approach.PositionB = FVector2D(2000.0, 0.0);
		Approach.Curve = { Approach.PositionA, Approach.PositionB };
		return Approach;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLinkGeomPlanStraightRayTest,
	"Airside.Solve.LinkGeom.PlanStraightRay",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FLinkGeomPlanStraightRayTest::RunTest(const FString& Parameters)
{
	using namespace LinkGeom;

	// AN AIRCRAFT'S RAY: bRecomputeDirFromCorner is false, so Plan leaves Param/Corner/Dir
	// exactly as the search (or the ray cast) found them - the painted line has no business
	// curving, and nothing here decides otherwise.
	{
		FLinkApproach Approach = StraightRoadApproach();
		Approach.Param = 0.5;
		Approach.At = FVector2D(1000.0, -500.0);
		Approach.Dir = FVector2D(0.0, 1.0);
		Approach.bRecomputeDirFromCorner = false;

		const FLinkGeometry Plan = LinkGeom::Plan(Approach);
		TestTrue(TEXT("no lane, no ray re-aim: StraightRay"), Plan.Shape == ELinkShape::StraightRay);
		TestEqual(TEXT("Param is untouched"), Plan.Param, 0.5);
		TestEqual(TEXT("Corner is the search's own hit"), Plan.Corner, FVector2D(1000.0, 0.0));
		TestEqual(TEXT("Dir is untouched - a ray's cast heading is the answer already"),
			Plan.Dir, FVector2D(0.0, 1.0));
		TestFalse(TEXT("no lane control"), Plan.Control.IsSet());
		TestEqual(TEXT("no lane run"), Plan.LaneRun, 0.0);
	}

	// AN ORDINARY SERVICE ANCHOR, found by proximity and leaving no lane: Dir is recomputed
	// from Corner - At, but Param/Corner still do not move, because there is no lane to leave
	// along and re-aim onto.
	{
		FLinkApproach Approach = StraightRoadApproach();
		Approach.Param = 0.5;
		Approach.At = FVector2D(1000.0, -500.0);
		Approach.Dir = FVector2D(1.0, 0.0); // Whatever it was, irrelevant off a Ray link.
		Approach.bRecomputeDirFromCorner = true;

		const FLinkGeometry Plan = LinkGeom::Plan(Approach);
		TestTrue(TEXT("still StraightRay"), Plan.Shape == ELinkShape::StraightRay);
		TestEqual(TEXT("Param still untouched"), Plan.Param, 0.5);
		TestEqual(TEXT("Dir now points from At to Corner"), Plan.Dir, FVector2D(0.0, 1.0));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLinkGeomPlanCrossingTest,
	"Airside.Solve.LinkGeom.PlanCrossing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FLinkGeomPlanCrossingTest::RunTest(const FString& Parameters)
{
	using namespace LinkGeom;

	// A LANE RUNNING STRAIGHT AT THE ROAD: Along is perpendicular to RoadDir, so the crossing
	// sits directly ahead, well within reach and with room in front of it - the CROSSING shape,
	// preferred whenever it exists (see Plan's own comment on why).
	//
	// THE SEARCH'S OWN HIT (Param 0.3) IS DELIBERATELY NOT WHERE THE LANE MEETS THE ROAD (x =
	// 1000, i.e. Param 0.5): that is the re-aim this test exists to catch. A Plan that forgot to
	// move Param/Corner would report the search's hit, at x = 600, instead.
	FLinkApproach Approach = StraightRoadApproach();
	Approach.Param = 0.3;
	Approach.At = FVector2D(1000.0, -2000.0);
	Approach.LaneRadius = 100.0;
	Approach.Reach = 20000.0;
	Approach.WeldTolerance = 10.0;
	Approach.bHasLaneAlong = true;
	Approach.Along = FVector2D(0.0, 1.0);

	const FLinkGeometry Plan = LinkGeom::Plan(Approach);
	TestTrue(TEXT("a crossing in reach with room is preferred"), Plan.Shape == ELinkShape::Crossing);
	TestTrue(TEXT("re-aimed onto where the lane actually meets the road"),
		FMath::IsNearlyEqual(Plan.Param, 0.5, 1e-6));
	TestEqual(TEXT("at (1000, 0), not the search's own (600, 0) hit"),
		Plan.Corner, FVector2D(1000.0, 0.0));
	TestEqual(TEXT("Dir runs straight from the entry to the crossing"),
		Plan.Dir, FVector2D(0.0, 1.0));
	TestFalse(TEXT("a crossing needs no control point - the lead-in is straight"),
		Plan.Control.IsSet());
	TestEqual(TEXT("and spends no run along the lane"), Plan.LaneRun, 0.0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLinkGeomPlanLaneChangeTest,
	"Airside.Solve.LinkGeom.PlanLaneChange",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FLinkGeomPlanLaneChangeTest::RunTest(const FString& Parameters)
{
	using namespace LinkGeom;

	// A LANE RUNNING ALONGSIDE THE ROAD: Along is PARALLEL to RoadDir, so the two lines never
	// meet (Converge is zero, MeetsAt reports "no crossing" per Plan's own -1.0 fallback) - the
	// LANE CHANGE shape, an S sized by GuidelineGeom::ShiftDeflectionFor.
	FLinkApproach Approach = StraightRoadApproach();
	Approach.Param = 0.0; // Nearest point (0,0), directly across from At.
	Approach.At = FVector2D(0.0, -500.0);
	Approach.LaneRadius = 100.0;
	Approach.Reach = 20000.0;
	Approach.WeldTolerance = 10.0;
	Approach.bHasLaneAlong = true;
	Approach.Along = FVector2D(1.0, 0.0);

	const FLinkGeometry Plan = LinkGeom::Plan(Approach);
	TestTrue(TEXT("parallel lines take the lane change"), Plan.Shape == ELinkShape::LaneChange);

	// ExpectedRun IS THE ORACLE, not a restated formula: ShiftDeflectionFor is the one evaluator
	// for this shape (GuidelineGeom's own doc comment), and Plan's job is to call it correctly
	// and apply what comes back, not to reimplement it - the same reasoning
	// Airside.Solve.GuidelineGeom's own Split tests use Eval to check Split by.
	double ExpectedRun = 0.0;
	GuidelineGeom::ShiftDeflectionFor(Approach.LaneRadius, /*Shift=*/500.0, ExpectedRun);
	TestTrue(TEXT("this shift is real - the S has room to spend"), ExpectedRun > 10.0);

	TestTrue(TEXT("LaneRun is ShiftDeflectionFor's own OutRun"),
		FMath::IsNearlyEqual(Plan.LaneRun, ExpectedRun, 1e-6));
	if (TestTrue(TEXT("the S gets a control point"), Plan.Control.IsSet()))
	{
		TestTrue(TEXT("a run of ExpectedRun along the lane from At"),
			FVector2D::Distance(Plan.Control.GetValue(), Approach.At + Approach.Along * ExpectedRun) < 1e-6);
	}
	TestTrue(TEXT("the corner it re-aimed onto is still on the road (y = 0)"),
		FMath::IsNearlyEqual(Plan.Corner.Y, 0.0, 1e-6));
	TestTrue(TEXT("at a legal parameter along it"), Plan.Param >= 0.0 && Plan.Param <= 1.0);

	return true;
}

#endif
