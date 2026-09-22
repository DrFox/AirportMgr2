#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Solve/PlotYard.h"
#include "Solve/RoadGeom.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** Same rectangle PlotYardTest's YardRect makes. NAMED APART because AirsideTests is a
	 *  UNITY build: two files' anonymous namespaces land in one translation unit, so a second
	 *  YardRect is a redefinition rather than a private copy. */
	TArray<FVector2D> ReserveRect(double Width, double Depth)
	{
		return { FVector2D(0.0, 0.0), FVector2D(Width, 0.0),
		         FVector2D(Width, Depth), FVector2D(0.0, Depth) };
	}

	/**
	 * Shed, tank, pump, at the grey-box figures and the design doc's 3/2/1 weights.
	 *
	 * DECLARED HERE RATHER THAN SHARED WITH PlotYardTest, whose Shed() names a FOOTPRINT.
	 * A spec is a footprint plus two integers, and a helper that served both files would
	 * have to grow a parameter for every field either one cares about.
	 */
	TArray<PlotYard::FKitSpec> DepotSpecs()
	{
		PlotYard::FKitSpec Shed;
		Shed.Footprint.LengthUu = 800.0;
		Shed.Footprint.WidthUu = 400.0;
		Shed.Footprint.bAgainstTheBackFence = true;
		Shed.ReserveWeight = 3;

		PlotYard::FKitSpec Tank;
		Tank.Footprint.LengthUu = 500.0;
		Tank.Footprint.WidthUu = 500.0;
		Tank.ReserveWeight = 2;

		PlotYard::FKitSpec Pump;
		Pump.Footprint.LengthUu = 300.0;
		Pump.Footprint.WidthUu = 200.0;
		Pump.ReserveWeight = 1;

		return { Shed, Tank, Pump };
	}

	/**
	 * The ground one stand actually occupies - the RUN's footprint, not one module's, end caps
	 * included. Through RunWidthUu, the product the solver itself reserves with.
	 */
	PlotYard::FFootprint RunFootprint(const TArray<PlotYard::FKitSpec>& Specs,
		const PlotYard::FReservedStand& Stand)
	{
		PlotYard::FFootprint Out = Specs[Stand.KitIndex].Footprint;
		Out.WidthUu = Specs[Stand.KitIndex].RunWidthUu(Stand.RunLength);
		return Out;
	}

	/**
	 * True when two stands' run footprints miss each other.
	 *
	 * Two convex shapes miss each other if and only if some axis separates them, so finding
	 * one is proof rather than evidence. Checked on the solver's own StandCorners so the test
	 * is not checking its own arithmetic.
	 */
	bool ReserveStandsSeparated(const TArray<PlotYard::FKitSpec>& Specs,
		const PlotYard::FReservedStand& StandA, const PlotYard::FReservedStand& StandB)
	{
		TArray<FVector2D> CornersA;
		TArray<FVector2D> CornersB;
		PlotYard::StandCorners(StandA, RunFootprint(Specs, StandA), CornersA);
		PlotYard::StandCorners(StandB, RunFootprint(Specs, StandB), CornersB);

		const FVector2D Axes[] = {
			(CornersA[1] - CornersA[0]).GetSafeNormal(),
			(CornersA[3] - CornersA[0]).GetSafeNormal(),
			(CornersB[1] - CornersB[0]).GetSafeNormal(),
			(CornersB[3] - CornersB[0]).GetSafeNormal() };

		for (const FVector2D& Axis : Axes)
		{
			double MinA = TNumericLimits<double>::Max();
			double MaxA = -TNumericLimits<double>::Max();
			double MinB = TNumericLimits<double>::Max();
			double MaxB = -TNumericLimits<double>::Max();
			for (const FVector2D& P : CornersA)
			{
				const double D = FVector2D::DotProduct(P, Axis);
				MinA = FMath::Min(MinA, D);
				MaxA = FMath::Max(MaxA, D);
			}
			for (const FVector2D& P : CornersB)
			{
				const double D = FVector2D::DotProduct(P, Axis);
				MinB = FMath::Min(MinB, D);
				MaxB = FMath::Max(MaxB, D);
			}
			if (MaxA < MinB || MaxB < MinA)
			{
				return true;
			}
		}
		return false;
	}
}

