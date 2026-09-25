#include "InspectorWidget.h"

#include "Blueprint/WidgetTree.h"
#include "BuildActions.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Model/FuelService.h"
#include "Model/InspectFacts.h"
#include "Model/RoadAgent.h"
#include "Present/OpsRuntime.h"
#include "Present/OpsRuntimeSubsystem.h"
#include "Present/RoadNetworkActor.h"
#include "RoadBuildController.h"
#include "UIStyle.h"

DEFINE_LOG_CATEGORY_STATIC(LogInspector, Log, All);

void UInspectorWidget::BuildOnce(const UUIStyle& Style)
{
	// PanelStyle is the BASE class's now (issue #187) - UAirportMgrPanelWidget::Initialize
	// sets it before calling this, from the same resolve BuildOnce's own parameter already is.

	EnsureSlots(&Style);
	if (DepartButton != nullptr) { DepartButton->OnClicked.AddDynamic(this, &UInspectorWidget::HandleDepart); }
	if (FollowButton != nullptr) { FollowButton->OnClicked.AddDynamic(this, &UInspectorWidget::HandleFollow); }
	// SelfHitTestInvisible, not Collapsed: see UAirportMgrPanelWidget::BuildOnce for why an
	// otherwise-empty panel must stay this way. Only the CARD hides; the root stays laid out.
	SetVisibility(ESlateVisibility::SelfHitTestInvisible);
	SetCardShown(false);
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
		if (SelectionSeen == 0) { DepartActionIndex = Index; }
		else if (SelectionSeen == 1) { FollowActionIndex = Index; }
		++SelectionSeen;
	}

	// Code-built chrome only where the asset gave none - the same skeleton the offer inbox
	// uses (EnsureCardRoot, issue #90). A bottom-left card: title, facts, status, then the two
	// verbs in a row.
	UVerticalBox* Column = Cast<UVerticalBox>(EnsureCardRoot(TEXT("InspectorCard"),
		FAnchors(0.0f, 1.0f, 0.0f, 1.0f), FVector2D(0.0, 1.0), FVector2D(12.0, -BottomOffset), true));
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
	Text(TitleText, TEXT("TitleText"), EUITextRole::Title, Style->Text);
	Text(FactsText, TEXT("FactsText"), EUITextRole::Body, Style->TextMuted);
	Text(StatusText, TEXT("StatusText"), EUITextRole::Body, Style->TextMuted);

	UHorizontalBox* Row = nullptr;
	if (Column != nullptr && (DepartButton == nullptr || FollowButton == nullptr))
	{
		Row = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("InspectorVerbs"));
		Column->AddChildToVerticalBox(Row)->SetPadding(FMargin(0.0f, 8.0f, 0.0f, 0.0f));
	}

	// LABEL FROM Action.Label, KEY IN THE TOOLTIP - the bar's own convention
	// (UBuildBarWidget::BuildButtons), so a verb reads the same wherever it appears.
	auto Button = [&](TObjectPtr<UButton>& Field, const TCHAR* Name, int32 ActionIndex, TObjectPtr<UTextBlock>* OutLabel)
	{
		if (Field != nullptr || !Actions.IsValidIndex(ActionIndex)) { return; }
		const FBuildAction& Action = Actions[ActionIndex];
		Field = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), Name);
		UTextBlock* Label = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
		Label->SetText(Action.Label);
		Style->ApplyText(*Label, EUITextRole::Label, Style->Text);
		Field->SetContent(Label);
		Field->SetBackgroundColor(Style->Button);
		if (OutLabel != nullptr) { *OutLabel = Label; }
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
	// DepartLabel is HELD, not re-found: Refresh recolours it every tick when Depart's
	// enabled state changes, and UBuildBarEntry holds its own Label for the same reason.
	Button(DepartButton, TEXT("DepartButton"), DepartActionIndex, &DepartLabel);
	Button(FollowButton, TEXT("FollowButton"), FollowActionIndex, nullptr);
}

void UInspectorWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	if (const ARoadBuildController* C = Controller())
	{
		// The controller already computed this frame's FAgentFacts for the bar's
		// selection.depart row (ARoadBuildController::SelectedAgentFactsThisFrame) - passed
		// through rather than asked for a second time (issue #187).
		FAgentFacts Facts;
		const bool bHaveFacts = C->SelectedAgentFactsThisFrame(Facts);
		Refresh(C->GetTarget(), C->GetSelection(), bHaveFacts ? &Facts : nullptr);
	}
}

