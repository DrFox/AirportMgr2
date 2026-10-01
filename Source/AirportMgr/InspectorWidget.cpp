#include "InspectorWidget.h"

#include "Blueprint/WidgetTree.h"
#include "BuildActions.h"
#include "BuildBarWidget.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/PanelWidget.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "InspectorFacilityRows.h"
#include "InspectorLog.h"
#include "Model/AgentRescue.h"
#include "Model/FlightBoard.h"
#include "Model/GroundTraffic.h"
#include "Model/OpsAlerts.h"
#include "Model/SimClock.h"
#include "Model/RoadAgent.h"
#include "Model/RoadNetwork.h"
#include "Present/OpsRuntime.h"
#include "Present/RoadNetworkActor.h"
#include "RoadBuildController.h"
#include "RoadBuildLog.h"
#include "UI/UiButton.h"
#include "UIStyle.h"

DEFINE_LOG_CATEGORY(LogInspector);

void UInspectorWidget::BuildOnce(const UUIStyle& Style)
{
	// PanelStyle is the BASE class's now (issue #187) - UAirportMgrPanelWidget::Initialize
	// sets it before calling this, from the same resolve BuildOnce's own parameter already is.

	EnsureSlots(&Style);
	if (DepartButton != nullptr) { DepartButton->OnClicked.AddDynamic(this, &UInspectorWidget::HandleDepart); }
	if (FollowButton != nullptr) { FollowButton->OnClicked.AddDynamic(this, &UInspectorWidget::HandleFollow); }
	if (RunwayButton != nullptr) { RunwayButton->OnClicked.AddDynamic(this, &UInspectorWidget::HandleRunway); }
	if (RunwayUseButton != nullptr) { RunwayUseButton->OnClicked.AddDynamic(this, &UInspectorWidget::HandleRunwayUse); }
	if (WaitingForButton != nullptr) { WaitingForButton->OnClicked.AddDynamic(this, &UInspectorWidget::HandleWaitingFor); }
	if (UnstickMenu != nullptr)
	{
		// WEAK, not this: a lambda held by a child widget that captured a raw pointer to its owner is
		// the shape that dangles the first time either is rebuilt.
		TWeakObjectPtr<UInspectorWidget> Weak(this);
		UnstickMenu->Items = [Weak]() { return Weak.IsValid() ? Weak->UnstickItems() : TArray<FUiMenuItem>(); };
		UnstickMenu->OnChosen.AddDynamic(this, &UInspectorWidget::HandleUnstickChosen);
	}
	// SelfHitTestInvisible, not Collapsed: see UAirportMgrPanelWidget::BuildOnce. The WINDOW
	// hides (SetShown); the root stays laid out.
	SetVisibility(ESlateVisibility::SelfHitTestInvisible);
	SetShown(false);
}