/**
 * A reservation contains only what it placed, and the ceilings are what it placed.
 *
 * "NEVER DROPS" IS NOT THE CLAIM LayOut MAKES. LayOut is handed a list it must try to honour
 * and reports what it could not fit; Reserve decides the list itself, so a dropped stand is
 * not a refusal, it is a bug. Every ghosted slot the player is shown is a promise that the
 * module fits there, and this test is what makes the promise true.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotReserveNeverDropsTest,
	"Airside.Solve.PlotReserveNeverDrops",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotReserveNeverDropsTest::RunTest(const FString& Parameters)
{
	const TArray<FVector2D> Outline = ReserveRect(3200.0, 2400.0);
	const TArray<PlotYard::FKitSpec> Specs = DepotSpecs();

	const PlotYard::FReservation Reservation = PlotYard::Reserve(
		Outline, FVector2D(0.0, 0.0), FVector2D(3200.0, 0.0), FVector2D(1600.0, 0.0),
		Specs, /*Seed=*/1234);

	TestTrue(TEXT("a 32 x 24 m plot reserves something"), Reservation.Stands.Num() > 0);

	for (const PlotYard::FReservedStand& Stand : Reservation.Stands)
	{
		TestTrue(TEXT("every reserved stand was placed"), Stand.bPlaced);
		TestTrue(TEXT("every reserved stand names a kit"),
			Specs.IsValidIndex(Stand.KitIndex));
		TestTrue(TEXT("every reserved stand holds at least one module"),
			Stand.RunLength >= 1);
	}

	// The ceilings are DERIVED from the stands, never counted alongside them: a second count
	// is a second thing to keep in agreement, for the same reason FYard::DroppedCount is
	// derived rather than stored.
	int32 Total = 0;
	for (int32 Kit = 0; Kit < Specs.Num(); ++Kit)
	{
		Total += Reservation.CeilingFor(Kit);
	}

	int32 Bays = 0;
	for (const PlotYard::FReservedStand& Stand : Reservation.Stands)
	{
		Bays += Stand.RunLength;
	}
	TestEqual(TEXT("the ceilings account for every reserved bay"), Total, Bays);

	return true;
}

/**
 * No two reserved stands overlap.
 *
 * THE INVARIANT THE GHOST SELLS. A ghosted slot promises the module fits there, and two
 * promises over one piece of ground is a mesh through a mesh - the one failure no camera
 * angle hides. Checked with the solver's own StandCorners so the test is not checking its
 * own arithmetic, which is the same reason that function is public at all.
 *
 * SEVERAL SEEDS, because one seed proves one roll and the sampler is what is under test.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotReserveNeverOverlapsTest,
	"Airside.Solve.PlotReserveNeverOverlaps",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotReserveNeverOverlapsTest::RunTest(const FString& Parameters)
{
	const TArray<FVector2D> Outline = ReserveRect(3200.0, 2400.0);
	const TArray<PlotYard::FKitSpec> Specs = DepotSpecs();

	for (int32 Seed = 0; Seed < 8; ++Seed)
	{
		const PlotYard::FReservation Reservation = PlotYard::Reserve(
			Outline, FVector2D(0.0, 0.0), FVector2D(3200.0, 0.0), FVector2D(1600.0, 0.0),
			Specs, Seed);

		for (int32 A = 0; A < Reservation.Stands.Num(); ++A)
		{
			for (int32 B = A + 1; B < Reservation.Stands.Num(); ++B)
			{
				const bool bSeparated = ReserveStandsSeparated(Specs,
					Reservation.Stands[A], Reservation.Stands[B]);

				TestTrue(*FString::Printf(
					TEXT("seed %d: stands %d and %d do not overlap"), Seed, A, B),
					bSeparated);
			}
		}
	}

	return true;
}

/**
 * Weights set the ratio: more sheds than tanks, more tanks than pumps.
 *
 * NOT AN EXACT 3:2:1. The cycle is three sheds, two tanks, one pump, but a shed is 32 m2
 * against a pump's 6, so the plot runs out of room for sheds long before it runs out for
 * pumps and the tail of the fill is small things. The ORDERING within a cycle is what the
 * weights buy and what is asserted; an exact ratio would be asserting the plot's area.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotReserveHonoursWeightsTest,
	"Airside.Solve.PlotReserveHonoursWeights",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotReserveHonoursWeightsTest::RunTest(const FString& Parameters)
{
	const TArray<FVector2D> Outline = ReserveRect(3200.0, 2400.0);
	const TArray<PlotYard::FKitSpec> Specs = DepotSpecs();

	const PlotYard::FReservation Reservation = PlotYard::Reserve(
		Outline, FVector2D(0.0, 0.0), FVector2D(3200.0, 0.0), FVector2D(1600.0, 0.0),
		Specs, /*Seed=*/1234);

	// TWO SHEDS, NOT ONE, and stated up front rather than guarded for below: the ordering
	// claim needs a second shed and a first pump to exist, and an "if they both exist" guard
	// would let this whole test pass vacuously on a plot that reserved neither.
	TestTrue(*FString::Printf(TEXT("a 32 x 24 m plot holds at least two sheds, got %d"),
		Reservation.CeilingFor(0)), Reservation.CeilingFor(0) >= 2);
	TestTrue(*FString::Printf(TEXT("and at least one tank, got %d"),
		Reservation.CeilingFor(1)), Reservation.CeilingFor(1) >= 1);
	TestTrue(*FString::Printf(TEXT("and at least one pump, got %d"),
		Reservation.CeilingFor(2)), Reservation.CeilingFor(2) >= 1);

	// EVERY KIT GETS A FOOTHOLD FIRST, one bay each, largest first - so the opening stands are
	// shed, tank, pump whatever the weights say. Without that pass a weight-3 shed took three
	// bays before the pump was offered the yard at all, and on the concept sheet's 12 x 8 m
	// plot that shed run WAS the whole plot: the Tier 1 depot stopped fitting in its own site.
	const int32 Footholds = Specs.Num();
	if (!TestTrue(TEXT("the plot is big enough to reserve past its footholds"),
		Reservation.Stands.Num() > Footholds))
	{
		return false;
	}
	for (int32 I = 0; I < Footholds; ++I)
	{
		TestEqual(TEXT("a foothold is one bay"), Reservation.Stands[I].RunLength, 1);
	}
	TestEqual(TEXT("the first foothold is the biggest kit"),
		Reservation.Stands[0].KitIndex, 0);

	// A HEAVIER KIT IS OFFERED THE YARD MORE OFTEN - AFTER the footholds, which is where the
	// weights actually apply. Sheds are weight 3 and pumps weight 1, so the cycle offers
	// three sheds before it offers a pump.
	int32 NextShed = INDEX_NONE;
	int32 NextPump = INDEX_NONE;
	for (int32 I = Footholds; I < Reservation.Stands.Num(); ++I)
	{
		if (Reservation.Stands[I].KitIndex == 0 && NextShed == INDEX_NONE) { NextShed = I; }
		if (Reservation.Stands[I].KitIndex == 2 && NextPump == INDEX_NONE) { NextPump = I; }
	}

	if (!TestTrue(TEXT("a further shed and a further pump were both reserved"),
		NextShed != INDEX_NONE && NextPump != INDEX_NONE))
	{
		return false;
	}
	TestTrue(*FString::Printf(
		TEXT("past the footholds a shed is reserved before a pump: %d then %d"),
		NextShed, NextPump), NextShed < NextPump);

	return true;
}

