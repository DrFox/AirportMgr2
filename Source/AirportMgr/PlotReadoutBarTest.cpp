#include "CoreMinimal.h"
#include "BuildActions.h"
#include "Misc/AutomationTest.h"
#include "RoadBuildController.h"
#include "Testing/AirsideTestWorld.h"
#include "Tool/ToolReadout.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE SEAM BETWEEN THE PLUGIN'S READOUT AND THE GAME MODULE'S BAR, at the level of the
 * composition. Airside.Tool.PlotReadoutMatchesPreview already proves the tool EMITS the
 * facts and Airside.Tool.ToolReadoutCollector proves the collector gathers them; neither
 * would notice if nothing on this side ever called BuildReadout, which is exactly the
 * "declared but never consumed" shape CLAUDE.md names three times.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotReadoutReachesTheBarTest,
	"AirportMgr.Actions.PlotReadoutReachesTheBar",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotReadoutReachesTheBarTest::RunTest(const FString& Parameters)
{
	// World-and-controller setup exactly as AirportMgr.Actions.ControllerOwnsCameraAndHud
	// does it - the same degraded path, with no asset and no level.
	FAirsideTestWorld TestWorld(/*bSpawnActor=*/false);
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }

	ARoadBuildController* C = TestWorld.World->SpawnActor<ARoadBuildController>();
	if (!TestNotNull(TEXT("controller spawned"), C)) { return false; }

	// THE BUILD ACTION IS IN THE REGISTRY, not a bespoke button. The bar builds itself from
	// this list and BarBuildsFromRegistry already guards it, so an action added anywhere else
	// would render nowhere and be tested by nothing.
	const FBuildAction* Build = FindAction(FName(TEXT("edit.build")));
	if (!TestNotNull(TEXT("a Build action in the registry"), Build)) { return false; }

	// DISABLED WHEN NOTHING IS COMMITTABLE. A fresh controller has no gesture in progress,
	// so the readout's bCommittable default of false must reach the button - a Build button
	// lit with nothing to build is the failure the sink's own comment names.
	TestFalse(TEXT("nothing to build, so Build is disabled"), Build->IsEnabled(*C));
	TestFalse(TEXT("and the readout says so"), C->GetToolReadout().bCommittable);

	// AND THE COLLECTOR IS REFILLED, not appended to. Two ticks with no gesture must leave
	// the readout empty rather than growing it - a fact from last frame describes a gesture
	// the player has already changed.
	C->CollectToolReadoutForTest();
	C->CollectToolReadoutForTest();
	TestEqual(TEXT("two idle frames leave no facts behind"),
		C->GetToolReadout().Facts.Num(), 0);

	// THE BAR NO LONGER DRAWS THEM. Its readout section was retired on 2026-09-17 in favour
	// of a panel at the plot - see ARoadBuildHUD::PanelLines and the four-point gesture spec.
	// What this file still guards is the half that did not move: that edit.build is in the
	// registry, and that the controller refills the readout every frame rather than letting
	// last frame's facts stand.
	return true;
}

#endif
