#include "CoreMinimal.h"
#include "AirlinesPanelWidget.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "BuildActions.h"
#include "BuildBarWidget.h"
#include "BuildHudLayer.h"
#include "Components/TextBlock.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Model/AirlineHistory.h"
#include "Model/AirlineRoster.h"
#include "Model/OpsEventBus.h"
#include "OpsRuntimeResolver.h"
#include "Present/OpsRuntime.h"
#include "RoadBuildController.h"
#include "Testing/AirsideTestWorld.h"
#include "UI/UiSparkline.h"
#include "UI/UiWindowHost.h"
#include "UIStyle.h"

#if WITH_DEV_AUTOMATION_TESTS

// THE AIRLINES WINDOW (spec 2026-10-02-airlines-panel section 2), driven through a real UOpsRuntime stood in for the world
// (OpsRuntimeResolver::SetOverrideForTest) - the panel finds its runtime the way it does in play. Prefixed names: unity build.

namespace
{
	/** A runtime whose roster has these airlines seeded, its history WIRED as UOpsRuntime::Attach wires it (so a day can close),
	 *  stood in for World until the scope ends. Unattached: no catalog, so each airline's name is its id. */
	struct FAirlinesPanelRig
	{
		const UWorld* World = nullptr;
		UOpsRuntime* Runtime = nullptr;

		FAirlinesPanelRig(const UWorld* InWorld, std::initializer_list<const TCHAR*> Ids) : World(InWorld)
		{
			Runtime = NewObject<UOpsRuntime>();
			Runtime->GetAirlines()->History = Runtime->GetAirlineHistory();
			for (const TCHAR* Id : Ids)
			{
				Runtime->GetAirlines()->Ensure(FName(Id));
			}
			OpsRuntimeResolver::SetOverrideForTest(World, Runtime);
		}
		~FAirlinesPanelRig() { OpsRuntimeResolver::SetOverrideForTest(World, nullptr); }

		void OnTime(const TCHAR* Id) { Runtime->GetAirlines()->OnFlightOffBlocks(FFlightOffBlocksEvent{ 1, FName(Id), 0.0 }); }
		void EndDay() { Runtime->GetAirlines()->OnDayEnded(FDayEndedEvent{ 1 }); }
	};