/**
 * Same plot and seed, same stands - to the bit.
 *
 * LOAD-BEARING, not a nicety. Nothing about the reservation is saved; it is re-derived every
 * time the presenter rebuilds. A solve that wandered would move a player's built depot when
 * they laid a road somewhere else on the airport.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotReserveIsDeterministicTest,
	"Airside.Solve.PlotReserveIsDeterministic",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotReserveIsDeterministicTest::RunTest(const FString& Parameters)
{
	const TArray<FVector2D> Outline = ReserveRect(3200.0, 2400.0);
	const TArray<PlotYard::FKitSpec> Specs = DepotSpecs();

	const PlotYard::FReservation A = PlotYard::Reserve(Outline, FVector2D(0.0, 0.0),
		FVector2D(3200.0, 0.0), FVector2D(1600.0, 0.0), Specs, /*Seed=*/77);
	const PlotYard::FReservation B = PlotYard::Reserve(Outline, FVector2D(0.0, 0.0),
		FVector2D(3200.0, 0.0), FVector2D(1600.0, 0.0), Specs, /*Seed=*/77);

	if (!TestEqual(TEXT("the same plot reserves the same number of stands"),
		A.Stands.Num(), B.Stands.Num()))
	{
		return false;
	}

	for (int32 I = 0; I < A.Stands.Num(); ++I)
	{
		TestEqual(TEXT("same kit"), A.Stands[I].KitIndex, B.Stands[I].KitIndex);
		TestEqual(TEXT("same run length"), A.Stands[I].RunLength, B.Stands[I].RunLength);
		TestTrue(TEXT("same centre, exactly"),
			A.Stands[I].Centre.Equals(B.Stands[I].Centre, 0.0));
		TestEqual(TEXT("same heading"), A.Stands[I].Heading, B.Stands[I].Heading);
	}

	return true;
}