void UInspectorWidget::EnsureSlots(const UUIStyle* Style)
{
	// FOUND ONCE, POSITIONALLY. BuildActions.cpp's own comment says these two rows exist so
	// the panel, the bar and the C key are one list (spec §6.2); walking the Selection section
	// in the order it is built - depart, then follow - is what lets this panel ask for "its"
	// two verbs without retyping their ids (issue #91).
	const TConstArrayView<FBuildAction> Actions = BuildActions();
	int32 SelectionSeen = 0;
	for (int32 Index = 0; Index < Actions.Num(); ++Index)
	{
		if (Actions[Index].Section != EActionSection::Selection) { continue; }
		if (Actions[Index].Id == FName(TEXT("selection.runway_in_use")))
		{
			// By id - see RunwayActionIndex. Not counted in SelectionSeen, so it cannot shift
			// the positional pair whichever side of them it is registered.
			RunwayActionIndex = Index;
			continue;
		}
		if (Actions[Index].Id == FName(TEXT("selection.runway_use")))
		{
			// By id, the runway row's reason.
			RunwayUseActionIndex = Index;
			continue;
		}
		if (Actions[Index].Id == FName(TEXT("selection.unstick")))
		{
			// By id, the runway row's reason.
			UnstickActionIndex = Index;
			continue;
		}
		// THE PURCHASE ROWS' THREE, found by id in UInspectorFacilityRows::Build: skipped here for the runway row's reason -
		// counted in SelectionSeen they would shift the positional Depart/Follow pair.
		if (UInspectorFacilityRows::OwnsAction(Actions[Index].Id)) { continue; }
		if (SelectionSeen == 0) { DepartActionIndex = Index; }
		else if (SelectionSeen == 1) { FollowActionIndex = Index; }
		++SelectionSeen;
	}

	// Code-built content only where the asset gave none - the same root the offer inbox uses
	// (EnsureContentRoot, issue #90): title, facts, status, then the two verbs in a row. Where it
	// sits - bottom-left, riding the bar - is its window's (WantsWindow, UUiWindowHost::DockAbove).
	UVerticalBox* Column = Cast<UVerticalBox>(EnsureContentRoot(TEXT("InspectorCard")));
	if (Column != nullptr)
	{
		UE_LOG(LogInspector, Log, TEXT("No inspector asset: building the code-only panel"));
	}

	auto Text = [&](TObjectPtr<UTextBlock>& Field, const TCHAR* Name, EUITextRole Role, FLinearColor Colour)
	{
		if (Field != nullptr) { return; }
		Field = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), Name);
		Style->ApplyText(*Field, Role, Colour);
		Field->SetAutoWrapText(true);
		Field->SetMinDesiredWidth(static_cast<float>(PanelWidth));
		if (Column != nullptr) { Column->AddChildToVerticalBox(Field)->SetPadding(FMargin(0.0f, 2.0f)); }
	};
	Text(TitleText, TEXT("TitleText"), EUITextRole::Title, Style->Ink);
	// UNDER THE TITLE, in the style's one warning colour: a deadlock is the card's most urgent fact.
	Text(DeadlockText, TEXT("DeadlockText"), EUITextRole::Body, Style->Warning);
	Text(FactsText, TEXT("FactsText"), EUITextRole::Body, Style->InkMuted);
	Text(StatusText, TEXT("StatusText"), EUITextRole::Body, Style->InkMuted);

	UHorizontalBox* Row = nullptr;
	if (Column != nullptr && (DepartButton == nullptr || FollowButton == nullptr || RunwayButton == nullptr
		|| RunwayUseButton == nullptr
		|| UnstickMenu == nullptr || WaitingForButton == nullptr))
	{
		Row = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("InspectorVerbs"));
		Column->AddChildToVerticalBox(Row)->SetPadding(FMargin(0.0f, 8.0f, 0.0f, 0.0f));
	}

	// LABEL FROM Action.Label, KEY IN THE TOOLTIP - the bar's own convention
	// (UBuildBarWidget::BuildButtons), so a verb reads the same wherever it appears.
	auto Button = [&](TObjectPtr<UUiButton>& Field, const TCHAR* Name, int32 ActionIndex)
	{
		if (Field != nullptr || !Actions.IsValidIndex(ActionIndex)) { return; }
		const FBuildAction& Action = Actions[ActionIndex];
		Field = WidgetTree->ConstructWidget<UUiButton>(UUiButton::StaticClass(), Name);
		Field->SetLabel(Action.Label);
		Field->Build(*Style, EUiButtonKind::Secondary);
		if (Action.Key.IsValid())
		{
			Field->SetToolTipText(FText::FromString(FString::Printf(TEXT("%s  (%s%s)"),
				*Action.Label.ToString(), Action.bRequiresCtrl ? TEXT("Ctrl+") : TEXT(""),
				*Action.Key.GetDisplayName().ToString())));
		}
		else
		{
			Field->SetToolTipText(Action.Label);
		}
		if (Row != nullptr) { Row->AddChildToHorizontalBox(Field)->SetPadding(FMargin(0.0f, 0.0f, 8.0f, 0.0f)); }
	};
	// The caption lives in the button (UUiButton::GetLabel): PaintVerbs recolours it through
	// SetState, and ShowFollowing retitles it, without either holding a second pointer.
	Button(DepartButton, TEXT("DepartButton"), DepartActionIndex);
	Button(FollowButton, TEXT("FollowButton"), FollowActionIndex);
	Button(RunwayButton, TEXT("RunwayButton"), RunwayActionIndex);
	Button(RunwayUseButton, TEXT("RunwayUseButton"), RunwayUseActionIndex);
	if (!Actions.IsValidIndex(RunwayUseActionIndex))
	{
		UE_LOG(LogInspector, Warning, TEXT("No selection.runway_use row in BuildActions(): the runway card has no mode button"));
	}
	if (!Actions.IsValidIndex(RunwayActionIndex))
	{
		UE_LOG(LogInspector, Warning, TEXT("No selection.runway_in_use row in BuildActions(): the runway card has no button"));
	}

	// THE UNSTICK POPUP, captioned by its row like every other verb here. Its own button opens it
	// (UUiMenuButton); the bar's row reaches it through the controller's request count (TickPanel).
	if (UnstickMenu == nullptr && Actions.IsValidIndex(UnstickActionIndex))
	{
		UnstickMenu = WidgetTree->ConstructWidget<UUiMenuButton>(UUiMenuButton::StaticClass(), TEXT("UnstickMenu"));
		UnstickMenu->Build(*Style, Actions[UnstickActionIndex].Label);
		UnstickMenu->SetToolTipText(NSLOCTEXT("AirportMgr", "InspectorUnstickTip", "Replan, send home or despawn a stuck agent"));
		if (Row != nullptr) { Row->AddChildToHorizontalBox(UnstickMenu)->SetPadding(FMargin(0.0f, 0.0f, 8.0f, 0.0f)); }
	}
	if (!Actions.IsValidIndex(UnstickActionIndex))
	{
		UE_LOG(LogInspector, Warning, TEXT("No selection.unstick row in BuildActions(): the agent card has no Unstick"));
	}

	// THE WAITED-FOR VERB - no BuildActions row (see WaitingForButton), so captioned here and retitled
	// per card in PaintVerbs.
	if (WaitingForButton == nullptr)
	{
		WaitingForButton = WidgetTree->ConstructWidget<UUiButton>(UUiButton::StaticClass(), TEXT("WaitingForButton"));
		WaitingForButton->SetLabel(NSLOCTEXT("AirportMgr", "InspectorShowBlocker", "Show"));
		WaitingForButton->Build(*Style, EUiButtonKind::Secondary);
		WaitingForButton->SetToolTipText(NSLOCTEXT("AirportMgr", "InspectorShowBlockerTip", "Select what this is waiting for"));
		if (Row != nullptr) { Row->AddChildToHorizontalBox(WaitingForButton)->SetPadding(FMargin(0.0f, 0.0f, 8.0f, 0.0f)); }
	}

	// THE PURCHASE ROWS, a sub-widget of their own (issue #441): built empty, and filled from the depot card's quote (PaintView).
	// Only into a column that exists: with an asset's content and no code-built column, a widget built here would be parented
	// nowhere and never drawn - a designer places one under the name FacilityRows instead, and it is built all the same.
	if (FacilityRows == nullptr && Column != nullptr)
	{
		FacilityRows = WidgetTree->ConstructWidget<UInspectorFacilityRows>(UInspectorFacilityRows::StaticClass(), TEXT("FacilityRows"));
		Column->AddChildToVerticalBox(FacilityRows);
	}
	if (FacilityRows != nullptr && !FacilityRows->IsBuilt())
	{
		// WEAK - UnstickMenu's reason (BuildOnce).
		TWeakObjectPtr<UInspectorWidget> WeakSelf(this);
		FacilityRows->RunActionSource = [WeakSelf](int32 ActionIndex, const FBuildActionArg& Arg) { if (WeakSelf.IsValid()) { WeakSelf->RunActionWith(ActionIndex, Arg); } };
		FacilityRows->Build(*Style);
	}
}

