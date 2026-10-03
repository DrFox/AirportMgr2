#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Components/Border.h"
#include "Misc/AutomationTest.h"
#include "NotificationCentre.h"
#include "Testing/AirsideTestWorld.h"
#include "ToastStackWidget.h"
#include "AlertsPanelWidget.h"
#include "Content/AirsideSettings.h"
#include "Model/JobBoard.h"
#include "Model/OpsAlerts.h"
#include "Model/OpsEvents.h"
#include "OpsRuntimeResolver.h"
#include "Present/OpsRuntime.h"
#include "Present/RoadNetworkActor.h"
#include "Model/RoadNetwork.h"
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
 * The real-seconds rule, measured through the WIDGET rather than the model.
 *
 * UNotificationCentre::Advance takes real seconds by contract, so a test against the model
 * alone cannot catch a caller that hands it game seconds - and the caller is where that
 * mistake lives. The plan for this work specified Advance(Delta * Multiplier(Speed)), which
 * at x32 would have given an eight-second toast a quarter of a second on screen. Half a real
 * second of frames must expire nothing, whatever the sim clock is doing.
 *
 * NAMED FOR WHAT IT MEASURES (2026-10 review): it was "ToastsSurviveAFastClock" and never set a clock. No clock reaches
 * the widget - TickFeed takes real seconds and nothing else - so there is none to set. What it pins is that TickFeed's
 * argument is taken as REAL seconds: scale it, as the plan's Advance(Delta * Multiplier(Speed)) would at x32, and half a
 * second of frames expires the toast.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FToastStackRealSecondsTest,
	"AirportMgr.UI.ToastLifetimeRunsOnRealSeconds",
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
	TestTrue(TEXT("and its radius is the style's WindowRadius, so the asset's value is the "
		"one on screen"),
		FMath::IsNearlyEqual(static_cast<float>(Brush.OutlineSettings.CornerRadii.X),
			Style->WindowRadius, KINDA_SMALL_NUMBER));
	return true;
}

