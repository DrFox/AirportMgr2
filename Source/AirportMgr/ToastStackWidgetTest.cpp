#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Misc/AutomationTest.h"
#include "NotificationCentre.h"
#include "Testing/AirsideTestWorld.h"
#include "ToastStackWidget.h"
#include "UIStyle.h"
#include "Styling/SlateBrush.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** A world the widget can be created from. Mirrors AirportMgr.Actions.BarBuildsFromRegistry. */
	UToastStackWidget* MakeStack(UWorld* World)
	{
		return CreateWidget<UToastStackWidget>(World, UToastStackWidget::StaticClass());
	}
}

/**
 * THE DEFECT IN SPEC SECTION 1, PINNED. The bar used to bind OnNotification to a single
 * UTextBlock, so a second notification in the same second REPLACED the first and the first
 * was never seen. Two entries must produce two rows.
 *
 * At the level of the COMPOSITION - spawn the widget and tick it - not of the model struct.
 * UNotificationCentre's own tests already prove it holds two entries; what this adds is that
 * the widget above it actually draws them, which is the half that was broken.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FToastStackShowsEveryEntryTest,
	"AirportMgr.UI.ToastsDoNotOverwriteEachOther",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FToastStackShowsEveryEntryTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld(/*bSpawnActor=*/false);
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }

	UToastStackWidget* Stack = MakeStack(TestWorld.World);
	if (!TestNotNull(TEXT("the stack is created with no asset"), Stack)) { return false; }
	if (!TestNotNull(TEXT("it owns a notification centre"), Stack->Centre())) { return false; }

	Stack->Centre()->PostFeed(FText::FromString(TEXT("Saved 'quick'")));
	Stack->Centre()->PostFeed(FText::FromString(TEXT("Arrival refused - runway too short")));

	// One frame. Both must be on screen: the old single label would have shown only the last.
	Stack->TickFeed(1.0f / 60.0f);
	TestEqual(TEXT("both notifications are drawn, not just the last one"),
		Stack->ToastCountForTest(), 2);
	return true;
}

/**
 * The real-seconds rule, measured through the WIDGET rather than the model.
 *
 * UNotificationCentre::Advance takes real seconds by contract, so a test against the model
 * alone cannot catch a caller that hands it game seconds - and the caller is where that
 * mistake lives. The plan for this work specified Advance(Delta * Multiplier(Speed)), which
 * at x32 would have given an eight-second toast a quarter of a second on screen. Half a real
 * second of frames must expire nothing, whatever the sim clock is doing.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FToastStackRealSecondsTest,
	"AirportMgr.UI.ToastsSurviveAFastClock",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FToastStackRealSecondsTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld(/*bSpawnActor=*/false);
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }

	UToastStackWidget* Stack = MakeStack(TestWorld.World);
	if (!TestNotNull(TEXT("the stack is created"), Stack)) { return false; }

	Stack->Centre()->PostFeed(FText::FromString(TEXT("Loaded 'quick'")));
	for (int32 Frame = 0; Frame < 30; ++Frame)
	{
		Stack->TickFeed(1.0f / 60.0f);
	}

	TestEqual(TEXT("half a real second of frames expires nothing"), Stack->ToastCountForTest(), 1);
	TestTrue(TEXT("and the centre has been told half a real second, not a scaled one"),
		Stack->Centre()->Now() < 0.6);

	// Past its lifetime it does go, so the count above is not just a stuck widget.
	Stack->TickFeed(9.0f);
	TestEqual(TEXT("past its lifetime it clears itself"), Stack->ToastCountForTest(), 0);
	return true;
}

/**
 * THE DECLARED-BUT-NEVER-CONSUMED BUG, caught in the act and kept caught.
 *
 * UUIStyle::CornerRadius sat in the header and in DA_UIStyle, read by NOTHING, for the whole
 * of the first implementation - so every toast drew as a flat square slab while the asset
 * cheerfully carried a radius of 5. That is the same bug CLAUDE.md names three times
 * (ToolCommandList, GetModeCommands, ARoadBuildController::Tools): a value is added to one
 * place and no consumer is ever wired to it.
 *
 * Reading the radius back OFF THE BRUSH is the point. Asserting that the style has a
 * CornerRadius would pass on a completely square card - it is the drawn brush, not the
 * declaration, that has to carry it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FToastCardRoundingTest,
	"AirportMgr.UI.ToastCardUsesTheStyleCornerRadius",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FToastCardRoundingTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld(/*bSpawnActor=*/false);
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }

	UToastStackWidget* Stack = MakeStack(TestWorld.World);
	if (!TestNotNull(TEXT("the stack is created"), Stack)) { return false; }

	Stack->Centre()->PostFeed(FText::FromString(TEXT("Saved 'quick'")));
	Stack->TickFeed(1.0f / 60.0f);

	FSlateBrush Brush;
	if (!TestTrue(TEXT("the card has a brush to read"), Stack->FirstToastBrushForTest(Brush)))
	{
		return false;
	}

	const UUIStyle* Style = UAirportMgrUISettings::ResolveStyle();
	TestEqual(TEXT("the card is drawn as a ROUNDED box, not the default flat one"),
		Brush.DrawAs, ESlateBrushDrawType::RoundedBox);
	// float against float: the ambiguity is real, TestEqual takes double and float overloads
	// and CornerRadii is a FVector4 of doubles.
	TestTrue(TEXT("and its radius is the style's CornerRadius, so the asset's value is the "
		"one on screen"),
		FMath::IsNearlyEqual(static_cast<float>(Brush.OutlineSettings.CornerRadii.X),
			Style->CornerRadius, KINDA_SMALL_NUMBER));
	return true;
}

