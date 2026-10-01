#include "CoreMinimal.h"
#include "UI/UiMenuButton.h"
#include "Testing/AirsideTestGraph.h"
#include "Present/OpsRuntime.h"
#include "Model/Airport.h"
#include "Blueprint/UserWidget.h"
#include "BuildActions.h"
#include "BuildBarWidget.h"
#include "Model/Pricing.h"
#include "OpsRuntimeResolver.h"
#include "Misc/AutomationTest.h"
#include "Model/RunwayFacts.h"
#include "Present/RoadNetworkActor.h"
#include "Profiles/RoadProfile.h"
#include "Tool/BuildSession.h"
#include "RoadBuildController.h"
#include "Testing/AirsideTestWorld.h"
#include "UIStyle.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildBarWidgetTest,
	"AirportMgr.Actions.BarBuildsFromRegistry",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBuildBarWidgetTest::RunTest(const FString& Parameters)
{
	// THE CONSUMER CHECK. The registry test proves the list is well formed; this proves the
	// bar READS it - one button per action, in the right section - with no asset at all,
	// which is the degraded path the design promises still works.
	FAirsideTestWorld TestWorld(/*bSpawnActor=*/false);
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }

	UBuildBarWidget* Bar = CreateWidget<UBuildBarWidget>(TestWorld.World, UBuildBarWidget::StaticClass());
	if (!TestNotNull(TEXT("the bar is created with no asset"), Bar)) { return false; }

	for (uint8 S = 0; S < static_cast<uint8>(EActionSection::Count); ++S)
	{
		const EActionSection Section = static_cast<EActionSection>(S);
		int32 Expected = 0;
		for (const FBuildAction& A : BuildActions()) { if (A.Section == Section && !A.bInspectorOnly) { ++Expected; } }
		TestEqual(*FString::Printf(TEXT("section %s has one button per action"), ActionSectionName(Section)),
			Bar->ButtonCountForTest(Section), Expected);
	}
	// THE THREE PURCHASE VERBS ARE ROWS, NOT BUTTONS: registered, and still one bar button fewer each.
	int32 InspectorOnly = 0;
	for (const FBuildAction& A : BuildActions()) { InspectorOnly += A.bInspectorOnly ? 1 : 0; }
	TestEqual(TEXT("three inspector-only rows exist, and the per-section counts above drew none of them"), InspectorOnly, 3);
	TestTrue(TEXT("the bar has a root widget to show"), Bar->HasRootWidgetForTest());
	return true;
}

/**
 * A NARROW BAR WRAPS; IT DOES NOT RUN OFF THE END.
 *
 * Stage 3 of the snap guides added eight buttons - a third more than the bar had - and the
 * whole Snap section went off the right-hand edge, where the player could not find it. Every
 * bar test stayed green, because they all ask whether the buttons EXIST and none asks whether
 * they FIT. This is the one that asks.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBarWrapsRatherThanClippingTest,
	"AirportMgr.Actions.BarWrapsRatherThanClipping",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBarWrapsRatherThanClippingTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld(/*bSpawnActor=*/false);
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }

	UBuildBarWidget* Bar = CreateWidget<UBuildBarWidget>(TestWorld.World, UBuildBarWidget::StaticClass());
	if (!TestNotNull(TEXT("the bar is created with no asset"), Bar)) { return false; }

	// ROOM FOR EVERYTHING: one line, and the width the sections actually want.
	const FVector2D Roomy = Bar->SectionRowSizeForTest(6000.0f);

	// MEASUREMENT FIRST. If the row measures to nothing this test proves nothing, and the
	// assertions below would pass on an empty widget - the failure mode it exists to catch.
	if (!TestTrue(*FString::Printf(TEXT("the row measures to a real size, got %s"), *Roomy.ToString()),
		Roomy.X > 100.0f && Roomy.Y > 1.0f))
	{
		return false;
	}

	// NARROWER THAN ITS CONTENT: two thirds of what it asked for, so at least one section
	// cannot stay on the first line.
	const float Narrow = static_cast<float>(Roomy.X) * 0.66f;
	const FVector2D Cramped = Bar->SectionRowSizeForTest(Narrow);

	TestTrue(*FString::Printf(
		TEXT("a bar %.0f wide lays its sections out within %.0f, got %.0f"),
		Narrow, Narrow, Cramped.X),
		Cramped.X <= Narrow + 1.0);

	TestTrue(*FString::Printf(
		TEXT("and gets taller because a line wrapped: %.0f cramped against %.0f roomy"),
		Cramped.Y, Roomy.Y),
		Cramped.Y > Roomy.Y);

	return true;
}

