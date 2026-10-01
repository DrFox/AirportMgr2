#include "CoreMinimal.h"
#include "Algo/Reverse.h"
#include "Misc/AutomationTest.h"
#include "Solve/PlotYard.h"
#include "Solve/RoadGeom.h"

#if WITH_DEV_AUTOMATION_TESTS

// THESE TESTS RUN ON Reserve, THE ONLY SAMPLER (#462). They used to run on PlotYard::LayOut, which took
// a caller's footprint list and had no production caller after the presenter and the tool moved to Reserve
// on 2026-09-20 - so what they measured was shared FYardSpace code reached through an entry point nobody
// shipped. LayOut and FYard are deleted; the five tests that were LayOut's alone (RoomForMore, DroppedCount,
// determinism, non-overlap, gate corridor) went with them, the last three because PlotReserveTest has
// the same claim on Reserve (IsDeterministic, NeverOverlaps, LeavesTheGateClear). What nothing on Reserve
// measured - containment, the clearance GAP, the back-fence shed's pose on awkward plots, seed variation
// - lives here.

namespace
{
	/** An axis-aligned rectangle, CCW, with its SOUTH edge (y = 0) as the frontage. */
	TArray<FVector2D> YardRect(double Width, double Depth)
	{
		return { FVector2D(0.0, 0.0), FVector2D(Width, 0.0),
		         FVector2D(Width, Depth), FVector2D(0.0, Depth) };
	}

	/** A shed-sized footprint, stood against the back fence. 8 m deep, 4 m wide. */
	PlotYard::FFootprint Shed()
	{
		PlotYard::FFootprint F;
		F.LengthUu = 800.0;
		F.WidthUu = 400.0;
		F.bAgainstTheBackFence = true;
		return F;
	}

	/** A tank-sized footprint: 5 m x 5 m, and it does not front the gate. */
	PlotYard::FFootprint Tank()
	{
		PlotYard::FFootprint F;
		F.LengthUu = 500.0;
		F.WidthUu = 500.0;
		F.bAgainstTheBackFence = false;
		return F;
	}

	/** A pump: 3 m x 2 m, low and small. */
	PlotYard::FFootprint Pump()
	{
		PlotYard::FFootprint F;
		F.LengthUu = 300.0;
		F.WidthUu = 200.0;
		F.bAgainstTheBackFence = false;
		return F;
	}

	/**
	 * One kit of one module per stand, weight 1: Reserve's FIRST pass gives each kit one bay, largest
	 * first, so Stands[0] of a shed-led mix is the back-fence shed and the rest is the fill cycle.
	 * RunCap 1 and no end caps, so a stand's footprint on the ground IS the module's own - what
	 * StandCorners is handed below.
	 */
	PlotYard::FKitSpec YardKit(const PlotYard::FFootprint& Footprint)
	{
		PlotYard::FKitSpec Kit;
		Kit.Footprint = Footprint;
		Kit.ReserveWeight = 1;
		Kit.RunCap = 1;
		return Kit;
	}

	/** Shed, tank, pump - the depot's own mix, shed first by area. */
	TArray<PlotYard::FKitSpec> DepotMix()
	{
		return { YardKit(Shed()), YardKit(Tank()), YardKit(Pump()) };
	}

	// StandsOverlap USED TO BE DEFINED HERE, by separating axis, as a private copy.
	// PlotYard::StandsOverlap is now public and this file calls that: one derivation, and a
	// test that computed overlap its own way would be checking its own arithmetic rather than
	// the solver's - the same reason StandCorners is public.

	/** How far P is from the segment AB - the same question ClosestPointOnSegment answers. */
	double DistanceToSegment(const FVector2D& P, const FVector2D& A, const FVector2D& B)
	{
		const double T = RoadGeom::ClosestPointOnSegment(A, B, P);
		return FVector2D::Distance(P, FMath::Lerp(A, B, T));
	}

