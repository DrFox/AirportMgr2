#include "CoreMinimal.h"
#include "Components/TextBlock.h"
#include "Components/HorizontalBox.h"
#include "Misc/AutomationTest.h"
#include "UI/UiButton.h"
#include "UIStyle.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE ONE COLOUR RULE, AS A TABLE. Four widgets carried a copy of it (the bar, the variant bar,
 * the inspector, the inbox) and one copy had already drifted: a disabled Depart painted LIGHTER
 * than an enabled one. Accent means armed/affirmative and nothing else; a DISABLED Primary must
 * not keep it, or an offer that cannot be accepted shouts "accept me".
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUiButtonLookTest, "AirportMgr.UI.Button.LookTable",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FUiButtonLookTest::RunTest(const FString& Parameters)
{
	const UUIStyle& S = *GetDefault<UUIStyle>();
	struct FRow { EUiButtonKind Kind; bool bEnabled; bool bSelected; FLinearColor Fill; FLinearColor Ink; const TCHAR* Why; };
	const FRow Rows[] = {
		{ EUiButtonKind::Secondary, true,  false, S.Control, S.Ink,         TEXT("an idle tool") },
		{ EUiButtonKind::Secondary, true,  true,  S.Accent,  S.InkOnAccent, TEXT("the armed tool") },
		{ EUiButtonKind::Secondary, false, false, S.Control, S.InkMuted,    TEXT("disabled dims the INK, never lightens the fill") },
		{ EUiButtonKind::Secondary, false, true,  S.Control, S.InkMuted,    TEXT("a disabled tool cannot read as armed") },
		{ EUiButtonKind::Primary,   true,  false, S.Accent,  S.InkOnAccent, TEXT("Accept") },
		{ EUiButtonKind::Primary,   false, false, S.Control, S.InkMuted,    TEXT("an offer that cannot be taken must not shout") },
		{ EUiButtonKind::Danger,    true,  false, S.Warning, S.InkOnAccent, TEXT("a destructive verb") },
		{ EUiButtonKind::Danger,    false, false, S.Control, S.InkMuted,    TEXT("disabled destructive verb") },
		{ EUiButtonKind::Ghost,     true,  false, FLinearColor::White, S.Ink, TEXT("ghost: fill comes from its own brushes") },
		{ EUiButtonKind::Ghost,     false, false, FLinearColor::White, S.InkMuted, TEXT("disabled ghost") },
	};
	for (const FRow& R : Rows)
	{
		const FUiButtonLook Look = UUiButton::LookFor(S, R.Kind, R.bEnabled, R.bSelected);
		TestEqual(FString::Printf(TEXT("fill: %s"), R.Why), Look.Fill, R.Fill);
		TestEqual(FString::Printf(TEXT("ink: %s"), R.Why), Look.Ink, R.Ink);
	}
	return true;
}

/**
 * THE BAR CALLS SetState ~70 TIMES A TICK. SetBackgroundColor has no early-out of its own
 * (the SetCardShown/SetText lesson, issue #187), so an unchanged state must paint nothing.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUiButtonIdempotentTest, "AirportMgr.UI.Button.UnchangedStatePaintsNothing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FUiButtonIdempotentTest::RunTest(const FString& Parameters)
{
	const UUIStyle& S = *GetDefault<UUIStyle>();
	UUiButton* B = NewObject<UUiButton>();
	B->SetLabel(FText::FromString(TEXT("Road")));
	B->Build(S, EUiButtonKind::Secondary);
	B->SetState(true, false);
	const int32 After = B->PaintCountForTest();
	B->SetState(true, false);
	B->SetState(true, false);
	TestEqual(TEXT("repeating a state paints nothing"), B->PaintCountForTest(), After);
	B->SetState(true, true);
	TestEqual(TEXT("a change paints once"), B->PaintCountForTest(), After + 1);
	TestEqual(TEXT("selected shows Accent"), B->GetBackgroundColor(), S.Accent);
	TestEqual(TEXT("label follows"), B->GetLabel()->GetColorAndOpacity().GetSpecifiedColor(), S.InkOnAccent);
	return true;
}

/**
 * CONTENT SHAPE. A label-only button's content IS its text block - the inspector (and its test)
 * read a caption by casting GetContent(). And a label that changes every tick (the bar's
 * DynamicLabel) must not rebuild the content tree.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUiButtonContentTest, "AirportMgr.UI.Button.ContentShape",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FUiButtonContentTest::RunTest(const FString& Parameters)
{
	const UUIStyle& S = *GetDefault<UUIStyle>();
	UUiButton* Plain = NewObject<UUiButton>();
	Plain->SetLabel(FText::FromString(TEXT("Depart")));
	Plain->Build(S, EUiButtonKind::Secondary);
	TestTrue(TEXT("label only: content is the text block"), Plain->GetContent() == Plain->GetLabel());

	UTextBlock* Before = Plain->GetLabel();
	Plain->SetLabel(FText::FromString(TEXT("Unfollow")));
	TestTrue(TEXT("relabel keeps the same text block"), Plain->GetLabel() == Before);
	TestEqual(TEXT("relabel sets the text"), Plain->GetLabel()->GetText().ToString(), FString(TEXT("Unfollow")));

	UUiButton* WithDetail = NewObject<UUiButton>();
	WithDetail->SetLabel(FText::FromString(TEXT("A320")));
	WithDetail->SetDetail(FText::FromString(TEXT("runway too short")));
	WithDetail->Build(S, EUiButtonKind::Secondary, EUiButtonLayout::Inline);
	TestNotNull(TEXT("label + detail inline: a horizontal box"), Cast<UHorizontalBox>(WithDetail->GetContent()));
	return true;
}

#endif