/**
 * No reserved stand stands in the gateway.
 *
 * A DEPOT THE TRUCK CANNOT LEAVE LOOKS PERFECTLY CORRECT FROM EVERY ANGLE - the reason
 * GateCorridorUu is a counted rule rather than something eyeballed in PIE.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotReserveLeavesTheGateClearTest,
	"Airside.Solve.PlotReserveLeavesTheGateClear",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotReserveLeavesTheGateClearTest::RunTest(const FString& Parameters)
{
	const TArray<FVector2D> Outline = ReserveRect(3200.0, 2400.0);
	const TArray<PlotYard::FKitSpec> Specs = DepotSpecs();
	const FVector2D Gate(1600.0, 0.0);
	const FVector2D Inward(0.0, 1.0);
	const FVector2D Across(1.0, 0.0);

	for (int32 Seed = 0; Seed < 8; ++Seed)
	{
		const PlotYard::FReservation Reservation = PlotYard::Reserve(
			Outline, FVector2D(0.0, 0.0), FVector2D(3200.0, 0.0), Gate, Specs, Seed);

		TArray<FVector2D> Corners;
		for (const PlotYard::FReservedStand& Stand : Reservation.Stands)
		{
			// THE SHED IS EXEMPT, exactly as it is under LayOut: it stands ON the gate's ray
			// by construction, pushed as deep as the plot allows, and the corridor rule is
			// what the SAMPLER obeys. Asserting it here would be asserting that the back
			// fence is more than one truck length from the road, which is a claim about the
			// plot the player drew.
			if (Specs[Stand.KitIndex].Footprint.bAgainstTheBackFence)
			{
				continue;
			}

			PlotYard::StandCorners(Stand, RunFootprint(Specs, Stand), Corners);

			for (const FVector2D& Corner : Corners)
			{
				const FVector2D FromGate = Corner - Gate;
				const double Into = FVector2D::DotProduct(FromGate, Inward);
				const bool bInCorridor = Into >= 0.0 && Into <= PlotYard::GateCorridorUu
					&& FMath::Abs(FVector2D::DotProduct(FromGate, Across))
						< PlotYard::GateCorridorUu * 0.5;
				TestFalse(*FString::Printf(TEXT("seed %d keeps the gate clear"), Seed),
					bInCorridor);
			}
		}
	}

	return true;
}

/**
 * Three sheds become one stand three bays wide, at one heading.
 *
 * RESERVED AT FULL WIDTH UP FRONT, never grown. A run that grew as the player bought bays
 * would need ground it was never promised, and the promise is the whole design: every
 * ghosted slot is a claim that the module fits there.
 *
 * NO JITTER INSIDE A RUN. The bays share walls, so a heading that wandered between them
 * would open a wedge of daylight down the middle of one building.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotReserveGroupsShedsIntoRunsTest,
	"Airside.Solve.PlotReserveGroupsShedsIntoRuns",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotReserveGroupsShedsIntoRunsTest::RunTest(const FString& Parameters)
{
	const TArray<FVector2D> Outline = ReserveRect(3200.0, 2400.0);

	TArray<PlotYard::FKitSpec> Specs = DepotSpecs();
	Specs[0].RunCap = 3;

	const PlotYard::FReservation Reservation = PlotYard::Reserve(
		Outline, FVector2D(0.0, 0.0), FVector2D(3200.0, 0.0), FVector2D(1600.0, 0.0),
		Specs, /*Seed=*/1234);

	int32 ShedStands = 0;
	int32 LongestRun = 0;
	for (const PlotYard::FReservedStand& Stand : Reservation.Stands)
	{
		if (Stand.KitIndex != 0)
		{
			// A KIT WITH RunCap 1 IS NEVER GROUPED, stated rather than assumed: a run rule
			// that ignored the cap would quietly weld two tanks into one object.
			TestEqual(TEXT("an ungrouped kit holds exactly one module"),
				Stand.RunLength, 1);
			continue;
		}
		++ShedStands;
		TestTrue(*FString::Printf(TEXT("a shed stand holds 1..RunCap bays, got %d"),
			Stand.RunLength), Stand.RunLength >= 1 && Stand.RunLength <= 3);
		LongestRun = FMath::Max(LongestRun, Stand.RunLength);
	}

	TestTrue(TEXT("a 32 x 24 m plot reserves at least one shed run"), ShedStands > 0);

	// GROUPING IS THE POINT, not an option the solver may decline. A plot this size has room
	// for a full run, and a solver that placed three one-bay stands would pass every
	// assertion above while doing none of the work.
	TestEqual(TEXT("and at least one run is full"), LongestRun, 3);

	// Grouping must not COST the plot sheds: three bays in one 12 m run occupy less ground
	// than three scattered 4 m sheds each carrying a 1 m clearance skirt.
	TestTrue(*FString::Printf(TEXT("grouping does not cost the plot sheds, got %d"),
		Reservation.CeilingFor(0)), Reservation.CeilingFor(0) >= 3);

	return true;
}