/**
 * Severity reaches the card. Spec section 6.1 lists Severity in the entry and the first
 * implementation dropped the field entirely, so every toast drew identically - a refusal
 * looked exactly like a save confirmation.
 *
 * READ OFF THE CARD'S OUTLINE, the drawn brush, not out of the model (2026-09-30 review): this test built
 * no widget - it asserted the centre kept the severity it was given and that two style slots differ - so
 * UToastStackWidget::ColourFor could have returned one slot for all three and it stayed green.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FToastSeverityTest,
	"AirportMgr.UI.ToastSeverityPicksItsColour",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FToastSeverityTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld(/*bSpawnActor=*/false);
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }

	UToastStackWidget* Stack = MakeStack(TestWorld.World);
	if (!TestNotNull(TEXT("the stack is created"), Stack)) { return false; }
	const UUIStyle* Style = Stack->PanelStyleForTest();
	if (!TestNotNull(TEXT("and resolved a style"), Style)) { return false; }

	// Three severities, three DIFFERENT slots. Accent is not among them on purpose: it means
	// the armed tool and nothing else, so a warning may never take it.
	Stack->Centre()->PostFeed(FText::FromString(TEXT("info")), ENotificationSeverity::Info);
	Stack->Centre()->PostFeed(FText::FromString(TEXT("good")), ENotificationSeverity::Success);
	Stack->Centre()->PostFeed(FText::FromString(TEXT("bad")), ENotificationSeverity::Warning);
	Stack->TickFeed(1.0f / 60.0f);
	if (!TestEqual(TEXT("three entries built three cards"), Stack->ToastCountForTest(), 3)) { return false; }

	// The outline is FLinearColor(slot.RGB, OutlineAlpha): compare the slot's RGB, not the alpha.
	const auto OutlineOf = [Stack](int32 Index, FLinearColor& Out)
	{
		const UBorder* Card = Stack->NthToastForTest(Index);
		if (Card == nullptr) { return false; }
		Out = Card->Background.OutlineSettings.Color.GetSpecifiedColor();
		return true;
	};
	const auto SameRgb = [](const FLinearColor& A, const FLinearColor& B)
	{
		return FMath::IsNearlyEqual(A.R, B.R, KINDA_SMALL_NUMBER) && FMath::IsNearlyEqual(A.G, B.G, KINDA_SMALL_NUMBER)
			&& FMath::IsNearlyEqual(A.B, B.B, KINDA_SMALL_NUMBER);
	};
	FLinearColor Info = FLinearColor::Black, Success = FLinearColor::Black, Warning = FLinearColor::Black;
	if (!TestTrue(TEXT("every card has a brush to read"), OutlineOf(0, Info) && OutlineOf(1, Success) && OutlineOf(2, Warning)))
	{
		return false;
	}

	// THE PREMISE: the three slots differ, or "the card wears its slot" could not tell them apart.
	TestFalse(TEXT("premise: warning and success are different colours, or severity says nothing"), SameRgb(Style->Warning, Style->Positive));
	TestFalse(TEXT("premise: nor is info the warning colour"), SameRgb(Style->InkMuted, Style->Warning));
	TestTrue(TEXT("an info card is drawn in InkMuted"), SameRgb(Info, Style->InkMuted));
	TestTrue(TEXT("a success card is drawn in Positive"), SameRgb(Success, Style->Positive));
	TestTrue(TEXT("a warning card is drawn in Warning"), SameRgb(Warning, Style->Warning));
	TestFalse(TEXT("a warning never takes Accent, which means the armed tool"), SameRgb(Warning, Style->Accent));
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FToastsFromOpsAlertsTest,
	"AirportMgr.UI.ToastsSayAlertsAndRefusals",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FToastsFromOpsAlertsTest::RunTest(const FString& Parameters)
{
	// OPS ALERTS STAGE 1 (spec 2026-09-29-ops-alerts §3): what used to be log lines now reaches the player -
	// an alert starting, a build or a landing refused. Bound through BindTo, the one seam the widget's own
	// runtime binding uses, so no world's event bus is needed.
	FAirsideTestWorld TestWorld;
	UToastStackWidget* Stack = MakeStack(TestWorld.World);
	if (!TestNotNull(TEXT("a toast stack"), Stack)) { return false; }
	UOpsEvents* Events = NewObject<UOpsEvents>();
	Stack->BindTo(*Events);

	FOpsAlert Alert;
	Alert.Key.Kind = EAlertKind::JobUnserviceable;
	Alert.Text = FText::FromString(TEXT("No fuel for stand 3: no road"));
	Events->OnAlertRaised.Broadcast(Alert);
	if (!TestEqual(TEXT("an alert starting is one toast"), Stack->Centre()->Entries().Num(), 1)) { return false; }
	TestEqual(TEXT("a Warning - the player has something to do"), Stack->Centre()->Entries()[0].Severity, ENotificationSeverity::Warning);
	TestEqual(TEXT("in the alert's own words"), Stack->Centre()->Entries()[0].Text.ToString(), Alert.Text.ToString());

	Events->OnAlertCleared.Broadcast(Alert.Key);
	TestEqual(TEXT("most clears are silent - the badge count says it"), Stack->Centre()->Entries().Num(), 1);

	FOpsAlert Again = Alert;
	Again.bReRaised = true;
	Events->OnAlertRaised.Broadcast(Again);
	TestEqual(TEXT("a re-raise after a load is not news - no toast"), Stack->Centre()->Entries().Num(), 1);

	Events->OnBalanceSignChanged.Broadcast(true);
	Events->OnBalanceSignChanged.Broadcast(false);
	TestEqual(TEXT("a dip and recovery with no Overdrawn alert toasts nothing - 'back in credit' from nowhere"),
		Stack->Centre()->Entries().Num(), 1);

	FOpsAlert Red;
	Red.Key.Kind = EAlertKind::Overdrawn;
	Red.Text = FText::FromString(TEXT("Overdrawn - building is locked"));
	Events->OnAlertRaised.Broadcast(Red);
	TestEqual(TEXT("going into the red is the Overdrawn alert's toast"), Stack->Centre()->Entries().Num(), 2);
	Events->OnAlertCleared.Broadcast(Red.Key);
	TestEqual(TEXT("its clear says nothing - the money event does"), Stack->Centre()->Entries().Num(), 2);
	Events->OnBalanceSignChanged.Broadcast(false);
	if (TestEqual(TEXT("coming out of the red, after the alert said so, is said"), Stack->Centre()->Entries().Num(), 3))
	{
		TestEqual(TEXT("as Info"), Stack->Centre()->Entries()[2].Severity, ENotificationSeverity::Info);
	}

	Events->OnBuildRefused.Broadcast(TEXT("Taxiway, 100 m"), TEXT("30,000"), TEXT("-1"));
	if (TestEqual(TEXT("a refused build is a toast"), Stack->Centre()->Entries().Num(), 4))
	{
		const FString Said = Stack->Centre()->Entries()[3].Text.ToString();
		TestTrue(TEXT("naming what, the price and the balance"),
			Said.Contains(TEXT("Taxiway, 100 m")) && Said.Contains(TEXT("30,000")) && Said.Contains(TEXT("-1")));
		TestEqual(TEXT("as a Warning"), Stack->Centre()->Entries()[3].Severity, ENotificationSeverity::Warning);
	}

	Events->OnLandRefused.Broadcast(EArrivalRefusal::NoRunway, FString());
	TestEqual(TEXT("and so is a refused key 7"), Stack->Centre()->Entries().Num(), 5);
	// THE REFUSAL'S OWN SENTENCE WHEN IT HAS ONE (#456 review) - not the reason-only wording, which says "not admitted to
	// that runway" for an arrivals-only field whose real reason is the departure.
	const FString Planned = TEXT("Arrival refused: no runway here can take the departure.");
	Events->OnLandRefused.Broadcast(EArrivalRefusal::NotAdmitted, Planned);
	if (TestEqual(TEXT("a worded refusal is a toast too"), Stack->Centre()->Entries().Num(), 6))
	{
		TestEqual(TEXT("in the plan's words"), Stack->Centre()->Entries()[5].Text.ToString(), Planned);
	}

	// #266: THE REPAIR'S LINE through the purchase face (its own warning delegate until #445 item 7) - unbound, the modules would be
	// removed and refunded in silence.
	FOpsPurchase Refund;
	Refund.Kind = EOpsPurchaseKind::ModulesRefunded;
	Refund.Name = FText::FromString(TEXT("Sheds"));
	Refund.Count = 2;
	Refund.Amount = 80000.0;
	Refund.Money = FText::FromString(TEXT("80,000"));
	Events->NotifyPurchase(Refund);
	if (TestEqual(TEXT("a refund is a toast"), Stack->Centre()->Entries().Num(), 7))
	{
		TestEqual(TEXT("as a Warning, where a purchase is Info"), Stack->Centre()->Entries()[6].Severity, ENotificationSeverity::Warning);
		TestTrue(TEXT("in the widget's words, with the count and the noun it was given"), Stack->Centre()->Entries()[6].Text.ToString().Contains(TEXT("2 Sheds removed")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FToastsWordSavesAndPurchasesTest,
	"AirportMgr.UI.ToastsWordSavesAndPurchases",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FToastsWordSavesAndPurchasesTest::RunTest(const FString& Parameters)
{
	// #445 item 7: UOpsRuntime worded every save, load and purchase toast as an English FString on a catch-all event, so the widget -
	// which says it is the one place that decides what the player is told - could not tell "Save to 'X' failed" from "Saved 'X'",
	// and showed a failed save as Info. The faces carry the case and the facts now; the words and the severity are decided HERE.
	// Each case by name, its sentence and its severity, so a case that went silent or lost its Warning is red by its name.
	// Mutation-checked 2026-10-01: SaveFailed posted at Info, this went red ("SaveFailed at its severity").
	FAirsideTestWorld TestWorld;
	UToastStackWidget* Stack = MakeStack(TestWorld.World);
	if (!TestNotNull(TEXT("a toast stack"), Stack)) { return false; }
	UOpsEvents* Events = NewObject<UOpsEvents>();
	Stack->BindTo(*Events);
	auto Last = [Stack]() { return Stack->Centre()->Entries().Last(); };

	struct FSaveCase { EOpsSaveOutcome Outcome; const TCHAR* Says; ENotificationSeverity Severity; const TCHAR* Why; };
	const FSaveCase SaveCases[] = {
		{ EOpsSaveOutcome::Saved, TEXT("Saved 'Slot1'"), ENotificationSeverity::Info, TEXT("it happened, no decision") },
		// THE PIN: a failed save is an error the player must see - not the Info a good save is.
		{ EOpsSaveOutcome::SaveFailed, TEXT("Save to 'Slot1' failed"), ENotificationSeverity::Warning, TEXT("the save did not happen") },
		{ EOpsSaveOutcome::Loaded, TEXT("Loaded 'Slot1'"), ENotificationSeverity::Info, TEXT("it happened, no decision") },
		{ EOpsSaveOutcome::NoSave, TEXT("No save 'Slot1'"), ENotificationSeverity::Warning, TEXT("the load found nothing - a refusal") },
	};
	for (const FSaveCase& Case : SaveCases)
	{
		const int32 Before = Stack->Centre()->Entries().Num();
		Events->NotifySaveSlot(Case.Outcome, TEXT("Slot1"));
		const FString Name = UEnum::GetValueAsString(Case.Outcome);
		if (!TestEqual(*FString::Printf(TEXT("%s is one toast"), *Name), Stack->Centre()->Entries().Num(), Before + 1)) { continue; }
		TestEqual(*FString::Printf(TEXT("%s says '%s'"), *Name, Case.Says), Last().Text.ToString(), FString(Case.Says));
		TestEqual(*FString::Printf(TEXT("%s at its severity: %s"), *Name, Case.Why), Last().Severity, Case.Severity);
	}

	FOpsPurchase Purchase;
	Purchase.Name = FText::FromString(TEXT("Fuel bowser"));
	Purchase.Money = FText::FromString(TEXT("$45,000"));
	struct FPurchaseCase { EOpsPurchaseKind Kind; double Amount; const TCHAR* Says; ENotificationSeverity Severity; };
	const FPurchaseCase PurchaseCases[] = {
		{ EOpsPurchaseKind::VehicleBought, 45000.0, TEXT("Bought Fuel bowser — $45,000"), ENotificationSeverity::Info },
		{ EOpsPurchaseKind::VehicleSold, 45000.0, TEXT("Sold Fuel bowser — $45,000"), ENotificationSeverity::Info },
		// A SALE WORTH NOTHING (#487) says no money: no em dash and no "$0".
		{ EOpsPurchaseKind::VehicleSold, 0.0, TEXT("Sold Fuel bowser"), ENotificationSeverity::Info },
		// A WITHDRAWN VEHICLE (#443) says where the credit came from, or that there was none.
		{ EOpsPurchaseKind::VehicleWithdrawn, 45000.0, TEXT("Depot removed — Fuel bowser credited $45,000"), ENotificationSeverity::Info },
		{ EOpsPurchaseKind::VehicleWithdrawn, 0.0, TEXT("Depot removed — Fuel bowser withdrawn"), ENotificationSeverity::Info },
		{ EOpsPurchaseKind::ModuleBought, 45000.0, TEXT("Bought Fuel bowser — $45,000"), ENotificationSeverity::Info },
		// LAND (land purchase spec R10): a receipt, worded as a module is (the runtime names it "land").
		{ EOpsPurchaseKind::LandBought, 45000.0, TEXT("Bought Fuel bowser — $45,000"), ENotificationSeverity::Info },
		{ EOpsPurchaseKind::ModulesRefunded, 0.0, TEXT("No room on its plot — 1 Fuel bowser removed"), ENotificationSeverity::Warning },
		// THE PAID REFUND (#499 review): the figure, and "refunded", when money came back.
		{ EOpsPurchaseKind::ModulesRefunded, 45000.0, TEXT("No room on its plot — 1 Fuel bowser removed, $45,000 refunded"), ENotificationSeverity::Warning },
		// FUEL (2026-10-03): an order, a signing - "a day", since nothing is charged until the day end - and a cancel, with its
		// charge or, with none owed, without a "$0".
		{ EOpsPurchaseKind::FuelOrdered, 45000.0, TEXT("Ordered Fuel bowser — $45,000"), ENotificationSeverity::Info },
		{ EOpsPurchaseKind::FuelContractSigned, 45000.0, TEXT("Signed Fuel bowser — $45,000 a day"), ENotificationSeverity::Info },
		{ EOpsPurchaseKind::FuelContractCancelled, 45000.0, TEXT("Cancelled Fuel bowser — $45,000 charged"), ENotificationSeverity::Info },
		{ EOpsPurchaseKind::FuelContractCancelled, 0.0, TEXT("Cancelled Fuel bowser"), ENotificationSeverity::Info },
		// TAKE-OR-PAY'S LOSS: a warning, naming what was lost, with no figure (the day's own line carries the money).
		{ EOpsPurchaseKind::FuelPouredAway, 0.0, TEXT("Tanks full — Fuel bowser poured away"), ENotificationSeverity::Warning },
	};
	for (const FPurchaseCase& Case : PurchaseCases)
	{
		const int32 Before = Stack->Centre()->Entries().Num();
		Purchase.Kind = Case.Kind;
		Purchase.Amount = Case.Amount;
		Events->NotifyPurchase(Purchase);
		const FString Name = FString::Printf(TEXT("%s at %.0f"), *UEnum::GetValueAsString(Case.Kind), Case.Amount);
		if (!TestEqual(*FString::Printf(TEXT("%s is one toast"), *Name), Stack->Centre()->Entries().Num(), Before + 1)) { continue; }
		TestEqual(*FString::Printf(TEXT("%s says '%s'"), *Name, Case.Says), Last().Text.ToString(), FString(Case.Says));
		TestEqual(*FString::Printf(TEXT("%s at its severity"), *Name), Last().Severity, Case.Severity);
	}

	// #471: A DISPATCH REFUSAL SAYS THE PLAN'S SENTENCE TOO - Airside's OnArrivalRefused used to reach the toast with the
	// reason alone, so the toast said "not admitted to that runway" where the log said why.
	const FString Dispatched = TEXT("Arrival refused: the runway admits a 24.0 m wingspan; this aircraft's is 36.0 m.");
	const int32 BeforeRefusal = Stack->Centre()->Entries().Num();
	Events->NotifyArrivalRefused(EArrivalRefusal::NotAdmitted, Dispatched);
	if (TestEqual(TEXT("a refused dispatch is a toast"), Stack->Centre()->Entries().Num(), BeforeRefusal + 1))
	{
		TestEqual(TEXT("in the plan's words, not the reason's"), Last().Text.ToString(), Dispatched);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FToastsOnceAcrossARoadTest,
	"AirportMgr.UI.ToastsAnUnserviceableJobOnceWhateverRoadsAreDrawn",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FToastsOnceAcrossARoadTest::RunTest(const FString& Parameters)
{
	// #445's PIN, at the toast: "raise an unserviceable job, commit an unrelated road, tick 3 frames - one toast total". The real runtime's passes, the
	// real stack bound to its events. A refused job was re-offered after the bids, the alert cleared for the frame between and was raised again with a
	// fresh Warning toast: one per road drawn while trying to connect a depot.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor"), TestWorld.Actor)) { return false; }
	const int32 A = TestWorld.Actor->PlaceNode(FVector2D(0.0, 0.0));
	const int32 B = TestWorld.Actor->PlaceNode(FVector2D(20000.0, 0.0));
	TestWorld.Actor->ConnectNodes(A, B);
	UOpsRuntime* Runtime = NewObject<UOpsRuntime>();
	Runtime->Attach(TestWorld.Actor);
	UToastStackWidget* Stack = MakeStack(TestWorld.World);
	if (!TestNotNull(TEXT("a toast stack"), Stack)) { return false; }
	Stack->BindTo(*Runtime->GetEvents());
	Runtime->Tick(0.0);

	URoadNetwork* Net = TestWorld.Actor->Network;
	FServiceJob& Job = Runtime->GetJobBoard()->AddJobForTest(5, EServiceJobState::Unserviceable, EServiceRefusal::NoDepot, Net->GetGuidelineRevision());
	Job.Stand.Index = 0;
	Runtime->GetBus().MarkDirty(TEXT("Alerts"));
	for (int32 Frame = 0; Frame < 3; ++Frame) { Runtime->Tick(0.0); }
	auto FuelToasts = [Stack]()
	{
		int32 Count = 0;
		for (const auto& Each : Stack->Centre()->Entries())
		{
			Count += Each.Text.ToString().Contains(TEXT("No fuel for stand")) ? 1 : 0;
		}
		return Count;
	};
	if (!TestEqual(TEXT("PRECONDITION: the refused job is one toast"), FuelToasts(), 1)) { return false; }

	const int32 C = TestWorld.Actor->PlaceNode(FVector2D(0.0, 50000.0));
	const int32 D = TestWorld.Actor->PlaceNode(FVector2D(20000.0, 50000.0));
	TestWorld.Actor->ConnectNodes(C, D);
	for (int32 Frame = 0; Frame < 3; ++Frame) { Runtime->Tick(0.0); }
	TestEqual(TEXT("an unrelated road and three frames later, still one toast in total"), FuelToasts(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEveryOpsEventDelegateHasAListenerTest,
	"AirportMgr.UI.EveryOpsEventDelegateHasAListener",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FEveryOpsEventDelegateHasAListenerTest::RunTest(const FString& Parameters)
{
	// #445 (the shape of closed #169): UOpsEvents::OnAgentPhaseChanged and OnSpeedChanged had NO listener - nothing bound them outside a test, there
	// are no widget Blueprints that do (a byte-grep of Content for "OpsEvents" is empty), and the wiring test counted a forwarder and a log line as a
	// consumer. The COMPOSED UI - the widgets that turn the runtime's events into what the player sees, built as play builds them, against a runtime -
	// is asked, by REFLECTION over every delegate UOpsEvents declares, whether each has a listener. A delegate added without a widget that binds it
	// is red by name.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor"), TestWorld.Actor)) { return false; }
	TestWorld.Actor->PlaceNode(FVector2D(0.0, 0.0));
	UOpsRuntime* Runtime = NewObject<UOpsRuntime>();
	Runtime->Attach(TestWorld.Actor);
	OpsRuntimeResolver::SetOverrideForTest(TestWorld.World, Runtime);
	ON_SCOPE_EXIT { OpsRuntimeResolver::SetOverrideForTest(TestWorld.World, nullptr); };

	// EACH WIDGET BINDS THROUGH ITS OWN BuildOnce, which asks the world's resolver for the runtime - the path play takes.
	UToastStackWidget* Toasts = MakeStack(TestWorld.World);
	UAlertsPanelWidget* Alerts = CreateWidget<UAlertsPanelWidget>(TestWorld.World, UAlertsPanelWidget::StaticClass());
	if (!TestTrue(TEXT("the widgets that listen are built"), Toasts != nullptr && Alerts != nullptr)) { return false; }

	UOpsEvents* Events = Runtime->GetEvents();
	int32 Seen = 0;
	for (TFieldIterator<FMulticastDelegateProperty> It(UOpsEvents::StaticClass()); It; ++It)
	{
		const FMulticastScriptDelegate* Delegate = It->GetMulticastDelegate(It->ContainerPtrToValuePtr<void>(Events));
		TestTrue(*FString::Printf(TEXT("UOpsEvents::%s has a listener in the composed UI"), *It->GetName()), Delegate != nullptr && Delegate->IsBound());
		++Seen;
	}
	TestTrue(TEXT("the reflection found the delegates at all"), Seen > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FToastsTaxiwaySplitTest, "AirportMgr.UI.ToastsSayTaxiwaySplits",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FToastsTaxiwaySplitTest::RunTest(const FString& Parameters)
{
	// "C split off from A" (taxiway naming spec): the player's own edit renamed part of a taxiway - Info, not a Warning,
	// since nothing needs fixing. Bound through BindTo, the seam the runtime binding uses.
	FAirsideTestWorld TestWorld;
	UToastStackWidget* Stack = MakeStack(TestWorld.World);
	if (!TestNotNull(TEXT("a toast stack"), Stack)) { return false; }
	UOpsEvents* Events = NewObject<UOpsEvents>();
	Stack->BindTo(*Events);
	Events->OnTaxiwaySplit.Broadcast(TEXT("C"), TEXT("A"));
	if (!TestEqual(TEXT("one toast"), Stack->Centre()->Entries().Num(), 1)) { return false; }
	TestEqual(TEXT("in the spec's words"), Stack->Centre()->Entries()[0].Text.ToString(), FString(TEXT("C split off from A")));
	TestEqual(TEXT("Info"), Stack->Centre()->Entries()[0].Severity, ENotificationSeverity::Info);
	return true;
}

#endif