/**
 * AND THE BAR GROWS TO SHOW THE LINE IT WRAPPED TO.
 *
 * Wrapping the row on its own only turns clipping at the right-hand edge into clipping at the
 * bottom: the bar is a fixed-height strip anchored to the bottom of the screen, and
 * BarHeightFor pays for exactly ONE section line. A second line has to be somewhere.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBarGrowsForTheLineItWrappedToTest,
	"AirportMgr.Actions.BarGrowsForTheLineItWrappedTo",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBarGrowsForTheLineItWrappedToTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld(/*bSpawnActor=*/false);
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }

	UBuildBarWidget* Bar = CreateWidget<UBuildBarWidget>(TestWorld.World, UBuildBarWidget::StaticClass());
	if (!TestNotNull(TEXT("the bar is created with no asset"), Bar)) { return false; }

	const float Wanted = static_cast<float>(Bar->SectionRowSizeForTest(6000.0f).X);
	if (!TestTrue(TEXT("the row measures to a real width"), Wanted > 100.0f)) { return false; }

	const float Roomy = Bar->BarReservedHeightForTest(Wanted * 1.1f);
	const float Cramped = Bar->BarReservedHeightForTest(Wanted * 0.66f);

	if (!TestTrue(*FString::Printf(TEXT("the bar reserves a real height, got %.0f"), Roomy),
		Roomy > 1.0f))
	{
		return false;
	}

	TestTrue(*FString::Printf(
		TEXT("a bar whose sections wrap reserves room for the extra line: %.0f cramped "
			 "against %.0f roomy"), Cramped, Roomy),
		Cramped > Roomy);

	return true;
}

