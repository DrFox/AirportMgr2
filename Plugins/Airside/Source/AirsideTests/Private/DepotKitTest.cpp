#include "Build/DepotKit.h"
#include "Content/AirsideContent.h"
#include "CoreMinimal.h"
#include "Entities/PlotModuleKit.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * A kit's figures win; no kit falls back to the grey-box table.
 *
 * THE FALLBACK IS THE POINT, not a convenience. Every other change in this slice - the
 * reservation solver, the presenter, the readout - lands and is testable before a single
 * mesh exists, and that is only true while a missing kit keeps working. A resolver that
 * returned a zero footprint instead would put every module on top of every other one, which
 * reads as a solver bug rather than as missing content.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDepotKitFallsBackTest,
	"Airside.Build.DepotKitFallsBackWhenUnauthored",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDepotKitFallsBackTest::RunTest(const FString& Parameters)
{
	// No content at all: the grey-box figures DepotKit.cpp has carried since the yard solver
	// was written.
	const PlotYard::FFootprint Bare = DepotFootprint(EDepotModule::Shed, nullptr);
	TestEqual(TEXT("unauthored shed keeps its grey-box length"), Bare.LengthUu, 800.0);
	TestEqual(TEXT("unauthored shed keeps its grey-box width"), Bare.WidthUu, 400.0);
	TestTrue(TEXT("unauthored shed still stands against the back fence"),
		Bare.bAgainstTheBackFence);

	// An authored kit overrides all three.
	UAirsideContent* Content = NewObject<UAirsideContent>();
	UPlotModuleKit* Kit = NewObject<UPlotModuleKit>();
	Kit->Footprint = FVector2D(1100.0, 500.0);
	Kit->bAgainstTheBackFence = false;
	Content->DepotKits.Add(EDepotModule::Shed, Kit);

	const PlotYard::FFootprint Authored = DepotFootprint(EDepotModule::Shed, Content);
	TestEqual(TEXT("an authored shed uses the kit's length"), Authored.LengthUu, 1100.0);
	TestEqual(TEXT("an authored shed uses the kit's width"), Authored.WidthUu, 500.0);
	TestFalse(TEXT("an authored shed uses the kit's back-fence flag"),
		Authored.bAgainstTheBackFence);

	// A kit for one module does not silently answer for another - the map is keyed, and a
	// lookup that fell through to "the first kit" would dress every module as a shed.
	const PlotYard::FFootprint Tank = DepotFootprint(EDepotModule::Tank, Content);
	TestEqual(TEXT("the tank still falls back"), Tank.LengthUu, 500.0);
	TestEqual(TEXT("the tank still falls back on width too"), Tank.WidthUu, 500.0);

	return true;
}

#endif
