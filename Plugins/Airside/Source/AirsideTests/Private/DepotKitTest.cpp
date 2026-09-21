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

/**
 * A kit's apron reaches the solver, and is NOT folded into the footprint.
 *
 * TWO RECTANGLES, NOT ONE. The footprint is the object - what the mesh is and what the
 * presenter draws - and the apron is the working room in front of it. Inflating the shed to
 * 4 x 10 m to buy its apron would draw a ten-metre shed today and disagree with a six-metre
 * mesh tomorrow, which is exactly what the footprint-versus-bounds test exists to catch.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDepotKitCarriesItsApronTest,
	"Airside.Build.DepotKitCarriesItsApron",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDepotKitCarriesItsApronTest::RunTest(const FString& Parameters)
{
	UAirsideContent* Content = NewObject<UAirsideContent>();
	UPlotModuleKit* Kit = NewObject<UPlotModuleKit>();
	Kit->Footprint = FVector2D(600.0, 400.0);
	Kit->ApronUu = FVector2D(400.0, 0.0);
	Content->DepotKits.Add(EDepotModule::Shed, Kit);

	const TArray<PlotYard::FKitSpec> Specs = DepotKitSpecs(Content);
	if (!TestTrue(TEXT("a spec per module"),
		Specs.Num() > static_cast<int32>(EDepotModule::Shed)))
	{
		return false;
	}

	const PlotYard::FKitSpec& Shed = Specs[static_cast<int32>(EDepotModule::Shed)];

	// THE FOOTPRINT IS UNTOUCHED BY THE APRON. If these ever merge, the presenter draws the
	// apron as building.
	TestEqual(TEXT("the footprint is the object"), Shed.Footprint.LengthUu, 600.0);
	TestEqual(TEXT("and its width too"), Shed.Footprint.WidthUu, 400.0);

	TestEqual(TEXT("the apron reaches the spec"), Shed.ApronUu.X, 400.0);
	TestEqual(TEXT("and its lateral half"), Shed.ApronUu.Y, 0.0);

	// AN UNAUTHORED KIT HAS NO APRON rather than a default one: a module that needs clear
	// ground says so, and one that does not keeps the clearance every module already gets.
	const TArray<PlotYard::FKitSpec> Bare = DepotKitSpecs(nullptr);
	TestEqual(TEXT("an unauthored tank has no apron"),
		Bare[static_cast<int32>(EDepotModule::Tank)].ApronUu.X, 0.0);

	return true;
}

/**
 * Every real EDepotModule gets a spec - walked to the sentinel, not to a hardcoded last value.
 *
 * "Pump is the last value; adding a module after it extends this loop with no edit" was false
 * the moment a module was actually added after Pump: DepotKitSpecs walked to
 * static_cast<int32>(EDepotModule::Pump) by name, so a new member got no spec and its owned
 * instances vanished from Specs uncounted (issue #193). EDepotModule::Count is the sentinel
 * that moves itself whenever a real member is inserted before it, which is what this pins:
 * Specs.Num() must equal Count, not a number copied from today's enum.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDepotKitSpecsCoverEveryModuleTest,
	"Airside.Build.DepotKitSpecsCoverEveryModule",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDepotKitSpecsCoverEveryModuleTest::RunTest(const FString& Parameters)
{
	const TArray<PlotYard::FKitSpec> Specs = DepotKitSpecs(nullptr);

	// THE SENTINEL ITSELF, not Pump: a loop that stopped one short of Count would still pass
	// a count taken from Pump, which is exactly the bug this test exists to catch.
	TestEqual(TEXT("one spec per real module, sized to the sentinel"),
		Specs.Num(), static_cast<int32>(EDepotModule::Count));

	return true;
}

#endif