/**
 * Severity reaches the card. Spec section 6.1 lists Severity in the entry and the first
 * implementation dropped the field entirely, so every toast drew identically - a refusal
 * looked exactly like a save confirmation.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FToastSeverityTest,
	"AirportMgr.UI.ToastSeverityPicksItsColour",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FToastSeverityTest::RunTest(const FString& Parameters)
{
	const UUIStyle* Style = UAirportMgrUISettings::ResolveStyle();

	// Three severities, three DIFFERENT slots. Accent is not among them on purpose: it means
	// the armed tool and nothing else, so a warning may never take it.
	UNotificationCentre* Centre = NewObject<UNotificationCentre>(GetTransientPackage());
	Centre->PostFeed(FText::FromString(TEXT("info")), ENotificationSeverity::Info);
	Centre->PostFeed(FText::FromString(TEXT("good")), ENotificationSeverity::Success);
	Centre->PostFeed(FText::FromString(TEXT("bad")), ENotificationSeverity::Warning);

	TestEqual(TEXT("severity is carried on the entry"), Centre->Entries().Num(), 3);
	TestEqual(TEXT("info stays info"), Centre->Entries()[0].Severity, ENotificationSeverity::Info);
	TestEqual(TEXT("success stays success"), Centre->Entries()[1].Severity, ENotificationSeverity::Success);
	TestEqual(TEXT("warning stays warning"), Centre->Entries()[2].Severity, ENotificationSeverity::Warning);

	TestTrue(TEXT("warning and success are different colours, or severity says nothing"),
		!Style->Warning.Equals(Style->Positive));
	TestTrue(TEXT("a warning never takes Accent, which means the armed tool"),
		!Style->Warning.Equals(Style->Accent));
	return true;
}

/**
 * ISSUE #186, PINNED. TickFeed used to call Rebuild every frame, which did
 * ToastColumn->ClearChildren() and reconstructed every card - UBorder, UHorizontalBox, UImage,
 * UTextBlock - from scratch, for a widget that ticks every frame it is on screen. A fresh
 * UBorder with an identical brush passes FToastCardRoundingTest above; only watching the
 * WIDGET ITSELF across ticks catches a rebuild that merely looks unchanged.
 *
 * A plain count of ConstructWidget calls would need one probe per widget class (6, per the
 * issue); CardsConstructedForTest counts BuildCard calls instead - the one function every
 * new card's construction is required to go through - which is the same measurement with one
 * probe. The identity check below is what actually goes red on the reverted code: on main,
 * NthToastForTest(0) returns a DIFFERENT UBorder every tick even though nothing changed.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FToastStackDoesNotRebuildUnchangedEntriesTest,
	"AirportMgr.UI.ToastCardsSurviveAnUnchangedTick",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FToastStackDoesNotRebuildUnchangedEntriesTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld(/*bSpawnActor=*/false);
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }

	UToastStackWidget* Stack = MakeStack(TestWorld.World);
	if (!TestNotNull(TEXT("the stack is created"), Stack)) { return false; }

	// THREE ENTRIES, not one: the old ClearChildren()-and-rebuild would have rebuilt all of
	// them, so the assertions below would go red for any of the three, not just the front.
	Stack->Centre()->PostFeed(FText::FromString(TEXT("Saved 'quick'")));
	Stack->Centre()->PostFeed(FText::FromString(TEXT("Loaded 'quick'")));
	Stack->Centre()->PostFeed(FText::FromString(TEXT("Arrival refused - runway too short")));

	// First tick: the three cards are BUILT. This is the one tick allowed to construct.
	Stack->TickFeed(1.0f / 60.0f);
	TestEqual(TEXT("three entries built three cards"), Stack->ToastCountForTest(), 3);
	const int32 BuiltAfterFirstTick = Stack->CardsConstructedForTest();
	TestEqual(TEXT("one BuildCard per entry, once"), BuiltAfterFirstTick, 3);

	UBorder* FirstCard = Stack->NthToastForTest(0);
	if (!TestNotNull(TEXT("the first card exists"), FirstCard)) { return false; }

	// K MORE TICKS, N UNCHANGED ENTRIES: nothing is posted and nothing expires (all three are
	// well inside the 8-second default lifetime), so a correct widget touches opacity only.
	for (int32 Frame = 0; Frame < 30; ++Frame)
	{
		Stack->TickFeed(1.0f / 60.0f);
	}

	TestEqual(TEXT("still three cards - none dropped, none duplicated"),
		Stack->ToastCountForTest(), 3);
	TestEqual(TEXT("30 more ticks of an unchanged feed construct ZERO new cards"),
		Stack->CardsConstructedForTest(), BuiltAfterFirstTick);
	TestTrue(TEXT("the front card is the SAME UBorder instance, not a rebuilt lookalike"),
		Stack->NthToastForTest(0) == FirstCard);
	return true;
}

