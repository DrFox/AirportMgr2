#include "BuildBarWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/SizeBox.h"
#include "Components/Spacer.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/WrapBox.h"
#include "Components/WrapBoxSlot.h"
#include "Model/OpsEvents.h"
#include "Model/SimClock.h"
#include "Model/Ledger.h"
#include "Model/Pricing.h"
#include "Present/OpsRuntime.h"
#include "Present/OpsRuntimeSubsystem.h"
#include "RoadBuildController.h"
#include "UIStyle.h"

DEFINE_LOG_CATEGORY_STATIC(LogBuildBar, Log, All);

namespace
{
	/**
	 * THE ONE TABLE the section switch and the Ensure() call list both used to be. Each
	 * entry's Slot is a pointer TO the named UPROPERTY, not a copy of it - the six BindWidgetOptional
	 * members in BuildBarWidget.h stay named (Blueprint binds a panel by that name, so they
	 * cannot become an array), but everything that used to switch on EActionSection to find
	 * one of them now walks this instead. A static_assert against EActionSection::Count means
	 * a seventh section with no row here fails the build, not silently drops its buttons.
	 */
	struct FSectionSpec
	{
		EActionSection Section;
		TObjectPtr<UPanelWidget> UBuildBarWidget::* Slot;
	};

	const FSectionSpec SectionSpecs[] =
	{
		{ EActionSection::Time,      &UBuildBarWidget::TimeSection },
		{ EActionSection::Tools,     &UBuildBarWidget::ToolsSection },
		{ EActionSection::Edit,      &UBuildBarWidget::EditSection },
		{ EActionSection::Aircraft,  &UBuildBarWidget::AircraftSection },
		{ EActionSection::Selection, &UBuildBarWidget::SelectionSection },
		{ EActionSection::Game,      &UBuildBarWidget::GameSection },
		{ EActionSection::Snap,      &UBuildBarWidget::SnapSection },
		{ EActionSection::SnapTo,    &UBuildBarWidget::SnapToSection },
	};
	static_assert(UE_ARRAY_COUNT(SectionSpecs) == static_cast<int32>(EActionSection::Count),
		"Every EActionSection needs a slot here - see BuildBarWidget.h's UPROPERTY list");
}

void UBuildBarEntry::HandleClicked()
{
	if (UBuildBarWidget* Bar = Owner.Get())
	{
		Bar->RunAction(ActionIndex);
	}
}

void UBuildBarWidget::BuildOnce(const UUIStyle& Style)
{
	// Threaded through rather than re-resolved: BuildOnce already has the resolved style in
	// hand, so EnsureSlots/BuildButtons take it instead of calling ResolveStyle() again.
	EnsureSlots(&Style);
	BuildButtons(&Style);

	// THE BAR IS NOT A NOTIFICATION SURFACE ANY MORE. It used to bind OnNotification to a
	// single UTextBlock that every notification overwrote and nothing ever cleared, so two
	// events in one second left only the second. One widget driving the tools AND showing
	// messages is how that came about; UToastStackWidget owns the feed now.
}

float UBuildBarWidget::BarHeightFor(const UUIStyle& Style)
{
	// DERIVED, so raising ButtonSize in the asset does not crop the buttons off the bottom
	// of the screen. Each term names the band it pays for, so a layout change here is one
	// line rather than a re-measured magic number.
	const float StatusStrip = 6.0f * 2.0f + 20.0f;                  // padding + the clock line
	const float SectionFrame = 6.0f * 2.0f + 6.0f;                  // row padding + frame padding
	const float Heading = 15.0f;                                    // the section's name
	const float ButtonStack = Style.ButtonSize * 0.5f + 25.0f;      // icon + label + button padding
	return StatusStrip + SectionFrame + Heading + ButtonStack;
}

