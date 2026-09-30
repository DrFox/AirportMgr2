#include "BuildActions.h"
#include "Model/Airport.h"
#include "Model/FlightBoard.h"
#include "Model/Pricing.h"
#include "Present/OpsRuntime.h"
#include "Present/RoadNetworkActor.h"
#include "Present/OpsRuntimeSubsystem.h"
#include "RoadBuildController.h"
#include "Tool/BuildSession.h"
#include "Tool/SnapToggleRegistry.h"

#define LOCTEXT_NAMESPACE "AirportMgr"

int32 FBuildActionContext::ConstructCalls = 0;

FBuildActionContext::FBuildActionContext(ARoadBuildController& InController)
	: Controller(InController)
	// SAME LOOKUP TryRun always did inline (UOpsRuntimeSubsystem::Get(GetWorld())) - moved
	// here so every verb reads it from the context rather than repeating the call, the way
	// HasRuntime and (before issue #191) StepLandingFee/LandAircraftNearViewFocus each did.
	, Runtime(UOpsRuntimeSubsystem::Get(InController.GetWorld()))
	, Target(InController.GetTarget())
{
	++ConstructCalls;   // See ConstructCountForTest.
}

namespace
{
	// ONE TABLE, not a switch: a static_assert ties its length to EActionSection::Count, so a
	// section added to the enum with no name here fails the build instead of printing "?" at
	// runtime the way the old switch's default-less fallthrough would have.
	constexpr const TCHAR* SectionNames[] =
	{
		TEXT("Time"), TEXT("Tools"), TEXT("Edit"), TEXT("Aircraft"), TEXT("Selection"), TEXT("Game"),
		TEXT("Snap"), TEXT("Snap to"),
	};
	static_assert(UE_ARRAY_COUNT(SectionNames) == static_cast<int32>(EActionSection::Count),
		"Every EActionSection needs a name here");
}

const TCHAR* ActionSectionName(EActionSection Section)
{
	const int32 Index = static_cast<int32>(Section);
	return (Index >= 0 && Index < UE_ARRAY_COUNT(SectionNames)) ? SectionNames[Index] : TEXT("?");
}

namespace
{
	bool Always(const FBuildActionContext&) { return true; }
	bool Never(const FBuildActionContext&) { return false; }
	/** Ctx.Runtime rather than C.HasOpsRuntime(): the SAME UOpsRuntimeSubsystem::Get the
	 *  controller's own query ran, resolved once per action rather than once per predicate -
	 *  see FBuildActionContext's own comment (issue #191). */
	bool HasRuntime(const FBuildActionContext& Ctx) { return Ctx.Runtime != nullptr; }

	FBuildAction Make(const TCHAR* Id, EActionSection Section, FText Label, FKey Key, bool bCtrl,
		TFunction<void(FBuildActionContext&)> Execute,
		TFunction<bool(const FBuildActionContext&)> IsActive,
		TFunction<bool(const FBuildActionContext&)> IsEnabled)
	{
		FBuildAction A;
		A.Id = Id;
		A.Section = Section;
		A.Label = MoveTemp(Label);
		A.Key = Key;
		A.bRequiresCtrl = bCtrl;
		A.Execute = MoveTemp(Execute);
		A.IsActive = MoveTemp(IsActive);
		A.IsEnabled = MoveTemp(IsEnabled);
		return A;
	}