void UInspectorWidget::Refresh(const ARoadNetworkActor* Target, const FSelection& Selection,
	const FAgentFacts* PrecomputedAgentFacts)
{
	if (Target == nullptr || !Selection.IsSet())
	{
		SetCardShown(false);
		bDepartEnabled = false;
		return;
	}

	FString Title, Facts, Status;
	bool bAircraft = false;
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
			SetCardShown(false);
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
			if (const UFuelService* Fuel = Runtime->GetFuelService())
			{
				F.Fuel = Fuel->DescribeAgent(F.Id);
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
			if (!F.Fuel.IsEmpty())
			{
				LastComposedFacts += FString::Format(
					*NSLOCTEXT("AirportMgr", "InspectorFuelLine", "\nFuel {0}").ToString(), { F.Fuel });
			}
			LastComposedStatus = F.Status;
		}
		Title = LastComposedTitle;
		Facts = LastComposedFacts;
		Status = LastComposedStatus;
	}
	else
	{
		FStandFacts S;
		if (Target->GetNetwork() == nullptr || !InspectFacts::DescribeStand(Target->GetGroundTraffic(), *Target->GetNetwork(), Selection.Id, S))
		{
			SetCardShown(false);
			bDepartEnabled = false;
			return;
		}
		// AN ENTITY, NOT ALWAYS A STAND, since the fuel slice. PoseRole is what tells the two
		// apart (see FStandFacts::PoseRole); a stand's own card is unchanged.
		if (S.PoseRole == EServiceRole::Aircraft)
		{
			// FString::Format, not Printf - see the aircraft branch's own comment on why
			// (issue #192, UE 5.8's compile-time Printf format check).
			Title = FString::Format(
				*NSLOCTEXT("AirportMgr", "InspectorStandTitle", "Stand {0}").ToString(), { S.Index });
			const FText Reachability = S.bReachable
				? NSLOCTEXT("AirportMgr", "InspectorStandReachable", "Reachable by taxiway")
				: NSLOCTEXT("AirportMgr", "InspectorStandUnreachable", "NOT reachable - no taxiway joins it");
			Facts = FString::Format(
				*NSLOCTEXT("AirportMgr", "InspectorStandFacts", "Code {0} ({1} m span)\n{2} service anchors\n{3}").ToString(),
				{
					S.SizeClass,
					FString::Printf(TEXT("%.0f"), S.DesignWingspan / 100.0),
					FString::FromInt(S.AnchorCount),
					Reachability.ToString(),
				});
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
		}
		bDepartEnabled = false;
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
		// PanelStyle is the base class's (issue #187) - never null once BuildOnce has run.
		const UUIStyle* Style = PanelStyle != nullptr ? PanelStyle.Get() : UAirportMgrUISettings::ResolveStyle();
		DepartButton->SetVisibility(bAircraft ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
		DepartButton->SetIsEnabled(bDepartEnabled);

		// THE BAR'S OWN RULE (UBuildBarWidget::RefreshState): the BUTTON stays Style->Button
		// always: disabled dims a button by darkening it under Button, never by brightening
		// it, and TextMuted is a LABEL slot (it is lighter than Button on purpose, for text
		// over a dark ground) - painting a disabled background with it made Depart look
		// MORE prominent while taxiing than while parked, backwards from the intent. Only the
		// CAPTION follows enabled state, exactly as the bar's icon/label content does.
		DepartButton->SetBackgroundColor(Style->Button);
		if (DepartLabel != nullptr)
		{
			DepartLabel->SetColorAndOpacity(FSlateColor(bDepartEnabled ? Style->Text : Style->TextMuted));
		}
	}
	if (FollowButton != nullptr)
	{
		FollowButton->SetVisibility(bAircraft ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	}
	SetCardShown(true);
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

bool UInspectorWidget::IsShownForTest() const { return CardWidget != nullptr && CardWidget->GetVisibility() != ESlateVisibility::Collapsed; }
bool UInspectorWidget::IsDepartEnabledForTest() const { return bDepartEnabled; }
FString UInspectorWidget::TitleForTest() const { return TitleText != nullptr ? TitleText->GetText().ToString() : FString(); }
FLinearColor UInspectorWidget::DepartLabelColourForTest() const
{
	return DepartLabel != nullptr ? DepartLabel->GetColorAndOpacity().GetSpecifiedColor() : FLinearColor::Black;
}
