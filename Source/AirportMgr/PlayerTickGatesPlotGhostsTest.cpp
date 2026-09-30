#include "CoreMinimal.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Present/AirsideBuildingsActor.h"
#include "Present/PlotPresenter.h"
#include "Present/RoadNetworkActor.h"
#include "RoadBuildController.h"
#include "Testing/AirsideTestWorld.h"
#include "Tool/BuildSession.h"
#include "Tool/Selection.h"

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

/**
 * THE READ PlayerTick hands ShowPlotGhosts (facility-upgrades spec R10): a selected PLOTTED depot is revealed;
 * anything else is not. The static first, then the composition - a tick with the depot selected must reveal
 * it, or the static could be right and never asked.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRevealedDepotFollowsSelectionTest,
	"AirportMgr.Actions.RevealedDepotFollowsTheSelection",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRevealedDepotFollowsSelectionTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor"), Actor)) { return false; }
	// SelectionNamesItsDepot's setup: a spawned actor's network is only placeable into after a clear.
	Actor->ClearNetwork();
	if (!TestNotNull(TEXT("a network"), Actor->Network.Get())) { return false; }
	URoadNetwork& Net = *Actor->Network;
	UEntityDefinition* DepotDef = UEntityDefinition::MakeFuelDepotTransient();
	FEntityPlacement Placement;
	Placement.Definition = DepotDef;
	Placement.Anchors = DepotDef->Anchors;
	Placement.Position = FVector2D(1000.0, 0.0);
	Placement.Heading = UE_DOUBLE_HALF_PI;
	Placement.PoseRole = EServiceRole::Fuel;
	const FEntityInstanceId Unplotted = Net.PlaceEntity(Placement);
	Placement.Position = FVector2D(1000.0, 10000.0);
	Placement.Outline = { FVector2D(0.0, 9000.0), FVector2D(3000.0, 9000.0), FVector2D(3000.0, 11400.0), FVector2D(0.0, 11400.0) };
	Placement.Modules = { EDepotModule::Shed };
	const FEntityInstanceId Depot = Net.PlaceEntity(Placement);
	UEntityDefinition* StandDef = UEntityDefinition::MakeStandTransient();
	const FEntityInstanceId Stand = Net.PlaceEntity(StandDef, StandDef->Anchors, FVector2D(20000.0, 0.0), 0.0, 0.0, StandDef->PoseRole, 0);
	if (!TestTrue(TEXT("setup: two depots and a stand"), Unplotted.IsSet() && Depot.IsSet() && Stand.IsSet())) { return false; }

	FSelection Sel;
	Sel.Kind = ESelectionKind::Stand;
	Sel.Id = Depot.Index;
	TestTrue(TEXT("a selected plotted depot is revealed"), ARoadBuildController::RevealedDepotFor(Actor, Sel) == Depot);
	Sel.Id = Unplotted.Index;
	TestFalse(TEXT("an unplotted depot has no yard to reveal"), ARoadBuildController::RevealedDepotFor(Actor, Sel).IsSet());
	Sel.Id = Stand.Index;
	TestFalse(TEXT("a selected stand reveals nothing"), ARoadBuildController::RevealedDepotFor(Actor, Sel).IsSet());
	Sel.Kind = ESelectionKind::Aircraft;
	Sel.Id = Depot.Index;
	TestFalse(TEXT("an aircraft selection reveals nothing, even at a depot's index"), ARoadBuildController::RevealedDepotFor(Actor, Sel).IsSet());
	TestFalse(TEXT("and no target reveals nothing"), ARoadBuildController::RevealedDepotFor(nullptr, Sel).IsSet());

	// THE COMPOSITION: PlayerTick in Select (not edit mode) with the depot selected.
	UPlotPresenter* Plots = TestWorld.Buildings != nullptr ? TestWorld.Buildings->GetPlotPresenter() : nullptr;
	if (!TestNotNull(TEXT("a plot presenter"), Plots)) { return false; }
	ARoadBuildController* C = TestWorld.World->SpawnActor<ARoadBuildController>();
	if (!TestNotNull(TEXT("controller spawned"), C)) { return false; }
	C->SetTargetForTest(Actor);
	C->SetBuildingsForTest(TestWorld.Buildings);
	C->InitInputSystem();
	C->SelectTool(0);
	C->PlayerTickForTest(1.0f / 60.0f);
	TestFalse(TEXT("nothing selected in Select: ghosts hidden, as before R10"), Plots->AreGhostsVisible());
	TestFalse(TEXT("and no depot is singled out"), Plots->GetGhostScope().IsSet());

	FSelection DepotSel;
	DepotSel.Kind = ESelectionKind::Stand;
	DepotSel.Id = Depot.Index;
	C->SelectForTest(DepotSel);
	C->PlayerTickForTest(1.0f / 60.0f);
	TestTrue(TEXT("a tick with the plotted depot selected shows the ghost layer"), Plots->AreGhostsVisible());
	TestTrue(TEXT("scoped to that depot alone"), Plots->GetGhostScope() == Depot);

	C->SelectForTest(FSelection());
	C->PlayerTickForTest(1.0f / 60.0f);
	TestFalse(TEXT("deselecting hides them again"), Plots->AreGhostsVisible());
	TestFalse(TEXT("and every yard is back in scope for edit mode"), Plots->GetGhostScope().IsSet());
	return true;
}

#endif