/**
 * Only one shed run stands against the back fence.
 *
 * THE RAY IS ONE LINE OF GROUND. PlaceAgainstTheBackFence walks depths along the gate's
 * inward ray, so a second stand offered the same ray either lands on the first or is
 * refused. The rest of the runs are sampled like anything else. Worth watching in PIE - if a
 * second run adrift in the yard reads wrong, cap shed runs at one rather than widening the
 * ray.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotReserveBacksOneRunOnlyTest,
	"Airside.Solve.PlotReserveBacksOneRunOnly",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotReserveBacksOneRunOnlyTest::RunTest(const FString& Parameters)
{
	// Deep and wide enough that more than one shed run fits.
	const TArray<FVector2D> Outline = ReserveRect(6000.0, 3000.0);

	TArray<PlotYard::FKitSpec> Specs = DepotSpecs();
	Specs[0].RunCap = 3;

	const PlotYard::FReservation Reservation = PlotYard::Reserve(
		Outline, FVector2D(0.0, 0.0), FVector2D(6000.0, 0.0), FVector2D(3000.0, 0.0),
		Specs, /*Seed=*/1234);

	// The back-fence heading is the inward bearing EXACTLY - the sampler always adds a
	// jitter, so counting stands at exactly that heading counts the ones that took the ray.
	int32 Squared = 0;
	int32 ShedStands = 0;
	for (const PlotYard::FReservedStand& Stand : Reservation.Stands)
	{
		if (Stand.KitIndex != 0)
		{
			continue;
		}
		++ShedStands;
		if (Stand.Heading == UE_DOUBLE_HALF_PI)
		{
			++Squared;
		}
	}

	// More than one shed run, or the claim below is about nothing.
	TestTrue(*FString::Printf(TEXT("a 60 x 30 m plot holds more than one shed run, got %d"),
		ShedStands), ShedStands > 1);
	TestEqual(TEXT("exactly one shed run is square against the back fence"), Squared, 1);

	return true;
}

/**
 * A run reserves its end caps: FKitSpec::RunWidthUu is the width Reserve samples with, so
 * runs of a capped kit never overlap once the caps are counted.
 *
 * CAPS WIDER THAN HALF THE CLEARANCE, deliberately. PlotYard keeps ClearanceUu between
 * stands, so a solver that sampled bare N x width would still stand two runs 100 uu apart -
 * and two 150 uu caps meeting across that gap overlap by 200. A cap narrower than half the
 * clearance would pass whether or not Reserve counted it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotReserveRunsClaimTheirEndCapsTest,
	"Airside.Solve.PlotReserveRunsClaimTheirEndCaps",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotReserveRunsClaimTheirEndCapsTest::RunTest(const FString& Parameters)
{
	TArray<PlotYard::FKitSpec> Specs = DepotSpecs();
	Specs[0].RunCap = 3;
	Specs[0].RunEndUu = 150.0;

	TestEqual(TEXT("a three-bay run is three bays and two caps wide"),
		Specs[0].RunWidthUu(3), 400.0 * 3 + 150.0 * 2);
	TestEqual(TEXT("and a kit with no caps is exactly N modules wide, as before caps"),
		Specs[1].RunWidthUu(2), 500.0 * 2);

	const TArray<FVector2D> Outline = ReserveRect(6000.0, 3000.0);
	int32 ShedRuns = 0;
	for (int32 Seed = 0; Seed < 8; ++Seed)
	{
		const PlotYard::FReservation Reservation = PlotYard::Reserve(
			Outline, FVector2D(0.0, 0.0), FVector2D(6000.0, 0.0), FVector2D(3000.0, 0.0),
			Specs, Seed);

		for (int32 A = 0; A < Reservation.Stands.Num(); ++A)
		{
			ShedRuns += Reservation.Stands[A].KitIndex == 0 ? 1 : 0;
			for (int32 B = A + 1; B < Reservation.Stands.Num(); ++B)
			{
				TestTrue(*FString::Printf(
					TEXT("seed %d: stands %d and %d do not overlap, caps counted"), Seed, A, B),
					ReserveStandsSeparated(Specs, Reservation.Stands[A], Reservation.Stands[B]));
			}
		}
	}

	// SEVERAL SHED RUNS, or the claim is about nothing: one run has no neighbour to overlap.
	TestTrue(*FString::Printf(TEXT("the plot held several capped runs, got %d over 8 seeds"),
		ShedRuns), ShedRuns > 8);
	return true;
}

#endif
