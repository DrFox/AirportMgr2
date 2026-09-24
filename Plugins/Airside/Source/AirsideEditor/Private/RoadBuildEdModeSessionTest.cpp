#include "CoreMinimal.h"
#include "Framework/Commands/UICommandList.h"
#include "InteractiveToolManager.h"
#include "Misc/AutomationTest.h"
#include "Present/RoadNetworkActor.h"
#include "RoadBuildEdMode.h"
#include "RoadBuildEdModeCommands.h"
#include "RoadBuildEditorTool.h"
#include "Testing/AirsideTestWorld.h"
#include "Tool/BuildSession.h"
#include "Tool/RoadDrawTool.h"
#include "Tool/RunwayTool.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * The editor's build session outlives the tool instances that drive it.
 *
 * THE BUG THIS EXISTS FOR, which shipped and was found by hand. FBuildSession used to be a
 * member of URoadBuildEditorTool, and the InteractiveTools framework builds a NEW tool object
 * on every activation - so the session, and with it every FRunwayTool's WidthIndex, was
 * destroyed and rebuilt each time a tool was picked. In the editor a runway could therefore
 * only ever be laid at the first width in the list, while the same key cycled 18/23/30/45/60
 * perfectly well in PIE, where ARoadBuildController holds one long-lived session.
 *
 * Two separate things had to be true for that to happen, so this checks both: the session is
 * SHARED (the tool points at the mode's), and a reselect on the shared session actually
 * cycles the width. Either alone would pass while the feature stayed broken.
 *
 * A THIRD THING joined them at issue #78's review: the reselect's FToolContext needs a real
 * Target now that FRunwayTool::NextWidth reads runway profiles through IRoadEditTarget
 * instead of content directly, and URoadBuildEdMode::StartToolAction had stopped resolving
 * one. This drives the reselect through Mode->MakeReselectContext() - the exact resolution
 * StartToolAction uses - rather than building an unrelated ARoadNetworkActor of its own,
 * which is what let an earlier version of this test pass while the mode stayed broken.
 *
 * IN AirsideEditor rather than AirsideTests, for the reason ToolCommandsMatchRegistry gives:
 * AirsideTests depends on Airside alone, and making it reach an editor class would invert the
 * direction the plugin is built on.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadBuildEdModeSessionTest,
	"Airside.Editor.ToolStateOutlivesTheToolInstance",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadBuildEdModeSessionTest::RunTest(const FString& Parameters)
{
	URoadBuildEdMode* Mode = NewObject<URoadBuildEdMode>(GetTransientPackage());
	if (!TestNotNull(TEXT("an ed mode"), Mode))
	{
		return false;
	}

	// A REAL WORLD, purely so MakeReselectContext's ARoadNetworkActor::FindOrCreate(GetWorld())
	// has something to search and spawn into - see WorldOverrideForTest's own comment for why
	// GetWorld() needs help here at all. Not the same as registering the mode with a live
	// editor (Enter() is never called); this substitutes for the ONE thing this test's
	// reselect assertions need from a real editor session.
	FAirsideTestWorld TestWorld(/*bSpawnActor=*/false, EWorldType::Editor);
	if (!TestNotNull(TEXT("a world for the mode to resolve a target in"), TestWorld.World))
	{
		return false;
	}
	Mode->WorldOverrideForTest = TestWorld.World;

	// The runway tool, found by its registry key rather than a literal index - the table is
	// not contiguous and an index written here would be a second claim about its order.
	int32 RunwayIndex = INDEX_NONE;
	for (int32 Index = 0; Index < ToolRegistry().Num(); ++Index)
	{
		if (ToolRegistry()[Index].Key == EKeys::Six)
		{
			RunwayIndex = Index;
		}
	}
	if (!TestTrue(TEXT("the registry has the runway tool on 6"), RunwayIndex != INDEX_NONE))
	{
		return false;
	}

	// THE SHARING, checked through the builder rather than by setting the pointer by hand:
	// it is the builder that has to do this, and a test that wired it itself would pass with
	// the builder unchanged - which is exactly how this shipped broken.
	URoadBuildEditorToolBuilder* Builder = NewObject<URoadBuildEditorToolBuilder>(Mode);
	Builder->ToolIndex = RunwayIndex;

	// A REAL tool manager, because BuildTool uses it as the new tool's OUTER and NewObject
	// with a null outer crashes rather than returning null - which is how the first run of
	// this test died, in BuildTool rather than in anything it was testing.
	FToolBuilderState State;
	State.ToolManager = NewObject<UInteractiveToolManager>(Mode);

	UInteractiveTool* Built = Builder->BuildTool(State);
	URoadBuildEditorTool* Tool = Cast<URoadBuildEditorTool>(Built);
	if (!TestNotNull(TEXT("the builder made a tool"), Tool))
	{
		return false;
	}
	TestEqual(TEXT("the tool drives the MODE's session, not one of its own"),
		Tool->SessionForTest(), static_cast<const FBuildSession*>(&Mode->GetSession()));

	// A second instance, as a second activation of the same palette entry would produce.
	URoadBuildEditorTool* Again = Cast<URoadBuildEditorTool>(Builder->BuildTool(State));
	if (!TestNotNull(TEXT("a second activation builds a second tool"), Again))
	{
		return false;
	}
	TestEqual(TEXT("and it drives the same session, so state survives the rebuild"),
		Again->SessionForTest(), Tool->SessionForTest());

	// THE RESELECT'S CONTEXT, THROUGH THE MODE - not a target this test builds itself.
	// StartToolAction's own lambda cannot be driven directly here: it is gated on
	// GetToolManager()/GetInteractiveToolsContext(), both null on a bare
	// NewObject<URoadBuildEdMode> with no Enter() and no real editor viewport (confirmed
	// against UEdMode.cpp - EditorToolsContext/ModeToolsContext are set only by
	// CreateInteractiveToolsContexts(), called from mode registration this test does not
	// do). Mode->MakeReselectContext() is what StartToolAction calls once that gate passes,
	// so calling it here exercises the SAME resolution a real reselect uses - unlike an
	// earlier version of this test, which built an unrelated ARoadNetworkActor of its own
	// and could not have caught the mode forgetting to resolve one (issue #78's review).
	FBuildSession& Session = Mode->GetSession();
	const FToolContext ReselectContext = Mode->MakeReselectContext();
	TestNotNull(TEXT("the mode resolves a real target for a reselect"), ReselectContext.Target);

	// THE RESELECT, on the shared session. Selecting the runway tool twice is what a second
	// press of its key does, and the second press must cycle the width rather than do nothing.
	Session.SelectTool(RunwayIndex, ReselectContext);
	FRunwayTool* Runway = static_cast<FRunwayTool*>(Session.GetActiveTool());
	if (!TestNotNull(TEXT("the runway tool is active on the shared session"), Runway))
	{
		return false;
	}
	TestEqual(TEXT("it starts on the first width"), Runway->WidthIndex, 0);

	Session.SelectTool(RunwayIndex, ReselectContext);
	TestEqual(TEXT("picking it again cycles the width - the press that did nothing before"),
		Runway->WidthIndex, 1);

	// And the choice survives the tool object being rebuilt, which is the whole point: the
	// same pointer, still at width 1, after a third activation.
	URoadBuildEditorTool* Third = Cast<URoadBuildEditorTool>(Builder->BuildTool(State));
	TestNotNull(TEXT("a third activation builds"), Third);
	TestEqual(TEXT("the width chosen before it was rebuilt is still chosen"),
		static_cast<FRunwayTool*>(Mode->GetSession().GetActiveTool())->WidthIndex, 1);
	return true;
}

/**
 * THE BUG ITSELF (issue #184), not the session logic StartToolAction reaches once a key press
 * gets to it - FRoadBuildEdModeSessionTest above already covers that a reselect on the shared
 * session cycles width, and would pass even if no key press could ever reach StartToolAction at
 * all. This test is about which of two MapAction calls a key press actually lands on.
 *
 * UEdMode::Enter() calls BindCommands() BEFORE running URoadBuildEdMode::Enter()'s own body,
 * where RegisterTool runs - and RegisterTool maps the SAME command onto the SAME toolkit command
 * list with the engine's own StartTool. UICommandList::MapAction is a TMap::Add, which replaces:
 * whichever call happens LAST wins. BindCommands used to map this mode's own StartToolAction for
 * every tool command, and RegisterTool always ran after it and always overwrote it - so every key
 * press restarted the tool via the engine's StartTool, and the reselect guard in StartToolAction
 * was dead code no test could see was dead, because FRoadBuildEdModeSessionTest drives the
 * session directly and never asks the command list to run anything.
 *
 * CANNOT DRIVE THE REAL RegisterTool HERE - same gate as everywhere else in this file:
 * RegisterTool needs a live UEditorInteractiveToolsContext, which only Enter() with a real
 * editor viewport creates, and Run-AirsideTests.ps1 runs with -nullrhi (see the script's own
 * comment). Standing in for it with a spy that MapAction's the SAME command, in the SAME order
 * Enter() actually calls things (BindCommands, then something standing in for RegisterTool, then
 * MapReselectAwareToolCommand) is what lets this test tell "our binding wins" apart from "our
 * binding happened to run and nothing since touched the list" - the second is true of the OLD
 * code too and would not have caught it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadBuildEdModeCommandBindingTest,
	"Airside.Editor.ToolCommandBindingSurvivesRegisterTool",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadBuildEdModeCommandBindingTest::RunTest(const FString& Parameters)
{
	if (!TestTrue(TEXT("the command set is registered"), FRoadBuildEdModeCommands::IsRegistered()))
	{
		return false;
	}

	URoadBuildEdMode* Mode = NewObject<URoadBuildEdMode>(GetTransientPackage());
	if (!TestNotNull(TEXT("an ed mode"), Mode))
	{
		return false;
	}

	// CreateToolkit() alone, not Enter(): FBaseToolkit's own constructor builds ToolkitCommands
	// (see BaseToolkit.cpp), so a real FUICommandList exists without Toolkit->Init()'s
	// IToolkitHost - which is the one piece of a real Enter() this test cannot get headlessly.
	Mode->CreateToolkit();

	const TSharedPtr<FUICommandList> CommandList = Mode->ToolkitCommandsForTest();
	if (!TestTrue(TEXT("CreateToolkit made a real command list"), CommandList.IsValid()))
	{
		return false;
	}

	const TArray<TSharedPtr<FUICommandInfo>> ToolCommands =
		FRoadBuildEdModeCommands::Get().ToolCommandsInOrder();
	if (!TestTrue(TEXT("the registry has at least one tool"), ToolCommands.Num() > 0))
	{
		return false;
	}
	const TSharedPtr<FUICommandInfo> Command = ToolCommands[0];

	// STEP ONE, in Enter()'s own order: BindCommands runs first. On today's fixed code this
	// only maps CancelGesture and leaves Command untouched; the OLD code mapped
	// StartToolAction(0) onto it here.
	Mode->BindCommands();

	// STEP TWO: RegisterTool, stood in for by a spy, because the real one needs a live
	// UEditorInteractiveToolsContext this test cannot create. This is exactly what RegisterTool
	// itself does at UEdMode.cpp:113 - MapAction the same command onto the same list.
	bool bEngineBindingRan = false;
	CommandList->MapAction(Command,
		FExecuteAction::CreateLambda([&bEngineBindingRan]() { bEngineBindingRan = true; }));

	// STEP THREE: what Enter() calls immediately after RegisterTool now (issue #184's fix).
	Mode->MapReselectAwareToolCommand(0, Command);

	const FUIAction* Action = CommandList->GetActionForCommand(Command);
	if (!TestNotNull(TEXT("the command still has a mapped action"), Action))
	{
		return false;
	}

	// THE ASSERTION THAT FAILS ON THE OLD ORDER. If BindCommands's binding were still the one
	// standing after RegisterTool - i.e. if this mode's own action, not the spy, survived
	// step two - this test would have nothing to say about issue #184 at all, because that is
	// not the bug: the bug is the spy (RegisterTool) winning over step three. Executing here
	// exercises the SAME TMap::Add path production hits when a key is pressed.
	Action->Execute();
	TestFalse(TEXT("the tool's own reselect-aware binding, not RegisterTool's restart, "
		"is what a key press reaches"), bEngineBindingRan);

	// The mapping is still a live, executable command, not merely "not the engine's" -
	// FCanExecuteAction() left unbound in MapReselectAwareToolCommand defaults to always
	// executable (FUIAction::CanExecute), the same as the pre-#184 hand-written binding did.
	TestTrue(TEXT("the command can still execute"), Action->CanExecute());

	return true;
}

/**
 * ISSUE #190: URoadBuildEditorTool::OwnSession used to be a VALUE member - a full nine-tool
 * FBuildSession (FRoadDrawTool x2, FApronDrawTool, FStandPlotTool, FGuidelineDrawTool,
 * FRunwayTool, FHoldingPointTool, FPlotPlaceTool, FSelectTool) constructed by every activation
 * ITF made, whether or not this instance ever read it - which it never does once
 * SetSharedSession has been called, since Sess() always prefers SharedSession. This is what
 * FRoadBuildEdModeSessionTest's own activations above have always paid without either test
 * being able to say so: nothing there asks whether OwnSession got built, only whether
 * SharedSession did.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadBuildEditorToolLazyOwnSessionTest,
	"Airside.Editor.SharedSessionActivationAllocatesNoOwnSession",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadBuildEditorToolLazyOwnSessionTest::RunTest(const FString& Parameters)
{
	URoadBuildEdMode* Mode = NewObject<URoadBuildEdMode>(GetTransientPackage());
	if (!TestNotNull(TEXT("an ed mode"), Mode))
	{
		return false;
	}

	URoadBuildEditorToolBuilder* Builder = NewObject<URoadBuildEditorToolBuilder>(Mode);
	Builder->ToolIndex = 0;
	FToolBuilderState State;
	State.ToolManager = NewObject<UInteractiveToolManager>(Mode);

	UInteractiveTool* Built = Builder->BuildTool(State);
	URoadBuildEditorTool* Tool = Cast<URoadBuildEditorTool>(Built);
	if (!TestNotNull(TEXT("the builder made a tool"), Tool))
	{
		return false;
	}
	TestNotNull(TEXT("the builder shares the mode's session"), Tool->SessionForTest());
	TestFalse(TEXT("BuildTool alone allocates nothing"), Tool->OwnSessionAllocatedForTest());

	// Setup() IS THE HEAVY CALLER - it runs Sess().SelectTool, GetActiveTool and
	// GetDisplayName, exactly the calls that used to find OwnSession sitting there built and
	// read SharedSession instead. If Sess() ever fell back to OwnSession despite SharedSession
	// being set, this is where it would have allocated it.
	Tool->Setup();
	TestFalse(TEXT("Setup() with a shared session still allocates no fallback session"),
		Tool->OwnSessionAllocatedForTest());

	return true;
}

/**
 * ISSUE #185: the fuel depot's last stage, OnCommit, had no editor command at all. The
 * editor mode's command set was the nine tool commands (checked above by
 * ToolCommandsMatchRegistry) plus Cancel and nothing else - "list declared, consumer
 * missing" in its fourth form (see CLAUDE.md's "Check where a list is CONSUMED"): PIE has
 * always had edit.build on Enter reaching IBuildTool::OnCommit (BuildActions.cpp), and the
 * editor mode had no equivalent key at all, so a depot could be drawn there and never placed.
 *
 * NAMES Build AND Cancel BY HAND rather than reading some third shared list, because there
 * is no table both drivers can see to build one from: AirsideEditor.Build.cs depends on
 * Airside only (the runtime plugin must never depend on the game), so this test cannot reach
 * across to Source/AirportMgr/BuildActions.cpp and enumerate PIE's own Edit section the way
 * ToolCommandsMatchRegistry enumerates ToolRegistry(), which lives in Airside where both
 * sides can see it. Cancel and Build are exactly the two non-tool verbs
 * FRoadBuildEdModeCommands hand-declares today; a third would be added to this same list by
 * hand, same as it would be added to that class.
 *
 * THE SAME SEAM ToolCommandBindingSurvivesRegisterTool uses just above - CreateToolkit() for
 * a real FUICommandList without a live IToolkitHost, then ToolkitCommandsForTest() to read
 * back what BindCommands() actually installed - because the bug this guards against is
 * exactly that shape: a command that EXISTS but was never MapAction'd onto the list a key
 * press actually reaches.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadBuildEdModeEditVerbTest,
	"Airside.Editor.EveryRuntimeEditVerbHasACommand",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadBuildEdModeEditVerbTest::RunTest(const FString& Parameters)
{
	if (!TestTrue(TEXT("the command set is registered"), FRoadBuildEdModeCommands::IsRegistered()))
	{
		return false;
	}

	URoadBuildEdMode* Mode = NewObject<URoadBuildEdMode>(GetTransientPackage());
	if (!TestNotNull(TEXT("an ed mode"), Mode))
	{
		return false;
	}

	// CreateToolkit() then BindCommands() directly, not Enter(): the same substitution
	// ToolCommandBindingSurvivesRegisterTool makes, for the same reason - Enter() needs a
	// live editor viewport this test does not have, and FBaseToolkit's own constructor
	// already builds a real FUICommandList without one.
	Mode->CreateToolkit();
	Mode->BindCommands();

	const TSharedPtr<FUICommandList> CommandList = Mode->ToolkitCommandsForTest();
	if (!TestTrue(TEXT("CreateToolkit made a real command list"), CommandList.IsValid()))
	{
		return false;
	}

	const FRoadBuildEdModeCommands& Commands = FRoadBuildEdModeCommands::Get();
	const TArray<TPair<FString, TSharedPtr<FUICommandInfo>>> EditVerbs =
	{
		{ TEXT("Cancel"), Commands.CancelGesture },
		{ TEXT("Build"),  Commands.Build },
	};

	for (const TPair<FString, TSharedPtr<FUICommandInfo>>& Verb : EditVerbs)
	{
		if (!TestTrue(*FString::Printf(TEXT("%s has a command"), *Verb.Key), Verb.Value.IsValid()))
		{
			continue;
		}

		// THE CONSUMED LIST, not the declared one - a command can exist and still never reach
		// the toolkit if BindCommands forgets to MapAction it, which is precisely issue #185's
		// shape before this PR: Build did not exist anywhere, so there was nothing here to
		// find at all.
		const FUIAction* Action = CommandList->GetActionForCommand(Verb.Value);
		TestNotNull(*FString::Printf(TEXT("%s is bound on the toolkit list after BindCommands"),
			*Verb.Key), Action);
	}

	return true;
}

/**
 * MakeReselectContext's HALF of issue #191/#92-#93's item (c): StartToolAction cannot be
 * driven directly here (same gate as everywhere else in this file - it needs a live
 * UEditorInteractiveToolsContext), so this exercises the piece it actually calls once that
 * gate passes - that the modifiers it is GIVEN land on the returned FToolContext, rather than
 * being silently dropped the way the old zero-argument version dropped them by construction.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMakeReselectContextCarriesModifiersTest,
	"Airside.Editor.MakeReselectContextCarriesModifiers",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMakeReselectContextCarriesModifiersTest::RunTest(const FString& Parameters)
{
	URoadBuildEdMode* Mode = NewObject<URoadBuildEdMode>(GetTransientPackage());
	if (!TestNotNull(TEXT("an ed mode"), Mode))
	{
		return false;
	}

	// A real world purely so FindOrCreate has somewhere to resolve a target - see
	// FRoadBuildEdModeSessionTest's own comment on WorldOverrideForTest above.
	FAirsideTestWorld TestWorld(/*bSpawnActor=*/false, EWorldType::Editor);
	if (!TestNotNull(TEXT("a world for the mode to resolve a target in"), TestWorld.World))
	{
		return false;
	}
	Mode->WorldOverrideForTest = TestWorld.World;

	const FToolContext Plain = Mode->MakeReselectContext();
	TestFalse(TEXT("no modifiers with no arguments, same as before this parameter existed"),
		Plain.bRemoveModifier);
	TestFalse(TEXT("no modifiers with no arguments, same as before this parameter existed"),
		Plain.bInsertModifier);

	const FToolContext WithRemove = Mode->MakeReselectContext(/*bRemoveModifier*/ true, /*bInsertModifier*/ false);
	TestTrue(TEXT("a held remove modifier reaches the reselect context"), WithRemove.bRemoveModifier);
	TestFalse(TEXT("insert was not asked for"), WithRemove.bInsertModifier);

	const FToolContext WithInsert = Mode->MakeReselectContext(/*bRemoveModifier*/ false, /*bInsertModifier*/ true);
	TestFalse(TEXT("remove was not asked for"), WithInsert.bRemoveModifier);
	TestTrue(TEXT("a held insert modifier reaches the reselect context"), WithInsert.bInsertModifier);

	return true;
}