void UBuildBarWidget::EnsureSlots(const UUIStyle* Style)
{
	// A root only if the asset gave none: BindWidgetOptional has already filled every slot
	// the asset supplies, and a code-built root would replace the designer's bar.
	//
	// The code-built chrome is a real bottom bar, not a placeholder: a canvas with the bar
	// anchored to the bottom edge. The feed that used to sit above it is UToastStackWidget's
	// now, and is a separate widget for exactly that reason. Python cannot author
	// the Blueprint on this engine build (UWidgetBlueprint::WidgetTree is not a scriptable
	// property), so this IS the default look, and a Blueprint is an optional restyle.
	if (WidgetTree->RootWidget == nullptr)
	{
		UCanvasPanel* Root = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("FallbackRoot"));
		WidgetTree->RootWidget = Root;

		// BarHeight is the floor; the style's own metrics decide the rest.
		const float Height = FMath::Max(static_cast<float>(BarHeight), BarHeightFor(*Style));

		UBorder* Border = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("BarBorder"));
		Border->SetBrushColor(Style->PanelDark);
		Border->SetPadding(FMargin(0.0f));
		UCanvasPanelSlot* BarSlot = Root->AddChildToCanvas(Border);
		// Stretched across the bottom edge: with both anchors on y=1, Offsets reads as
		// (left margin, top margin, right margin, HEIGHT).
		BarSlot->SetAnchors(FAnchors(0.0f, 1.0f, 1.0f, 1.0f));
		BarSlot->SetAlignment(FVector2D(0.0, 1.0));
		BarSlot->SetOffsets(FMargin(0.0f, 0.0f, 0.0f, Height));

		// THE HEIGHT IS NOW THE CONTENT'S, NOT A FIGURE. Anchored 0..1 horizontally the width
		// still comes from the anchors (SConstraintCanvas takes the stretch branch first), so
		// AutoSize only decides the height - and it has to, because the row below wraps. A bar
		// pinned to one line's height would have turned clipping at the right-hand edge into
		// clipping at the bottom, which is the same bug lying down.
		BarSlot->SetAutoSize(true);

		// THE FLOOR SURVIVES AS A MINIMUM. BarHeight and BarHeightFor stop being THE height
		// and become the least it may be, which is what they were always for - see BarHeight's
		// own comment about not cropping buttons when ButtonSize rises.
		USizeBox* Floor = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass(), TEXT("BarFloor"));
		Floor->SetMinDesiredHeight(Height);
		Border->SetContent(Floor);

		// TWO ROWS. The status strip sits on the darker slot so it reads as a different
		// SURFACE from the tools below it, not as the same bar with a gap in it.
		UVerticalBox* Rows = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("BarRows"));
		Floor->AddChild(Rows);

		UBorder* StatusBorder = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("StatusBorder"));
		StatusBorder->SetBrushColor(Style->PanelDark);
		StatusBorder->SetPadding(FMargin(Style->SectionPadding, 6.0f));
		Rows->AddChildToVerticalBox(StatusBorder);
		UHorizontalBox* StatusBox = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("StatusRow"));
		StatusBorder->SetContent(StatusBox);
		StatusRow = StatusBox;

		UBorder* ToolsBorder = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("ToolsBorder"));
		ToolsBorder->SetBrushColor(Style->Panel);
		ToolsBorder->SetPadding(FMargin(Style->SectionPadding, 6.0f));
		Rows->AddChildToVerticalBox(ToolsBorder);
		// A WRAP BOX, NOT A HORIZONTAL BOX. A horizontal row has no answer for more buttons
		// than fit: it lays them out past its own right edge and the overflow is simply not
		// drawn. That is not hypothetical - snap guides stage 3 added eight toggles and the
		// whole Snap section went off the end, silently, with every bar test still green.
		// Left at bExplicitWrapSize=false so SWrapBox takes the wrap width from the geometry
		// it is actually given; the bar is anchored to both screen edges, so that width is
		// the window's.
		SectionRow = WidgetTree->ConstructWidget<UWrapBox>(UWrapBox::StaticClass(), TEXT("ToolsRow"));
		ToolsBorder->SetContent(SectionRow);

		UE_LOG(LogBuildBar, Log, TEXT("No bar asset: building the code-only bar, %.0f uu tall"), Height);
	}

	auto Ensure = [&](TObjectPtr<UPanelWidget>& Section, const TCHAR* Name, EActionSection Which)
	{
		if (Section != nullptr)
		{
			return;
		}
		UHorizontalBox* Box = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), Name);

		// TIME IS THE ONLY SECTION ON THE STATUS ROW. It is not a tool - it does not change
		// what a click does - so grouping it with the build tools is what made seventeen
		// identically-weighted buttons read as one undifferentiated run.
		UPanelWidget* Row = (Which == EActionSection::Time) ? StatusRow.Get() : SectionRow.Get();
		if (Row == nullptr)
		{
			if (UPanelWidget* Root = Cast<UPanelWidget>(WidgetTree->RootWidget))
			{
				// The asset has a root but not this row: append to the root so the buttons
				// are at least visible, and say so - the designer forgot a panel.
				Root->AddChild(Box);
				UE_LOG(LogBuildBar, Warning, TEXT("Bar asset has no row for '%s'; appended a plain one to the root"), Name);
			}
			Section = Box;
			return;
		}

		if (Which == EActionSection::Time)
		{
			if (UHorizontalBox* RowBox = Cast<UHorizontalBox>(Row))
			{
				UHorizontalBoxSlot* BoxSlot = RowBox->AddChildToHorizontalBox(Box);
				BoxSlot->SetVerticalAlignment(VAlign_Center);
			}
			else
			{
				Row->AddChild(Box);
			}
			Section = Box;
			return;
		}

		// A FRAME AND A HEADING EACH. EActionSection already carried these six groupings and
		// the bar drew them as nothing; this is the whole of what makes them visible.
		UBorder* Frame = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
		Frame->SetBrushColor(Style->Panel);
		Frame->SetPadding(FMargin(8.0f, 2.0f, 8.0f, 4.0f));
		UVerticalBox* Group = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
		Frame->SetContent(Group);

		// ActionSectionName is already THE one source for these names (BuildActions.h) - a
		// retyped string here would be a second list to keep in agreement.
		UTextBlock* Heading = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
		Heading->SetText(FText::FromString(FString(ActionSectionName(Which)).ToUpper()));
		// Fallback/size/letter-spacing/colour: see UUIStyle::ApplyText (issue #89). The wide
		// spacing that makes a heading read as a heading, not a short label, lives there now.
		Style->ApplyText(*Heading, EUITextRole::Heading, Style->TextMuted);
		Group->AddChildToVerticalBox(Heading)->SetHorizontalAlignment(HAlign_Left);
		Group->AddChildToVerticalBox(Box);

		// Honour the cast: a Blueprint may supply any UPanelWidget as the row, and a blind
		// AddChildToHorizontalBox on a Grid would be a null dereference on somebody's asset.
		if (UWrapBox* WrapRow = Cast<UWrapBox>(Row))
		{
			// THE FRAME IS THE UNIT THAT WRAPS, not the button: a section broken across two
			// lines would put half of "Tools" under a heading and half under nothing, which
			// is worse than the clipping this replaces.
			UWrapBoxSlot* FrameSlot = WrapRow->AddChildToWrapBox(Frame);
			FrameSlot->SetPadding(FMargin(0.0f, 0.0f, Style->SectionPadding, 4.0f));
		}
		else if (UHorizontalBox* RowBox = Cast<UHorizontalBox>(Row))
		{
			UHorizontalBoxSlot* FrameSlot = RowBox->AddChildToHorizontalBox(Frame);
			FrameSlot->SetPadding(FMargin(0.0f, 0.0f, Style->SectionPadding, 0.0f));
			FrameSlot->SetVerticalAlignment(VAlign_Fill);
		}
		else
		{
			Row->AddChild(Frame);
		}

		Section = Box;
	};
	// Slot NAME is ActionSectionName + "Section" - exactly the literals this used to retype
	// ("TimeSection", "GameSection", ...) - so BuildActions.h stays the one source for the name
	// half too.
	for (const FSectionSpec& Spec : SectionSpecs)
	{
		Ensure(this->*Spec.Slot, *FString::Printf(TEXT("%sSection"), ActionSectionName(Spec.Section)), Spec.Section);
	}

	if (ClockText == nullptr)
	{
		ClockText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("ClockText"));
		Style->ApplyText(*ClockText, EUITextRole::Clock, Style->Text);
		if (UHorizontalBox* Box = Cast<UHorizontalBox>(TimeSection))
		{
			UHorizontalBoxSlot* ClockSlot = Box->AddChildToHorizontalBox(ClockText);
			ClockSlot->SetPadding(FMargin(0.0f, 0.0f, 14.0f, 0.0f));
			ClockSlot->SetVerticalAlignment(VAlign_Center);
		}
		else
		{
			TimeSection->AddChild(ClockText);
		}
	}

	// THE SLOT THAT WAS RESERVED. It held a sized USpacer while UScenario::StartingBalance was
	// authored and consumed by nobody; the spacer's comment said a readout would wait for the
	// ledger, and this is it. The Fill spacer STAYS, pushing the balance to the right-hand end
	// of the strip - it was always doing two jobs, and only one of them has been taken over.
	if (UHorizontalBox* StatusBox = Cast<UHorizontalBox>(StatusRow))
	{
		USpacer* Gap = WidgetTree->ConstructWidget<USpacer>(USpacer::StaticClass(), TEXT("LedgerSlot"));
		StatusBox->AddChildToHorizontalBox(Gap)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));

		if (BalanceText == nullptr)
		{
			BalanceText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(),
				TEXT("BalanceText"));
			// THE CLOCK'S ROLE, not Label: the balance is the other number the player watches
			// without looking for it, and a smaller one beside the clock would read as a
			// caption rather than as a readout.
			Style->ApplyText(*BalanceText, EUITextRole::Clock, Style->Text);
			UHorizontalBoxSlot* MoneySlot = StatusBox->AddChildToHorizontalBox(BalanceText);
			MoneySlot->SetPadding(FMargin(0.0f, 0.0f, 14.0f, 0.0f));
			MoneySlot->SetVerticalAlignment(VAlign_Center);
		}
	}
}

