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
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Model/JobBoard.h"
#include "Model/FlightBoard.h"
#include "Model/Flight.h"
#include "ArrivalViewModels.h"
#include "Model/InspectFacts.h"
#include "Model/RoadAgent.h"
#include "Model/RoadNetwork.h"
#include "Present/OpsRuntime.h"
#include "Present/OpsRuntimeSubsystem.h"
#include "Present/RoadNetworkActor.h"
#include "RoadBuildController.h"
#include "UI/UiButton.h"
#include "UIStyle.h"

DEFINE_LOG_CATEGORY_STATIC(LogInspector, Log, All);

void UInspectorWidget::BuildOnce(const UUIStyle& Style)
{
	// PanelStyle is the BASE class's now (issue #187) - UAirportMgrPanelWidget::Initialize
	// sets it before calling this, from the same resolve BuildOnce's own parameter already is.

	EnsureSlots(&Style);
	if (DepartButton != nullptr) { DepartButton->OnClicked.AddDynamic(this, &UInspectorWidget::HandleDepart); }
	if (FollowButton != nullptr) { FollowButton->OnClicked.AddDynamic(this, &UInspectorWidget::HandleFollow); }
	if (RunwayButton != nullptr) { RunwayButton->OnClicked.AddDynamic(this, &UInspectorWidget::HandleRunway); }
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
	Text(FactsText, TEXT("FactsText"), EUITextRole::Body, Style->InkMuted);
	Text(StatusText, TEXT("StatusText"), EUITextRole::Body, Style->InkMuted);

	UHorizontalBox* Row = nullptr;
	if (Column != nullptr && (DepartButton == nullptr || FollowButton == nullptr || RunwayButton == nullptr))
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
	// The caption lives in the button (UUiButton::GetLabel): Refresh recolours it through
	// SetState, and ShowFollowing retitles it, without either holding a second pointer.
	Button(DepartButton, TEXT("DepartButton"), DepartActionIndex);
	Button(FollowButton, TEXT("FollowButton"), FollowActionIndex);
	Button(RunwayButton, TEXT("RunwayButton"), RunwayActionIndex);
	if (!Actions.IsValidIndex(RunwayActionIndex))
	{
		UE_LOG(LogInspector, Warning, TEXT("No selection.runway_in_use row in BuildActions(): the runway card has no button"));
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
	// Compared first: SetText has no early-out of its own (see Refresh's gate), and this runs
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
	if (Target == nullptr || !Selection.IsSet())
	{
		SetShown(false);
		bDepartEnabled = false;
		LastSelection = FSelection();
		return;
	}
	// A NEW SELECTION REOPENS A WINDOW THE PLAYER CLOSED: the close meant "not this one", and
	// clicking another aircraft is asking to see it (Review Focus 4 of the step 2 plan).
	const bool bNewSelection = Selection.Kind != LastSelection.Kind || Selection.Id != LastSelection.Id;
	if (bNewSelection)
	{
		ForgetPlayerClose();
		LastSelection = Selection;
	}

	// ONE CARD PER KIND, counted at compile time (review fix 3): the branches below are None
	// (handled above), Aircraft, Runway, Taxiway and Stand. Appending a kind to ESelectionKind
	// moves Count and stops this compiling until the kind gets its branch and this number.
	static_assert(static_cast<int32>(ESelectionKind::Count) == 5,
		"a new ESelectionKind needs an inspector card - add its branch below, then update this count");

	FString Title, Facts, Status;
	bool bAircraft = false;
	bool bRunway = false;
	FText RunwayCaption;
	if (Selection.Kind == ESelectionKind::Aircraft)
	{
		FAgentFacts F;
		bool bOk;
		if (PrecomputedAgentFacts != nullptr)
		{
			F = *PrecomputedAgentFacts;
			bOk = true;
		}
		else
		{
			bOk = Target->GetGroundTraffic() != nullptr
				&& InspectFacts::DescribeAgent(*Target->GetGroundTraffic(), Target->GetNetwork(), Selection.Id, F);
		}
		if (!bOk)
		{
			SetShown(false);
			bDepartEnabled = false;
			return;
		}
		bAircraft = true;
		bDepartEnabled = F.bCanDepart;

		// THE FUEL LINE, from the layer that knows what fuel is. Reached through the ops
		// subsystem rather than through Target, because the airport actor is Airside's and
		// must not carry a pointer to a service it is forbidden to know about - see
		// FAgentFacts::Fuel, the field this fills and DescribeAgent deliberately leaves empty.
		//
		// READ EVERY TICK, NOT GATED: this is the fuel service's own cheap lookup (a
		// FindByPredicate over active trucks), not the cost FInspectorKey targets below - its
		// RESULT is one of the key's fields, so it has to run before the key can be compared.
		if (const UOpsRuntime* Runtime = UOpsRuntimeSubsystem::Get(GetWorld()))
		{
			if (const UJobBoard* Fuel = Runtime->GetJobBoard())
			{
				F.Fuel = Fuel->DescribeAgent(F.Id, Runtime->GetClock() != nullptr ? Runtime->GetClock()->Now() : 0.0);
			}
			// THE CONTRACT, from the flight that owns this aircraft - its minute resolution keeps
			// the gate below from recomposing more than once a game minute.
			if (const UFlightBoard* Board = Runtime->GetFlightBoard())
			{
				if (const UFlight* Flight = Board->FlightForAgent(F.Id); Flight != nullptr && Runtime->GetClock() != nullptr)
				{
					F.Turnaround = UArrivalRowViewModel::DescribeTurnaround(*Flight, Runtime->GetClock()->Now()).ToString();
				}
			}
		}

		// MAGNITUDE. FAgentMotion::GroundSpeed became signed on 2026-09-20 so the view could
		// roll a reversing vehicle's wheels backwards, and a readout is not that view: an
		// aircraft on a pushback would otherwise report "-1.5 m/s (-3 kt)", which reads as a
		// fault rather than as a direction. Which way it is going is the Status line's job.
		const double Shown = FMath::Abs(F.GroundSpeed);

		// THE GATE, ONE LEVEL EARLIER THAN LastTitle/LastFacts/LastStatus (issue #309): built
		// from the FINEST rounding the Printf specifiers below use for each quantity, so two
		// facts that would compose to an identical sentence never fail this cheaper check first.
		// See FInspectorKey's own comment for why Phase holds F.Status rather than F.Phase, and
		// why speed keys on tenths of m/s rather than the coarser whole-knot figure also printed
		// below (PR #329 review: a change that moves the m/s decimal without moving the rounded
		// knot integer was composing nothing, leaving the m/s line stale).
		FInspectorKey Key;
		Key.Id = F.Id;
		Key.Phase = F.Status;
		Key.HeadingRounded = FMath::RoundToInt(F.HeadingDegrees);
		Key.SpeedTenthsRounded = FMath::RoundToInt(Shown / 100.0 * 10.0);
		Key.AltitudeRounded = FMath::RoundToInt(F.Altitude / 100.0);
		Key.Destination = F.Destination;
		Key.bEngineRunning = F.bEngineRunning;
		Key.Fuel = F.Fuel;
		Key.Pushback = F.Pushback;
		Key.Turnaround = F.Turnaround;

		if (Key != LastComposedKey)
		{
			++ComposeCalls;   // See ComposeCountForTest.
			LastComposedKey = Key;

			LastComposedTitle = FString::Printf(TEXT("%s  #%d"), *F.TypeName, F.Id);
			// LOCTEXT for the words, FString::Format (not Printf) for the sentence - issue #192.
			// UE 5.8's FString::Printf format string must be a compile-time literal
			// (FormatStringSan), so an NSLOCTEXT result cannot be its Fmt argument. Every number is
			// pre-formatted with the SAME %-specifier as before into its own FString, then dropped
			// into the translatable template as a plain {n} string substitution, so every digit this
			// already printed is unchanged - only the words around them can now be translated.
			const FText EngineState = F.bEngineRunning
				? NSLOCTEXT("AirportMgr", "InspectorEngineRunning", "running")
				: NSLOCTEXT("AirportMgr", "InspectorEngineOff", "off");
			LastComposedFacts = FString::Format(
				*NSLOCTEXT("AirportMgr", "InspectorAircraftFacts",
					"Heading {0}\nSpeed {1} m/s ({2} kt)\nAltitude {3} m\nTo {4}\nEngine {5}").ToString(),
				{
					FString::Printf(TEXT("%03.0f"), F.HeadingDegrees),
					FString::Printf(TEXT("%.1f"), Shown / 100.0),
					FString::Printf(TEXT("%.0f"), Shown / 100.0 * 1.94384),
					FString::Printf(TEXT("%.0f"), F.Altitude / 100.0),
					F.Destination,
					EngineState.ToString(),
				});
			// THE DEMANDS BLOCK (2026-09-28): what the aircraft wants, one line each. The fuel
			// line is AirportOps's whole sentence (it names itself "Fuel ..."); pushback is the
			// airframe's need, which nothing services yet.
			if (!F.Fuel.IsEmpty() || !F.Pushback.IsEmpty())
			{
				LastComposedFacts += NSLOCTEXT("AirportMgr", "InspectorDemandsHeading", "\n\nDemands").ToString();
				if (!F.Fuel.IsEmpty())
				{
					LastComposedFacts += TEXT("\n") + F.Fuel;
				}
				if (!F.Pushback.IsEmpty())
				{
					LastComposedFacts += FString::Format(
						*NSLOCTEXT("AirportMgr", "InspectorPushbackLine", "\nPushback {0}").ToString(), { F.Pushback });
				}
			}
			// THE CONTRACT, after the demands it depends on - see UArrivalRowViewModel::DescribeTurnaround.
			if (!F.Turnaround.IsEmpty())
			{
				LastComposedFacts += TEXT("\n\n") + F.Turnaround;
			}
			LastComposedStatus = F.Status;
		}
		Title = LastComposedTitle;
		Facts = LastComposedFacts;
		Status = LastComposedStatus;
	}
	else if (Selection.Kind == ESelectionKind::Runway)
	{
		FRunwayCardFacts R;
		if (Target->GetNetwork() == nullptr || !InspectFacts::DescribeRunway(*Target->GetNetwork(), Selection.Id, R))
		{
			SetShown(false);
			bDepartEnabled = false;
			return;
		}
		// THE RUNWAY IN USE CARD (spec 2026-09-28-runway-in-use). Composed every tick without
		// FInspectorKey's gate: nothing on it moves, and the SetText gate below already makes
		// an unchanged sentence free.
		bRunway = true;
		const FString InUse = FString::Printf(TEXT("%02d"), R.InUse);
		const FString Other = FString::Printf(TEXT("%02d"), R.Other);
		Title = FString::Format(*NSLOCTEXT("AirportMgr", "InspectorRunwayTitle", "Runway {0}").ToString(), { R.Pair });
		Facts = FString::Format(
			*NSLOCTEXT("AirportMgr", "InspectorRunwayFacts", "In use: {0}\n{1}, {2} approach\n{3} m long").ToString(),
			{ InUse, FString(Pavement::Name(R.Surface)), FString(RunwayApproachName(R.Approach)),
				FString::Printf(TEXT("%.0f"), R.Length / 100.0) });
		Status = FString::Format(*NSLOCTEXT("AirportMgr", "InspectorRunwayStatus",
			"Landing and taking off {0}. A change reaches the next flight planned.").ToString(), { InUse });
		RunwayCaption = FText::Format(NSLOCTEXT("AirportMgr", "InspectorRunwayUse", "Use {0}"), FText::FromString(Other));
		bDepartEnabled = false;
	}
	else if (Selection.Kind == ESelectionKind::Taxiway)
	{
		FTaxiwayCardFacts T;
		if (Target->GetNetwork() == nullptr || !InspectFacts::DescribeTaxiway(*Target->GetNetwork(), Selection.Id, T))
		{
			SetShown(false);
			bDepartEnabled = false;
			return;
		}
		// THE TAXIWAY CARD (strip stage 6): its letter, strip and the widest span it admits -
		// every taxiway limits wingspan to its letter (user 2026-09-29) - and, restricted, what
		// restricts it (spec: "max span 65 m - restricted by building at ..."). Composed every
		// tick like the runway card; the SetText gate below makes an unchanged one free.
		Title = FString::Format(*NSLOCTEXT("AirportMgr", "InspectorTaxiwayTitle", "Taxiway {0}").ToString(), { T.Index });
		Facts = FString::Format(
			*NSLOCTEXT("AirportMgr", "InspectorTaxiwayFacts", "Code {0}, {1} m wide, {2}\nStrip {3} m each side\nMax span {4} m").ToString(),
			{ T.Letter, FString::Printf(TEXT("%.1f"), T.Width / 100.0), FString(Pavement::Name(T.Surface)),
				FString::Printf(TEXT("%.1f"), T.Strip / 100.0), FString::Printf(TEXT("%.0f"), T.MaxWingspan / 100.0) });
		if (T.RestrictedTo.IsSet())
		{
			Facts += FString::Format(*NSLOCTEXT("AirportMgr", "InspectorTaxiwayRestricted",
				"\nRestricted to Code {0} by {1} - move it clear of the strip").ToString(),
				{ T.RestrictedTo.GetValue(), T.RestrictedBy.IsEmpty() ? FString(TEXT("something in its strip")) : T.RestrictedBy });
		}
		Status = T.RestrictedTo.IsSet()
			? NSLOCTEXT("AirportMgr", "InspectorTaxiwayStatusRestricted", "Restricted").ToString()
			: NSLOCTEXT("AirportMgr", "InspectorTaxiwayStatusOpen", "Open to its letter").ToString();
		bDepartEnabled = false;
	}
	else if (Selection.Kind == ESelectionKind::Stand)
	{
		FStandFacts S;
		if (Target->GetNetwork() == nullptr || !InspectFacts::DescribeStand(Target->GetGroundTraffic(), *Target->GetNetwork(), Selection.Id, S))
		{
			SetShown(false);
			bDepartEnabled = false;
			return;
		}
		// AN ENTITY, NOT ALWAYS A STAND, since the fuel slice. PoseRole is what tells the two
		// apart (see FStandFacts::PoseRole); a stand's own card is unchanged.
		if (S.PoseRole == EServiceRole::Aircraft)
		{
			// FString::Format, not Printf - see the aircraft branch's own comment on why
			// (issue #192, UE 5.8's compile-time Printf format check).
			// THE NUMBER, not the index - the one painted at the stand's turn-off; an index is
			// recycled by the next stand placed after a delete (FEntityInstance::StandNumber).
			Title = FString::Format(
				*NSLOCTEXT("AirportMgr", "InspectorStandTitle", "Stand {0}").ToString(), { S.Number });
			const FText Reachability = S.bReachable
				? NSLOCTEXT("AirportMgr", "InspectorStandReachable", "Reachable by taxiway")
				: NSLOCTEXT("AirportMgr", "InspectorStandUnreachable", "NOT reachable - no taxiway joins it");
			// SERVICE ROAD, beside Reachability (far-side-entry spec §2): a stand is placed
			// with no service road at all, so this NAMES THE FIX rather than refusing the
			// stand - the same choice Reachability itself already made for a taxiway.
			const FText ServiceRoad = S.bServiceable
				? NSLOCTEXT("AirportMgr", "InspectorStandServiceable", "Service road: joined")
				: NSLOCTEXT("AirportMgr", "InspectorStandUnserviceable",
					"Service road: not joined - draw a service road along the far edge");
			Facts = FString::Format(
				*NSLOCTEXT("AirportMgr", "InspectorStandFacts", "Code {0} ({1} m span)\n{2} service anchors\n{3}\n{4}").ToString(),
				{
					S.SizeClass,
					FString::Printf(TEXT("%.0f"), S.DesignWingspan / 100.0),
					FString::FromInt(S.AnchorCount),
					Reachability.ToString(),
					ServiceRoad.ToString(),
				});
			// CLOSED BY A STRIP (strip stage 6): the reason, and the figures to fix it by.
			if (!S.ClosedBecause.IsEmpty())
			{
				Facts += FString::Format(*NSLOCTEXT("AirportMgr", "InspectorStandClosed",
					"\nClosed to new arrivals: {0}").ToString(), { S.ClosedBecause });
			}
			Status = S.OccupantAgent == 0
				? NSLOCTEXT("AirportMgr", "InspectorStandEmpty", "Empty").ToString()
				: S.bOccupantParked
					? FString::Format(*NSLOCTEXT("AirportMgr", "InspectorStandOccupied",
						"Occupied by aircraft #{0}").ToString(), { S.OccupantAgent })
					: FString::Format(*NSLOCTEXT("AirportMgr", "InspectorStandReserved",
						"Reserved for aircraft #{0}").ToString(), { S.OccupantAgent });
		}
		else
		{
			// bReachable is the pose node having line on it, which for a depot means a
			// SERVICE ROAD within its lead-in reach. The message names the fix rather than
			// the symptom: the road is the thing the player goes and draws.
			Title = FString::Format(
				*NSLOCTEXT("AirportMgr", "InspectorDepotTitle", "Fuel depot {0}").ToString(), { S.Index });
			Facts = S.bReachable
				? NSLOCTEXT("AirportMgr", "InspectorDepotOnRoad", "On a service road").ToString()
				: NSLOCTEXT("AirportMgr", "InspectorDepotNotOnRoad", "Fuel depot: not on a road").ToString();
			Status = S.bReachable
				? NSLOCTEXT("AirportMgr", "InspectorDepotReady", "Ready").ToString()
				: NSLOCTEXT("AirportMgr", "InspectorDepotCannotDispatch", "Cannot dispatch").ToString();

			// HOW FAR BEHIND IT IS (user, 2026-09-28): its vehicles and their jobs, and a summary
			// that replaces "Ready" - through the ops subsystem, for the fuel line's reason above:
			// the airport actor is Airside's and may not know what a job is. An off-road depot
			// keeps "Cannot dispatch", which names the fix; its backlog is empty anyway.
			if (const UOpsRuntime* Runtime = UOpsRuntimeSubsystem::Get(GetWorld()); Runtime != nullptr && S.bReachable)
			{
				if (const UJobBoard* Board = Runtime->GetJobBoard())
				{
					const FDepotBacklog Backlog = Board->DescribeDepot(Target->GetNetwork()->EntityIdAt(Selection.Id),
						Runtime->GetClock() != nullptr ? Runtime->GetClock()->Now() : 0.0);
					Status = Backlog.Summary;
					if (!Backlog.Detail.IsEmpty())
					{
						Facts += TEXT("\n") + Backlog.Detail;
					}
				}
			}
		}
		bDepartEnabled = false;
	}
	else
	{
		// A KIND WITH NO CARD - only a value outside the enum reaches here now: an appended kind
		// fails the static_assert above until it has a branch. It used to fall into the stand
		// branch and describe ENTITY Id; now it says so ONCE per selection (Refresh runs every
		// tick) and shows nothing.
		// ENFORCED BY: the static_assert on ESelectionKind::Count above;
		// AirportMgr.Inspector.UnknownKindWarnsOnce (once per selection, not per tick)
		if (bNewSelection)
		{
			UE_LOG(LogInspector, Warning, TEXT("Inspector: no card for selection kind %d"), static_cast<int32>(Selection.Kind));
		}
		SetShown(false);
		bDepartEnabled = false;
		return;
	}

	// THE GATE. Compared against the COMPOSED text rather than a (selection id, phase) key -
	// see LastTitle/LastFacts/LastStatus's own comment for why phase alone would freeze a
	// moving aircraft's numbers. SetText has no early-out of its own (UIStyle.cpp's own note
	// on why UBuildBarWidget gates its clock and balance the same way), so the common case -
	// nothing selected, or a parked aircraft awaiting dispatch - now sets no text at all.
	if (TitleText != nullptr && Title != LastTitle)
	{
		TitleText->SetText(FText::FromString(Title));
		LastTitle = Title;
		++SetTextCalls;
	}
	if (FactsText != nullptr && Facts != LastFacts)
	{
		FactsText->SetText(FText::FromString(Facts));
		LastFacts = Facts;
		++SetTextCalls;
	}
	if (StatusText != nullptr && Status != LastStatus)
	{
		StatusText->SetText(FText::FromString(Status));
		LastStatus = Status;
		++SetTextCalls;
	}
	if (DepartButton != nullptr)
	{
		DepartButton->SetVisibility(bAircraft ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);

		// THE BAR'S OWN RULE, now UUiButton::LookFor: the BUTTON stays Control always - disabled
		// dims a button by its ink, never by brightening the fill. Painting a disabled background
		// with the muted LABEL slot made Depart look MORE prominent while taxiing than while
		// parked, backwards from the intent. Only the CAPTION follows enabled state.
		DepartButton->SetState(bDepartEnabled, false);
	}
	if (FollowButton != nullptr)
	{
		FollowButton->SetVisibility(bAircraft ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	}
	if (RunwayButton != nullptr)
	{
		RunwayButton->SetVisibility(bRunway ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
		// Retitled through the button's own caption (UUiButton::GetLabel), the Follow button's way.
		const UTextBlock* Caption = RunwayButton->GetLabel();
		if (bRunway && (Caption == nullptr || !Caption->GetText().EqualTo(RunwayCaption)))
		{
			RunwayButton->SetLabel(RunwayCaption);
		}
	}
	SetShown(true);
}

void UInspectorWidget::RunAction(int32 ActionIndex)
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
	Actions[ActionIndex].TryRun(*C, TEXT("Inspector"));
}

void UInspectorWidget::HandleDepart() { RunAction(DepartActionIndex); }
void UInspectorWidget::HandleFollow() { RunAction(FollowActionIndex); }
void UInspectorWidget::HandleRunway() { RunAction(RunwayActionIndex); }

bool UInspectorWidget::IsShownForTest() const { return IsShown(); }
bool UInspectorWidget::IsDepartEnabledForTest() const { return bDepartEnabled; }
FString UInspectorWidget::TitleForTest() const { return TitleText != nullptr ? TitleText->GetText().ToString() : FString(); }
FString UInspectorWidget::FactsForTest() const { return FactsText != nullptr ? FactsText->GetText().ToString() : FString(); }
FLinearColor UInspectorWidget::DepartLabelColourForTest() const
{
	const UTextBlock* Caption = DepartButton != nullptr ? DepartButton->GetLabel() : nullptr;
	return Caption != nullptr ? Caption->GetColorAndOpacity().GetSpecifiedColor() : FLinearColor::Black;
}
