#include "CoreMinimal.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Present/OpsRuntime.h"
#include "Present/OpsRuntimeSubsystem.h"
#include "Present/RoadNetworkActor.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/**
	 * A UWorld sitting behind a REAL UGameInstance - what FAirsideTestWorld deliberately does
	 * not give (issue #189's own header: most fixtures need no game instance at all), and
	 * exactly what UOpsRuntimeSubsystem needs: it is a UGameInstanceSubsystem (see its own
	 * header), so nothing creates or initialises it without one.
	 *
	 * SAME CONSTRUCTION AS THE ENGINE'S OWN TEST HELPER - Engine/Private/Tests/
	 * AutomationCommon.cpp's FTestWorldWrapper: a fresh WorldContext, the instance and the
	 * world pointed at each other, then GameInstance->Init(), which is the one call that
	 * actually creates and initialises every UGameInstanceSubsystem - this one included.
	 * FAirsideTestWorld's own World/DestroyWorldContext teardown, plus GameInstance->Shutdown()
	 * first, matching FTestWorldWrapper::DestroyTestWorld's order.
	 */
	struct FOpsSubsystemTestWorld
	{
		UWorld* World = nullptr;
		UGameInstance* GameInstance = nullptr;

		FOpsSubsystemTestWorld()
		{
			// Outer must be a UEngine: UGameInstance::GetEngine() is CastChecked<UEngine>(GetOuter()).
			GameInstance = NewObject<UGameInstance>(GEngine);
			World = UWorld::CreateWorld(EWorldType::Game, false);
			if (World == nullptr) { return; }
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
			Context.OwningGameInstance = GameInstance;
			World->SetGameInstance(GameInstance);
			Context.SetCurrentWorld(World);
			GameInstance->Init();
		}

		~FOpsSubsystemTestWorld()
		{
			if (World == nullptr) { return; }
			if (World->GetGameInstance() != nullptr)
			{
				World->GetGameInstance()->Shutdown();
			}
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
		}

		FOpsSubsystemTestWorld(const FOpsSubsystemTestWorld&) = delete;
		FOpsSubsystemTestWorld& operator=(const FOpsSubsystemTestWorld&) = delete;
	};
}

/**
 * UOpsRuntimeSubsystem's own header calls itself "a forwarder only... nothing here has logic
 * worth a test" - but EnsureAttached (private, reached only through Tick) is the one path
 * that recovers UOpsRuntime's target after a level change, and issue #194 found nothing
 * measuring it. Per the brief: test the OBSERVABLE rule (a tick moves GetTarget() onto a
 * live actor), not the TActorIterator another PR may replace it with.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOpsRuntimeSubsystemReattachTest,
	"AirportOps.Present.OpsRuntimeSubsystemReattaches",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FOpsRuntimeSubsystemReattachTest::RunTest(const FString& Parameters)
{
	FOpsSubsystemTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World))
	{
		return false;
	}

	UOpsRuntimeSubsystem* Sub = TestWorld.GameInstance->GetSubsystem<UOpsRuntimeSubsystem>();
	if (!TestNotNull(TEXT("the subsystem exists on a real game instance"), Sub))
	{
		return false;
	}
	if (!TestNotNull(TEXT("Initialize gave it a runtime"), Sub->GetRuntime()))
	{
		return false;
	}

	ARoadNetworkActor* First = TestWorld.World->SpawnActor<ARoadNetworkActor>();
	if (!TestNotNull(TEXT("an actor to attach to"), First))
	{
		return false;
	}

	// NEVER FOUND YET. Nothing calls Attach() directly here - EnsureAttached, reached only
	// through Tick, is the one thing under test.
	Sub->Tick(0.1f);
	TestEqual(TEXT("a tick attaches to the only actor in the world, with no explicit Attach()"),
		Sub->GetRuntime()->GetTarget(), First);

	// THE TARGET IS GONE - a PIE stop or a level change, per the header's own reasoning for
	// EnsureAttached existing. AActor::Destroy() marks it invalid immediately (MarkAsGarbage
	// and removal from the level's actor list), which is exactly the IsValid check
	// EnsureAttached makes - no GC pass needed for this rule to see it.
	First->Destroy();

	ARoadNetworkActor* Second = TestWorld.World->SpawnActor<ARoadNetworkActor>();
	if (!TestNotNull(TEXT("a second actor to re-attach to"), Second))
	{
		return false;
	}

	// RE-ATTACH WHEN THE TARGET IS GONE, not only when it was never found - the exact clause
	// the header names. A stale Runtime->GetTarget() here would mean the old, destroyed actor
	// silently kept driving the sim.
	Sub->Tick(0.1f);
	TestEqual(TEXT("a tick after the old target is destroyed re-attaches to the new actor"),
		Sub->GetRuntime()->GetTarget(), Second);

	return true;
}

/**
 * ISSUE #190: EnsureAttached used to run TActorIterator over the whole level EVERY tick for
 * as long as the target stayed unfound - the entire span before a player has placed a road,
 * or a whole main menu with no game-world actor at all. Measures the fix directly rather
 * than merely naming it: GetActorScanCountForTest is the real cost (a scan of every actor),
 * not a proxy for it - the same reason FFuelBusyWaitSkipsChooseDepotTest counts
 * ChooseDepot calls rather than trusting the shape of the code that calls it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOpsRuntimeSubsystemIdleCostsNoScanTest,
	"AirportOps.Present.OpsRuntimeSubsystemIdleCostsNoScan",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FOpsRuntimeSubsystemIdleCostsNoScanTest::RunTest(const FString& Parameters)
{
	FOpsSubsystemTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World))
	{
		return false;
	}

	UOpsRuntimeSubsystem* Sub = TestWorld.GameInstance->GetSubsystem<UOpsRuntimeSubsystem>();
	if (!TestNotNull(TEXT("the subsystem exists on a real game instance"), Sub))
	{
		return false;
	}

	// NO ACTOR, EVER - the case the ticket names: a world with nothing to find. The first
	// tick may still scan once, to catch an ARoadNetworkActor already placed before this
	// subsystem noticed the world (see AttachToWorld's own comment).
	Sub->Tick(0.1f);
	const int32 AfterFirstTick = Sub->GetActorScanCountForTest();
	TestTrue(TEXT("the first tick scans at most once"), AfterFirstTick <= 1);

	for (int32 Index = 0; Index < 20; ++Index)
	{
		Sub->Tick(0.1f);
	}

	TestEqual(TEXT("twenty more idle ticks against the SAME world scan zero more times"),
		Sub->GetActorScanCountForTest(), AfterFirstTick);

	return true;
}

#endif