UPanelWidget* UBuildBarWidget::SectionPanel(EActionSection Section) const
{
	for (const FSectionSpec& Spec : SectionSpecs)
	{
		if (Spec.Section == Section)
		{
			return this->*Spec.Slot;
		}
	}
	return nullptr;
}

void UBuildBarWidget::BuildButtons(const UUIStyle* Style)
{
	const TConstArrayView<FBuildAction> Actions = BuildActions();
	int32 WithIcon = 0;
	for (int32 Index = 0; Index < Actions.Num(); ++Index)
	{
		const FBuildAction& Action = Actions[Index];
		UPanelWidget* Panel = SectionPanel(Action.Section);
		if (Panel == nullptr)
		{
			continue;
		}

		UBuildBarEntry* Entry = NewObject<UBuildBarEntry>(this);
		Entry->ActionIndex = Index;
		Entry->Owner = this;
		Entry->Button = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass());
		Entry->Label = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());

		UVerticalBox* Stack = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());

		// The time controls carry no texture, deliberately: slower, pause and faster are
		// geometric glyphs that render exactly as text. IconFor returns null for them and
		// AirportMgr.UI.EveryActionResolvesAnIcon exempts them BY SECTION, so a fourth time
		// control needs neither an icon nor that test edited.
		if (UTexture2D* Icon = Style->IconFor(Action.Id))
		{
			UImage* Image = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass());
			Image->SetBrushFromTexture(Icon, false);
			Image->SetDesiredSizeOverride(FVector2D(Style->ButtonSize * 0.5f));
			// The glyph is white with a transparent ground, so the tint IS the icon colour.
			Image->SetColorAndOpacity(Style->Text);
			Entry->Icon = Image;
			Stack->AddChildToVerticalBox(Image)->SetHorizontalAlignment(HAlign_Center);
			++WithIcon;
		}

		// THE LABEL IS NOW THE LABEL. The key used to be appended here - "Taxiway (1)" -
		// which is most of what made the bar read as a debug menu. It moves to the tooltip,
		// where it still teaches the shortcut without shouting it on every button forever.
		Entry->Label->SetText(Action.Label);
		Style->ApplyText(*Entry->Label, EUITextRole::Label, Style->Text);
		Stack->AddChildToVerticalBox(Entry->Label)->SetHorizontalAlignment(HAlign_Center);

		Entry->Button->SetContent(Stack);
		Entry->Button->SetBackgroundColor(Style->Button);
		Entry->Button->OnClicked.AddDynamic(Entry, &UBuildBarEntry::HandleClicked);

		if (Action.Key.IsValid())
		{
			Entry->Button->SetToolTipText(FText::FromString(FString::Printf(TEXT("%s  (%s%s)"),
				*Action.Label.ToString(), Action.bRequiresCtrl ? TEXT("Ctrl+") : TEXT(""),
				*Action.Key.GetDisplayName().ToString())));
		}
		else
		{
			Entry->Button->SetToolTipText(Action.Label);
		}

		if (UHorizontalBox* Box = Cast<UHorizontalBox>(Panel))
		{
			UHorizontalBoxSlot* ButtonSlot = Box->AddChildToHorizontalBox(Entry->Button);
			ButtonSlot->SetPadding(FMargin(3.0f, 0.0f));
			ButtonSlot->SetVerticalAlignment(VAlign_Fill);
		}
		else
		{
			Panel->AddChild(Entry->Button);
		}
		Entries.Add(Entry);
	}

	// The icon count is logged beside the button count on purpose: an unmapped action draws
	// a label with a hole above it, which is easy to miss on a screenshot and impossible to
	// miss here. A log line is this project's primary diagnostic.
	UE_LOG(LogBuildBar, Log, TEXT("Build bar: %d buttons from %d actions, %d with icons"),
		Entries.Num(), Actions.Num(), WithIcon);
}

