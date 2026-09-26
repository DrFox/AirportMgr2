#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "RoadBuildController.h"
#include "BuildActions.h"
#include "Testing/AirsideTestWorld.h"
#include "Tool/BuildSession.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FClickModifierTest,
	"AirportMgr.Actions.ClickModifier",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FClickModifierTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld(/*bSpawnActor=*/false);
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }

	ARoadBuildController* C = TestWorld.World->SpawnActor<ARoadBuildController>();
	if (!TestNotNull(TEXT("controller spawned"), C)) { return false; }

	// ONE ENUM, NOT A SET OF FLAGS: every illegal combination is unrepresentable, and that
	// now includes Edit. It used to be a SECOND field on this class, so Remove and Edit
	// could both be lit - the bar drew both and Edit silently won, because it decides which
	// tool runs at all. Reported from play, 2026-09-20.
	TestEqual(TEXT("starts in Build"), C->GetGestureMode(), EGestureMode::Build);

	C->ToggleGestureMode(EGestureMode::Remove);
	TestEqual(TEXT("Remove sticks"), C->GetGestureMode(), EGestureMode::Remove);

	C->ToggleGestureMode(EGestureMode::Insert);
	TestEqual(TEXT("Insert replaces Remove rather than joining it"),
		C->GetGestureMode(), EGestureMode::Insert);

	// THE REPORTED CLASH. Entering Edit must LEAVE the modifier, not sit on top of it.
	C->ToggleGestureMode(EGestureMode::Remove);
	C->ToggleGestureMode(EGestureMode::Edit);
	TestEqual(TEXT("Edit replaces Remove rather than overriding it while it stays lit"),
		C->GetGestureMode(), EGestureMode::Edit);

	// AND THE OTHER WAY ROUND, which is the half a one-directional fix would miss.
	C->ToggleGestureMode(EGestureMode::Remove);
	TestEqual(TEXT("and Remove replaces Edit"), C->GetGestureMode(), EGestureMode::Remove);

	C->ToggleGestureMode(EGestureMode::Remove);
	TestEqual(TEXT("toggling the lit one returns to Build"),
		C->GetGestureMode(), EGestureMode::Build);

	// A BUILD MODIFIER IS CHOSEN FOR A TOOL. Picking another drops it, so a Remove lit for
	// the road tool cannot silently delete the first stand the player clicks.
	C->ToggleGestureMode(EGestureMode::Remove);
	C->SelectTool(1);
	TestEqual(TEXT("selecting a tool clears a sticky build modifier"),
		C->GetGestureMode(), EGestureMode::Build);
	TestEqual(TEXT("and the tool changed"), C->GetActiveToolIndex(), 1);

	// AND SO DOES EDIT (2026-09-26, reversing the rule that kept it). Picking a tool is asking
	// to BUILD with it: Edit kept across a switch left the edit tool lit over the fuel depot,
	// which has no handles, and the player picked a tool and could not place one - reported
	// from play, the log reading "Bar: tool.fuel depot" then "Tool: Edit".
	C->ToggleGestureMode(EGestureMode::Edit);
	C->SelectTool(2);
	TestEqual(TEXT("selecting a tool leaves Edit for Build - a picked tool places"),
		C->GetGestureMode(), EGestureMode::Build);
	TestEqual(TEXT("and that tool changed too"), C->GetActiveToolIndex(), 2);

	// RESELECTING the lit tool is not a switch, and leaves the mode alone: the key pressed
	// again cycles a width, and must not also drop the player out of Edit.
	C->ToggleGestureMode(EGestureMode::Edit);
	C->SelectTool(2);
	TestEqual(TEXT("reselecting the lit tool keeps Edit"), C->GetGestureMode(), EGestureMode::Edit);

	C->ToggleGestureMode(EGestureMode::Build);
	C->SelectTool(99);
	TestEqual(TEXT("selecting out of range is refused, leaving the tool as it was"),
		C->GetActiveToolIndex(), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGestureModeCoverageTest,
	"AirportMgr.Actions.EveryGestureModeIsHandled",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGestureModeCoverageTest::RunTest(const FString& Parameters)
{
	// THE ONE-LIST CHECK FOR EGestureMode, and the reason it exists rather than a Strategy:
	// the mode is read at five sites, and NOTHING makes a new member be considered at any of
	// them. UBT's SwitchWarningLevel is off - PreviewPalette.cpp says so at length - so a
	// fifth mode added tomorrow compiles perfectly and behaves as Build everywhere. That is
	// "check where a list is CONSUMED" in embryo, and this codebase has shipped it three
	// times.
	//
	// PreviewPalette solved exactly this shape with a test that walks every value rather
	// than with a pattern; FRoadBuildHUDLooksTest is that test, and it is what stopped
	// EPreviewStyle::Handle slipping through unseeded. This is its counterpart.
	//
	// COUNTED THROUGH REFLECTION, not up to a hand-written bound. EGestureMode is a UENUM for
	// exactly this: FRoadBuildHUDLooksTest walks EPreviewStyle up to a literal last member and
	// its own comment calls that bound "the fifth list a new style has to appear in, and the
	// only one nothing else would have caught". Adding a mode here needs no edit to this test
	// at all - it simply starts being checked.
	FAirsideTestWorld TestWorld(/*bSpawnActor=*/false);
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }

	ARoadBuildController* C = TestWorld.World->SpawnActor<ARoadBuildController>();
	if (!TestNotNull(TEXT("controller spawned"), C)) { return false; }

	const UEnum* Enum = StaticEnum<EGestureMode>();
	if (!TestNotNull(TEXT("EGestureMode is reflected, so this test can count it"), Enum))
	{
		return false;
	}

	// NumEnums() includes the generated _MAX sentinel, which is not a mode.
	const int32 Count = Enum->NumEnums() - 1;
	TestTrue(TEXT("there is more than one mode to be exclusive about"), Count >= 2);

	for (int32 M = 0; M < Count; ++M)
	{
		const EGestureMode Mode = static_cast<EGestureMode>(Enum->GetValueByIndex(M));

		// 1. EVERY MODE IS REACHABLE AND STICKS. A member nothing can enter is a member that
		// does nothing, which is how a mode comes to exist only in the enum.
		C->SelectTool(1);                          // Taxiway: has edit handles, so Edit is legal
		C->ToggleGestureMode(EGestureMode::Build); // a known starting point
		if (Mode != EGestureMode::Build)
		{
			C->ToggleGestureMode(Mode);
		}
		TestEqual(*FString::Printf(TEXT("%s can be entered and sticks"),
			*Enum->GetNameStringByIndex(M)),
			C->GetGestureMode(), Mode);

		// 2. EVERY MODE EXCLUDES EVERY OTHER. The reported defect was two lit at once, so
		// this is measured for each pair rather than asserted once for the pair that broke.
		for (int32 O = 0; O < Count; ++O)
		{
			const EGestureMode Other = static_cast<EGestureMode>(Enum->GetValueByIndex(O));
			if (Other == Mode)
			{
				continue;
			}
			C->ToggleGestureMode(Other);
			TestEqual(*FString::Printf(
				TEXT("entering %s leaves %s rather than sitting on top of it"),
				*Enum->GetNameStringByIndex(O), *Enum->GetNameStringByIndex(M)),
				C->GetGestureMode(), Other);

			C->ToggleGestureMode(EGestureMode::Build);
			if (Mode != EGestureMode::Build)
			{
				C->ToggleGestureMode(Mode);
			}
		}
	}

	// 3. EVERY MODE HAS A BAR ROW, so none is reachable only from code. The rows are what
	// make a mode visible at all, and a mode with no button is one the player cannot leave.
	int32 Rows = 0;
	for (const FBuildAction& Action : BuildActions())
	{
		Rows += (Action.Section == EActionSection::Edit && Action.IsActive
			&& Action.Id != FName(TEXT("edit.build"))
			&& Action.Id != FName(TEXT("edit.undo"))
			&& Action.Id != FName(TEXT("edit.redo"))
			&& Action.Id != FName(TEXT("edit.clear"))) ? 1 : 0;
	}
	TestEqual(TEXT("every mode but Build has a bar row - Build is the absence of the others"),
		Rows, Count - 1);
	return true;
}

#endif
