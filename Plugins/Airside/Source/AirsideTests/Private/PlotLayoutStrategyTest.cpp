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

#endif