bool UInspectorWidget::WantsWindow(FUiWindowSpec& Out) const
{
	Out.Id = TEXT("inspector");
	Out.Title = NSLOCTEXT("AirportMgr", "InspectorWindow", "Inspector");
	Out.Anchor = EUiWindowAnchor::AboveBarLeft;
	Out.Offset = FVector2D(12.0, BarGap);
	return true;
}

void UInspectorWidget::TickPanel(float InDeltaTime)
{
	// The dock above the bar is the host's now (UUiWindowHost::TickWindows), not this tick's.
	if (const ARoadBuildController* C = Controller())
	{
		// The controller already computed this frame's FAgentFacts for the bar's
		// selection.depart row (ARoadBuildController::SelectedAgentFactsThisFrame) - passed
		// through rather than asked for a second time (issue #187).
		FAgentFacts Facts;
		const bool bHaveFacts = C->SelectedAgentFactsThisFrame(Facts);
		Refresh(C->GetTarget(), C->GetSelection(), bHaveFacts ? &Facts : nullptr);
		ShowFollowing(C->IsWatchingAgent());
	}
}

void UInspectorWidget::OpenUnstickMenu()
{
	// THE BAR'S Unstick, arriving - UBuildHudLayer::OpenUnstickMenu calls this from the row's Execute. Opened only on an
	// agent card; a request made with nothing to unstick is spent, not kept for the next card.
	if (UnstickMenu != nullptr && UnstickMenu->GetVisibility() == ESlateVisibility::Visible && IsShown())
	{
		++UnstickOpens;
		UnstickMenu->Open();
		return;
	}
	// SAID, NOT SWALLOWED: "Unstick did nothing" must have a line to grep. The press is spent, not kept for the next card (see above).
	UE_LOG(LogInspector, Log, TEXT("Unstick menu: press spent - %s"),
		UnstickMenu == nullptr ? TEXT("this inspector has no popup")
		: !IsShown() ? TEXT("the inspector is closed") : TEXT("no agent card is showing"));
}