	TArray<FBuildAction> MakeActions()
	{
		TArray<FBuildAction> Out;

		// --- Time ---
		Out.Add(Make(TEXT("time.slower"), EActionSection::Time, LOCTEXT("Slower", "Slower"), EKeys::Comma, false,
			[](FBuildActionContext& Ctx) { Ctx.Controller.StepSpeed(-1); }, Never, HasRuntime));
		Out.Add(Make(TEXT("time.pause"), EActionSection::Time, LOCTEXT("Pause", "Pause"), EKeys::P, false,
			[](FBuildActionContext& Ctx) { Ctx.Controller.TogglePause(); },
			[](const FBuildActionContext& Ctx) { return Ctx.Controller.IsPaused(); }, HasRuntime));
		Out.Add(Make(TEXT("time.faster"), EActionSection::Time, LOCTEXT("Faster", "Faster"), EKeys::Period, false,
			[](FBuildActionContext& Ctx) { Ctx.Controller.StepSpeed(+1); }, Never, HasRuntime));

		// --- Tools: GENERATED from Airside's registry, never listed here ---
		const TConstArrayView<FToolRegistration> Registry = ToolRegistry();
		for (int32 Index = 0; Index < Registry.Num(); ++Index)
		{
			const FToolRegistration& Tool = Registry[Index];
			Out.Add(Make(*FString::Printf(TEXT("tool.%s"), *Tool.Name.ToString().ToLower()),
				EActionSection::Tools, Tool.Name, Tool.Key, false,
				[Index](FBuildActionContext& Ctx) { Ctx.Controller.SelectTool(Index); },
				[Index](const FBuildActionContext& Ctx) { return Ctx.Controller.GetActiveToolIndex() == Index; },
				Always));
		}

		// --- Edit ---
		// BUILD IS AN EDIT VERB, and the first of them: it is the positive counterpart of
		// Undo and Clear, and a staged gesture's last click LOCKS rather than commits, so
		// something has to say "now". IsEnabled reads the readout the active tool filled
		// THIS frame rather than asking the tool a second question, which is the whole
		// reason IToolReadoutSink carries Committable beside the facts.
		//
		// ENTER, and the key is the point rather than a convenience: ARoadBuildHUD draws
		// "Build [Enter]" at the cursor, and it reads the key back off this entry. A bar-only
		// action would leave that prompt naming no key at all.
		Out.Add(Make(TEXT("edit.build"), EActionSection::Edit, LOCTEXT("Build", "Build"), EKeys::Enter, false,
			[](FBuildActionContext& Ctx) { Ctx.Controller.OnBuild(); }, Never,
			[](const FBuildActionContext& Ctx) { return Ctx.Controller.GetToolReadout().bCommittable; }));
		// THE THREE MODES, GENERATED FROM Airside's BuildVerbRegistry() (issue #304) rather
		// than hand-typed here beside it - the same move ToolRegistry() already made for the
		// Tools section above. Before this, FRoadBuildEdModeCommands had nothing for any of
		// the three (`grep GestureMode AirsideEditor/Private/*.cpp` was 0) because there was no
		// shared table an editor command loop could read the way it already read ToolRegistry() -
		// only this hand-written trio, which the editor module cannot see (AirportMgr depends
		// on Airside, never the other way). One row each and one toggle behind all three: they
		// are mutually exclusive because they are one enum on the session, not because these
		// rows agree to be - see EGestureMode, and the report that made that necessary: Remove
		// and Edit could both be lit, the bar drew both, and Edit silently won.
		for (int32 Index = 0; Index < BuildVerbRegistry().Num(); ++Index)
		{
			const FBuildVerbRegistration& Verb = BuildVerbRegistry()[Index];
			Out.Add(Make(*FString::Printf(TEXT("edit.%s"), *Verb.Id.ToString().ToLower()),
				EActionSection::Edit, Verb.Name, Verb.Key, false,
				[Index](FBuildActionContext& Ctx) { Ctx.Controller.ApplyVerb(BuildVerbRegistry()[Index]); },
				[Index](const FBuildActionContext& Ctx) { return BuildVerbRegistry()[Index].IsActive(Ctx.Controller.GetSession()); },
				[Index](const FBuildActionContext& Ctx) { return BuildVerbRegistry()[Index].IsEnabled(Ctx.Controller.GetSession()); }));
		}
		Out.Add(Make(TEXT("edit.undo"), EActionSection::Edit, LOCTEXT("Undo", "Undo"), EKeys::Z, true,
			[](FBuildActionContext& Ctx) { Ctx.Controller.OnUndo(); }, Never,
			[](const FBuildActionContext& Ctx) { return Ctx.Controller.CanUndo(); }));
		Out.Add(Make(TEXT("edit.redo"), EActionSection::Edit, LOCTEXT("Redo", "Redo"), EKeys::Y, true,
			[](FBuildActionContext& Ctx) { Ctx.Controller.OnRedo(); }, Never,
			[](const FBuildActionContext& Ctx) { return Ctx.Controller.CanRedo(); }));
		Out.Add(Make(TEXT("edit.clear"), EActionSection::Edit, LOCTEXT("Clear", "Clear"), EKeys::BackSpace, false,
			[](FBuildActionContext& Ctx) { Ctx.Controller.OnClearNetwork(); }, Never,
			[](const FBuildActionContext& Ctx) { return Ctx.Controller.HasNetworkContent(); }));

		// --- Aircraft ---
		Out.Add(Make(TEXT("aircraft.land"), EActionSection::Aircraft, LOCTEXT("Land", "Land"), EKeys::Seven, false,
			// OPENS THE PANEL rather than landing a default (2026-09-27): the panel's rows are
			// what land now, each its own type - see ULandAircraftPanelWidget.
			[](FBuildActionContext& Ctx) { Ctx.Controller.ToggleLandPanel(); },
			[](const FBuildActionContext& Ctx) { return Ctx.Controller.IsLandPanelShowing(); },
			// AND OPEN: a closed airport admits no arrivals, the debug one included (ruling I1, 2026-09-30). No runtime
			// (the editor mode) is not a closure.
			// ENFORCED BY: AirportMgr.Actions.LandGreyedWhileClosed
			[](const FBuildActionContext& Ctx)
			{
				return Ctx.Controller.HasRunway()
					&& (Ctx.Runtime == nullptr || Ctx.Runtime->GetAirport()->Status() == EAirportStatus::Open);
			}));
		Out.Add(Make(TEXT("aircraft.guidelines"), EActionSection::Aircraft, LOCTEXT("Guidelines", "Guidelines"), EKeys::G, false,
			[](FBuildActionContext& Ctx) { Ctx.Controller.OnToggleGuidelines(); },
			[](const FBuildActionContext& Ctx) { return Ctx.Controller.IsGuidelineOverlayOn(); }, Always));

		// --- Selection: the inspector's verbs. Rows HERE so the panel's buttons, the bar
		// and the C key are one list (spec §6.2). Depart has no key: a key that departed
		// whatever happened to be selected is a misclick away from an unintended take-off.
		Out.Add(Make(TEXT("selection.depart"), EActionSection::Selection, LOCTEXT("Depart", "Depart"), EKeys::Invalid, false,
			[](FBuildActionContext& Ctx) { Ctx.Controller.DepartSelected(); }, Never,
			[](const FBuildActionContext& Ctx) { return Ctx.Controller.CanDepartSelected(); }));
		Out.Add(Make(TEXT("selection.follow"), EActionSection::Selection, LOCTEXT("Follow", "Follow"), EKeys::C, false,
			[](FBuildActionContext& Ctx) { Ctx.Controller.ToggleWatchAgent(); },
			[](const FBuildActionContext& Ctx) { return Ctx.Controller.IsWatchingAgent(); },
			[](const FBuildActionContext& Ctx) { return Ctx.Controller.HasSelectedAircraft() || Ctx.Controller.HasAgent() || Ctx.Controller.IsWatchingAgent(); }));
		// THE RUNWAY IN USE, flipped from the selected runway's card (spec 2026-09-28-runway-in-
		// use, ruling 4): a run-time traffic control, so a Selection verb and not a runway-tool
		// modifier. No key, Depart's reason: a key that reversed whatever runway happened to be
		// selected is a misclick away from sending the next arrival the other way. Captioned with
		// the end it would CHANGE TO - "Use 27" - so the button says what pressing it does.
		{
			FBuildAction Flip = Make(TEXT("selection.runway_in_use"), EActionSection::Selection,
				LOCTEXT("RunwayInUse", "Change runway in use"), EKeys::Invalid, false,
				[](FBuildActionContext& Ctx) { Ctx.Controller.FlipSelectedRunway(); }, Never,
				[](const FBuildActionContext& Ctx) { return Ctx.Controller.CanFlipSelectedRunway(); });
			Flip.DynamicLabel = [](const FBuildActionContext& Ctx)
			{
				FRunwayCardFacts Card;
				return Ctx.Controller.SelectedRunwayFacts(Card)
					? FText::Format(LOCTEXT("RunwayUse", "Use {0}"), FText::FromString(FString::Printf(TEXT("%02d"), Card.Other)))
					: LOCTEXT("RunwayInUse", "Change runway in use");
			};
			Out.Add(MoveTemp(Flip));
		}
		// WHAT TRAFFIC THE SELECTED RUNWAY TAKES (2026-09-29, samples/2runways.png): mixed, arrivals
		// only, departures only - the flip's neighbour on the runway card, and keyless for its
		// reason. Captioned with the CURRENT mode, not the next: three states read as a setting,
		// and "Arrivals only" on the button says what the strip is doing now.
		{
			FBuildAction Mode = Make(TEXT("selection.runway_use"), EActionSection::Selection,
				LOCTEXT("RunwayUseMode", "Runway takes"), EKeys::Invalid, false,
				[](FBuildActionContext& Ctx) { Ctx.Controller.CycleSelectedRunwayUse(); }, Never,
				[](const FBuildActionContext& Ctx) { return Ctx.Controller.CanFlipSelectedRunway(); });
			Mode.DynamicLabel = [](const FBuildActionContext& Ctx)
			{
				FRunwayCardFacts Card;
				if (!Ctx.Controller.SelectedRunwayFacts(Card))
				{
					return LOCTEXT("RunwayUseMode", "Runway takes");
				}
				switch (Card.Use)
				{
				case ERunwayUse::ArrivalsOnly:   return LOCTEXT("RunwayUseArrivals", "Arrivals only");
				case ERunwayUse::DeparturesOnly: return LOCTEXT("RunwayUseDepartures", "Departures only");
				default:                         return LOCTEXT("RunwayUseMixed", "Mixed ops");
				}
			};
			Out.Add(MoveTemp(Mode));
		}
		// UNSTICK (spec 2026-09-29-unstick-agent): one verb, three sub-choices in the inspector's popup
		// (UUiMenuButton) - so ONE row here, whose Execute asks the inspector to open that popup; the
		// three choices are UAgentRescue's, not three rows, or the bar would grow three buttons for a
		// rescue the player needs rarely. No key, Depart's reason: a despawn is a misclick away.
		// Enabled whenever an agent is selected at all - Despawn always is (UAgentRescue::Decide).
		Out.Add(Make(TEXT("selection.unstick"), EActionSection::Selection, LOCTEXT("Unstick", "Unstick"), EKeys::Invalid, false,
			[](FBuildActionContext& Ctx) { Ctx.Controller.RequestUnstickMenu(); }, Never,
			[](const FBuildActionContext& Ctx) { return Ctx.Controller.CanUnstickSelected(EUnstickAction::Despawn).bAllowed; }));

		// FACILITY PURCHASES (spec 2026-09-29-facility-upgrades §4): the depot card's three verbs, INSPECTOR
		// ONLY. Buy-vehicle and sell carry an argument a row cannot: the card CHOOSES the type / ARMS the
		// vehicle on the controller, then runs the row - so a sale takes two clicks (a destructive gesture
		// needs a deliberate second one - memory). Keyless: a key that spent money on whatever was selected
		// is a misclick.
		{
			FBuildAction Module = Make(TEXT("selection.buy_module"), EActionSection::Selection,
				LOCTEXT("BuyModule", "Buy module"), EKeys::Invalid, false,
				[](FBuildActionContext& Ctx) { Ctx.Controller.BuySelectedModule(); }, Never,
				[](const FBuildActionContext& Ctx) { return Ctx.Controller.CanBuySelectedModule(); });
			Module.bInspectorOnly = true;
			Out.Add(MoveTemp(Module));

			FBuildAction Vehicle = Make(TEXT("selection.buy_vehicle"), EActionSection::Selection,
				LOCTEXT("BuyVehicle", "Buy vehicle"), EKeys::Invalid, false,
				[](FBuildActionContext& Ctx) { Ctx.Controller.BuyChosenVehicle(); }, Never,
				[](const FBuildActionContext& Ctx) { return Ctx.Controller.CanBuyChosenVehicle(); });
			Vehicle.bInspectorOnly = true;
			Out.Add(MoveTemp(Vehicle));

			FBuildAction Sell = Make(TEXT("selection.sell_vehicle"), EActionSection::Selection,
				LOCTEXT("SellVehicle", "Sell vehicle"), EKeys::Invalid, false,
				[](FBuildActionContext& Ctx) { Ctx.Controller.SellArmedVehicle(); }, Never,
				[](const FBuildActionContext& Ctx) { return Ctx.Controller.CanSellArmedVehicle(); });
			Sell.bInspectorOnly = true;
			Out.Add(MoveTemp(Sell));
		}

		// --- Game ---
		Out.Add(Make(TEXT("game.save"), EActionSection::Game, LOCTEXT("Save", "Save"), EKeys::K, false,
			[](FBuildActionContext& Ctx) { Ctx.Controller.QuickSave(); }, Never, HasRuntime));
		Out.Add(Make(TEXT("game.load"), EActionSection::Game, LOCTEXT("Load", "Load"), EKeys::L, false,
			[](FBuildActionContext& Ctx) { Ctx.Controller.QuickLoad(); }, Never, HasRuntime));

		// THE DRIVE SIDE (spec 2026-09-23 §2), through this table for the fee lever's reason
		// below. Lit while traffic keeps left. No key: a mis-hit re-lanes the whole airport.
		// Bound to Ctx.Target, the road actor, which owns the edit and its undo step.
		Out.Add(Make(TEXT("game.driveside"), EActionSection::Game, LOCTEXT("DriveLeft", "Drive left"),
			EKeys::Invalid, false,
			[](FBuildActionContext& Ctx)
			{
				if (Ctx.Target != nullptr)
				{
					Ctx.Target->SetDriveSide(Ctx.Target->GetDriveSide() == EDriveSide::Left
						? EDriveSide::Right : EDriveSide::Left);
				}
			},
			[](const FBuildActionContext& Ctx) { return Ctx.Target != nullptr && Ctx.Target->GetDriveSide() == EDriveSide::Left; },
			[](const FBuildActionContext& Ctx) { return Ctx.Target != nullptr; }));

		// THE FEE LEVER, and it goes THROUGH THIS TABLE rather than beside it. BuildActions is
		// already the one list the bar, the key bindings and the inspector all read, and a pair
		// of hand-added buttons next to the generated ones is this codebase's named recurring
		// bug - see CLAUDE.md, "Check where a list is CONSUMED".
		//
		// BOUND STRAIGHT TO Ctx.Runtime's UPricing, not a controller forwarder (issue #191):
		// StepLandingFee's step size, floor and ceiling are UPricing's own decision now (see
		// its header), and this verb's real owner was never ARoadBuildController - the
		// controller used to exist only as a proxy onto UOpsRuntime::GetPricing(). This is the
		// case FBuildActionContext exists for: a verb whose owner is not the controller no
		// longer has to become a controller method to be reachable from here.
		//
		// No keys: a mis-hit that silently repriced every future offer is worse than a click.
		Out.Add(Make(TEXT("game.feedown"), EActionSection::Game, LOCTEXT("FeeDown", "Fee -"),
			EKeys::Invalid, false,
			[](FBuildActionContext& Ctx)
			{
				if (UPricing* Pricing = Ctx.Runtime != nullptr ? Ctx.Runtime->GetPricing() : nullptr)
				{
					Pricing->StepLandingFee(-1);
				}
			},
			Never, HasRuntime));
		Out.Add(Make(TEXT("game.feeup"), EActionSection::Game, LOCTEXT("FeeUp", "Fee +"),
			EKeys::Invalid, false,
			[](FBuildActionContext& Ctx)
			{
				if (UPricing* Pricing = Ctx.Runtime != nullptr ? Ctx.Runtime->GetPricing() : nullptr)
				{
					Pricing->StepLandingFee(+1);
				}
			},
			Never, HasRuntime));

		// SETTINGS, on Escape - what a player presses for a game's settings (spec, ruled 2026-09-28).
		// In PIE the editor's Stop takes Escape before the game sees it (DebuggerCommands.cpp:358,
		// PlayLevel.cpp:3092); the gear on the bar is the way in there. Always enabled: it edits the
		// player's own values, which exist with or without a runtime. Its id is named once, in
		// SettingsActionId for the controller's modal key gate, which lets exactly this action through;
		// Make takes a literal, so AirportMgr.Actions.SettingsOnEscape checks the two agree.
		Out.Add(Make(TEXT("game.settings"), EActionSection::Game, LOCTEXT("Settings", "Settings"),
			EKeys::Escape, false,
			[](FBuildActionContext& Ctx) { Ctx.Controller.ToggleSettings(); },
			[](const FBuildActionContext& Ctx) { return Ctx.Controller.IsSettingsShowing(); }, Always));

		// B, and a key is fine here where the fee lever's is not: opening a panel changes
		// nothing about the airport, so a mis-hit costs a keystroke rather than repricing
		// every future offer. IsActive lights the button while the panel is open, the way
		// pause and the overlay toggles already do.
		Out.Add(Make(TEXT("game.ledger"), EActionSection::Game, LOCTEXT("Ledger", "Ledger"),
			EKeys::B, false,
			[](FBuildActionContext& Ctx) { Ctx.Controller.ToggleLedger(); },
			[](const FBuildActionContext& Ctx) { return Ctx.Controller.IsLedgerShowing(); }, HasRuntime));

		// THE ALERTS BADGE (ops alerts spec 2026-09-29 §3): lit while anything needs the player, counting
		// how many, and opening the window that lists them. NO KEY: a panel toggle needs none, and the
		// free letters are fewer than the tools still to come.
		{
			FBuildAction Alerts = Make(TEXT("game.alerts"), EActionSection::Game, LOCTEXT("Alerts", "Alerts"),
				EKeys::Invalid, false,
				[](FBuildActionContext& Ctx) { Ctx.Controller.ToggleAlerts(); },
				[](const FBuildActionContext& Ctx) { return Ctx.Controller.AlertCount() > 0 || Ctx.Controller.IsAlertsShowing(); },
				HasRuntime);
			Alerts.DynamicLabel = [](const FBuildActionContext& Ctx)
			{
				const int32 Count = Ctx.Controller.AlertCount();
				return Count > 0 ? FText::Format(LOCTEXT("AlertsCount", "Alerts ({0})"), FText::AsNumber(Count)) : LOCTEXT("Alerts", "Alerts");
			};
			Out.Add(MoveTemp(Alerts));
		}

		// THE AIRPORT'S STATUS (spec 2026-09-29-ops-batch3 §3): close it, reopen it, and read why it is not open. A MENU
		// VERB, because closing cancels every flight not yet arrived - a destructive gesture, so it confirms at the
		// cursor with the inspector Unstick's popup (UUiMenuButton + bConfirm) rather than on one click. Reopening loses
		// nothing and is a plain line. NO KEY, Depart's reason. Bound to Ctx.Runtime, the fee lever's reason.
		{
			FBuildAction Airport = Make(TEXT("game.airport"), EActionSection::Game, LOCTEXT("CloseAirport", "Close airport"),
				EKeys::Invalid, false,
				// EXECUTE ONLY REOPENS: it is the door with no confirm (a key, were one ever bound), and a close must
				// never be reachable without one.
				// ENFORCED BY: AirportMgr.Actions.AirportExecuteNeverCloses
				[](FBuildActionContext& Ctx)
				{
					if (Ctx.Runtime != nullptr && Ctx.Runtime->GetAirport()->IsClosedByPlayer())
					{
						Ctx.Runtime->SetAirportClosed(false);
					}
				},
				// NEVER LIT (whole-stack review M6): ACCENT MEANS ARMED AND NOTHING ELSE (UBuildBarWidget), and a closed
				// airport is not an armed tool. It used to light while not open; the caption ("Closed", "No runway") is
				// the one glance that says the airport is taking no traffic.
				// ENFORCED BY: AirportMgr.Actions.AirportStatusCaption ("closed: NOT lit")
				[](const FBuildActionContext&) { return false; },
				HasRuntime);
			Airport.MenuItems = [](const FBuildActionContext& Ctx)
			{
				TArray<FUiMenuItem> Out;
				if (Ctx.Runtime == nullptr)
				{
					return Out;
				}
				// TWO FIXED LINES, each enabled only when it applies (review M9): line 0 always closes and line 1 always
				// opens, so Choose acts on the line the player saw rather than toggling whatever the state is by then.
				const bool bClosed = Ctx.Runtime->GetAirport()->IsClosedByPlayer();
				FUiMenuItem& Close = Out.AddDefaulted_GetRef();
				// THE COST, ASKED AS THE POPUP OPENS: what a close would cancel now (UFlightBoard::UnarrivedCount).
				Close.Label = LOCTEXT("CloseAirport", "Close airport");
				Close.bEnabled = !bClosed;
				Close.Why = LOCTEXT("CloseAirportWhy", "The airport is already closed");
				Close.bConfirm = true;
				Close.ConfirmLabel = FText::Format(LOCTEXT("CloseAirportConfirm", "Close? {0} {0}|plural(one=flight,other=flights) will be cancelled"),
					FText::AsNumber(Ctx.Runtime->GetFlightBoard()->UnarrivedCount()));
				FUiMenuItem& Open = Out.AddDefaulted_GetRef();
				Open.Label = LOCTEXT("OpenAirport", "Open airport");
				Open.bEnabled = bClosed;
				Open.Why = LOCTEXT("OpenAirportWhy", "The airport is not closed");
				return Out;
			};
			// THE LINE SHOWN, not a toggle: 0 closes, 1 opens - a no-op when already so (SetAirportClosed re-derives to
			// no change).
			// ENFORCED BY: AirportMgr.Actions.AirportChooseActsOnTheLine
			Airport.Choose = [](FBuildActionContext& Ctx, int32 Line)
			{
				if (Ctx.Runtime != nullptr && (Line == 0 || Line == 1))
				{
					Ctx.Runtime->SetAirportClosed(Line == 0);
				}
			};
			// THE CAPTION IS THE STATUS whenever it is not simply open - "Closed (draining: 3)" counts the aircraft a
			// closed airport is still seeing off (UFlightBoard::OnGroundCount); draining is a readout, not a state.
			Airport.DynamicLabel = [](const FBuildActionContext& Ctx)
			{
				const EAirportStatus Status = Ctx.Runtime != nullptr ? Ctx.Runtime->GetAirport()->Status() : EAirportStatus::Open;
				if (Status == EAirportStatus::NoRunway)
				{
					return LOCTEXT("AirportNoRunway", "No runway");
				}
				if (Status == EAirportStatus::ClosedByPlayer)
				{
					const int32 Draining = Ctx.Runtime->GetFlightBoard()->OnGroundCount();
					return Draining > 0
						? FText::Format(LOCTEXT("AirportDraining", "Closed (draining: {0})"), FText::AsNumber(Draining))
						: LOCTEXT("AirportClosed", "Closed");
				}
				return LOCTEXT("CloseAirport", "Close airport");
			};
			Out.Add(MoveTemp(Airport));
		}

		// THE GUIDE GRID'S SWITCHES, GENERATED FROM Airside's SnapToggleRegistry() (issue #440) - the
		// move BuildVerbRegistry() made for the sticky verbs above, and for the same reason: these
		// were fourteen rows typed HERE, in the game module, which the editor mode cannot read, so
		// the editor reached them only through the Details panel and H (keyless since 2026-09-30)
		// existed in PIE alone. The
		// WHY of each row (no keys, "Direction" not "Parallel", one cycling Grid button, the
		// Taxiway/Service road split) travelled with it to SnapToggleRegistry.cpp.
		//
		// ENABLED ALWAYS, as the hand-typed rows were: a toggle with no airport is inert, not
		// illegal - ApplySnapToggle says so in the log. LIT AND CAPTIONED FROM THE TARGET'S OWN
		// SETTINGS (Ctx.Target), the one copy (FSnapGuideSettings' header on why a per-driver
		// copy is the failure). NO AIRPORT READS THE FIXED NAME, not a caption: "Grid: world" over
		// a level with no airport claimed a state nothing held (#440 changed this; it used to).
		// ENFORCED BY: AirportMgr.Actions.SnapRowsComeFromTheRegistry
		for (int32 Index = 0; Index < SnapToggleRegistry().Num(); ++Index)
		{
			const FSnapToggleRegistration& Toggle = SnapToggleRegistry()[Index];
			FBuildAction Row = Make(*Toggle.Id.ToString(),
				Toggle.Group == ESnapToggleGroup::SnapTo ? EActionSection::SnapTo : EActionSection::Snap,
				Toggle.Name, Toggle.Key, false,
				[Index](FBuildActionContext& Ctx) { Ctx.Controller.ApplySnapToggle(SnapToggleRegistry()[Index]); },
				[Index](const FBuildActionContext& Ctx)
				{
					return Ctx.Target != nullptr && SnapToggleRegistry()[Index].IsActive(Ctx.Target->GuideSources);
				},
				Always);
			if (Toggle.DynamicLabel)
			{
				Row.DynamicLabel = [Index](const FBuildActionContext& Ctx)
				{
					const FSnapToggleRegistration& Entry = SnapToggleRegistry()[Index];
					return Ctx.Target != nullptr ? Entry.DynamicLabel(Ctx.Target->GuideSources) : Entry.Name;
				};
			}
			Out.Add(MoveTemp(Row));
		}
		return Out;
	}
}

