#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "BuildActions.h"
#include "BuildBarWidget.h"
#include "Misc/AutomationTest.h"
#include "Testing/AirsideTestWorld.h"

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

#endif