/**
 * THE BAR USED TO RESOLVE THE STYLE TWICE A TICK (RefreshState AND RefreshBalance, each its
 * own UAirportMgrUISettings::ResolveStyle() - a TSoftObjectPtr::LoadSynchronous). Issue #187
 * moves that resolve into UAirportMgrPanelWidget::Initialize, once, before BuildOnce ever
 * runs - measured here as a DELTA across several ticks, the same idiom
 * FPlayerTickBuildsOneContextTest uses for issue #167, rather than trusting a trace of the
 * two call sites by eye.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBarCachesStyleAcrossTicksTest,
	"AirportMgr.Actions.BarCachesStyleAcrossTicks",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBarCachesStyleAcrossTicksTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }

	ARoadBuildController* C = TestWorld.World->SpawnActor<ARoadBuildController>();
	if (!TestNotNull(TEXT("controller spawned"), C)) { return false; }
	C->SetTargetForTest(TestWorld.Actor);
	// A PlayerInput is required before any of RefreshState's IsEnabled/IsActive queries would
	// be safe to run against - same precedent as FPlayerTickBuildsOneContextTest.
	C->InitInputSystem();

	// THE CONTROLLER IS REGISTERED BY HAND (2026-09-30 review: this test skipped it, so every tick returned at
	// RefreshState's null controller and "zero ResolveStyle calls" held for the wrong reason). FAirsideTestWorld's
	// bare UWorld::CreateWorld never calls InitializeActorsForPlay, so AController::PostInitializeComponents
	// never ran and C never joined the world's PlayerControllerList - which is what
	// UAirportMgrPanelWidget::Controller()'s GetFirstPlayerController() fallback reads (see
	// UBuildBarWidget::RefreshStateFor). CreateWidget(APlayerController*, ...) refuses a controller with no
	// local player, so the bar is created against the World.
	TestWorld.World->AddController(C);
	UBuildBarWidget* Bar = CreateWidget<UBuildBarWidget>(TestWorld.World, UBuildBarWidget::StaticClass());
	if (!TestNotNull(TEXT("the bar is created with no asset"), Bar)) { return false; }

	// Construction (Initialize -> BuildOnce) has already resolved the style once - that call
	// is not what this test is about, so the baseline is taken AFTER it.
	const int32 Before = UAirportMgrUISettings::ResolveCallCountForTest();
	const int32 ContextsBefore = FBuildActionContext::ConstructCountForTest();
	constexpr int32 Ticks = 5;
	for (int32 Tick = 0; Tick < Ticks; ++Tick)
	{
		Bar->NativeTickForTest(1.0f / 60.0f);
	}
	const int32 After = UAirportMgrUISettings::ResolveCallCountForTest();

	// THE PREMISE, so the zero below is about the body and not about an early return: every tick that reaches
	// RefreshStateFor builds its one FBuildActionContext (FBarTickBuildsOneActionContextTest's measurement).
	if (!TestEqual(TEXT("premise: each of the five ticks reached RefreshState's body - one action context apiece"),
		FBuildActionContext::ConstructCountForTest() - ContextsBefore, Ticks)) { return false; }

	TestEqual(TEXT("five ticks with a controller present resolve the style zero times - it was "
		"cached at construction, not re-asked from RefreshState or RefreshBalance"),
		After - Before, 0);

	return true;
}

/**
 * ONE CONTEXT PER BAR TICK, not one per IsEnabled/IsActive call - issue #309, the same shape
 * FPlayerTickBuildsOneContextTest pins for PlayerTick/DrawHUD. RefreshState used to let
 * `Action.IsEnabled(*C)` / `Action.IsActive(*C)` convert `*C` through FBuildActionContext's own
 * constructor - a GetSubsystem lookup (BuildActions.cpp:11-19) - IMPLICITLY, once per call, for
 * every one of BuildActions()'s few dozen entries: ~70 constructions a tick for a context
 * BuildActions.h's own comment says is "resolved ONCE". Making the constructor explicit is what
 * turned that silent conversion into a compile error at RefreshState, which is what this test
 * would have caught as "2 or more" before the fix built one context and threaded it through.
 *
 * RefreshStateForTest(*C), not NativeTickForTest: NativeTick reaches this work through
 * Controller(), whose GetFirstPlayerController() fallback needs the controller registered in
 * UWorld::PlayerControllerList - and FAirsideTestWorld's bare UWorld::CreateWorld never calls
 * UWorld::InitializeActorsForPlay, so AActor::PostActorConstruction never calls
 * PostInitializeComponents on anything spawned into it (verified with a diagnostic UE_LOG: C
 * was a valid, spawned controller and GetFirstPlayerController() still came back null). A
 * NativeTickForTest-driven version of this test would measure RefreshState's early return, not
 * its body - see RefreshStateFor's own comment for the mechanism. FBarCachesStyleAcrossTicksTest
 * above never noticed, because its own assertion (zero ResolveStyle calls) held either way; it now
 * registers the controller with the world (AddController) and asserts the ticks reached this body.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBarTickBuildsOneActionContextTest,
	"AirportMgr.Actions.BarTickBuildsOneActionContext",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBarTickBuildsOneActionContextTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }

	ARoadBuildController* C = TestWorld.World->SpawnActor<ARoadBuildController>();
	if (!TestNotNull(TEXT("controller spawned"), C)) { return false; }
	C->SetTargetForTest(TestWorld.Actor);
	// A PlayerInput is required before any of RefreshState's IsEnabled/IsActive queries would
	// be safe to run against - same precedent as FPlayerTickBuildsOneContextTest.
	C->InitInputSystem();

	UBuildBarWidget* Bar = CreateWidget<UBuildBarWidget>(TestWorld.World, UBuildBarWidget::StaticClass());
	if (!TestNotNull(TEXT("the bar is created with no asset"), Bar)) { return false; }

	const int32 Before = FBuildActionContext::ConstructCountForTest();
	Bar->RefreshStateForTest(*C);
	const int32 After = FBuildActionContext::ConstructCountForTest();

	// EXACTLY ONE: RefreshState builds one FBuildActionContext and passes it to every entry's
	// IsEnabled and IsActive - this goes to 2 or more the moment either call reverts to
	// converting a bare controller reference of its own.
	TestEqual(TEXT("one bar tick builds exactly one FBuildActionContext for every entry's "
		"IsEnabled and IsActive together"),
		After - Before, 1);

	return true;
}

namespace
{
	/** Registry index of the tool with this Id, or INDEX_NONE. VarRow prefix: unity build. */
	int32 VarRowToolIndex(const TCHAR* Id)
	{
		for (int32 Index = 0; Index < ToolRegistry().Num(); ++Index)
		{
			if (ToolRegistry()[Index].Id == FName(Id)) { return Index; }
		}
		return INDEX_NONE;
	}
}

