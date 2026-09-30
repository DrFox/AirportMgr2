#include "CoreMinimal.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Present/OpsRuntime.h"
#include "Present/OpsRuntimeSubsystem.h"
#include "Present/RoadNetworkActor.h"
#include "Testing/AirsideTestWorld.h"

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
 * UOpsRuntimeSubsystem's own header calls itself "mostly a forwarder" - but the attach is the one
 * path that recovers UOpsRuntime's target after a level change, and issue #194 found nothing
 * measuring it. Per the brief: test the OBSERVABLE rule (GetTarget() follows the world's airport),
 * not the mechanism - which #446 replaced: it was an EnsureAttached reached only through Tick (a
 * catch-up scan, an OnActorSpawned hook and a per-tick IsValid), and is URoadNetworkRegistry's
 * announcement now. So NO Tick below: an attach or detach that needed one would be the poll back.
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
	TestNull(TEXT("control: nothing attached before an airport exists"), Sub->GetRuntime()->GetTarget());

	ARoadNetworkActor* First = TestWorld.World->SpawnActor<ARoadNetworkActor>();
	if (!TestNotNull(TEXT("an actor to attach to"), First))
	{
		return false;
	}

	// NO TICK, NO EXPLICIT Attach(): the actor registered itself, and the registry told the subsystem.
	TestEqual(TEXT("the airport's registration attaches ops to it, with no tick and no explicit Attach()"),
		Sub->GetRuntime()->GetTarget(), First);

	// THE TARGET IS GONE - a PIE stop or a level change. A REAL DETACH (#446): the actor gives its slot back as
	// it goes (EndPlay / UnregisterAllComponents), and ops unbinds from it then - it used to keep the dead
	// pointer until a later tick's IsValid noticed.
	First->Destroy();
	TestNull(TEXT("the airport leaving detaches ops at once, with no tick"), Sub->GetRuntime()->GetTarget());

	ARoadNetworkActor* Second = TestWorld.World->SpawnActor<ARoadNetworkActor>();
	if (!TestNotNull(TEXT("a second actor to re-attach to"), Second))
	{
		return false;
	}

	// RE-ATTACH WHEN THE TARGET IS GONE, not only when it was never found. A stale Runtime->GetTarget() here
	// would mean the old, destroyed actor silently kept driving the sim.
	TestEqual(TEXT("a new airport after the old one left attaches ops to it"),
		Sub->GetRuntime()->GetTarget(), Second);

	return true;
}

/**
 * THE REGISTRY'S LIST IS EVERY WORLD'S (#446) - one static delegate, because this game-instance subsystem outlives
 * worlds - so the subsystem filters: an airport registered in a world this game instance does not own (the editor
 * world, another PIE instance's) must not attach ops. Replaces AirportOps.Present.OpsRuntimeSubsystemIdleCostsNoScan,
 * whose measurement (the catch-up scan ran once per world, not once per tick - issue #190) went with the scan itself.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOpsRuntimeSubsystemOtherWorldTest,
	"AirportOps.Present.OpsRuntimeSubsystemIgnoresAnotherWorldsAirport",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FOpsRuntimeSubsystemOtherWorldTest::RunTest(const FString& Parameters)
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

	// ANOTHER WORLD, owned by no game instance - an editor world's shape. Its actor registers and announces.
	FAirsideTestWorld Elsewhere;
	if (!TestNotNull(TEXT("an airport in another world"), Elsewhere.Actor))
	{
		return false;
	}
	TestNull(TEXT("another world's airport does not attach this game instance's ops"), Sub->GetRuntime()->GetTarget());

	ARoadNetworkActor* Ours = TestWorld.World->SpawnActor<ARoadNetworkActor>();
	if (!TestNotNull(TEXT("an airport in our own world"), Ours))
	{
		return false;
	}
	TestEqual(TEXT("our own does (the control: the filter is not simply refusing everything)"),
		Sub->GetRuntime()->GetTarget(), Ours);

	Elsewhere.Actor->Destroy();
	TestEqual(TEXT("and another world's airport LEAVING does not detach us"), Sub->GetRuntime()->GetTarget(), Ours);
	return true;
}

#endif
