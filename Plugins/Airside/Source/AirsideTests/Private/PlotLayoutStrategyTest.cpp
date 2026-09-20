#include "Build/PlotLayoutStrategy.h"
#include "CoreMinimal.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Solve/PlotYard.h"
#include "Solve/RoadGeom.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** NAMED APART from PlotReserveTest's ReserveRect: AirsideTests is a unity build, so a
	 *  second definition in another file is a redefinition rather than a private copy. */
	TArray<FVector2D> StrategyRect(double Width, double Depth)
	{
		return { FVector2D(0.0, 0.0), FVector2D(Width, 0.0),
		         FVector2D(Width, Depth), FVector2D(0.0, Depth) };
	}

	/** The grey-box depot, with the shed's four-metre apron. */
	TArray<PlotYard::FKitSpec> StrategySpecs()
	{
		PlotYard::FKitSpec Shed;
		Shed.Footprint.LengthUu = 800.0;
		Shed.Footprint.WidthUu = 400.0;
		Shed.Footprint.bAgainstTheBackFence = true;
		Shed.ApronUu = FVector2D(400.0, 0.0);
		Shed.ReserveWeight = 3;
		Shed.RunCap = 3;

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
	 * A plot that is NOT a rectangle: the back edge is wider than the frontage.
	 *
	 * THE ORDINARY CASE, not an edge case. The gesture pins four corners freely, so a slightly
	 * off-square plot is what a player actually draws - and every other fixture here is a
	 * rectangle, which is why a column placed at the plot's WIDEST lateral extent looked
	 * correct in tests and put both columns outside the plot in PIE.
	 */
	TArray<FVector2D> FlaredRect(double FrontWidth, double BackWidth, double Depth)
	{
		const double Flare = (BackWidth - FrontWidth) * 0.5;
		return { FVector2D(0.0, 0.0), FVector2D(FrontWidth, 0.0),
		         FVector2D(FrontWidth + Flare, Depth), FVector2D(-Flare, Depth) };
	}

	FPlotSite StrategySite(const TArray<FVector2D>& Outline, double Width)
	{
		FPlotSite Site;
		Site.Outline = Outline;
		Site.FrontageA = FVector2D(0.0, 0.0);
		Site.FrontageB = FVector2D(Width, 0.0);
		Site.Gate = FVector2D(Width * 0.5, 0.0);
		Site.Seed = 1234;
		return Site;
	}
}