/**
 * THE VARIANT ROW FOLLOWS THE LIT TOOL - hidden over a tool with no choices, one button per
 * option over one with them, rebuilt when the tool changes, and a click lands on the tool.
 *
 * AT THE COMPOSITION: a real controller and the bar's own refresh, so an unwired forwarder
 * (controller -> session -> tool) or a row that never rebuilds reads red here even with every
 * Airside.Tool.Variants test green. RefreshStateForTest for RefreshStateFor's own reason (#309).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FVariantRowFollowsToolTest,
	"AirportMgr.Actions.VariantRowFollowsTool",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVariantRowFollowsToolTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadBuildController* C = TestWorld.World->SpawnActor<ARoadBuildController>();
	if (!TestNotNull(TEXT("controller spawned"), C)) { return false; }
	C->SetTargetForTest(TestWorld.Actor);
	C->InitInputSystem();
	UBuildBarWidget* Bar = CreateWidget<UBuildBarWidget>(TestWorld.World, UBuildBarWidget::StaticClass());
	if (!TestNotNull(TEXT("the bar is created with no asset"), Bar)) { return false; }

	const int32 Taxiways = TestWorld.Actor->GetWidthCount(ERoadKind::Taxiway);
	const int32 Roads = TestWorld.Actor->GetWidthCount(ERoadKind::ServiceRoad);
	if (!TestTrue(TEXT("the content set has taxiway and road widths, of different counts - or the "
		"rebuild below proves nothing"), Taxiways > 1 && Roads > 1 && Taxiways != Roads))
	{
		return false;
	}

	C->SelectTool(VarRowToolIndex(TEXT("Select")));
	Bar->RefreshStateForTest(*C);
	TestFalse(TEXT("over the select tool the row is hidden"), Bar->IsVariantSectionVisibleForTest());
	TestEqual(TEXT("and holds no buttons"), Bar->VariantButtonCountForTest(), 0);

	C->SelectTool(VarRowToolIndex(TEXT("Taxiway")));
	Bar->RefreshStateForTest(*C);
	TestTrue(TEXT("over the taxiway tool the row shows"), Bar->IsVariantSectionVisibleForTest());
	// THE PROFILE'S LIST, not an enum's count: every road and taxiway profile offers tarmac and
	// grass (URoadProfile::AllowedPavements, authored 2026-09-27). Read off the profile the
	// click would lay, so a content edit to the list moves this figure with it.
	const URoadProfile* TaxiwayDefault = TestWorld.Actor->ResolveProfileFor(ERoadKind::Taxiway, INDEX_NONE);
	if (!TestNotNull(TEXT("the taxiway tool resolves a profile"), TaxiwayDefault)) { return false; }
	const int32 Surfaces = TaxiwayDefault->AllowedPavements.Num();
	TestEqual(TEXT("which offers the two road pavements"), Surfaces, 2);
	// PLUS THE MODE ROW'S TWO (Build / Upgrade, strip stage 6), leading every road tool's rows.
	constexpr int32 Modes = 2;
	TestEqual(TEXT("one button per mode, per taxiway width, and per surface"), Bar->VariantButtonCountForTest(),
		Modes + Taxiways + Surfaces);

	C->SelectTool(VarRowToolIndex(TEXT("Road")));
	Bar->RefreshStateForTest(*C);
	TestEqual(TEXT("switching to the road tool REBUILDS the row to the road's widths"),
		Bar->VariantButtonCountForTest(), Modes + Roads + Surfaces);

	C->SelectTool(VarRowToolIndex(TEXT("Runway")));
	Bar->RefreshStateForTest(*C);
	TestEqual(TEXT("the runway gets a row each for width, surface and approach"),
		Bar->VariantButtonCountForTest(),
		TestWorld.Actor->GetRunwayProfileCount() + static_cast<int32>(EPavement::Count)
			+ static_cast<int32>(ERunwayApproach::Count));

	// A CLICK LANDS ON THE TOOL - through the bar, the controller and the session.
	C->SelectTool(VarRowToolIndex(TEXT("Taxiway")));
	Bar->RefreshStateForTest(*C);
	// NOT WHAT IS ALREADY LIT: the level's default may light a preset, and picking that one
	// would pass with the click going nowhere.
	TArray<FToolVariantAxis> Axes;
	C->GetActiveVariantAxes(Axes);
	if (!TestEqual(TEXT("the taxiway has three rows, mode, width and surface"), Axes.Num(), 3)) { return false; }
	const int32 Pick = Axes[1].Current == 1 ? 2 : 1;
	Bar->RunVariantFor(*C, 1, Pick);
	C->GetActiveVariantAxes(Axes);
	TestEqual(TEXT("the clicked width is now what is lit"), Axes[1].Current, Pick);
	return true;
}

/**
 * A MENU VERB IS BUILT AS A MENU: an action with MenuItems (game.airport) gets a UUiMenuButton - the popup its confirm
 * lives in - not a plain button whose click would run Execute. Counted against the registry, so a second menu verb
 * needs no edit here.
 *
 * AND THE BAR PAINTS THROUGH UUiButton (2026-10: this absorbed AirportMgr.Actions.BarToolsAreUiButtons, which asserted its
 * last line verbatim on the identical fixture): the armed tool is Accent with InkOnAccent and every other tool Control with
 * Ink - the rule has one home, UUiButton::LookFor - so a bar that bypassed it (a raw UButton slipped back in) fails the last
 * assertion here rather than drifting quietly.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBarBuildsMenusTest, "AirportMgr.Actions.BarBuildsMenuActionsAsMenus",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FBarBuildsMenusTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld(/*bSpawnActor=*/false);
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	UBuildBarWidget* Bar = CreateWidget<UBuildBarWidget>(TestWorld.World, UBuildBarWidget::StaticClass());
	if (!TestNotNull(TEXT("the bar"), Bar)) { return false; }
	int32 Expected = 0;
	for (const FBuildAction& A : BuildActions()) { Expected += A.MenuItems ? 1 : 0; }
	TestTrue(TEXT("the registry has a menu verb (game.airport)"), Expected > 0);
	TestEqual(TEXT("each menu verb is a menu button on the bar"), Bar->MenuButtonCountForTest(), Expected);
	TestTrue(TEXT("and still a UUiButton with a label, like every other"), Bar->AllButtonsAreUiButtonsForTest());
	return true;
}