void UBuildBarWidget::RunAction(int32 ActionIndex)
{
	ARoadBuildController* C = Controller();
	const TConstArrayView<FBuildAction> Actions = BuildActions();
	if (C == nullptr || !Actions.IsValidIndex(ActionIndex))
	{
		UE_LOG(LogBuildBar, Warning, TEXT("Bar click %d ignored: no controller or no such action"), ActionIndex);
		return;
	}
	Actions[ActionIndex].TryRun(*C, TEXT("Bar"));
}

void UBuildBarWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	RefreshState();
	RefreshClock();
	RefreshBalance();
}

void UBuildBarWidget::RefreshState()
{
	// NON-CONST, though this method never mutates the controller itself: Controller() already
	// returns a mutable pointer, and Action.IsEnabled/IsActive now build an FBuildActionContext
	// from it (issue #191), which holds a non-const ARoadBuildController& because Execute
	// shares the same field and does mutate. A const C here would refuse to bind to that.
	ARoadBuildController* C = Controller();
	if (C == nullptr)
	{
		return;
	}
	RefreshStateFor(*C);
}

void UBuildBarWidget::RefreshStateFor(ARoadBuildController& C)
{
	// PanelStyle is the base class's (issue #187): this used to call ResolveStyle() - a
	// TSoftObjectPtr::LoadSynchronous - here AND in RefreshBalance below, twice a tick, for a
	// style that cannot change once BuildOnce has resolved it.
	const UUIStyle* Style = PanelStyle;
	const TConstArrayView<FBuildAction> Actions = BuildActions();

	// ONE CONTEXT FOR THE WHOLE TICK (issue #309, FBuildActionContext::ConstructCountForTest
	// pins the delta at 1): `Action.IsEnabled(*C)` / `Action.IsActive(*C)` used to convert `*C`
	// through FBuildActionContext's own constructor - a GetSubsystem lookup - IMPLICITLY, once
	// per call, ~70 times a tick over every entry's two questions. Making that constructor
	// explicit (BuildActions.h) is what turned the silent conversion into a compile error here,
	// which is how this loop found out it needed to build the context itself, once, instead.
	FBuildActionContext Ctx(C);
	for (UBuildBarEntry* Entry : Entries)
	{
		if (Entry == nullptr || Entry->Button == nullptr || !Actions.IsValidIndex(Entry->ActionIndex))
		{
			continue;
		}
		const FBuildAction& Action = Actions[Entry->ActionIndex];
		const bool bEnabled = Action.IsEnabled(Ctx);
		const bool bActive = bEnabled && Action.IsActive(Ctx);
		Entry->Button->SetIsEnabled(bEnabled);

		// ACCENT MEANS ARMED AND NOTHING ELSE. If a second thing takes it, the player loses
		// the one glance that says which tool is live - which is the whole job the colour has.
		Entry->Button->SetBackgroundColor(bActive ? Style->Accent : Style->Button);

		// Icon and label follow the button, not the other way round: on the accent the
		// cream glyph would disappear, so the armed button draws its contents in PanelDark.
		const FLinearColor Content = bActive ? Style->PanelDark : (bEnabled ? Style->Text : Style->TextMuted);
		Entry->Label->SetColorAndOpacity(FSlateColor(Content));
		if (Entry->Icon != nullptr)
		{
			Entry->Icon->SetColorAndOpacity(Content);
		}
	}

}

