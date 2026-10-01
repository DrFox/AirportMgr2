#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
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
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world to spawn into"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor spawned"), Actor)) { return false; }
	Actor->PlaceNode(FVector2D::ZeroVector);  // forces the network into existence; no runway on it

	// THE MODEL'S OWN DELEGATE, through the presenter's GetModel() - the presenter stopped relaying it (#445 item 6), so this is
	// where AirportOps' bridge binds too.
	TArray<EArrivalRefusal> Refusals;
	TArray<FString> Sentences;
	Actor->GetTraffic()->GetModel()->OnArrivalRefused.AddLambda([&Refusals, &Sentences](EArrivalRefusal Why, const FString& Sentence)
	{
		Refusals.Add(Why);
		Sentences.Add(Sentence);
	});

	// THE REFUSAL IS AN EVENT, not only a log line. AirportOps' flight board has to divert a
	// flight when the airfield cannot take it, and a warning in the log is not something code
	// can act on.
	const bool bDispatched = Actor->DispatchArrival(FVector2D(1000.0, 0.0), UAirsideSettings::ResolveDefaultAirframe());
	TestFalse(TEXT("an airport with no runway refuses the arrival"), bDispatched);
	TestEqual(TEXT("the refusal is announced exactly once"), Refusals.Num(), 1);
	if (Refusals.Num() == 1)
	{
		TestEqual(TEXT("and names the reason the planner found"), Refusals[0], EArrivalRefusal::NoRunway);
		// #471: AND THE PLAN'S OWN SENTENCE, on the model's own delegate - the line the model logs, so a listener (the ops
		// toast, through its bridge) can say what the log says. Empty would mean the broadcast dropped it.
		// No occupancy: a field with no runway is refused before the table is read, so the sentence is the same.
		const FArrivalPlan Plan = ArrivalPlanner::Plan(*Actor->Network, FVector2D(1000.0, 0.0), UAirsideSettings::ResolveDefaultAirframe());
		TestEqual(TEXT("with the plan's own sentence"), Sentences[0], ArrivalPlanner::DescribeRefusal(Plan));
		TestFalse(TEXT("which is not empty"), Sentences[0].IsEmpty());
	}
	TestEqual(TEXT("no agent exists after a refusal"), Actor->GetTraffic()->GetAgentCount(), 0);
	return true;
}

#endif
