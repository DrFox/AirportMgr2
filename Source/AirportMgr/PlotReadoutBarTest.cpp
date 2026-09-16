#include "CoreMinimal.h"
#include "BuildActions.h"
#include "BuildBarWidget.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "RoadBuildController.h"
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
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("a world"), World)) { return false; }
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
	Context.SetCurrentWorld(World);
	ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };

	ARoadBuildController* C = World->SpawnActor<ARoadBuildController>();
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

	// AND THE BAR DRAWS THEM. ApplyReadout is called with a hand-built readout rather than
	// one squeezed out of a real gesture: the controller half is pinned above, and what is
	// left to prove here is that the widget turns facts and warnings into lines at all.
	// Without this, ReadoutSection could be created and never filled and every other
	// assertion in this file would still pass.
	UBuildBarWidget* Bar = CreateWidget<UBuildBarWidget>(World, UBuildBarWidget::StaticClass());
	if (!TestNotNull(TEXT("the bar is created with no asset"), Bar)) { return false; }
	TestEqual(TEXT("an untouched bar shows no readout"), Bar->ReadoutLineCountForTest(), 0);

	FToolReadout Readout;
	Readout.Facts.Emplace(TEXT("Bays"), TEXT("3"));
	Readout.Facts.Emplace(TEXT("Rows"), TEXT("2"));
	Readout.Warnings.Add(TEXT("No room to grow"));
	Bar->ApplyReadout(Readout);
	TestEqual(TEXT("two facts and a warning draw three lines"), Bar->ReadoutLineCountForTest(), 3);

	// THE SAME REFILL CONTRACT, ONE LEVEL UP. The collector resetting is worth nothing if
	// the widget it feeds keeps last frame's lines on screen beside this frame's.
	Bar->ApplyReadout(FToolReadout());
	TestEqual(TEXT("an empty readout clears the section"), Bar->ReadoutLineCountForTest(), 0);

	return true;
}

#endif
