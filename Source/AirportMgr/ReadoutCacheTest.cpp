#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Present/RoadNetworkActor.h"
#include "RoadBuildController.h"
#include "Testing/AirsideTestWorld.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * ONE READOUT REBUILD FOR A STILL CURSOR, issue #190. Before this,
 * ARoadBuildController::CollectToolReadout ran Tool->BuildReadout every PlayerTick regardless
 * of whether anything the tool could answer differently about had changed - a
 * TArray<TPair<FString,FString>> plus a Printf per fact, paid every frame a gesture sat idle
 * under a motionless mouse.
 *
 * COUNTED THROUGH GetToolReadoutRevision, bumped only when CollectToolReadout actually calls
 * BuildReadout - see FToolReadoutKey's own comment for what the cache compares and why a
 * still cursor is exactly the case it exists for.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FReadoutRebuildsOnceForStillCursorTest,
	"AirportMgr.Actions.ReadoutRebuildsOnceForStillCursor",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FReadoutRebuildsOnceForStillCursorTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Target = TestWorld.Actor;
	if (!TestNotNull(TEXT("a target actor"), Target)) { return false; }

	ARoadBuildController* C = TestWorld.World->SpawnActor<ARoadBuildController>();
	if (!TestNotNull(TEXT("controller spawned"), C)) { return false; }

	// Same bypass and same reason as FPlayerTickBuildsOneContextTest: no level here for
	// BeginPlay's TActorIterator to find Target in, and no PlayerInput without this call.
	C->SetTargetForTest(Target);
	C->InitInputSystem();

	// Taxiway: a tool whose BuildReadout does real work over the pending gesture, not
	// Select's near-empty idle path - see FPlayerTickBuildsOneContextTest's own comment.
	C->SelectTool(1);

	const int32 Before = C->GetToolReadoutRevision();

	// K TICKS, NO MOUSE MOVED BETWEEN THEM. This test drives no viewport, so
	// MakeToolContext's CursorOnRoadPlane fails every call and PlayerTick's FrameContext
	// falls back to Session.LastPlaneHit() - the same position each time, with no click or
	// drag between ticks to advance the tool's own stage.
	constexpr int32 Ticks = 5;
	for (int32 Index = 0; Index < Ticks; ++Index)
	{
		C->PlayerTickForTest(1.0f / 60.0f);
	}

	const int32 After = C->GetToolReadoutRevision();

	// EXACTLY ONE: the first tick has no cached key yet and must build the readout; the
	// remaining Ticks - 1 see an unchanged FToolReadoutKey and must not rebuild it. This goes
	// to Ticks if the cache is ever bypassed, and to 0 if BuildReadout stops running at all.
	TestEqual(TEXT("a still cursor rebuilds the readout once across several ticks, not once "
					"per tick"),
		After - Before, 1);

	return true;
}

#endif