void UBuildBarWidget::RefreshClock()
{
	if (ClockText == nullptr)
	{
		return;
	}
	const UOpsRuntime* Runtime = UOpsRuntimeSubsystem::Get(GetWorld());
	FString Text;
	if (Runtime == nullptr)
	{
		// LOCTEXT, not TEXT(): a fallback readout is still a word the player reads (issue #192)
		// - the composed clock sentence below stays TEXT()/Printf because it is numbers and
		// symbols (day/hour/minute/speed), not language, once this fallback no longer applies.
		Text = NSLOCTEXT("AirportMgr", "BarNoClock", "no clock").ToString();
	}
	else
	{
		const USimClock* Clock = Runtime->GetClock();
		const int32 Hour = static_cast<int32>(Clock->TimeOfDay() / 3600.0);
		const int32 Minute = static_cast<int32>(FMath::Fmod(Clock->TimeOfDay(), 3600.0) / 60.0);
		Text = FString::Printf(TEXT("Day %d  %02d:%02d  x%.0f%s"),
			Clock->Day() + 1, Hour, Minute, USimClock::Multiplier(Clock->GetSpeed()),
			Clock->GetSpeed() == ESimSpeed::Paused ? TEXT("  PAUSED") : TEXT(""));
	}

	// THE GATE. See LastClockText's own comment for why the composed sentence, not a minute
	// key, is what gets compared - SetText has no early-out of its own (UTextBlock.cpp), so an
	// unpaused clock sitting on the same displayed minute (the common case, 59 times out of 60)
	// used to re-invalidate this text block's layout every tick for an unchanged string.
	if (Text == LastClockText)
	{
		return;
	}
	LastClockText = Text;
	ClockText->SetText(FText::FromString(Text));
	++SetTextCalls;
}