	/**
	 * The minimum gap between two disjoint convex quads - every vertex against every edge of
	 * the OTHER quad, both ways round. That covers vertex-vertex too, since a vertex is an
	 * endpoint of two edges, and is the whole of what "closest approach of two convex shapes"
	 * needs when they do not overlap (StandsOverlap already guards that they do not).
	 */
	double MinGapBetweenQuads(const TArray<FVector2D>& A, const TArray<FVector2D>& B)
	{
		double MinGap = TNumericLimits<double>::Max();
		for (const FVector2D& P : A)
		{
			for (int32 J = 0; J < B.Num(); ++J)
			{
				MinGap = FMath::Min(MinGap,
					DistanceToSegment(P, B[J], B[(J + 1) % B.Num()]));
			}
		}
		for (const FVector2D& P : B)
		{
			for (int32 J = 0; J < A.Num(); ++J)
			{
				MinGap = FMath::Min(MinGap,
					DistanceToSegment(P, A[J], A[(J + 1) % A.Num()]));
			}
		}
		return MinGap;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotYardStandsTheShedAtTheBackTest,
	"Airside.Solve.PlotYardStandsTheShedAtTheBack",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotYardStandsTheShedAtTheBackTest::RunTest(const FString& Parameters)
{
	const TArray<FVector2D> Outline = YardRect(2400.0, 1600.0);
	const FVector2D FrontageA(0.0, 0.0);
	const FVector2D FrontageB(2400.0, 0.0);
	const FVector2D Gate(1200.0, 0.0);

	// THE SHED IS STANDS[0]: Reserve's first pass takes the kits largest-area first, one bay each,
	// and the first stand of a back-fence kit is the one that takes the gate's ray (PlaceOne's
	// bTakesTheRay). Stands after it are the fill cycle's sheds, sampled like anything else, so
	// only Stands[0] is asserted to be AT the back.
	const TArray<PlotYard::FKitSpec> Kits = { YardKit(Shed()) };

	const PlotYard::FReservation Reservation =
		PlotYard::Reserve(Outline, FrontageA, FrontageB, Gate, Kits, /*Seed=*/1234);

	if (!TestTrue(TEXT("the shed was placed"), Reservation.Stands.Num() > 0))
	{
		return false;
	}
	const PlotYard::FReservedStand& First = Reservation.Stands[0];
	TestTrue(TEXT("and it is placed, not a stand that failed"), First.bPlaced);

	// SQUARE TO THE FRONTAGE, not jittered. The truck drives out of the shed, so its heading
	// is functional - the one module that may not be turned for looks. Interior is +Y here,
	// so the inward bearing is +90 degrees.
	TestEqual(TEXT("the shed faces away from the road, square"), First.Heading, UE_DOUBLE_HALF_PI);

	// AND IT STANDS AT THE BACK. It stood in the gateway until 2026-09-17 - a depot whose
	// only way in is blocked by the building you drive out of. The plot is 16 m deep and the
	// shed 8 m long, so its centre belongs at 12 m: hard against the back fence.
	TestTrue(*FString::Printf(TEXT("the shed is against the back fence, got y %.0f"), First.Centre.Y),
		FMath::IsNearlyEqual(First.Centre.Y, 1200.0, 1.0));

	// NOT IN THE GATEWAY, stated as its own claim: "deep in the yard" and "clear of the gate"
	// are different facts, and it was the second that failed.
	TestTrue(TEXT("and well clear of the gate it used to block"),
		FVector2D::Distance(First.Centre, Gate) > PlotYard::GateCorridorUu);

	// A PLOT TOO SHALLOW for the shed's own pose (800 long, 600 deep) never reserves one hanging across
	// the road - the case no other plot reaches. This asserted the CLAMPED POSE of the shed LayOut
	// handed back DROPPED (centre y >= 400, "off the road"); Reserve returns only what it placed, so a
	// dropped shed has no pose to read and what is left to measure is the promise a reservation makes -
	// every stand it does list is wholly inside the fence. (#462: LayOut and that assertion went together.)
	const TArray<FVector2D> ShallowOutline = YardRect(2400.0, 600.0);
	const PlotYard::FReservation Shallow = PlotYard::Reserve(
		ShallowOutline, FrontageA, FrontageB, Gate, Kits, 1234);
	if (TestTrue(TEXT("a shallow plot still reserves something to check"), Shallow.Stands.Num() > 0))
	{
		for (const PlotYard::FReservedStand& Stand : Shallow.Stands)
		{
			TArray<FVector2D> ShallowCorners;
			PlotYard::StandCorners(Stand, Shed(), ShallowCorners);
			for (const FVector2D& Corner : ShallowCorners)
			{
				TestTrue(*FString::Printf(
					TEXT("a shallow plot keeps every shed off the road: corner (%.0f, %.0f) is inside"), Corner.X, Corner.Y),
					RoadGeom::PointInPolygon(ShallowOutline, Corner));
			}
		}
	}

	// AND A SLANTED BACK FENCE KEEPS IT IN, which a rectangle cannot test: with the back edge
	// square to the gate ray the deepest POINT and the boundary above the shed are the same
	// depth, so a placement that only checks its centre still looks right. The four-point
	// gesture makes slanted backs the ordinary case, and PIE on 2026-09-17 showed the shed
	// "quite often sticks out of the back boundary of the plot".
	//
	// This quad runs 24 m deep at its left corner and 10 m at its right; the gate sits in the
	// middle, so the deepest POINT is nowhere near the boundary above the shed.
	const TArray<FVector2D> Wedge = {
		FVector2D(0.0, 0.0), FVector2D(2400.0, 0.0),
		FVector2D(2400.0, 1000.0), FVector2D(0.0, 2400.0) };

	const PlotYard::FReservation Slanted = PlotYard::Reserve(
		Wedge, FrontageA, FrontageB, Gate, Kits, /*Seed=*/1234);

	// PLACED, ASSERTED - not skipped. The wedge is 17 m deep above the gate and the shed is
	// 8 m, so there is room; guarding the corner checks behind a placement would let this pass
	// on a solver that simply gave up, which is the vacuous shape it is meant to catch.
	if (!TestTrue(TEXT("the shed stands on the wedge"), Slanted.Stands.Num() > 0))
	{
		return false;
	}

	TArray<FVector2D> Corners;
	PlotYard::StandCorners(Slanted.Stands[0], Shed(), Corners);
	for (const FVector2D& Corner : Corners)
	{
		// EVERY CORNER, not the centre. A centre-only placement is exactly what put the shed
		// through the fence.
		TestTrue(*FString::Printf(
			TEXT("shed corner (%.0f, %.0f) is inside the wedge"), Corner.X, Corner.Y),
			RoadGeom::PointInPolygon(Wedge, Corner));
	}

	// AND IT IS STILL AT THE BACK, not shoved to the front to make the corners fit. The
	// boundary above the gate is 17 m out and the shed is 8 m long, so a centre nearer the
	// road than 8 m means the scan gave up rather than found the deepest fit.
	TestTrue(*FString::Printf(TEXT("and still deep in the plot, got y %.0f"),
		Slanted.Stands[0].Centre.Y), Slanted.Stands[0].Centre.Y > 800.0);

	return true;
}

/**
 * A back-fence kit refused the gate ray is still sampled like anything else afterwards.
 *
 * bTakesTheRay used to stay true forever once CeilingFor(Kit) stayed at zero - which a FAILED
 * back-fence attempt also leaves at zero - so every later offer of a back-fence kit was
 * forced onto the same dead ray and never even tried anywhere else (issue #193). This plot
 * notches the gate ray to a corridor narrower than the shed the whole way up, so
 * PlaceAgainstTheBackFence refuses it at every depth, and opens into a wide lobe off to one
 * side that has nothing to do with the ray at all.
 *
 * SEVERAL SEEDS, not one: PlaceAgainstTheBackFence has no randomness and fails the buggy way
 * identically for every seed, while TryPlace samples - "at least one of several seeds finds
 * the lobe" is the discriminator that does not depend on a single lucky draw. Reserve also
 * keeps offering a placed kit more of itself until a whole cycle places nothing, so a fixed
 * solver gets many independent tries at the lobe even within one seed.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotYardFreesABackFenceKitAfterAFailedRayTest,
	"Airside.Solve.PlotYardFreesABackFenceKitAfterAFailedRay",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotYardFreesABackFenceKitAfterAFailedRayTest::RunTest(const FString& Parameters)
{
	// THE NECK: 300 uu wide, against an 800 x 400 shed - too narrow at every depth - running
	// the plot's full height so the gate ray never clears it. THE LOBE: a wide extension to
	// the right from halfway up, sharing no ground with the ray's own column.
	const TArray<FVector2D> Outline = {
		FVector2D(850.0, 0.0), FVector2D(1150.0, 0.0),
		FVector2D(1150.0, 1000.0), FVector2D(3150.0, 1000.0),
		FVector2D(3150.0, 2000.0), FVector2D(850.0, 2000.0) };
	const FVector2D FrontageA(850.0, 0.0);
	const FVector2D FrontageB(1150.0, 0.0);
	const FVector2D Gate(1000.0, 0.0);

	PlotYard::FKitSpec Spec;
	Spec.Footprint = Shed();
	Spec.ReserveWeight = 3;
	Spec.RunCap = 1;

	const TArray<PlotYard::FKitSpec> Kits = { Spec };

	bool bFoundElsewhere = false;
	for (int32 Seed = 1; Seed <= 8 && !bFoundElsewhere; ++Seed)
	{
		const PlotYard::FReservation Reservation =
			PlotYard::Reserve(Outline, FrontageA, FrontageB, Gate, Kits, Seed);
		if (Reservation.CeilingFor(0) > 0)
		{
			bFoundElsewhere = true;
		}
	}

	TestTrue(TEXT("a shed refused the gate ray is still sampled into the open lobe, "
		"given enough seeds"), bFoundElsewhere);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotYardKeepsModulesInsideThePlotTest,
	"Airside.Solve.PlotYardKeepsModulesInsideThePlot",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotYardKeepsModulesInsideThePlotTest::RunTest(const FString& Parameters)
{
	const TArray<PlotYard::FKitSpec> Kits = DepotMix();

	// TWO PLOTS, THE SECOND A WEDGE (#462). On a RECTANGLE the sampler's own bounding-box draw already keeps every centre inside, so
	// the containment test beside it never decides anything - deleting it left this green - and the whole claim is only measured by
	// an outline whose edges are not axis-aligned: this one runs 24 m deep at its left corner and 10 m at its right, the shape the
	// four-point gesture makes ordinary (see Airside.Solve.PlotYardStandsTheShedAtTheBack's wedge).
	const TArray<FVector2D> Outlines[] = {
		YardRect(2400.0, 1600.0),
		{ FVector2D(0.0, 0.0), FVector2D(2400.0, 0.0), FVector2D(2400.0, 1000.0), FVector2D(0.0, 2400.0) } };

	// SEVERAL SEEDS, not one. A sampler that happens to keep everything inside on seed 1234
	// and hangs a tank over the fence on 1235 is exactly the bug this guards, and a
	// single-seed test would ship it.
	int32 Checked = 0;
	for (const TArray<FVector2D>& Outline : Outlines)
	{
		for (int32 Seed = 1; Seed <= 8; ++Seed)
		{
			const PlotYard::FReservation Reservation = PlotYard::Reserve(
				Outline, FVector2D(0.0, 0.0), FVector2D(2400.0, 0.0), FVector2D(1200.0, 0.0),
				Kits, Seed);

			for (int32 Index = 0; Index < Reservation.Stands.Num(); ++Index)
			{
				const PlotYard::FReservedStand& Stand = Reservation.Stands[Index];
				TArray<FVector2D> Corners;
				PlotYard::StandCorners(Stand, Kits[Stand.KitIndex].Footprint, Corners);
				for (const FVector2D& Corner : Corners)
				{
					// EVERY CORNER, not the centre. A centre-only test accepts a module hanging
					// out of the plot and the player watches a tank stand on the grass.
					TestTrue(*FString::Printf(
						TEXT("%d-sided plot, seed %d: module %d corner (%.0f, %.0f) is inside the plot"),
						Outline.Num(), Seed, Index, Corner.X, Corner.Y),
						RoadGeom::PointInPolygon(Outline, Corner));
					++Checked;
				}
			}
		}
	}
	// A FLOOR, NOT A COUNT: a Reserve that returned nothing would pass every loop above.
	TestTrue(*FString::Printf(TEXT("corners were actually checked (%d)"), Checked), Checked >= 2 * 8 * 3 * 4);
	return true;
}

/**
 * No two stands overlap, and every pair lands at least ClearanceUu apart - measured, not merely
 * non-overlapping, which two modules that TOUCH already satisfy and read as one building.
 *
 * ONE TEST, WHERE THERE WERE TWO (#462): DoesNotOverlapModules asserted the overlap half of what
 * this measures on the same tight plot, so the gap loop below carries both. The overlap test
 * stays in front of the gap one because MinGapBetweenQuads is only meaningful for DISJOINT quads
 * - two crossing quads read a gap from their vertices and could slip past it.
 *
 * PlaceAgainstTheBackFence stored UNPADDED corners in Taken until issue #193, so a back-fence
 * shed's gap to whatever the sampler stood beside it was half of ClearanceUu - and grep
 * across this file's tests before that fix found no gap measured anywhere.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotYardKeepsClearanceBetweenEveryPairTest,
	"Airside.Solve.PlotYardKeepsClearanceBetweenEveryPair",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotYardKeepsClearanceBetweenEveryPairTest::RunTest(const FString& Parameters)
{
	// TIGHT, BUT NOT SO TIGHT NOTHING FITS: on a large plot a naive sampler passes by luck,
	// and on too small a one there is no pair left to compare. Small enough that stands
	// pack close together, so a shrunk clearance shows up as a measured gap rather than acres
	// of empty space either way.
	//
	// It was 8 m x 16 m until 2026-09-17, when the shed moved from the gateway to the back
	// fence. The shed then took the back, the gate corridor took the front, and on a plot
	// only 8 m wide the strips either side of the corridor are too narrow for anything -
	// one module stood and this test's own guard caught it.
	const TArray<FVector2D> Outline = YardRect(1600.0, 2000.0);
	const TArray<PlotYard::FKitSpec> Kits = DepotMix();

	int32 Pairs = 0;
	for (int32 Seed = 1; Seed <= 8; ++Seed)
	{
		const PlotYard::FReservation Reservation = PlotYard::Reserve(
			Outline, FVector2D(0.0, 0.0), FVector2D(1600.0, 0.0), FVector2D(800.0, 0.0),
			Kits, Seed);

		// AT LEAST TWO STANDING, or the loops below compare nothing and this test goes
		// green for the wrong reason.
		TestTrue(*FString::Printf(
			TEXT("seed %d: at least two modules stand, so there is a pair to compare"), Seed),
			Reservation.Stands.Num() >= 2);

		TArray<TArray<FVector2D>> Corners;
		Corners.SetNum(Reservation.Stands.Num());
		for (int32 Index = 0; Index < Reservation.Stands.Num(); ++Index)
		{
			PlotYard::StandCorners(Reservation.Stands[Index],
				Kits[Reservation.Stands[Index].KitIndex].Footprint, Corners[Index]);
		}

		for (int32 A = 0; A < Reservation.Stands.Num(); ++A)
		{
			for (int32 B = A + 1; B < Reservation.Stands.Num(); ++B)
			{
				++Pairs;
				// TWO MODULES IN ONE SPACE is the one failure that cannot be argued as
				// styling - it is a mesh through a mesh, and no camera angle hides it.
				TestFalse(*FString::Printf(TEXT("seed %d: stand %d and %d do not intersect"), Seed, A, B),
					PlotYard::StandsOverlap(Reservation.Stands[A], Kits[Reservation.Stands[A].KitIndex].Footprint,
						Reservation.Stands[B], Kits[Reservation.Stands[B].KitIndex].Footprint));

				// A ONE-UNIT TOLERANCE for CornerInsetUu, which pulls every corner in by 1 uu
				// and so trims a hair off the true footprint on both sides of the gap.
				const double Gap = MinGapBetweenQuads(Corners[A], Corners[B]);
				TestTrue(*FString::Printf(
					TEXT("seed %d: stand %d and %d are at least ClearanceUu apart, got %.1f"),
					Seed, A, B, Gap),
					Gap >= PlotYard::ClearanceUu - 1.0);
			}
		}
	}
	TestTrue(*FString::Printf(TEXT("pairs were actually measured (%d)"), Pairs), Pairs >= 8);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotYardVariesWithSeedTest,
	"Airside.Solve.PlotYardVariesWithSeed",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotYardVariesWithSeedTest::RunTest(const FString& Parameters)
{
	const TArray<FVector2D> Outline = YardRect(2400.0, 1600.0);
	const TArray<PlotYard::FKitSpec> Kits = DepotMix();

	auto LaySeeded = [&](int32 Seed)
	{
		return PlotYard::Reserve(Outline, FVector2D(0.0, 0.0), FVector2D(2400.0, 0.0),
			FVector2D(1200.0, 0.0), Kits, Seed);
	};

	const PlotYard::FReservation A = LaySeeded(1);
	const PlotYard::FReservation B = LaySeeded(2);
	if (!TestTrue(TEXT("both seeds reserve something, shed first"),
		A.Stands.Num() > 0 && B.Stands.Num() > 0 && A.Stands[0].KitIndex == 0 && B.Stands[0].KitIndex == 0))
	{
		return false;
	}

	// WITHOUT THIS, a solver that ignored the seed entirely - or one that quietly placed
	// everything on a grid again - would pass every other test in this file. Two depots
	// looking identical IS the complaint this whole feature answers. A different COUNT is a
	// difference too: the two reservations are not the same depot.
	bool bAnyDifference = A.Stands.Num() != B.Stands.Num();
	for (int32 Index = 0; Index < A.Stands.Num() && Index < B.Stands.Num() && !bAnyDifference; ++Index)
	{
		bAnyDifference = A.Stands[Index].Centre != B.Stands[Index].Centre
			|| A.Stands[Index].Heading != B.Stands[Index].Heading;
	}
	TestTrue(TEXT("two seeds lay out two different yards"), bAnyDifference);

	// THE SHED IS THE EXCEPTION and must NOT vary: its pose is functional, not decorative.
	TestTrue(TEXT("but the shed still faces the gate in both"),
		A.Stands[0].Heading == B.Stands[0].Heading);
	TestTrue(TEXT("and stands in the same place in both"),
		A.Stands[0].Centre == B.Stands[0].Centre);

	return true;
}

/**
 * EITHER WINDING FITS THE SAME PLOT - moved from the now-deleted
 * Airside.Solve.PlotFitFacesAwayFromRoad, which pinned the identical property for
 * PlotFit::FitBays before issue #182 retired it. PlotYard::InwardOf derives the interior
 * side from the outline's own signed area rather than assuming counter-clockwise, for the
 * reason its own comment gives: a plot whose clicks happened to run clockwise must still aim
 * its modules INTO the plot, not out across the road.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotYardInwardOfHonoursEitherWindingTest,
	"Airside.Solve.PlotYardInwardOfHonoursEitherWinding",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotYardInwardOfHonoursEitherWindingTest::RunTest(const FString& Parameters)
{
	const TArray<FVector2D> Ccw = YardRect(1200.0, 1200.0);
	const FVector2D InwardCcw =
		PlotYard::InwardOf(Ccw, FVector2D(0.0, 0.0), FVector2D(1200.0, 0.0));
	TestTrue(TEXT("a CCW plot's inward normal points north, into the plot"),
		InwardCcw.Y > 0.9);

	// THE SAME PLOT, WOUND THE OTHER WAY - the frontage travels with it, in the winding's
	// own direction, exactly as URoadEditFacade::PlaceEntityInPlot swaps FrontageA/B when it
	// corrects a clockwise outline to CCW for the mesh builder.
	TArray<FVector2D> Cw = Ccw;
	Algo::Reverse(Cw);
	const FVector2D InwardCw =
		PlotYard::InwardOf(Cw, FVector2D(1200.0, 0.0), FVector2D(0.0, 0.0));
	TestTrue(TEXT("a CW plot's inward normal still points north, into the plot"),
		InwardCw.Y > 0.9);

	return true;
}

#endif
