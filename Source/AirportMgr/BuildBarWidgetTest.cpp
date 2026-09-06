#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "BuildActions.h"
#include "BuildBarWidget.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"

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
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("a world"), World)) { return false; }
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
	Context.SetCurrentWorld(World);
	ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };

	UBuildBarWidget* Bar = CreateWidget<UBuildBarWidget>(World, UBuildBarWidget::StaticClass());
	if (!TestNotNull(TEXT("the bar is created with no asset"), Bar)) { return false; }

	for (uint8 S = 0; S <= static_cast<uint8>(EActionSection::Game); ++S)
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

#endif
