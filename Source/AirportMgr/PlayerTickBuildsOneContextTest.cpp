#include "CoreMinimal.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Present/RoadNetworkActor.h"
#include "RoadBuildController.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * ONE CONTEXT PER FRAME, issue #167. Before this, ARoadBuildController::PlayerTick's own
 * CollectToolReadout and Tool->Tick each called MakeToolContext() independently - and
 * ARoadBuildHUD::DrawHUD, after PlayerTick returns, called it a third time for the same
 * frame - so a still cursor paid for the whole snap + guide pipeline (a junction solve per
 * live node outside its cheap-reject radius, then the guide chain over every source) three
 * to five times for one position. This measures the count directly rather than tracing the
 * call sites by eye, which is what let the duplication ship unnoticed in the first place.
 *
 * COUNTED THROUGH FBuildSession::MakeContextCallCountForTest, the one function every
 * MakeToolContext call on either driver funnels through - see that method's own comment for
 * why this is the chokepoint rather than a mock IBuildTool substituted into ToolRegistry(),
 * which is a fixed, function-local table with nowhere to inject one.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayerTickBuildsOneContextTest,
	"AirportMgr.Actions.PlayerTickBuildsOneContext",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlayerTickBuildsOneContextTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("a world"), World)) { return false; }
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
	Context.SetCurrentWorld(World);
	ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };

	ARoadNetworkActor* Target = World->SpawnActor<ARoadNetworkActor>();
	if (!TestNotNull(TEXT("a target actor"), Target)) { return false; }

	ARoadBuildController* C = World->SpawnActor<ARoadBuildController>();
	if (!TestNotNull(TEXT("controller spawned"), C)) { return false; }

	// BYPASSES BeginPlay's level search, same precedent as the camera test's
	// CreateBuildCamera call - there is no level here for TActorIterator to find Target in.
	C->SetTargetForTest(Target);

	// A PlayerInput IS REQUIRED before PlayerTick will run at all: APlayerController::
	// TickPlayerInput asserts one exists, and a bare SpawnActor never creates it - only
	// possession or the engine's own per-frame actor tick does, and this test drives neither.
	C->InitInputSystem();

	// Taxiway: a tool whose Tick and BuildReadout both do real work over the pending gesture,
	// not Select's near-empty idle path - so a bug that let either rebuild its own context
	// has something to actually call MakeContext from.
	C->SelectTool(1);

	const int32 Before = C->MakeContextCallCountForTest();
	C->PlayerTickForTest(1.0f / 60.0f);
	const int32 After = C->MakeContextCallCountForTest();

	// EXACTLY ONE: CollectToolReadout and Tick share the frame's one FrameContext. With no
	// mouse press this tick, UpdateDrag returns before building anything of its own - see its
	// own comment on why a drag frame is allowed one more.
	TestEqual(TEXT("PlayerTick builds exactly one context for CollectToolReadout and Tick "
					"together - this goes to 2 or more if either stops reading FrameContext"),
		After - Before, 1);

	return true;
}

#endif