/**
 * THE BAR'S PRODUCTION DOOR, end to end (review I3): the menu verb's popup, chosen twice (arm, confirm), reaches the
 * runtime through OnChosen -> UBuildBarEntry::HandleChosen -> ChooseAction -> TryChoose -> Choose. The test world has
 * no game instance, so the controller and runtime the bar would look up are handed in (UseForTest).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBarMenuVerbReachesRuntimeTest, "AirportMgr.Actions.BarMenuVerbReachesTheRuntime",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FBarMenuVerbReachesRuntimeTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor"), TestWorld.Actor)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	// A NODE FIRST: the actor's network is made by its first edit, and the close command needs one to derive against.
	Actor->PlaceNode(FVector2D(0.0, 30000.0));
	Actor->MinimumRunwayLength = 100.0;
	Actor->PlaceRunway(FVector2D(0.0, -50000.0), FVector2D(6000.0, -50000.0), TestProfiles::Runway());
	UOpsRuntime* Runtime = NewObject<UOpsRuntime>();
	Runtime->Attach(Actor);
	Runtime->Tick(0.0);
	if (!TestEqual(TEXT("an open airport"), Runtime->GetAirport()->Status(), EAirportStatus::Open)) { return false; }
	ARoadBuildController* C = TestWorld.World->SpawnActor<ARoadBuildController>();
	if (!TestNotNull(TEXT("controller spawned"), C)) { return false; }

	UBuildBarWidget* Bar = CreateWidget<UBuildBarWidget>(TestWorld.World, UBuildBarWidget::StaticClass());
	if (!TestNotNull(TEXT("the bar"), Bar)) { return false; }
	OpsRuntimeResolver::SetOverrideForTest(TestWorld.World, Runtime);
	Bar->UseForTest(C);
	UUiMenuButton* Menu = Bar->MenuForTest(FName(TEXT("game.airport")));
	if (!TestNotNull(TEXT("game.airport is a menu on the bar"), Menu)) { return false; }
	Menu->BuildMenu();
	Menu->Choose(0);
	TestEqual(TEXT("armed: still open"), Runtime->GetAirport()->Status(), EAirportStatus::Open);
	Menu->Choose(0);
	TestEqual(TEXT("confirmed through the bar: closed"), Runtime->GetAirport()->Status(), EAirportStatus::ClosedByPlayer);
	return true;
}

/**
 * #426 PIN 2: THE BAR'S BALANCE AFTER A LOAD. The readout is gated on ULedger::Revision (RefreshBalance's THE GATE), and a
 * load restored the balance with no Post - so the bar kept showing the replaced session's money until the next fee.
 * Post, save, post, load: the text the bar shows must be the saved balance, on the first tick after the load.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBarBalanceFollowsALoadTest, "AirportMgr.Actions.BarBalanceFollowsALoad",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FBarBalanceFollowsALoadTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor"), TestWorld.Actor)) { return false; }
	TestWorld.Actor->PlaceNode(FVector2D(0.0, 0.0));
	UOpsRuntime* Runtime = NewObject<UOpsRuntime>();
	Runtime->Attach(TestWorld.Actor);
	UBuildBarWidget* Bar = CreateWidget<UBuildBarWidget>(TestWorld.World, UBuildBarWidget::StaticClass());
	if (!TestNotNull(TEXT("the bar"), Bar)) { return false; }
	OpsRuntimeResolver::SetOverrideForTest(TestWorld.World, Runtime);

	ULedger* Ledger = Runtime->GetLedger();
	Ledger->Post(0.0, ELedgerCategory::LandingFee, 1200.0, FText::FromString(TEXT("saved fee")));
	const FString Saved = Runtime->GetPricing()->Format(Ledger->Balance()).ToString();
	const FString Slot = TEXT("AirportMgrTest_BarBalanceLoad");
	if (!TestTrue(TEXT("save writes"), Runtime->SaveToSlot(Slot))) { return false; }

	Ledger->Post(0.0, ELedgerCategory::LandingFee, 900.0, FText::FromString(TEXT("unsaved fee")));
	Bar->RefreshBalanceForTest();
	const FString Unsaved = Runtime->GetPricing()->Format(Ledger->Balance()).ToString();
	if (!TestTrue(TEXT("the bar shows the unsaved balance before the load - or the check below measures nothing"),
		Bar->BalanceTextForTest().ToString().StartsWith(Unsaved) && Unsaved != Saved)) { return false; }

	if (!TestTrue(TEXT("load reads"), Runtime->LoadFromSlot(Slot))) { return false; }
	Bar->RefreshBalanceForTest();
	TestTrue(TEXT("the first bar tick after the load shows the SAVED balance - the revision gate opened"),
		Bar->BalanceTextForTest().ToString().StartsWith(Saved));
	return true;
}

#endif