TConstArrayView<FBuildAction> BuildActions()
{
	static const TArray<FBuildAction> Actions = MakeActions();
	return Actions;
}

bool FBuildAction::TryRun(ARoadBuildController& C, const TCHAR* Via) const
{
	// THE ONE PLACE a bare controller reference becomes an FBuildActionContext (issue #191) -
	// every caller above this keeps passing what it always held.
	FBuildActionContext Context(C);
	if (!IsEnabled(Context))
	{
		return false;
	}
	UE_LOG(LogRoadBuild, Log, TEXT("%s: %s"), Via, *Id.ToString());
	Execute(Context);
	return true;
}

bool FBuildAction::TryChoose(ARoadBuildController& C, int32 Line, const TCHAR* Via) const
{
	FBuildActionContext Context(C);
	return TryChoose(Context, Line, Via);
}

bool FBuildAction::TryChoose(FBuildActionContext& Context, int32 Line, const TCHAR* Via) const
{
	// TryRun's shape exactly - one context, the enabled gate, one log line - so a menu verb's line leaves the same
	// trace a button or a key does.
	if (!Choose || !IsEnabled(Context))
	{
		return false;
	}
	UE_LOG(LogRoadBuild, Log, TEXT("%s: %s line %d"), Via, *Id.ToString(), Line);
	Choose(Context, Line);
	return true;
}

const FBuildAction* FindAction(FName Id)
{
	for (const FBuildAction& Action : BuildActions())
	{
		if (Action.Id == Id)
		{
			return &Action;
		}
	}
	return nullptr;
}

const FBuildAction* FindAction(FKey Key, bool bRequiresCtrl)
{
	for (const FBuildAction& Action : BuildActions())
	{
		if (Action.Key == Key && Action.bRequiresCtrl == bRequiresCtrl)
		{
			return &Action;
		}
	}
	return nullptr;
}

#undef LOCTEXT_NAMESPACE
