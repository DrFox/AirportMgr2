#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Present/RoadNetworkActor.h"
#include "RoadBuildController.h"
#include "Testing/AirsideTestWorld.h"
#include "Tool/SnapToggleRegistry.h"

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
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Target = TestWorld.Actor;
	if (!TestNotNull(TEXT("a target actor"), Target)) { return false; }

	ARoadBuildController* C = TestWorld.World->SpawnActor<ARoadBuildController>();
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

/**
 * A SNAP TOGGLE UNDER A STILL CURSOR RE-READS (#468's review). The frame's context is cached on a
 * key (FBuildSession's FFrameContextKey), and a switched guide or grid step must not keep serving
 * the previous frame's context until the cursor moves - the preview would snap to a guide the
 * player had just turned off. It does not, and NOT because ARoadBuildController::ApplySnapToggle
 * invalidates (it does not; the review asked for that on the premise that the key lacked the
 * settings): the key's Tunables carry the airport's GuideSources, and FBuildSessionTunables::
 * operator== compares every field. This pins THAT, counted through the chokepoint
 * PlayerTickBuildsOneContext above already uses.
 *
 * MUTATION-CHECKED (2026-09-30): with GuideSources.GridStep dropped from that operator==, the tick
 * after a Grid toggle built 0 contexts and this failed; restored, it builds 1.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSnapToggleRereadsTheStillCursorTest,
	"AirportMgr.Actions.SnapToggleRereadsTheStillCursor",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSnapToggleRereadsTheStillCursorTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world with an airport"), TestWorld.Actor)) { return false; }
	ARoadBuildController* C = TestWorld.World->SpawnActor<ARoadBuildController>();
	if (!TestNotNull(TEXT("controller spawned"), C)) { return false; }
	C->SetTargetForTest(TestWorld.Actor);
	C->InitInputSystem();   // PlayerTickBuildsOneContext's reason: TickPlayerInput needs one.
	C->SelectTool(1);

	// Settle, then CONTROL: a second still tick is served from the cache - otherwise the count
	// below would move with or without the toggle and measure nothing.
	C->PlayerTickForTest(1.0f / 60.0f);
	const int32 Settled = C->MakeContextCallCountForTest();
	C->PlayerTickForTest(1.0f / 60.0f);
	if (!TestEqual(TEXT("control: a still cursor's second tick reuses the frame's context"),
		C->MakeContextCallCountForTest() - Settled, 0))
	{
		return false;
	}

	const FSnapToggleRegistration* Grid = SnapToggleRegistry().FindByPredicate(
		[](const FSnapToggleRegistration& Toggle) { return Toggle.Id == FName(TEXT("snap.grid")); });
	if (!TestNotNull(TEXT("the registry has the Grid toggle"), Grid)) { return false; }
	C->ApplySnapToggle(*Grid);

	const int32 BeforeTick = C->MakeContextCallCountForTest();
	C->PlayerTickForTest(1.0f / 60.0f);
	TestEqual(TEXT("the tick after a snap toggle rebuilds the context - the cursor did not move, the grid did"),
		C->MakeContextCallCountForTest() - BeforeTick, 1);
	return true;
}

#endif
