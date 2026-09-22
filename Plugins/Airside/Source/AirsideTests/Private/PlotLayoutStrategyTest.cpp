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
 * Only the strategy that actually claims an apron as part of a stand's rectangle says so.
 *
 * PlotYard::Reserve (what the scatter forwards to) never reads ApronUu when it samples a
 * pose - grep confirms - so its Stand.Centre is the footprint's own centre and
 * bStandsIncludeApron must stay false, or UPlotPresenter and FPlotPlaceTool::BuildPreview
 * both draw the object half an apron away from ground the sampler never fenced off (issue
 * #193). UFuelYardBandsStrategy centres every stand on ClaimedBy's footprint-plus-apron
 * rectangle, so it must say true.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FReservationSaysWhetherItsStandsIncludeApronTest,
	"Airside.Build.ReservationSaysWhetherItsStandsIncludeApron",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FReservationSaysWhetherItsStandsIncludeApronTest::RunTest(const FString& Parameters)
{
	const TArray<FVector2D> Outline = StrategyRect(3200.0, 2400.0);
	const TArray<PlotYard::FKitSpec> Specs = StrategySpecs();
	const FPlotSite Site = StrategySite(Outline, 3200.0);

	const UScatterLayoutStrategy* Scatter = NewObject<UScatterLayoutStrategy>();
	TestFalse(TEXT("the scatter never claims an apron"),
		Scatter->Solve(Site, Specs).bStandsIncludeApron);

	const UFuelYardBandsStrategy* Bands = NewObject<UFuelYardBandsStrategy>();
	TestTrue(TEXT("the band layout claims footprint-plus-apron for every stand"),
		Bands->Solve(Site, Specs).bStandsIncludeApron);

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
 * 20 x 14 m, AND NEITHER FIGURE IS THE SHEET'S 11 x 8.
 *
 * THE DEPTH went to 12 m because an 8 m shed in an 8 m site leaves nothing in front of the
 * door, and with its apron the depot did not fit its own plot at all - see
 * PlotFit::BayDepthUu, which this test moved.
 *
 * THE WIDTH is 15 m because the columns need 13 m - tank 5.0 + clearance 1.0 + shed 4.0 +
 * clearance 1.0 + pump 2.0 - and FPlotPlaceTool::FrontageStepUu snaps a frontage to 5 m
 * steps, so 13 m is not drawable and 15 m is the next one that is. An 11 m plot was never
 * drawable either; the sheet's figure is art, not a gesture the player can make.
 *
 * 20 m SINCE 2026-09-22, when the gate lane (PlotYard::GateCorridorUu) went from 6.2 to 8 m
 * for the 8.5 m truck that ships, and 14 m deep with it: the side columns start one lane in
 * from the gate, so a 5 m tank needs 8 + 5 = 13 m of depth. The user chose the wider gate over
 * keeping the 15 x 12 m plot.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFuelYardFitsTheConceptSheetTest,
	"Airside.Build.FuelYardFitsTheConceptSheet",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFuelYardFitsTheConceptSheetTest::RunTest(const FString& Parameters)
{
	const TArray<FVector2D> Outline = StrategyRect(2000.0, 1400.0);
	const TArray<PlotYard::FKitSpec> Specs = StrategySpecs();
	const FPlotSite Site = StrategySite(Outline, 2000.0);

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
 * Every module has open ground beside it.
 *
 * WHAT REACHABILITY ACTUALLY NEEDS, and what this test asserted a proxy for until 2026-09-20.
 * It used to compare every column stand against a single ShedFront - the shallowest front edge
 * of any shed on the plot - which was itself a global extreme, and the kind this file has been
 * bitten by four times. Once the shed row followed a slanted back edge, stands sat at a dozen
 * different depths and one ShedFront described none of them: the test flagged columns that had
 * no shed within twenty metres.
 *
 * SO IT ASKS THE QUESTION DIRECTLY. A module is reachable if a truck can get to a face of it,
 * so every stand must have a truck-corridor of clear ground on one of THREE sides: either
 * flank, or the front. Three and not two, because a shed row is meant to be shoulder to
 * shoulder - a run of three sheds has no flank and never should - and what a shed offers is
 * its front, which is why it carries an apron and a tank does not. A stacked column has
 * neither, which is the arrangement this refuses.
 *
 * IT IS STILL NOT A ROUTE. Nothing here walks from the gate; clear ground beside a module does
 * not prove a path to it. Circulation remains out of scope - design doc section 3 - and this
 * is the floor under that decision, not a substitute for it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFuelYardLeavesGroundBesideEveryModuleTest,
	"Airside.Build.FuelYardLeavesGroundBesideEveryModule",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFuelYardLeavesGroundBesideEveryModuleTest::RunTest(const FString& Parameters)
{
	const TArray<PlotYard::FKitSpec> Specs = StrategySpecs();

	for (double WidthUu = 1500.0; WidthUu <= 6000.0; WidthUu += 500.0)
	{
		for (double DepthUu = 1200.0; DepthUu <= 4000.0; DepthUu += 400.0)
		{
			const TArray<FVector2D> Outline = StrategyRect(WidthUu, DepthUu);
			const FPlotSite Site = StrategySite(Outline, WidthUu);
			const PlotYard::FReservation R =
				PlotLayoutFor(EPlotLayout::FuelYardBands)->Solve(Site, Specs);

			auto ClaimedOf = [&Specs](const PlotYard::FReservedStand& Stand)
			{
				const PlotYard::FKitSpec& Kit = Specs[Stand.KitIndex];
				PlotYard::FFootprint Out;
				Out.LengthUu = Kit.Footprint.LengthUu + Kit.ApronUu.X;
				Out.WidthUu = Kit.Footprint.WidthUu * Stand.RunLength + Kit.ApronUu.Y * 2.0;
				return Out;
			};

			for (int32 I = 0; I < R.Stands.Num(); ++I)
			{
				const PlotYard::FFootprint Claimed = ClaimedOf(R.Stands[I]);

				// OFF THE STAND'S OWN HEADING, not off the fixture's axes: the scatter jitters
				// headings and a skewed plot turns the bands, so a lane built in X and Y would
				// measure the wrong ground the moment either happened.
				const FVector2D Forward(FMath::Cos(R.Stands[I].Heading),
					FMath::Sin(R.Stands[I].Heading));
				const FVector2D Side = RoadGeom::PerpCCW(Forward);

				// The three pieces of ground a truck could stand on. A flank lane runs the
				// module's whole length; the front lane spans its whole width, beyond the
				// apron the module already claims.
				PlotYard::FStand Lanes[3];
				PlotYard::FFootprint LaneGround[3];
				for (int32 L = 0; L < 3; ++L) { Lanes[L] = R.Stands[I]; }

				const double Flank = (Claimed.WidthUu + PlotYard::GateCorridorUu) * 0.5;
				Lanes[0].Centre += Side * Flank;
				Lanes[1].Centre -= Side * Flank;
				LaneGround[0].LengthUu = Claimed.LengthUu;
				LaneGround[0].WidthUu = PlotYard::GateCorridorUu;
				LaneGround[1] = LaneGround[0];

				// FORWARD IS INWARD, so the front - the side the gate is on - is behind it.
				Lanes[2].Centre -= Forward * (Claimed.LengthUu + PlotYard::GateCorridorUu) * 0.5;
				LaneGround[2].LengthUu = PlotYard::GateCorridorUu;
				LaneGround[2].WidthUu = Claimed.WidthUu;

				bool bOpen = false;
				for (int32 L = 0; L < 3 && !bOpen; ++L)
				{
					bool bClear = true;
					for (int32 J = 0; J < R.Stands.Num() && bClear; ++J)
					{
						if (J == I) { continue; }
						bClear = !PlotYard::StandsOverlap(
							Lanes[L], LaneGround[L], R.Stands[J], ClaimedOf(R.Stands[J]));
					}
					bOpen = bClear;
				}

				TestTrue(*FString::Printf(
					TEXT("%.0f x %.0f: kit %d has open ground on a face"),
					WidthUu, DepthUu, R.Stands[I].KitIndex), bOpen);
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
 *
 * TOTALS, NOT PER KIT, since 2026-09-20. Sheds now have precedence: they take the back edge
 * first and the columns get what is left. Four hundred more millimetres of depth can buy a
 * fourth shed that pushes a tank column out, so kit 1 legitimately falls by one while the
 * plot as a whole gains. Asserting per kit forbade precedence itself, which is a rule the
 * player asked for - so it is the total that must not go backwards.
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
		int32 Previous = 0;

		for (double DepthUu = 800.0; DepthUu <= 4000.0; DepthUu += 50.0)
		{
			const TArray<FVector2D> Outline = StrategyRect(WidthUu, DepthUu);
			const PlotYard::FReservation R =
				Strategy->Solve(StrategySite(Outline, WidthUu), Specs);

			// BAYS, not stands: a shed run of three is three bays the player buys, and a
			// layout that swapped three runs of one for one run of three would otherwise
			// read as a loss.
			int32 Now = 0;
			for (int32 Kit = 0; Kit < Specs.Num(); ++Kit)
			{
				Now += R.CeilingFor(Kit);
			}

			TestTrue(*FString::Printf(
				TEXT("%.0f x %.0f did not lose capacity: %d -> %d"),
				WidthUu, DepthUu, Previous, Now), Now >= Previous);
			Previous = Now;
		}
	}

	return true;
}

/**
 * A depot is sized by its trucks, not by its spare ground.
 *
 * THE NUMBER THAT PROMPTED THIS: a 50 m plot reserved 6 sheds, 15 tanks and 28 pumps, because
 * the tank and pump columns ran until they met the fence. "28 pumps for 6 vehicles, nearly 5
 * pumps per vehicle." Free ground is not a reason to put a pump on it.
 *
 * THE RATIO IS THE KIT'S WEIGHT, the field the design doc gave them and which the band layout
 * ignored until 2026-09-20. This asserts the layout reads it - not that any particular mix is
 * correct, which is an authoring question and settled on the data asset.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFuelYardIsSizedByItsShedsTest,
	"Airside.Build.FuelYardIsSizedByItsSheds",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFuelYardIsSizedByItsShedsTest::RunTest(const FString& Parameters)
{
	const TArray<PlotYard::FKitSpec> Specs = StrategySpecs();
	const UPlotLayoutStrategy* Strategy = PlotLayoutFor(EPlotLayout::FuelYardBands);

	for (double WidthUu = 1500.0; WidthUu <= 8000.0; WidthUu += 500.0)
	{
		for (double DepthUu = 1200.0; DepthUu <= 6000.0; DepthUu += 400.0)
		{
			const TArray<FVector2D> Outline = StrategyRect(WidthUu, DepthUu);
			const PlotYard::FReservation R =
				Strategy->Solve(StrategySite(Outline, WidthUu), Specs);

			const int32 Sheds = R.CeilingFor(0);
			for (int32 Kit = 1; Kit < Specs.Num(); ++Kit)
			{
				// ONE OF EACH IS ALWAYS ALLOWED: a plot too shallow for a shed is still a
				// depot, and the layout says so deliberately.
				const int32 Allowed = FMath::Max(1, FMath::DivideAndRoundUp(
					Sheds * Specs[Kit].ReserveWeight, Specs[0].ReserveWeight));

				TestTrue(*FString::Printf(
					TEXT("%.0f x %.0f: %d sheds allow %d of kit %d, got %d"),
					WidthUu, DepthUu, Sheds, Allowed, Kit, R.CeilingFor(Kit)),
					R.CeilingFor(Kit) <= Allowed);
			}
		}
	}

	return true;
}

/**
 * The lane held back at each end of the shed row for a column kit must match what that
 * column actually claims - footprint PLUS its lateral apron, via ClaimedBy - not the bare
 * footprint alone (issue #193). A margin sized off Footprint.WidthUu let the shed row reach
 * into ground a kit with a real lateral apron needed, so the column's own first stand was
 * refused for overlapping a shed that had no business standing there, and the whole column
 * ended empty - "the column loop breaks at index 0".
 *
 * NUMBERS CHOSEN FOR A CLEAR MARGIN, not a knife edge: the buggy formula here comes up 800uu
 * short of the correct one - well over a full shed pitch - so a bug in this line puts a real
 * 400uu of overlap between the shed row and the tank's own claimed ground, not a
 * floating-point coin flip on a shared boundary.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFuelYardBandsMarginMatchesTheColumnsClaimTest,
	"Airside.Build.FuelYardBandsMarginMatchesTheColumnsClaim",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFuelYardBandsMarginMatchesTheColumnsClaimTest::RunTest(const FString& Parameters)
{
	const TArray<FVector2D> Outline = StrategyRect(2600.0, 1600.0);
	const FPlotSite Site = StrategySite(Outline, 2600.0);

	PlotYard::FKitSpec Shed;
	Shed.Footprint.LengthUu = 800.0;
	Shed.Footprint.WidthUu = 400.0;
	Shed.Footprint.bAgainstTheBackFence = true;
	Shed.ApronUu = FVector2D(400.0, 0.0);
	Shed.ReserveWeight = 3;
	Shed.RunCap = 1;

	// THE HIGH-SIDE COLUMN (kit index 1 - UsesTheHighSide), with a LATERAL apron: the one
	// dimension an unauthored grey-box kit never has, and exactly the value the buggy margin
	// left out.
	PlotYard::FKitSpec Tank;
	Tank.Footprint.LengthUu = 500.0;
	Tank.Footprint.WidthUu = 500.0;
	Tank.ApronUu = FVector2D(0.0, 400.0);
	Tank.ReserveWeight = 2;
	Tank.RunCap = 1;

	PlotYard::FKitSpec Pump;
	Pump.Footprint.LengthUu = 300.0;
	Pump.Footprint.WidthUu = 200.0;
	Pump.ReserveWeight = 1;
	Pump.RunCap = 1;

	const TArray<PlotYard::FKitSpec> Specs = { Shed, Tank, Pump };

	const UFuelYardBandsStrategy* Strategy = NewObject<UFuelYardBandsStrategy>();
	const PlotYard::FReservation Reservation = Strategy->Solve(Site, Specs);

	int32 TankStands = 0;
	for (const PlotYard::FReservedStand& Stand : Reservation.Stands)
	{
		if (Stand.KitIndex == 1) { ++TankStands; }
	}

	TestTrue(TEXT("the lateral-apron kit still gets a stand - the margin left it room, "
		"the shed row did not reach into its claimed ground"), TankStands > 0);

	return true;
}

#endif
