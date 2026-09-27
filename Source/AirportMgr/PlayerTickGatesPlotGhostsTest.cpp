#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Present/AirsideBuildingsActor.h"
#include "Present/PlotPresenter.h"
#include "Present/RoadNetworkActor.h"
#include "RoadBuildController.h"
#include "Testing/AirsideTestWorld.h"
#include "Tool/BuildSession.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * PlayerTick pushes the session's ghost-bay answer at the buildings actor.
 *
 * AT THE COMPOSITION, not the session: Airside.Tool.PlotGhostsStandDownOutsideTheDepotTool
 * proves the session answers correctly, and it would stay green if nothing ever asked. The
 * reported defect (2026-09-27) was what the player SAW - cyan unbought bays in normal play
 * that, zoomed out, read as the depot itself.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayerTickGatesPlotGhostsTest,
	"AirportMgr.Actions.PlayerTickGatesPlotGhosts",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlayerTickGatesPlotGhostsTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	UPlotPresenter* Plots = TestWorld.Buildings != nullptr ? TestWorld.Buildings->GetPlotPresenter() : nullptr;
	if (!TestNotNull(TEXT("a plot presenter"), Plots)) { return false; }

	ARoadBuildController* C = TestWorld.World->SpawnActor<ARoadBuildController>();
	if (!TestNotNull(TEXT("controller spawned"), C)) { return false; }
	C->SetTargetForTest(TestWorld.Actor);
	C->SetBuildingsForTest(TestWorld.Buildings);
	// PlayerTickBuildsOneContextTest's reason: PlayerTick asserts a PlayerInput exists.
	C->InitInputSystem();

	int32 DepotIndex = INDEX_NONE;
	const TConstArrayView<FToolRegistration> Registry = ToolRegistry();
	for (int32 Index = 0; Index < Registry.Num(); ++Index)
	{
		if (Registry[Index].Id == FName(TEXT("FuelDepot")))
		{
			DepotIndex = Index;
		}
	}
	if (!TestNotEqual(TEXT("the registry has a fuel depot tool"), DepotIndex, int32(INDEX_NONE)))
	{
		return false;
	}

	TestTrue(TEXT("visible before any driver has ticked - the design-time default"),
		Plots->AreGhostsVisible());

	C->SelectTool(0);                            // Select: normal operations
	C->PlayerTickForTest(1.0f / 60.0f);
	TestFalse(TEXT("a tick in Select hides the ghost bays"), Plots->AreGhostsVisible());

	C->SelectTool(DepotIndex);
	C->PlayerTickForTest(1.0f / 60.0f);
	TestTrue(TEXT("a tick with the fuel depot tool lit shows them"), Plots->AreGhostsVisible());

	C->SelectTool(0);
	C->PlayerTickForTest(1.0f / 60.0f);
	TestFalse(TEXT("and back in Select they go away again"), Plots->AreGhostsVisible());
	return true;
}

#endif