/**
 * The scatter strategy is the scatter, exactly.
 *
 * A WRAPPER AND NOTHING MORE. PlotYard::Reserve has seven tests written against it and they
 * keep testing the thing they were written for only while this adds no behaviour of its own.
 * If these two ever disagree, the seven tests are describing code nobody runs.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FScatterStrategyIsTheScatterTest,
	"Airside.Build.ScatterStrategyIsTheScatter",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FScatterStrategyIsTheScatterTest::RunTest(const FString& Parameters)
{
	const TArray<FVector2D> Outline = StrategyRect(3200.0, 2400.0);
	const TArray<PlotYard::FKitSpec> Specs = StrategySpecs();
	const FPlotSite Site = StrategySite(Outline, 3200.0);

	const UScatterLayoutStrategy* Strategy = NewObject<UScatterLayoutStrategy>();
	const PlotYard::FReservation Mine = Strategy->Solve(Site, Specs);
	const PlotYard::FReservation Theirs = PlotYard::Reserve(
		Site.Outline, Site.FrontageA, Site.FrontageB, Site.Gate, Specs, Site.Seed);

	if (!TestEqual(TEXT("the same number of stands"),
		Mine.Stands.Num(), Theirs.Stands.Num()))
	{
		return false;
	}
	TestTrue(TEXT("and a plot this size reserves something"), Mine.Stands.Num() > 0);

	for (int32 I = 0; I < Mine.Stands.Num(); ++I)
	{
		TestEqual(TEXT("same kit"), Mine.Stands[I].KitIndex, Theirs.Stands[I].KitIndex);
		TestEqual(TEXT("same run length"),
			Mine.Stands[I].RunLength, Theirs.Stands[I].RunLength);
		TestTrue(TEXT("same centre, exactly"),
			Mine.Stands[I].Centre.Equals(Theirs.Stands[I].Centre, 0.0));
		TestEqual(TEXT("same heading"), Mine.Stands[I].Heading, Theirs.Stands[I].Heading);
	}

	return true;
}

/**
 * Every layout resolves to a strategy.
 *
 * WALKED, NOT LISTED, which is the lesson AircraftLookTest paid for: a test that names its
 * subjects catches only the subjects somebody remembered. A layout added to the enum with no
 * strategy behind it fails here rather than drawing an empty depot in a shipped build.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEveryPlotLayoutResolvesTest,
	"Airside.Build.EveryPlotLayoutResolves",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FEveryPlotLayoutResolvesTest::RunTest(const FString& Parameters)
{
	const TArray<FVector2D> Outline = StrategyRect(3200.0, 2400.0);
	const TArray<PlotYard::FKitSpec> Specs = StrategySpecs();
	const FPlotSite Site = StrategySite(Outline, 3200.0);

	for (int32 Raw = 0; Raw <= static_cast<int32>(EPlotLayout::FuelYardBands); ++Raw)
	{
		const EPlotLayout Layout = static_cast<EPlotLayout>(Raw);
		const UPlotLayoutStrategy* Strategy = PlotLayoutFor(Layout);

		if (!TestNotNull(*FString::Printf(TEXT("layout %d has a strategy"), Raw), Strategy))
		{
			continue;
		}

		// AND IT ANSWERS. A strategy that resolved but returned nothing would pass a null
		// check and draw an empty plot, which is the failure this walk is for.
		const PlotYard::FReservation R = Strategy->Solve(Site, Specs);
		TestTrue(*FString::Printf(TEXT("layout %d reserves something on a 32 x 24 m plot"),
			Raw), R.Stands.Num() > 0);
	}

	// AND THE FUEL DEPOT ASKS FOR THE BAND LAYOUT rather than defaulting into it - the
	// default is the scatter, so a definition that never stated a layout keeps the old
	// behaviour instead of silently changing shape.
	const UEntityDefinition* Depot = UEntityDefinition::MakeFuelDepotTransient();
	if (TestNotNull(TEXT("a depot definition"), Depot))
	{
		TestEqual(TEXT("a fuel depot lays out in bands"),
			static_cast<int32>(Depot->Layout),
			static_cast<int32>(EPlotLayout::FuelYardBands));
	}

	return true;
}

/**
 * The band yard leaves room to work in.
 *
 * THE NUMBER THAT PROMPTED THIS: a 45 x 35 m plot reserved 23 sheds, 9 tanks and 12 pumps -
 * 1,033 m2 of building on 1,575 m2 of ground, 65% coverage, wall to wall. "It is not a
 * practical fuel yard."
 *
 * 30% IS A CEILING, NOT A TARGET, and it is asserted rather than tuned to: a band layout that
 * crept back above it has stopped leaving a yard, whatever else it does.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFuelYardLeavesRoomTest,
	"Airside.Build.FuelYardLeavesRoom",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFuelYardLeavesRoomTest::RunTest(const FString& Parameters)
{
	// The plot from the screenshot: 45 m of frontage, 35 m deep.
	const TArray<FVector2D> Outline = StrategyRect(4500.0, 3500.0);
	const TArray<PlotYard::FKitSpec> Specs = StrategySpecs();
	const FPlotSite Site = StrategySite(Outline, 4500.0);

	const UPlotLayoutStrategy* Strategy = PlotLayoutFor(EPlotLayout::FuelYardBands);
	if (!TestNotNull(TEXT("a band strategy"), Strategy)) { return false; }

	const PlotYard::FReservation R = Strategy->Solve(Site, Specs);
	if (!TestTrue(TEXT("a 45 x 35 m plot holds something"), R.Stands.Num() > 0))
	{
		return false;
	}

	double Covered = 0.0;
	for (const PlotYard::FReservedStand& Stand : R.Stands)
	{
		const PlotYard::FKitSpec& Kit = Specs[Stand.KitIndex];
		Covered += Kit.Footprint.LengthUu * Kit.Footprint.WidthUu * Stand.RunLength;
	}
	const double Coverage = Covered / (4500.0 * 3500.0);

	TestTrue(*FString::Printf(TEXT("coverage is under 30 percent, got %.0f"),
		Coverage * 100.0), Coverage < 0.30);

	// AND EVERY KIT IS REPRESENTED. A floor, not a ceiling.
	//
	// THE CEILING WAS REMOVED ON PURPOSE. It used to assert at most twelve sheds and six
	// pumps, which were numbers I picked - and picking them is the thing this design does not
	// do. Coverage above IS the statement about what a yard looks like; a hand-set count
	// beside it would be a second opinion, and the first one to move when the layout changed.
	for (int32 Kit = 0; Kit < Specs.Num(); ++Kit)
	{
		TestTrue(*FString::Printf(TEXT("a 45 x 35 m plot holds kit %d, got %d"),
			Kit, R.CeilingFor(Kit)), R.CeilingFor(Kit) >= 1);
	}

	return true;
}

/**
 * The smallest DRAWABLE plot still holds the concept sheet depot.
 *
 * ONE OF EACH - "Essential fuel infrastructure for general aviation airfields. Compact,
 * reliable, easy to maintain." A layout that needs a big plot before it produces anything has
 * moved the Tier 1 depot out of reach of the tier it is for, which is a subtler failure than
 * 65% coverage and a harder one to see.
 *
 * 15 x 12 m, AND NEITHER FIGURE IS THE SHEET'S 11 x 8.
 *
 * THE DEPTH went to 12 m because an 8 m shed in an 8 m site leaves nothing in front of the
 * door, and with its apron the depot did not fit its own plot at all - see
 * PlotFit::BayDepthUu, which this test moved.
 *
 * THE WIDTH is 15 m because the columns need 13 m - tank 5.0 + clearance 1.0 + shed 4.0 +
 * clearance 1.0 + pump 2.0 - and FPlotPlaceTool::FrontageStepUu snaps a frontage to 5 m
 * steps, so 13 m is not drawable and 15 m is the next one that is. An 11 m plot was never
 * drawable either; the sheet's figure is art, not a gesture the player can make.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFuelYardFitsTheConceptSheetTest,
	"Airside.Build.FuelYardFitsTheConceptSheet",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFuelYardFitsTheConceptSheetTest::RunTest(const FString& Parameters)
{
	const TArray<FVector2D> Outline = StrategyRect(1500.0, 1200.0);
	const TArray<PlotYard::FKitSpec> Specs = StrategySpecs();
	const FPlotSite Site = StrategySite(Outline, 1500.0);

	const PlotYard::FReservation R =
		PlotLayoutFor(EPlotLayout::FuelYardBands)->Solve(Site, Specs);

	// AT LEAST ONE OF EACH, and not many more: this is the smallest depot that works, so a
	// layout that fits five sheds here has packed the plot rather than laid it out.
	for (int32 Kit = 0; Kit < Specs.Num(); ++Kit)
	{
		const int32 Held = R.CeilingFor(Kit);
		TestTrue(*FString::Printf(TEXT("kit %d gets at least one, got %d"), Kit, Held),
			Held >= 1);
		TestTrue(*FString::Printf(TEXT("kit %d gets no more than three, got %d"), Kit, Held),
			Held <= 3);
	}

	return true;
}

/**
 * Nothing overlaps, and nothing stands on a shed apron.
 *
 * THE APRON IS THE POINT OF THE FIELD. A tank parked in front of a shed door is a depot whose
 * truck cannot get out, and it looks perfectly correct from every angle - the same failure
 * the gate corridor exists to prevent, one step further in.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFuelYardKeepsApronsClearTest,
	"Airside.Build.FuelYardKeepsApronsClear",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFuelYardKeepsApronsClearTest::RunTest(const FString& Parameters)
{
	const TArray<FVector2D> Outline = StrategyRect(4500.0, 3500.0);
	const TArray<PlotYard::FKitSpec> Specs = StrategySpecs();
	const FPlotSite Site = StrategySite(Outline, 4500.0);

	const PlotYard::FReservation R =
		PlotLayoutFor(EPlotLayout::FuelYardBands)->Solve(Site, Specs);

	// The ground each stand actually claims: footprint plus apron, run width included.
	auto Claimed = [&Specs](const PlotYard::FReservedStand& Stand)
	{
		const PlotYard::FKitSpec& Kit = Specs[Stand.KitIndex];
		PlotYard::FFootprint Out;
		Out.LengthUu = Kit.Footprint.LengthUu + Kit.ApronUu.X;
		Out.WidthUu = Kit.Footprint.WidthUu * Stand.RunLength + Kit.ApronUu.Y * 2.0;
		return Out;
	};

	for (int32 A = 0; A < R.Stands.Num(); ++A)
	{
		for (int32 B = A + 1; B < R.Stands.Num(); ++B)
		{
			TestFalse(*FString::Printf(TEXT("stands %d and %d claim separate ground"), A, B),
				PlotYard::StandsOverlap(R.Stands[A], Claimed(R.Stands[A]),
					R.Stands[B], Claimed(R.Stands[B])));
		}
	}

	return true;
}

/**
 * Sheds stand at the back, square, facing the gate.
 *
 * FUNCTIONAL, NOT DECORATIVE: a truck drives out of a shed, so its heading is the one this
 * layout may not turn for looks. BuildFuelDepot records that +X faces AWAY from the road, and
 * that this was once written the wrong way round.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFuelYardStandsShedsAtTheBackTest,
	"Airside.Build.FuelYardStandsShedsAtTheBack",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFuelYardStandsShedsAtTheBackTest::RunTest(const FString& Parameters)
{
	const TArray<FVector2D> Outline = StrategyRect(4500.0, 3500.0);
	const TArray<PlotYard::FKitSpec> Specs = StrategySpecs();
	const FPlotSite Site = StrategySite(Outline, 4500.0);

	const PlotYard::FReservation R =
		PlotLayoutFor(EPlotLayout::FuelYardBands)->Solve(Site, Specs);

	int32 Sheds = 0;
	for (const PlotYard::FReservedStand& Stand : R.Stands)
	{
		if (Stand.KitIndex != 0) { continue; }
		++Sheds;

		// Interior is +Y here, so the inward bearing is +90 degrees, exactly - no jitter.
		TestEqual(TEXT("a shed is square to the frontage"),
			Stand.Heading, UE_DOUBLE_HALF_PI);

		// AT THE BACK: the 8 m shed plus its 4 m apron is 12 m of claimed depth, so a stand
		// hard against a 35 m back fence has its centre at 35 - 6 = 29 m.
		TestTrue(*FString::Printf(TEXT("a shed is at the back, got y %.0f"), Stand.Centre.Y),
			FMath::IsNearlyEqual(Stand.Centre.Y, 2900.0, 1.0));
	}
	TestTrue(TEXT("sheds were placed"), Sheds > 0);

	return true;
}

/**
 * No column runs up the side of the sheds.
 *
 * A COLUMN SITS AGAINST THE PLOT EDGE, so the only way to a tank is across the middle of the
 * yard - and at the shed band's depth the middle IS sheds. A column that runs past them walls
 * its far end in between the fence and the shed row.
 *
 * PIE ON 2026-09-20 DID EXACTLY THAT: five tanks up the left edge and eight pumps up the
 * right, with the back of each column unreachable. "Some of the pumps and the tanks are
 * inaccessible as they are down the side of the sheds."
 *
 * THIS IS NOT A REACHABILITY PROOF, and the difference matters. Nothing here traces a route;
 * it refuses the one arrangement that provably has none. Circulation is out of scope - design
 * doc section 3 - and this test is the floor under that decision, not a substitute for it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFuelYardKeepsColumnsClearOfTheShedsTest,
	"Airside.Build.FuelYardKeepsColumnsClearOfTheSheds",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFuelYardKeepsColumnsClearOfTheShedsTest::RunTest(const FString& Parameters)
{
	const TArray<PlotYard::FKitSpec> Specs = StrategySpecs();

	// SEVERAL PLOT SIZES, because the failure showed up on a big one: a small plot has no
	// room for a column long enough to reach the sheds, and would pass while proving nothing.
	for (double WidthUu = 1500.0; WidthUu <= 6000.0; WidthUu += 500.0)
	{
		for (double DepthUu = 1200.0; DepthUu <= 4000.0; DepthUu += 400.0)
		{
			const TArray<FVector2D> Outline = StrategyRect(WidthUu, DepthUu);
			const FPlotSite Site = StrategySite(Outline, WidthUu);
			const PlotYard::FReservation R =
				PlotLayoutFor(EPlotLayout::FuelYardBands)->Solve(Site, Specs);

			// Inward is +Y here and the gate is on y = 0, so depth is simply Centre.Y.
			double ShedFront = TNumericLimits<double>::Max();
			for (const PlotYard::FReservedStand& Stand : R.Stands)
			{
				if (Stand.KitIndex != 0) { continue; }
				const double Claimed =
					Specs[0].Footprint.LengthUu + Specs[0].ApronUu.X;
				ShedFront = FMath::Min(ShedFront, Stand.Centre.Y - Claimed * 0.5);
			}

			if (ShedFront == TNumericLimits<double>::Max())
			{
				continue;  // No sheds on this plot, so nothing to be walled in by.
			}

			// AT MOST ONE PER FILE may stand beside the sheds: the first, which is reached
			// straight from the gate. A second one there is behind the first, with a
			// neighbour on one side and the shed row on the other.
			//
			// PER FILE, NOT PER KIT. A band grows sideways when it runs out of depth, and
			// every file's own first stand is reachable the same way - so a kit with three
			// files may legitimately have three stands at that depth, one per file. Counting
			// per kit called two a failure when it was the feature working.
			//
			// KEYED ON THE LATERAL OFFSET, rounded to 10 cm, because that is what a file IS:
			// stands sharing a lateral position and marching into the plot. Inward is +Y in
			// these fixtures, so the lateral offset is simply the centre's X.
			TMap<TPair<int32, int32>, int32> BesidePerFile;

			for (const PlotYard::FReservedStand& Stand : R.Stands)
			{
				if (Stand.KitIndex == 0) { continue; }
				const double Claimed =
					Specs[Stand.KitIndex].Footprint.LengthUu + Specs[Stand.KitIndex].ApronUu.X;
				if (Stand.Centre.Y + Claimed * 0.5 > ShedFront + 1.0)
				{
					const TPair<int32, int32> File(
						Stand.KitIndex, FMath::RoundToInt(Stand.Centre.X / 10.0));
					BesidePerFile.FindOrAdd(File) += 1;
				}
			}

			for (const TPair<TPair<int32, int32>, int32>& File : BesidePerFile)
			{
				TestTrue(*FString::Printf(
					TEXT("%.0f x %.0f: kit %d file at x=%d has one beside the sheds, got %d"),
					WidthUu, DepthUu, File.Key.Key, File.Key.Value * 10, File.Value),
					File.Value <= 1);
			}
		}
	}

	return true;
}

/**
 * A plot that is not square still gets its tanks and pumps.
 *
 * PIE ON 2026-09-20: "I can get a 45 meter plot to show 6 sheds and 0 tanks and 0 pumps if it
 * is slightly off square." Six is the shed cap, so the sheds were fine and BOTH COLUMNS HAD
 * VANISHED - placed at the plot's widest lateral extent, which on a flared quad is at the
 * back, and therefore outside the outline down at the frontage where the column starts. The
 * first stand failed containment and the band returned having placed nothing.
 *
 * EVERY OTHER FIXTURE IN THIS FILE IS A RECTANGLE, which is exactly why this passed the
 * tests and failed the moment somebody drew a real plot. The gesture pins four corners
 * freely; square is the special case.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFuelYardFillsASkewedPlotTest,
	"Airside.Build.FuelYardFillsASkewedPlot",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFuelYardFillsASkewedPlotTest::RunTest(const FString& Parameters)
{
	const TArray<PlotYard::FKitSpec> Specs = StrategySpecs();

	// Flared, pinched, and square, at the 45 m frontage that showed the failure. A pinched
	// plot is the mirror case: widest at the FRONT, so a naive HalfSpan is wrong at the back.
	struct FCase { const TCHAR* What; TArray<FVector2D> Outline; double Frontage; };
	const TArray<FCase> Cases = {
		{ TEXT("flared"),  FlaredRect(4500.0, 5500.0, 3000.0), 4500.0 },
		{ TEXT("pinched"), FlaredRect(4500.0, 3500.0, 3000.0), 4500.0 },
		{ TEXT("square"),  StrategyRect(4500.0, 3000.0),       4500.0 },
	};

	for (const FCase& Case : Cases)
	{
		const PlotYard::FReservation R =
			PlotLayoutFor(EPlotLayout::FuelYardBands)->Solve(
				StrategySite(Case.Outline, Case.Frontage), Specs);

		for (int32 Kit = 0; Kit < Specs.Num(); ++Kit)
		{
			TestTrue(*FString::Printf(TEXT("a %s 45 m plot holds kit %d, got %d"),
				Case.What, Kit, R.CeilingFor(Kit)), R.CeilingFor(Kit) >= 1);
		}

		// AND NOTHING HANGS OUT OF THE PLOT. A column shoved against an edge it measured at
		// the wrong depth is the failure this test exists for, and a stand half outside the
		// fence looks correct from directly above.
		TArray<FVector2D> Corners;
		for (const PlotYard::FReservedStand& Stand : R.Stands)
		{
			const PlotYard::FKitSpec& Kit = Specs[Stand.KitIndex];
			PlotYard::FFootprint Claimed;
			Claimed.LengthUu = Kit.Footprint.LengthUu + Kit.ApronUu.X;
			Claimed.WidthUu = Kit.Footprint.WidthUu * Stand.RunLength + Kit.ApronUu.Y * 2.0;

			PlotYard::StandCorners(Stand, Claimed, Corners);
			for (const FVector2D& Corner : Corners)
			{
				TestTrue(*FString::Printf(TEXT("a %s plot keeps kit %d inside the fence"),
					Case.What, Stand.KitIndex),
					RoadGeom::PointInPolygon(Case.Outline, Corner));
			}
		}
	}

	return true;
}

/**
 * Growing a plot never costs it capacity.
 *
 * ASSERTED FOR THIS STRATEGY ONLY, and that limit is the point. The scatter is not monotonic
 * - a sweep of 845 plot sizes found 336 regressions, worst drop 11 bays - and the seam
 * promises nothing either way. A band layout that lost this would be a bug in the bands.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFuelYardIsMonotonicTest,
	"Airside.Build.FuelYardIsMonotonic",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFuelYardIsMonotonicTest::RunTest(const FString& Parameters)
{
	const TArray<PlotYard::FKitSpec> Specs = StrategySpecs();
	const UPlotLayoutStrategy* Strategy = PlotLayoutFor(EPlotLayout::FuelYardBands);

	for (double WidthUu = 1200.0; WidthUu <= 5000.0; WidthUu += 400.0)
	{
		TArray<int32> Previous;
		Previous.SetNumZeroed(Specs.Num());

		for (double DepthUu = 800.0; DepthUu <= 4000.0; DepthUu += 50.0)
		{
			const TArray<FVector2D> Outline = StrategyRect(WidthUu, DepthUu);
			const PlotYard::FReservation R =
				Strategy->Solve(StrategySite(Outline, WidthUu), Specs);

			for (int32 Kit = 0; Kit < Specs.Num(); ++Kit)
			{
				const int32 Now = R.CeilingFor(Kit);
				TestTrue(*FString::Printf(
					TEXT("%.0f x %.0f kit %d did not lose capacity: %d -> %d"),
					WidthUu, DepthUu, Kit, Previous[Kit], Now), Now >= Previous[Kit]);
				Previous[Kit] = Now;
			}
		}
	}

	return true;
}

#endif