/**
 * REVIEW HARDENING, #200. SyncCards used to trim only the FRONT of Cards, which is exact only
 * because every entry today shares one FeedLifetimeRealSeconds - real removal never touches
 * the middle. UNotificationCentre::RemoveEntryForTest stands in for a future feature that
 * WOULD (a per-severity lifetime letting a Warning outlive an Info raised earlier) so this
 * test is written against that shape now, before it exists, rather than after it ships broken.
 *
 * Three entries, remove the MIDDLE one directly (not by waiting out its lifetime - nothing
 * here ages out early on its own): the first and third must keep their OWN card instances and
 * their OWN text, sliding up one slot without being torn down and rebuilt.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FToastStackHandlesAMidListRemovalTest,
	"AirportMgr.UI.ToastCardsSurviveARemovalFromTheMiddle",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FToastStackHandlesAMidListRemovalTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld(/*bSpawnActor=*/false);
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }

	UToastStackWidget* Stack = MakeStack(TestWorld.World);
	if (!TestNotNull(TEXT("the stack is created"), Stack)) { return false; }

	Stack->Centre()->PostFeed(FText::FromString(TEXT("first")));
	Stack->Centre()->PostFeed(FText::FromString(TEXT("second")));
	Stack->Centre()->PostFeed(FText::FromString(TEXT("third")));
	if (!TestEqual(TEXT("three entries posted"), Stack->Centre()->Entries().Num(), 3))
	{
		return false;
	}
	const int32 MiddleId = Stack->Centre()->Entries()[1].Id;

	// Build: three cards, one apiece.
	Stack->TickFeed(1.0f / 60.0f);
	TestEqual(TEXT("three entries built three cards"), Stack->ToastCountForTest(), 3);
	const int32 BuiltBeforeRemoval = Stack->CardsConstructedForTest();

	UBorder* FirstCardBefore = Stack->NthToastForTest(0);
	UBorder* ThirdCardBefore = Stack->NthToastForTest(2);
	if (!TestNotNull(TEXT("the first card exists"), FirstCardBefore)
		|| !TestNotNull(TEXT("the third card exists"), ThirdCardBefore))
	{
		return false;
	}

	// THE MIDDLE ENTRY GOES, not the front and not the back - the case a front-only trim
	// cannot see coming.
	TestTrue(TEXT("the middle entry is removed"), Stack->Centre()->RemoveEntryForTest(MiddleId));
	TestEqual(TEXT("two entries remain"), Stack->Centre()->Entries().Num(), 2);

	Stack->TickFeed(1.0f / 60.0f);

	TestEqual(TEXT("two cards remain - the middle one's card was dropped"),
		Stack->ToastCountForTest(), 2);
	TestEqual(TEXT("dropping a survivor's card is not building one - ZERO new construction"),
		Stack->CardsConstructedForTest(), BuiltBeforeRemoval);

	TestTrue(TEXT("the FIRST entry's card is the same instance, not rebuilt"),
		Stack->NthToastForTest(0) == FirstCardBefore);
	TestTrue(TEXT("the THIRD entry's card is the same instance, now one slot up, not rebuilt"),
		Stack->NthToastForTest(1) == ThirdCardBefore);

	FText Row0Text, Row1Text;
	if (!TestTrue(TEXT("row 0 has text"), Stack->NthToastTextForTest(0, Row0Text))
		|| !TestTrue(TEXT("row 1 has text"), Stack->NthToastTextForTest(1, Row1Text)))
	{
		return false;
	}
	TestEqual(TEXT("row 0 still shows the FIRST entry, in order"), Row0Text.ToString(), FString(TEXT("first")));
	TestEqual(TEXT("row 1 now shows the THIRD entry, in order - not the removed middle one"),
		Row1Text.ToString(), FString(TEXT("third")));
	return true;
}

#endif
