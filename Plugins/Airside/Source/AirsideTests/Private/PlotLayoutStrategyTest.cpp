#include "Build/PlotLayoutStrategy.h"
#include "CoreMinimal.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Solve/PlotYard.h"

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
		Shed.MaxPerPlot = 6;

		PlotYard::FKitSpec Tank;
		Tank.Footprint.LengthUu = 500.0;
		Tank.Footprint.WidthUu = 500.0;
		Tank.ReserveWeight = 2;
		Tank.MaxPerPlot = 4;

		PlotYard::FKitSpec Pump;
		Pump.Footprint.LengthUu = 300.0;
		Pump.Footprint.WidthUu = 200.0;
		Pump.ReserveWeight = 1;
		Pump.MaxPerPlot = 2;

		return { Shed, Tank, Pump };
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

	// AND THE COUNTS ARE A DEPOT. Twelve pumps serving nine tanks is not one at any tier.
	TestTrue(*FString::Printf(TEXT("a sane shed count, got %d"), R.CeilingFor(0)),
		R.CeilingFor(0) >= 2 && R.CeilingFor(0) <= 12);
	TestTrue(*FString::Printf(TEXT("a sane pump count, got %d"), R.CeilingFor(2)),
		R.CeilingFor(2) >= 1 && R.CeilingFor(2) <= 6);

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