void UInspectorWidget::ShowFollowing(bool bFollowing)
{
	// FOUND THROUGH THE BUTTON, not held: UUiButton owns its caption, and a button built with
	// no label (a Blueprint's) simply has none to retitle.
	UTextBlock* Caption = FollowButton != nullptr ? FollowButton->GetLabel() : nullptr;
	const TConstArrayView<FBuildAction> Actions = BuildActions();
	if (Caption == nullptr || !Actions.IsValidIndex(FollowActionIndex))
	{
		return;
	}
	// The resting word stays the action's Label, so the bar and this button still say the
	// same thing whenever nothing is being followed.
	const FText Wanted = bFollowing
		? NSLOCTEXT("AirportMgr", "InspectorUnfollow", "Unfollow")
		: Actions[FollowActionIndex].Label;
	// Compared first: SetText has no early-out of its own (see PaintTexts' gate), and this runs
	// every tick.
	if (!Caption->GetText().EqualTo(Wanted))
	{
		Caption->SetText(Wanted);
	}
}

FString UInspectorWidget::FollowCaptionForTest() const
{
	const UTextBlock* Caption = FollowButton != nullptr ? FollowButton->GetLabel() : nullptr;
	return Caption != nullptr ? Caption->GetText().ToString() : FString();
}

void UInspectorWidget::Refresh(const ARoadNetworkActor* Target, const FSelection& Selection,
	const FAgentFacts* PrecomputedAgentFacts)
{
	// THE OPS RUNTIME, found here and handed down - see RefreshWith. OpsRuntime(), the controller's (the
	// runtime its verbs act on) else the subsystem's: ONE runtime for every line of the card, the depot's
	// purchase rows and the aircraft's fuel line alike.
	RefreshWith(OpsRuntime(), Target, Selection, PrecomputedAgentFacts);
}

void UInspectorWidget::RefreshWith(const UOpsRuntime* Runtime, const ARoadNetworkActor* Target,
	const FSelection& Selection, const FAgentFacts* PrecomputedAgentFacts)
{
	// ONE RESET, before any early return: only the aircraft card sets it again, so every other
	// path - no selection, a gone agent, another kind of card - leaves Show with nothing to select.
	WaitedForId = 0;
	if (Target == nullptr || !Selection.IsSet())
	{
		// NOTHING SELECTED: hidden. The sale armed on the card that has just gone is disarmed by the selection EVENT
		// (HandleSelectionChanged), not by this paint noticing the selection is gone - the window is all Refresh owns here.
		SetShown(false);
		bDepartEnabled = false;
		return;
	}

	// THE CARD FOR THE SELECTION, from the table: one card per kind, its own describe and its own change key (issue #441).
	IInspectorCard* Card = Cards.Find(FInspectorCards::CardFor(Selection, Target->GetNetwork()));
	if (Card == nullptr)
	{
		// A KIND WITH NO CARD - only a value outside the enum reaches here now: an appended kind
		// fails the static_assert in FInspectorCards::CardFor until it has a case. It used to fall into the stand
		// branch and describe ENTITY Id; now it says so ONCE per selection (Refresh runs every
		// tick - bWarnedNoCard, re-armed by the selection event) and shows nothing.
		// ENFORCED BY: the static_assert on ESelectionKind::Count in FInspectorCards::CardFor;
		// AirportMgr.Inspector.UnknownKindWarnsOnce (once per selection, not per tick)
		if (!bWarnedNoCard)
		{
			bWarnedNoCard = true;
			UE_LOG(LogInspector, Warning, TEXT("Inspector: no card for selection kind %d"), static_cast<int32>(Selection.Kind));
		}
		HideCard();
		return;
	}
	FInspectorCardInput In;
	In.Runtime = Runtime;
	In.Target = Target;
	In.Selection = Selection;
	In.PrecomputedAgentFacts = PrecomputedAgentFacts;
	In.Flights = Flights(Runtime);
	In.Clock = GameClock(Runtime);
	const FInspectorCardView* View = Card->Describe(In);
	if (View == nullptr)
	{
		// A gone agent, a dead entity, a facts call that failed: nothing to show, and nothing left lit.
		HideCard();
		return;
	}
	PaintView(*View, Selection, *Target);
}

