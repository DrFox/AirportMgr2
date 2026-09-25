#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "BuildActions.h"
#include "BuildBarWidget.h"
#include "Misc/AutomationTest.h"
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
		for (const FBuildAction& A : BuildActions()) { if (A.Section == Section) { ++Expected; } }
		TestEqual(*FString::Printf(TEXT("section %s has one button per action"), ActionSectionName(Section)),
			Bar->ButtonCountForTest(Section), Expected);
	}
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

	// CreateWidget(APlayerController*, ...) refuses a controller with no attached local
	// player (this one has none - a bare SpawnActor, same as every other controller test in
	// this file). Created against the World instead: AController::PostInitializeComponents
	// added C to the world's PlayerControllerList regardless of possession, which is what
	// UAirportMgrPanelWidget::Controller()'s GetFirstPlayerController() fallback reads.
	UBuildBarWidget* Bar = CreateWidget<UBuildBarWidget>(TestWorld.World, UBuildBarWidget::StaticClass());
	if (!TestNotNull(TEXT("the bar is created with no asset"), Bar)) { return false; }

	// Construction (Initialize -> BuildOnce) has already resolved the style once - that call
	// is not what this test is about, so the baseline is taken AFTER it.
	const int32 Before = UAirportMgrUISettings::ResolveCallCountForTest();
	for (int32 Tick = 0; Tick < 5; ++Tick)
	{
		Bar->NativeTickForTest(1.0f / 60.0f);
	}
	const int32 After = UAirportMgrUISettings::ResolveCallCountForTest();

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
 * above never noticed, because its own assertion (zero ResolveStyle calls) holds either way.
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

#endif