	/** The first visible text block reading exactly Words, or null. */
	const UTextBlock* AirlinesPanelText(const UAirlinesPanelWidget& Panel, const FString& Words)
	{
		TArray<UWidget*> All;
		Panel.WidgetTree->GetAllWidgets(All);
		for (UWidget* Each : All)
		{
			const UTextBlock* Text = Cast<UTextBlock>(Each);
			if (Text != nullptr && Text->GetText().ToString() == Words) { return Text; }
		}
		return nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirlinesPanelRegisteredTest, "AirportMgr.Airlines.Panel.Registered",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FAirlinesPanelRegisteredTest::RunTest(const FString&)
{
	// THE FOUR LISTS THAT MUST AGREE (enum, name table, PanelFor, WireWindows) plus the bar's row, measured through their consumers:
	// the action's Execute toggles the window through the HUD, and WireWindows hosts it.
	const FBuildAction* Action = FindAction(FName(TEXT("game.airlines")));
	if (!TestNotNull(TEXT("the bar has an Airlines button"), Action)) { return false; }
	TestEqual(TEXT("in the Game section"), Action->Section, EActionSection::Game);
	TestFalse(TEXT("with no key - Alerts' reason: free letters are scarce"), Action->Key.IsValid());
	const TConstArrayView<FBuildAction> All = BuildActions();
	const int32 AlertsAt = All.IndexOfByPredicate([](const FBuildAction& A) { return A.Id == FName(TEXT("game.alerts")); });
	const int32 AirlinesAt = All.IndexOfByPredicate([](const FBuildAction& A) { return A.Id == FName(TEXT("game.airlines")); });
	TestEqual(TEXT("right after Alerts on the bar"), AirlinesAt, AlertsAt + 1);

	FAirsideTestWorld TestWorld(/*bSpawnActor=*/false);
	ARoadBuildController* C = TestWorld.World->SpawnActor<ARoadBuildController>();
	if (!TestNotNull(TEXT("controller spawned"), C) || !TestNotNull(TEXT("with a HUD layer"), C->GetHud())) { return false; }
	UBuildHudLayer* Hud = C->GetHud();
	Hud->BuildBar = CreateWidget<UBuildBarWidget>(TestWorld.World, UBuildBarWidget::StaticClass());
	Hud->WindowHost = CreateWidget<UUiWindowHost>(TestWorld.World, UUiWindowHost::StaticClass());
	Hud->AirlinesPanel = CreateWidget<UAirlinesPanelWidget>(TestWorld.World, UAirlinesPanelWidget::StaticClass());
	Hud->WireWindows();
	TestNotNull(TEXT("WireWindows hosts it - a panel created and not wired is never shown"), Hud->WindowHost->WindowForTest(TEXT("airlines")));

	FBuildActionContext Ctx(*C);
	TestFalse(TEXT("closed to start"), Hud->IsWindowShowing(EHudWindow::Airlines));
	TestFalse(TEXT("and the button unlit"), Action->IsActive(Ctx));
	Action->Execute(Ctx);
	TestTrue(TEXT("the button opens the window"), Hud->IsWindowShowing(EHudWindow::Airlines));
	TestTrue(TEXT("and lights while it is open"), Action->IsActive(Ctx));
	Action->Execute(Ctx);
	TestFalse(TEXT("pressed again it closes"), Hud->IsWindowShowing(EHudWindow::Airlines));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirlinesPanelSelectTest, "AirportMgr.Airlines.Panel.SelectingRowShowsDetail",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FAirlinesPanelSelectTest::RunTest(const FString&)
{
	FAirsideTestWorld TestWorld(/*bSpawnActor=*/false);
	FAirlinesPanelRig Rig(TestWorld.World, { TEXT("BravoAir"), TEXT("AlphaAir") });
	UAirlinesPanelWidget* Panel = CreateWidget<UAirlinesPanelWidget>(TestWorld.World, UAirlinesPanelWidget::StaticClass());
	if (!TestNotNull(TEXT("an airlines window"), Panel)) { return false; }
	Panel->Refresh();

	if (!TestEqual(TEXT("one list row per airline"), Panel->ListRowCountForTest(), 2)) { return false; }
	TestEqual(TEXT("the first row (by name) is selected to start, so the detail is never blank"), Panel->GetSelected(), FName(TEXT("AlphaAir")));
	TestTrue(TEXT("and drawn selected"), Panel->IsRowSelectedForTest(0));
	TestEqual(TEXT("its name heads the detail"), Panel->HeaderNameForTest().ToString(), FString(TEXT("AlphaAir")));
	TestEqual(TEXT("with its percentage"), Panel->HeaderPercentForTest().ToString(), FString(TEXT("50%")));

	const int32 ListBuiltBefore = Panel->ListRebuildsForTest();
	TestTrue(TEXT("the second row's button is clicked"), Panel->ClickListRowForTest(1));
	TestEqual(TEXT("selecting the second row selects its airline"), Panel->GetSelected(), FName(TEXT("BravoAir")));
	TestEqual(TEXT("and puts its name in the detail header"), Panel->HeaderNameForTest().ToString(), FString(TEXT("BravoAir")));
	TestTrue(TEXT("the second row is lit"), Panel->IsRowSelectedForTest(1));
	TestFalse(TEXT("and the first is not"), Panel->IsRowSelectedForTest(0));
	TestEqual(TEXT("a click relights the rows and rebuilds no button - rebuilding under the cursor eats the next click"),
		Panel->ListRebuildsForTest(), ListBuiltBefore);

	// THE MEMO KEY: nothing moved, nothing rebuilt; a standing moves, both halves repaint.
	const int32 DetailBuilt = Panel->DetailRebuildsForTest();
	Panel->Refresh();
	Panel->Refresh();
	TestEqual(TEXT("a quiet refresh rebuilds no list"), Panel->ListRebuildsForTest(), ListBuiltBefore);
	TestEqual(TEXT("and no detail"), Panel->DetailRebuildsForTest(), DetailBuilt);
	Rig.OnTime(TEXT("BravoAir"));
	Panel->Refresh();
	TestEqual(TEXT("a moved standing rebuilds the list once"), Panel->ListRebuildsForTest(), ListBuiltBefore + 1);
	TestEqual(TEXT("and the detail once"), Panel->DetailRebuildsForTest(), DetailBuilt + 1);
	TestEqual(TEXT("which shows the new figure"), Panel->HeaderPercentForTest().ToString(), FString(TEXT("53%")));
	TestEqual(TEXT("the selection survives the list's rebuild"), Panel->GetSelected(), FName(TEXT("BravoAir")));
	TestTrue(TEXT("and is still lit"), Panel->IsRowSelectedForTest(1));

	// TODAY'S CAUSE, coloured by its sign from the style - not by parsing the text.
	const UTextBlock* Delta = AirlinesPanelText(*Panel, TEXT("+3%"));
	if (TestNotNull(TEXT("the on-time tally row shows its delta"), Delta) && TestNotNull(TEXT("a style"), Panel->PanelStyleForTest()))
	{
		TestEqual(TEXT("in Positive"), Delta->GetColorAndOpacity().GetSpecifiedColor(), Panel->PanelStyleForTest()->Positive);
	}
	TestTrue(TEXT("beside its cause"), Panel->ShowsTextForTest(TEXT("on time")));

	// THE SPARKLINE IS IN THE TREE - its paint path had never run before this window (Task 4).
	UUiSparkline* Trend = Panel->TrendForTest();
	if (TestNotNull(TEXT("a trend line"), Trend))
	{
		TestNotNull(TEXT("added to the window, not merely constructed"), Trend->GetParent());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirlinesPanelEmptyTest, "AirportMgr.Airlines.Panel.EmptyStates",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FAirlinesPanelEmptyTest::RunTest(const FString&)
{
	// REVIEW FOCUS 1: a fresh game's first minute. Every section says what it lacks; nothing reads 0% or draws an empty line.
	FAirsideTestWorld TestWorld(/*bSpawnActor=*/false);
	FAirlinesPanelRig Rig(TestWorld.World, { TEXT("SoloAir") });
	UAirlinesPanelWidget* Panel = CreateWidget<UAirlinesPanelWidget>(TestWorld.World, UAirlinesPanelWidget::StaticClass());
	if (!TestNotNull(TEXT("an airlines window"), Panel)) { return false; }
	Panel->Refresh();

	TestTrue(TEXT("no closed day: \"No history yet\""), Panel->ShowsTextForTest(TEXT("No history yet")));
	UUiSparkline* Trend = Panel->TrendForTest();
	if (!TestNotNull(TEXT("a trend line"), Trend)) { return false; }
	// GetVisibility, not IsVisible: the latter asks Slate, and a headless widget has no Slate widget - it answers false for anything.
	TestEqual(TEXT("and no sparkline of nothing"), Trend->GetVisibility(), ESlateVisibility::Collapsed);
	TestTrue(TEXT("no verdicts: \"Not judged yet\", never a fleet of crosses"), Panel->ShowsTextForTest(TEXT("Not judged yet")));
	TestTrue(TEXT("no offers: \"No offers\""), Panel->ShowsTextForTest(TEXT("No offers")));
	TestTrue(TEXT("no flights: \"None at the airport\""), Panel->ShowsTextForTest(TEXT("None at the airport")));
	TestEqual(TEXT("CONTROL: a seeded airline shows its start"), Panel->HeaderPercentForTest().ToString(), FString(TEXT("50%")));

	// AN AIRLINE WITH NO STANDING prints a dash, never "0%".
	Panel->Select(TEXT("GhostAir"));
	TestEqual(TEXT("its name heads the detail"), Panel->HeaderNameForTest().ToString(), FString(TEXT("GhostAir")));
	TestEqual(TEXT("and its percentage is a dash"), Panel->HeaderPercentForTest().ToString(), FString(TEXT("—")));

	// CONTROL: a day closes, and the empty state gives way to the line - so the assertions above measured the state, not a constant.
	Panel->Select(TEXT("SoloAir"));
	Rig.EndDay();
	Panel->Refresh();
	TestFalse(TEXT("a closed day is history: the empty line goes"), Panel->ShowsTextForTest(TEXT("No history yet")));
	TestNotEqual(TEXT("and the sparkline shows"), Trend->GetVisibility(), ESlateVisibility::Collapsed);
	return true;
}

#endif