void UInspectorWidget::HandleSelectionChanged(const FSelection& Old, const FSelection& New)
{
	// THE EVENT'S WORK, once per real change (#446) - see the header. The warning about a kind with no card is said again for the new one.
	bWarnedNoCard = false;
	if (!New.IsSet())
	{
		// NOTHING SELECTED DISARMS TOO - a sale armed and then clicked away from must not survive to the
		// next time this depot is picked.
		if (Old.IsSet() && FacilityRows != nullptr) { FacilityRows->DisarmSale(); }
		return;
	}
	OnNewSelection();
}

void UInspectorWidget::OnNewSelection()
{
	// A NEW SELECTION REOPENS A WINDOW THE PLAYER CLOSED: the close meant "not this one", and
	// clicking another aircraft is asking to see it (Review Focus 4 of the step 2 plan).
	ForgetPlayerClose();
	// A POPUP FOR THE OLD AGENT closes with its card: its lines were asked of that agent, and a
	// confirm armed for one aeroplane must not despawn the next one clicked.
	if (UnstickMenu != nullptr) { UnstickMenu->Close(); }
	// The purchase rows' own (its buy menu closes, its fleet rows rebuild, any armed sale goes). AN INSPECTOR WITH NO ROWS has nothing
	// armed to forget: the armed sale lives in the rows alone now (#448) - the controller held an id too until the id travelled with
	// the run, and this used to reach past the rows to clear it.
	if (FacilityRows != nullptr) { FacilityRows->OnNewCard(); }
}

void UInspectorWidget::HideCard()
{
	SetShown(false);
	bDepartEnabled = false;
}

void UInspectorWidget::PaintView(const FInspectorCardView& View, const FSelection& Selection, const ARoadNetworkActor& Target)
{
	WaitedForId = View.WaitedForId;
	bDepartEnabled = View.bCanDepart;
	PaintTexts(View);
	PaintVerbs(View, Selection, Target);
	// EVERY CARD SAYS whether it has purchase rows - a non-depot's default quote collapses them.
	if (FacilityRows != nullptr) { FacilityRows->Show(View.Quote); }
	SetShown(true);
}

void UInspectorWidget::PaintTexts(const FInspectorCardView& View)
{
	// THE GATE. Compared against the COMPOSED text rather than a (selection id, phase) key -
	// see LastTitle/LastFacts/LastStatus's own comment for why phase alone would freeze a
	// moving aircraft's numbers. SetText has no early-out of its own (UIStyle.cpp's own note
	// on why UBuildBarWidget gates its clock and balance the same way), so the common case -
	// nothing selected, or a parked aircraft awaiting dispatch - now sets no text at all.
	if (TitleText != nullptr && View.Title != LastTitle)
	{
		TitleText->SetText(FText::FromString(View.Title));
		LastTitle = View.Title;
		++SetTextCalls;
	}
	if (FactsText != nullptr && View.Facts != LastFacts)
	{
		FactsText->SetText(FText::FromString(View.Facts));
		LastFacts = View.Facts;
		++SetTextCalls;
	}
	if (StatusText != nullptr && View.Status != LastStatus)
	{
		StatusText->SetText(FText::FromString(View.Status));
		LastStatus = View.Status;
		++SetTextCalls;
	}
	if (DeadlockText != nullptr)
	{
		if (View.Deadlock != LastDeadlock)
		{
			DeadlockText->SetText(FText::FromString(View.Deadlock));
			LastDeadlock = View.Deadlock;
			++SetTextCalls;
		}
		// Collapsed, not an empty line: an empty row would still take its padding under the title.
		const ESlateVisibility Wanted = View.Deadlock.IsEmpty() ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible;
		if (DeadlockText->GetVisibility() != Wanted) { DeadlockText->SetVisibility(Wanted); }
	}
}

