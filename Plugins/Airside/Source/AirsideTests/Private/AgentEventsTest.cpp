#include "CoreMinimal.h"
#include "Content/AirsideSettings.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Model/ArrivalPlanner.h"
#include "Present/AirsideTraffic.h"
#include "Present/RoadNetworkActor.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FArrivalRefusedEventTest,
	"Airside.Present.ArrivalRefusedEvent",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FArrivalRefusedEventTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("a world to spawn into"), World)) { return false; }
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
	Context.SetCurrentWorld(World);
	ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };

	ARoadNetworkActor* Actor = World->SpawnActor<ARoadNetworkActor>();
	if (!TestNotNull(TEXT("actor spawned"), Actor)) { return false; }
	Actor->PlaceNode(FVector2D::ZeroVector);  // forces the network into existence; no runway on it

	TArray<EArrivalRefusal> Refusals;
	Actor->GetTraffic()->OnArrivalRefused.AddLambda([&Refusals](EArrivalRefusal Why) { Refusals.Add(Why); });

	// THE REFUSAL IS AN EVENT, not only a log line. AirportOps' flight board has to divert a
	// flight when the airfield cannot take it, and a warning in the log is not something code
	// can act on.
	const bool bDispatched = Actor->DispatchArrival(FVector2D(1000.0, 0.0), UAirsideSettings::ResolveDefaultAirframe());
	TestFalse(TEXT("an airport with no runway refuses the arrival"), bDispatched);
	TestEqual(TEXT("the refusal is announced exactly once"), Refusals.Num(), 1);
	if (Refusals.Num() == 1)
	{
		TestEqual(TEXT("and names the reason the planner found"), Refusals[0], EArrivalRefusal::NoRunway);
	}
	TestEqual(TEXT("no agent exists after a refusal"), Actor->GetTraffic()->GetAgentCount(), 0);
	return true;
}

#endif