void UBuildBarWidget::RefreshBalance()
{
	if (BalanceText == nullptr)
	{
		return;
	}

	const UOpsRuntime* Runtime = UOpsRuntimeSubsystem::Get(GetWorld());
	const ULedger* Ledger = Runtime != nullptr ? Runtime->GetLedger() : nullptr;
	const UPricing* Pricing = Runtime != nullptr ? Runtime->GetPricing() : nullptr;
	if (Ledger == nullptr || Pricing == nullptr)
	{
		if (!bLastBalanceWasFallback)
		{
			// SAME SHAPE AS RefreshClock's "no clock": an empty readout would look like a
			// balance of nothing, which is a very different thing from no game running.
			// LOCTEXT, not TEXT() (issue #192).
			BalanceText->SetText(NSLOCTEXT("AirportMgr", "BarNoLedger", "no ledger"));
			++SetTextCalls;
			bLastBalanceWasFallback = true;
		}
		return;
	}

	// THE GATE: the same Revision() idiom ULedgerPanelWidget::Refresh already uses - the
	// cheapest question the ledger can answer, so this skips Pricing->Format (money formatting)
	// and the FString::Printf below, not only the SetText that used to run unconditionally
	// every tick. THE FEE MULTIPLIER JOINS THE KEY, because StepLandingFee changes what this
	// line reads without posting to the ledger - Revision alone would leave the fee stale for
	// up to a minute after the player moved the lever.
	const int32 Revision = Ledger->Revision();
	const double FeeMultiplier = Pricing->LandingFeeMultiplier;
	if (!bLastBalanceWasFallback && Revision == LastLedgerRevision && FeeMultiplier == LastFeeMultiplier)
	{
		return;
	}
	bLastBalanceWasFallback = false;
	LastLedgerRevision = Revision;
	LastFeeMultiplier = FeeMultiplier;

	// THE FEE BESIDE THE MONEY, because the lever only means anything next to what it earns -
	// a percentage on its own tells the player nothing about whether to move it.
	const FText Balance = Pricing->Format(Ledger->Balance());
	BalanceText->SetText(FText::FromString(FString::Printf(TEXT("%s   fee %.0f%%"),
		*Balance.ToString(), FeeMultiplier * 100.0)));
	++SetTextCalls;

	// RED BELOW ZERO, through the style's semantic slot rather than a literal colour - see
	// UUIStyle. A negative balance locks placement, so it has to be visible without reading.
	// PanelStyle is the base class's (issue #187), never null once BuildOnce has run.
	if (PanelStyle != nullptr)
	{
		BalanceText->SetColorAndOpacity(FSlateColor(
			Ledger->Balance() < 0.0 ? PanelStyle->Warning : PanelStyle->Text));
	}
}