void UInspectorWidget::PaintVerbs(const FInspectorCardView& View, const FSelection& Selection, const ARoadNetworkActor& Target)
{
	// EXACTLY THE VERBS THE VIEW NAMES: per-kind button visibility is the card's to say (EInspectorVerbs), so a new card adds
	// no branch here.
	const auto Has = [&View](EInspectorVerbs Verb) { return EnumHasAllFlags(View.Verbs, Verb); };
	// Retitled through the button's own caption (UUiButton::GetLabel), the Follow button's way.
	const auto Caption = [](UUiButton& Button, bool bShown, const FText& Wanted)
	{
		Button.SetVisibility(bShown ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
		const UTextBlock* Current = Button.GetLabel();
		if (bShown && (Current == nullptr || !Current->GetText().EqualTo(Wanted)))
		{
			Button.SetLabel(Wanted);
		}
	};
	if (WaitingForButton != nullptr)
	{
		Caption(*WaitingForButton, Has(EInspectorVerbs::WaitingFor), View.WaitingForCaption);
	}
	if (DepartButton != nullptr)
	{
		DepartButton->SetVisibility(Has(EInspectorVerbs::Depart) ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);

		// THE BAR'S OWN RULE, now UUiButton::LookFor: the BUTTON stays Control always - disabled
		// dims a button by its ink, never by brightening the fill. Painting a disabled background
		// with the muted LABEL slot made Depart look MORE prominent while taxiing than while
		// parked, backwards from the intent. Only the CAPTION follows enabled state.
		DepartButton->SetState(bDepartEnabled, false);
	}
	if (FollowButton != nullptr)
	{
		FollowButton->SetVisibility(Has(EInspectorVerbs::Follow) ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	}
	if (UnstickMenu != nullptr)
	{
		const bool bUnstick = Has(EInspectorVerbs::Unstick);
		UnstickMenu->SetVisibility(bUnstick ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
		// LIT WHEN IT LOOKS STUCK - Selected is the Accent fill (UUiButton::LookFor), the one "look at
		// me" this style has. Read off the model agent: IsStuck - the one definition of stuck, Airside's (#429;
		// UAgentRescue::LooksStuck until then, a third spelling) - also asks Stranded, which
		// FAgentFacts::Hold (the stall clock only while waiting on someone) does not carry. Per tick and outside the
		// view on purpose: it moves with the stall clock, which no card key holds.
		const FRoadAgent* Agent = bUnstick && Target.GetGroundTraffic() != nullptr
			? Target.GetGroundTraffic()->FindAgent(Selection.Id) : nullptr;
		bUnstickHighlighted = Agent != nullptr && Agent->IsStuck(UnstickHighlightSeconds);
		if (UUiButton* B = UnstickMenu->GetButton())
		{
			B->SetState(true, bUnstickHighlighted);
		}
	}
	if (RunwayButton != nullptr)
	{
		Caption(*RunwayButton, Has(EInspectorVerbs::Runway), View.RunwayCaption);
	}
	if (RunwayUseButton != nullptr)
	{
		Caption(*RunwayUseButton, Has(EInspectorVerbs::RunwayUse), View.RunwayUseCaption);
	}
}

void UInspectorWidget::RunAction(int32 ActionIndex)
{
	RunActionWith(ActionIndex, FBuildActionArg());
}

void UInspectorWidget::RunActionWith(int32 ActionIndex, const FBuildActionArg& Arg)
{
	ARoadBuildController* C = Controller();
	if (C == nullptr)
	{
		UE_LOG(LogInspector, Warning, TEXT("Inspector click %d ignored: no controller"), ActionIndex);
		return;
	}
	const TConstArrayView<FBuildAction> Actions = BuildActions();
	if (!Actions.IsValidIndex(ActionIndex))
	{
		// Should not happen - EnsureSlots finds both indices from the registry itself - but a
		// missing row degrades to a no-op rather than a crash, same as the old by-id lookup.
		UE_LOG(LogInspector, Warning, TEXT("Inspector click %d ignored: no such action"), ActionIndex);
		return;
	}
	// TryRunWith for every run, an empty Arg included: a plain click is a run with no argument. A REFUSED RUN IS SAID: TryRun logs only
	// the runs that go ahead, so a click the gate turned away - a sale of a vehicle that just went busy, a buy the purse cannot cover -
	// would leave nothing in the log to grep, and "the button did nothing" is the report this project diagnoses from a log.
	if (!Actions[ActionIndex].TryRunWith(*C, Arg, TEXT("Inspector")))
	{
		UE_LOG(LogInspector, Log, TEXT("Inspector: %s refused%s"), *Actions[ActionIndex].Id.ToString(),
			Arg.IsSet() ? *(TEXT(" ") + Arg.Describe()) : TEXT(""));
	}
}

TArray<FUiMenuItem> UInspectorWidget::UnstickItems() const
{
	TArray<FUiMenuItem> Out;
	const ARoadBuildController* C = Controller();
	if (C == nullptr)
	{
		return Out;
	}
	const UGroundTraffic* Traffic = C->GetTarget() != nullptr ? C->GetTarget()->GetGroundTraffic() : nullptr;
	const FSelection& Selected = C->GetSelection();
	const FRoadAgent* Agent = Traffic != nullptr ? Traffic->FindAgent(Selected.Id) : nullptr;
	const bool bVehicle = Agent != nullptr && Agent->AsVehicle() != nullptr;
	// THE RUNTIME'S OWN VERDICT, asked of it with the selected agent (spec 2026-09-29-unstick-agent): the lines here and the action they
	// run are the one decision UAgentRescue makes, and this reads it from the runtime directly - not through a controller forwarder, which
	// existed only to hand the runtime a selection (#448). REFUSED "Nothing selected" with no agent selected or no runtime (a headless
	// test's world has none unless it stood one in).
	const UOpsRuntime* Runtime = OpsRuntime();

	// ENUM ORDER, one line per action - see UnstickItems' header.
	auto Line = [&](EUnstickAction Action, const FText& Label)
	{
		const FUnstickVerdict Verdict = Runtime != nullptr && Selected.Kind == ESelectionKind::Aircraft
			? Runtime->CanUnstick(Selected.Id, Action)
			: FUnstickVerdict::No(NSLOCTEXT("AirportMgr", "UnstickNothing", "Nothing selected"));
		FUiMenuItem& Item = Out.AddDefaulted_GetRef();
		Item.Label = Label;
		Item.bEnabled = Verdict.bAllowed;
		Item.Why = Verdict.Why;
		check(Out.Num() - 1 == static_cast<int32>(Action));
	};
	Line(EUnstickAction::Replan, NSLOCTEXT("AirportMgr", "UnstickReplan", "Replan"));
	Line(EUnstickAction::SendHome, bVehicle ? NSLOCTEXT("AirportMgr", "UnstickHome", "Send home")
		: NSLOCTEXT("AirportMgr", "UnstickStand", "Find a stand"));
	Line(EUnstickAction::Despawn, NSLOCTEXT("AirportMgr", "UnstickDespawn", "Despawn"));
	Out.Last().bConfirm = true;
	Out.Last().ConfirmLabel = NSLOCTEXT("AirportMgr", "UnstickDespawnConfirm", "Despawn - click to confirm");
	return Out;
}

void UInspectorWidget::HandleUnstickChosen(int32 Index)
{
	ARoadBuildController* C = Controller();
	if (C == nullptr || Index < 0 || Index > static_cast<int32>(EUnstickAction::Despawn))
	{
		UE_LOG(LogInspector, Warning, TEXT("Unstick line %d ignored: no controller or no such action"), Index);
		return;
	}
	// THE RUNTIME'S Unstick, with the selected agent - the verb bound to its owner (#448), as the lines above ask its CanUnstick.
	const EUnstickAction Action = static_cast<EUnstickAction>(Index);
	const FSelection& Selected = C->GetSelection();
	UOpsRuntime* Runtime = OpsRuntime();
	if (Selected.Kind != ESelectionKind::Aircraft || Runtime == nullptr)
	{
		UE_LOG(LogRoadBuild, Warning, TEXT("Unstick %s: no agent selected, or no ops runtime."), *UEnum::GetValueAsString(Action));
		return;
	}
	// UAgentRescue logs the "Unstick: agent N ... -> done|refused" line; this one says the click arrived.
	UE_LOG(LogRoadBuild, Log, TEXT("Unstick %s: agent %d"), *UEnum::GetValueAsString(Action), Selected.Id);
	Runtime->Unstick(Selected.Id, Action);
}

bool UInspectorWidget::ShowWaitedFor(ARoadBuildController& InController)
{
	if (WaitedForId == 0)
	{
		UE_LOG(LogInspector, Warning, TEXT("Inspector Show ignored: the card waits for nobody"));
		return false;
	}
	// THE CARD'S OWN LINE FIRST: SelectAndFocus logs as "Alert Go", and a grep for what the player
	// clicked must find the inspector, not an alert nobody pressed.
	UE_LOG(LogInspector, Log, TEXT("Inspector Show: agent %d waits for %d"), InController.GetSelection().Id, WaitedForId);
	// THE ALERT GO'S PATH, not a second selection mechanism: it leaves a build tool, moves the
	// camera, selects as the select tool would, and logs "Alert Go: ... -> ..." either way.
	FAlertFocus Focus;
	Focus.Kind = EAlertFocusKind::Agent;
	Focus.Id = WaitedForId;
	return InController.SelectAndFocus(Focus);
}

void UInspectorWidget::HandleWaitingFor()
{
	if (ARoadBuildController* C = Controller())
	{
		ShowWaitedFor(*C);
		return;
	}
	UE_LOG(LogInspector, Warning, TEXT("Inspector Show ignored: no controller"));
}

void UInspectorWidget::UseFlightBoardForTest(const UFlightBoard* Board) { FlightBoardForTest = Board; }

void UInspectorWidget::UseClockForTest(const USimClock* Clock) { ClockForTest = Clock; }

const USimClock* UInspectorWidget::GameClock(const UOpsRuntime* Runtime) const
{
	if (const USimClock* Clock = ClockForTest.Get())
	{
		return Clock;
	}
	return Runtime != nullptr ? Runtime->GetClock() : nullptr;
}

const UFlightBoard* UInspectorWidget::Flights(const UOpsRuntime* Runtime) const
{
	if (const UFlightBoard* Board = FlightBoardForTest.Get())
	{
		return Board;
	}
	return Runtime != nullptr ? Runtime->GetFlightBoard() : nullptr;
}

void UInspectorWidget::HandleDepart() { RunAction(DepartActionIndex); }
void UInspectorWidget::HandleFollow() { RunAction(FollowActionIndex); }
void UInspectorWidget::HandleRunway() { RunAction(RunwayActionIndex); }
void UInspectorWidget::HandleRunwayUse() { RunAction(RunwayUseActionIndex); }

bool UInspectorWidget::IsShownForTest() const { return IsShown(); }
bool UInspectorWidget::IsDepartEnabledForTest() const { return bDepartEnabled; }
FString UInspectorWidget::TitleForTest() const { return TitleText != nullptr ? TitleText->GetText().ToString() : FString(); }
FString UInspectorWidget::FactsForTest() const { return FactsText != nullptr ? FactsText->GetText().ToString() : FString(); }
FString UInspectorWidget::StatusForTest() const { return StatusText != nullptr ? StatusText->GetText().ToString() : FString(); }
FString UInspectorWidget::DeadlockForTest() const
{
	return DeadlockText != nullptr && DeadlockText->GetVisibility() != ESlateVisibility::Collapsed
		? DeadlockText->GetText().ToString() : FString();
}
FString UInspectorWidget::WaitingForCaptionForTest() const
{
	const UTextBlock* Caption = WaitingForButton != nullptr && WaitingForButton->GetVisibility() != ESlateVisibility::Collapsed
		? WaitingForButton->GetLabel() : nullptr;
	return Caption != nullptr ? Caption->GetText().ToString() : FString();
}
FLinearColor UInspectorWidget::DepartLabelColourForTest() const
{
	const UTextBlock* Caption = DepartButton != nullptr ? DepartButton->GetLabel() : nullptr;
	return Caption != nullptr ? Caption->GetColorAndOpacity().GetSpecifiedColor() : FLinearColor::Black;
}
