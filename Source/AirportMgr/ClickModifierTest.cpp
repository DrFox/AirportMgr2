#include "CoreMinimal.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "RoadBuildController.h"
#include "Tool/BuildSession.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FClickModifierTest,
	"AirportMgr.Actions.ClickModifier",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FClickModifierTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("a world"), World)) { return false; }
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
	Context.SetCurrentWorld(World);
	ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };

	ARoadBuildController* C = World->SpawnActor<ARoadBuildController>();
	if (!TestNotNull(TEXT("controller spawned"), C)) { return false; }

	// ONE ENUM, NOT TWO BOOLS: the illegal "remove and insert at once" is unrepresentable.
	TestEqual(TEXT("starts with no modifier"), C->GetClickModifier(), EClickModifier::None);
	C->ToggleClickModifier(EClickModifier::Remove);
	TestEqual(TEXT("Remove sticks"), C->GetClickModifier(), EClickModifier::Remove);
	C->ToggleClickModifier(EClickModifier::Insert);
	TestEqual(TEXT("Insert replaces Remove rather than joining it"), C->GetClickModifier(), EClickModifier::Insert);
	C->ToggleClickModifier(EClickModifier::Insert);
	TestEqual(TEXT("toggling the lit one clears it"), C->GetClickModifier(), EClickModifier::None);

	// A mode is chosen for a tool. Picking another tool drops it, so a Remove lit for the
	// road tool cannot silently delete the first stand the player clicks.
	C->ToggleClickModifier(EClickModifier::Remove);
	C->SelectTool(1);
	TestEqual(TEXT("selecting a tool clears the modifier"), C->GetClickModifier(), EClickModifier::None);
	TestEqual(TEXT("and the tool changed"), C->GetActiveToolIndex(), 1);
	C->SelectTool(99);
	TestEqual(TEXT("selecting out of range is refused"), C->GetActiveToolIndex(), 1);
	return true;
}

#endif
