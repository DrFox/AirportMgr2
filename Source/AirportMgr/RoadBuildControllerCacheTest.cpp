#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "RoadBuildController.h"
#include "Testing/AirsideTestWorld.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * HasRunway USED TO RE-WALK THE NETWORK EVERY CALL, on the stated ground that "a cache would
 * need invalidating on every edit... for a saving nobody would measure" (its own old comment,
 * quoted in the fix). That premise stopped holding once it became one of roughly thirty rows
 * UBuildBarWidget::RefreshState asks every tick (issue #187) - this measures the cache
 * directly, the same way FPlayerTickBuildsOneContextTest measures issue #167's, rather than
 * trusting a trace of the call sites by eye.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHasRunwayCachesUntilTopologyChangesTest,
	"AirportMgr.Actions.HasRunwayCachesUntilTopologyChanges",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FHasRunwayCachesUntilTopologyChangesTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Target = TestWorld.Actor;
	if (!TestNotNull(TEXT("a target actor"), Target)) { return false; }

	ARoadBuildController* C = TestWorld.World->SpawnActor<ARoadBuildController>();
	if (!TestNotNull(TEXT("controller spawned"), C)) { return false; }
	// Binds the cache invalidation exactly as BeginPlay would - see SetTargetForTest's own
	// comment on why this bypasses the level search rather than skipping the binding.
	C->SetTargetForTest(Target);

	C->HasRunway();
	const int32 AfterFirstCall = C->HasRunwayRecomputeCountForTest();
	C->HasRunway();
	C->HasRunway();
	TestEqual(TEXT("repeated queries with nothing changed do not re-walk the network"),
		C->HasRunwayRecomputeCountForTest(), AfterFirstCall);

	// A TOPOLOGY EDIT, not a runway - the cache is invalidated by the KIND of change, not by
	// whether this particular edit happened to add a runway. PlaceNode's CommitAndNotify
	// notifies EChangeKind::Topology by construction (every scope-committing mutator except
	// MoveNode does - see URoadEditFacade's own class comment).
	Target->PlaceNode(FVector2D(0.0, 0.0));

	C->HasRunway();
	TestTrue(TEXT("a topology change invalidates the cache, so the next query re-walks it"),
		C->HasRunwayRecomputeCountForTest() > AfterFirstCall);

	return true;
}

#endif