/**
 * Setup()'s OWN half of issue #191/#92-#93's item (c): its SelectContext used to be
 * `FToolContext SelectContext; SelectContext.Target = Target;` - both modifiers left at their
 * false defaults regardless of what ITF had already told this instance was held. That matters
 * because a fresh tool object's Setup() can itself land on FBuildSession's RESELECT branch
 * (Index == ActiveTool already) rather than a switch - exactly what a second activation of an
 * already-active palette entry produces, which this test drives directly.
 *
 * FRunwayTool::OnReselect is the one place the distinction is externally visible without a
 * viewport: Ctrl chooses NextApproach over NextWidth (see its own comment), so an Approach that
 * moved while WidthIndex did not is what "the modifier reached Setup's context" looks like from
 * outside FBuildSession. Chosen over the taxiway tool's own OnReselect (width-only) for exactly
 * that reason - a width still cycling would not distinguish "modifier arrived" from "modifier
 * ignored, cycled the default way".
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadBuildEditorToolSetupCarriesModifiersTest,
	"Airside.Editor.SetupSelectContextCarriesHeldModifiers",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadBuildEditorToolSetupCarriesModifiersTest::RunTest(const FString& Parameters)
{
	URoadBuildEdMode* Mode = NewObject<URoadBuildEdMode>(GetTransientPackage());
	if (!TestNotNull(TEXT("an ed mode"), Mode))
	{
		return false;
	}

	int32 RunwayIndex = INDEX_NONE;
	for (int32 Index = 0; Index < ToolRegistry().Num(); ++Index)
	{
		if (ToolRegistry()[Index].Key == EKeys::Six)
		{
			RunwayIndex = Index;
		}
	}
	if (!TestTrue(TEXT("the registry has the runway tool on 6"), RunwayIndex != INDEX_NONE))
	{
		return false;
	}

	URoadBuildEditorToolBuilder* Builder = NewObject<URoadBuildEditorToolBuilder>(Mode);
	Builder->ToolIndex = RunwayIndex;
	FToolBuilderState State;
	State.ToolManager = NewObject<UInteractiveToolManager>(Mode);

	// FIRST ACTIVATION: switches the shared session onto the runway tool (Index != the
	// session's default ActiveTool 0), so Setup()'s own SelectTool call takes the SWITCH
	// branch, not reselect. No target needed - NextApproach/NextSurface do not read one, and
	// this test's probe (Approach) is one of those two, not NextWidth.
	URoadBuildEditorTool* First = Cast<URoadBuildEditorTool>(Builder->BuildTool(State));
	if (!TestNotNull(TEXT("the builder made a tool"), First))
	{
		return false;
	}
	First->Setup();

	FRunwayTool* Runway = static_cast<FRunwayTool*>(Mode->GetSession().GetActiveTool());
	if (!TestNotNull(TEXT("the runway tool is active on the shared session"), Runway))
	{
		return false;
	}
	const ERunwayApproach StartApproach = Runway->Approach;
	const int32 StartWidth = Runway->WidthIndex;

	// SECOND ACTIVATION of the SAME palette entry, the way pressing 6 again does: ITF always
	// builds a fresh tool object (see URoadBuildEdMode::GetSession's own comment on why the
	// SESSION, not the tool, is what has to persist), told Ctrl is down BEFORE its own Setup()
	// runs - the same order OnUpdateModifierState arrives in once Setup has registered the
	// behaviours that report it, just on the tool that is about to become active rather than
	// the one leaving.
	URoadBuildEditorTool* Second = Cast<URoadBuildEditorTool>(Builder->BuildTool(State));
	if (!TestNotNull(TEXT("a second activation builds a second tool"), Second))
	{
		return false;
	}
	Second->OnUpdateModifierState(URoadBuildEditorTool::RemoveModifierId, true);
	Second->Setup();

	TestEqual(TEXT("still the same runway tool instance on the shared session"),
		Mode->GetSession().GetActiveTool(), static_cast<IBuildTool*>(Runway));
	TestEqual(TEXT("Ctrl held into Setup's SelectContext cycles the approach, not the width"),
		Runway->WidthIndex, StartWidth);
	TestTrue(TEXT("the approach actually moved, so the modifier reached OnReselect"),
		Runway->Approach != StartApproach);

	return true;
}

/**
 * ITEM (d) OF ISSUE #191/#92-#93: PIE calls Tool->OnDeactivate on Undo/Redo/Clear
 * (ARoadBuildController::OnUndo/OnRedo/OnClearNetwork) because the tool may be part-way
 * through a chain built on a graph node the undo/redo just changed. This mode had NO
 * FEditorUndoClient at all before now (`grep PostEditUndo|FEditorUndoClient|PostUndo` found
 * zero hits), so FRoadDrawTool kept chaining from a node an editor Ctrl+Z had already removed.
 *
 * URoadBuildEdMode::PostUndo/PostRedo need a live UEditorInteractiveToolsContext to find "the
 * active tool" through GetToolManager()->GetActiveTool() - the same gate every other test in
 * this file works around - so this drives URoadBuildEditorTool::DeactivateOnUndo directly,
 * which is the entire body of what PostUndo/PostRedo call once that gate passes (see
 * DeactivateActiveToolOnUndo's own comment). What this actually measures: that deactivating a
 * REAL FRoadDrawTool mid-chain, through a REAL IRoadEditTarget, drops the pending node - the
 * concrete shape "FRoadDrawTool chains from a node Ctrl+Z removed" takes, not a re-statement
 * that OnDeactivate exists.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadBuildEditorToolDeactivateOnUndoTest,
	"Airside.Editor.UndoDeactivatesTheActiveBuildTool",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadBuildEditorToolDeactivateOnUndoTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World))
	{
		return false;
	}
	if (!TestNotNull(TEXT("a target actor"), TestWorld.Actor))
	{
		return false;
	}

	URoadBuildEdMode* Mode = NewObject<URoadBuildEdMode>(GetTransientPackage());
	if (!TestNotNull(TEXT("an ed mode"), Mode))
	{
		return false;
	}

	int32 TaxiwayIndex = INDEX_NONE;
	for (int32 Index = 0; Index < ToolRegistry().Num(); ++Index)
	{
		if (ToolRegistry()[Index].Key == EKeys::One)
		{
			TaxiwayIndex = Index;
		}
	}
	if (!TestTrue(TEXT("the registry has the taxiway tool on 1"), TaxiwayIndex != INDEX_NONE))
	{
		return false;
	}

	URoadBuildEditorToolBuilder* Builder = NewObject<URoadBuildEditorToolBuilder>(Mode);
	Builder->ToolIndex = TaxiwayIndex;
	FToolBuilderState State;
	State.ToolManager = NewObject<UInteractiveToolManager>(Mode);
	URoadBuildEditorTool* Tool = Cast<URoadBuildEditorTool>(Builder->BuildTool(State));
	if (!TestNotNull(TEXT("the builder made a tool"), Tool))
	{
		return false;
	}

	// Setup() FIRST, THEN SetTargetForTest - not the other order. Setup() unconditionally
	// overwrites Target with ResolveTarget()'s own answer (null here: the bare
	// UInteractiveToolManager this harness builds has no world to find TestWorld.Actor in), so
	// calling SetTargetForTest before Setup() would have its assignment discarded the moment
	// Setup() ran. Setup() also puts this instance's session onto the taxiway tool, which is
	// what DeactivateOnUndo below needs Target for.
	Tool->Setup();
	Tool->SetTargetForTest(TestWorld.Actor);

	FRoadDrawTool* Draw = static_cast<FRoadDrawTool*>(Mode->GetSession().GetActiveTool());
	if (!TestNotNull(TEXT("the taxiway tool is active"), Draw))
	{
		return false;
	}

	// A CLICK THROUGH THE REAL TARGET, so the tool is genuinely chaining from a live node
	// rather than a stand-in for one.
	FToolContext ClickContext;
	ClickContext.Target = TestWorld.Actor;
	ClickContext.Cursor = FVector2D(0.0, 0.0);
	Draw->OnClick(ClickContext);
	if (!TestTrue(TEXT("the click left a pending node to chain from"),
		Draw->GetPendingNode() != INDEX_NONE))
	{
		return false;
	}

	// THE FIX ITSELF: before issue #191, nothing called this on an editor undo at all.
	Tool->DeactivateOnUndo();

	TestTrue(TEXT("undo deactivated the tool, dropping the node it was chaining from - the "
		"exact bug report this closes"), Draw->GetPendingNode() == INDEX_NONE);

	return true;
}

#endif