int32 UBuildBarWidget::ButtonCountForTest(EActionSection Section) const
{
	const TConstArrayView<FBuildAction> Actions = BuildActions();
	int32 Count = 0;
	for (const UBuildBarEntry* Entry : Entries)
	{
		if (Entry != nullptr && Actions.IsValidIndex(Entry->ActionIndex) && Actions[Entry->ActionIndex].Section == Section)
		{
			++Count;
		}
	}
	return Count;
}

FVector2D UBuildBarWidget::SectionRowSizeForTest(float AvailableWidth) const
{
	if (SectionRow == nullptr)
	{
		return FVector2D::ZeroVector;
	}

	if (UWrapBox* Wrapping = Cast<UWrapBox>(SectionRow))
	{
		Wrapping->SetExplicitWrapSize(true);
		Wrapping->SetWrapSize(AvailableWidth);
	}

	const TSharedRef<SWidget> Slate = SectionRow->TakeWidget();
	Slate->MarkPrepassAsDirty();
	Slate->SlatePrepass(1.0f);
	return FVector2D(Slate->GetDesiredSize());
}

float UBuildBarWidget::BarReservedHeightForTest(float AvailableWidth) const
{
	// Force the wrap width first: the height being asked about is a CONSEQUENCE of how many
	// lines that width produces.
	SectionRowSizeForTest(AvailableWidth);

	const UWidget* Bar = WidgetTree != nullptr ? WidgetTree->FindWidget(TEXT("BarBorder")) : nullptr;
	const UCanvasPanelSlot* BarSlot = Bar != nullptr ? Cast<UCanvasPanelSlot>(Bar->Slot) : nullptr;
	if (BarSlot == nullptr)
	{
		return 0.0f;
	}

	// AUTO-SIZED means the canvas hands over whatever the content asks for; otherwise the
	// bottom offset IS the height, and content taller than it is simply cut off.
	if (BarSlot->GetAutoSize())
	{
		const TSharedRef<SWidget> Slate = const_cast<UWidget*>(Bar)->TakeWidget();
		Slate->MarkPrepassAsDirty();
		Slate->SlatePrepass(1.0f);
		return static_cast<float>(Slate->GetDesiredSize().Y);
	}
	return BarSlot->GetOffsets().Bottom;
}

bool UBuildBarWidget::HasRootWidgetForTest() const
{
	return WidgetTree != nullptr && WidgetTree->RootWidget != nullptr;
}
