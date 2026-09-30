#include "CoreMinimal.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Model/Ledger.h"
#include "Model/SimClock.h"
#include "Present/OpsRuntime.h"
#include "Present/OpsRuntimeSubsystem.h"
#include "Present/RoadNetworkActor.h"
#include "Present/RoadNetworkRegistry.h"
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
 * A GAME INSTANCE THAT SHUTS DOWN WITH ITS AIRPORT STILL ATTACHED (#445): Deinitialize drops the registry's handle - the one that detaches
 * on the airport's Left announcement - so a runtime that simply went would leave the facade's raw IBuildPurse* pointing at its ledger and the
 * actor's delegates bound to it. Deinitialize detaches, through the same Detach the announcement uses.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOpsRuntimeSubsystemDeinitializeTest,
	"AirportOps.Present.OpsRuntimeSubsystemDetachesOnDeinitialize",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FOpsRuntimeSubsystemDeinitializeTest::RunTest(const FString& Parameters)
{
	FOpsSubsystemTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	UOpsRuntimeSubsystem* Sub = TestWorld.GameInstance->GetSubsystem<UOpsRuntimeSubsystem>();
	if (!TestNotNull(TEXT("the subsystem"), Sub) || !TestNotNull(TEXT("with a runtime"), Sub->GetRuntime())) { return false; }
	ARoadNetworkActor* Airport = TestWorld.World->SpawnActor<ARoadNetworkActor>();
	if (!TestNotNull(TEXT("an airport"), Airport)) { return false; }
	UOpsRuntime* Runtime = Sub->GetRuntime();
	if (!TestEqual(TEXT("setup: attached to it"), Runtime->GetTarget(), Airport)) { return false; }
	TestTrue(TEXT("setup: its ledger publishes onto the runtime's bus"), Runtime->GetLedger()->Bus == &Runtime->GetBus());

	Sub->Deinitialize();
	TestNull(TEXT("deinitialised with the airport still there, the runtime is detached from it"), Runtime->GetTarget());
	for (const UOpsRuntime::FOpsBusPublisher& Each : Runtime->Publishers())
	{
		TestNull(*FString::Printf(TEXT("and the %s no longer points at the bus"), Each.Name), *Each.Slot);
	}
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

/**
 * A REREGISTER IS NOT A DEPARTURE (#446 review). The engine unregisters an actor's components with bForReregister FALSE
 * and then reregisters them - AActor::PreEditChange/PostEditChangeProperty on a Details edit in Simulate-In-Editor
 * (ActorEditor.cpp), a construction-script rerun - and the airport is still there throughout. An Attach is a NEW GAME
 * (the clock restarts, the ledger reopens, airlines and alerts reset), so a registry that read the unregister as the
 * airport leaving, and the reregister as a new one arriving, restarted the player's game on a property edit.
 * Measured on what a restart would lose - the game time and the balance - and on the registry's own "left" line.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOpsRuntimeSubsystemReregisterTest,
	"AirportOps.Present.OpsRuntimeSubsystemSurvivesAReregister",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FOpsRuntimeSubsystemReregisterTest::RunTest(const FString& Parameters)
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
	ARoadNetworkActor* Airport = TestWorld.World->SpawnActor<ARoadNetworkActor>();
	if (!TestNotNull(TEXT("an airport"), Airport))
	{
		return false;
	}
	UOpsRuntime* Runtime = Sub->GetRuntime();
	if (!TestEqual(TEXT("setup: ops attached to it"), Runtime->GetTarget(), Airport))
	{
		return false;
	}

	// A GAME IN PROGRESS: time has passed and money has moved, so a restart would show on both.
	Sub->Tick(5.0f);
	Runtime->GetLedger()->PostDailyUpkeep(1234.0, Runtime->GetClock()->Now());
	const double Now = Runtime->GetClock()->Now();
	const double Balance = Runtime->GetLedger()->Balance();

	FLogLineSpy Spy(FName(TEXT("LogAirside")));
	GLog->AddOutputDevice(&Spy);
	// THE ENGINE'S SHAPE, not UnregisterAllComponents(true): the Details-edit path passes false, then reregisters.
	Airport->UnregisterAllComponents();
	Airport->ReregisterAllComponents();
	GLog->RemoveOutputDevice(&Spy);

	TestEqual(TEXT("ops is still attached to the same airport"), Runtime->GetTarget(), Airport);
	TestEqual(TEXT("the game time did not restart - no re-attach ran"), Runtime->GetClock()->Now(), Now, 1e-9);
	TestEqual(TEXT("the ledger did not reopen"), Runtime->GetLedger()->Balance(), Balance, 1e-9);
	TestEqual(TEXT("and the registry never said the airport left"),
		Spy.CapturedLines.FilterByPredicate([](const FString& Line) { return Line.Contains(TEXT("Airport registry")) && Line.Contains(TEXT(" left ")); }).Num(), 0);

	// DEFENCE IN DEPTH: even an arrival re-announced for the airport being played restarts nothing - the subsystem
	// treats "attach to what is attached" as a no-op, because an Attach is a new game.
	URoadNetworkRegistry::OnAirportChanged().Broadcast(*TestWorld.World, *Airport, EAirportRegistration::Arrived);
	TestEqual(TEXT("a re-announced arrival of the target is no re-attach: the game time holds"), Runtime->GetClock()->Now(), Now, 1e-9);
	TestEqual(TEXT("and so does the balance"), Runtime->GetLedger()->Balance(), Balance, 1e-9);
	return true;
}

#endif
