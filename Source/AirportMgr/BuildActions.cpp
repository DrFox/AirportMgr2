#include "BuildActions.h"
#include "BuildHudLayer.h"
#include "Model/AgentRescue.h"
#include "Model/Airport.h"
#include "Model/DeparturePlanner.h"
#include "Model/FacilityPurchases.h"
#include "Model/FlightBoard.h"
#include "Model/FuelSupply.h"
#include "Model/InspectFacts.h"
#include "Model/OpsDesignDefaults.h"
#include "Model/Pricing.h"
#include "Model/RoadNetwork.h"
#include "OpsRuntimeResolver.h"
#include "Present/OpsRuntime.h"
#include "Present/RoadNetworkActor.h"
#include "RoadBuildController.h"
#include "RoadBuildLog.h"
#include "Tool/BuildSession.h"
#include "Tool/SnapToggleRegistry.h"

#define LOCTEXT_NAMESPACE "AirportMgr"

int32 FBuildActionContext::ConstructCalls = 0;

FString FBuildActionArg::Describe() const
{
	if (!Code.IsNone())
	{
		return FString::Printf(TEXT("code %s"), *Code.ToString());
	}
	return Id != 0 ? FString::Printf(TEXT("id %d"), Id) : FString();
}

FBuildActionContext::FBuildActionContext(ARoadBuildController& InController)
	: Controller(InController)
	// THE RESOLVER'S ANSWER (#448), the one door to the runtime (ENFORCED BY: Check-Architecture rule 55) - what TryRun always looked up inline, moved here so every
	// verb reads it from the context rather than repeating the call, the way HasRuntime and (before issue #191)
	// StepLandingFee/LandAircraftNearViewFocus each did. It honours a test's stand-in, so the depot verbs, the bar and the
	// inspector all see the one runtime a headless test handed the world.
	, Runtime(OpsRuntimeResolver::Resolve(InController.GetWorld()))
	, Target(InController.GetTarget())
	, Hud(InController.GetHud())
	, Selection(InController.GetSelection())
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

	// --- THE VERBS' BODIES, bound to their OWNERS (#448) -----------------------------------------------------------------
	//
	// Each takes the context's parts - the road actor, the runtime, the HUD, the selection, the argument - and acts on the thing that owns the
	// work. They were ARoadBuildController methods, one forwarder per verb, because the context gave a verb no selection and no argument; the
	// fee lever and the drive side below already bound straight to Ctx.Runtime / Ctx.Target, and these are the same shape. A NAMED namespace
	// inside this anonymous one: the module is a UNITY build, and a bare name here could collide with another file's.
	namespace BuildActionVerbs
	{
		void ToggleWindow(const FBuildActionContext& Ctx, EHudWindow Window)
		{
			if (Ctx.Hud != nullptr)
			{
				Ctx.Hud->ToggleWindow(Window);
			}
		}

		bool WindowShowing(const FBuildActionContext& Ctx, EHudWindow Window)
		{
			return Ctx.Hud != nullptr && Ctx.Hud->IsWindowShowing(Window);
		}

		int32 AlertCount(const FBuildActionContext& Ctx)
		{
			return Ctx.Hud != nullptr ? Ctx.Hud->AlertCount() : 0;
		}

		/** The selected aircraft can depart - THROUGH THE PER-FRAME CACHE (issue #187): the bar polls this every tick, and UInspectorWidget::Refresh asks
		 *  the same question of the same selection the same frame - see ARoadBuildController::SelectedAgentFactsThisFrame's own comment. */
		bool CanDepart(const FBuildActionContext& Ctx)
		{
			FAgentFacts Facts;
			return Ctx.Controller.SelectedAgentFactsThisFrame(Facts) && Facts.bCanDepart;
		}

		/** Depart the selected aircraft; logs the planner's answer. The road actor's own DepartAgent does the work. */
		void Depart(const FBuildActionContext& Ctx)
		{
			if (Ctx.Selection.Kind != ESelectionKind::Aircraft || Ctx.Target == nullptr)
			{
				UE_LOG(LogRoadBuild, Warning, TEXT("Depart: no aircraft selected."));
				return;
			}
			const EDepartureRefusal Why = Ctx.Target->DepartAgent(Ctx.Selection.Id);
			UE_LOG(LogRoadBuild, Log, TEXT("Depart aircraft %d: %s"), Ctx.Selection.Id,
				Why == EDepartureRefusal::None ? TEXT("accepted") : *UEnum::GetValueAsString(Why));
		}

		/**
		 * A runway is selected (ESelectionKind::Runway) and still describes - the card and the verb. THROUGH THE PER-FRAME CACHE (#446): the
		 * bar asks this in IsEnabled and in DynamicLabel for both runway rows every tick - four InspectFacts::DescribeRunway calls while a
		 * runway was selected - and the controller answers all four from one (ARoadBuildController::SelectedRunwayFactsThisFrame). The verbs
		 * below read through it too: the cache is retired by the very network change a flip makes, so a second verb in the frame sees the first's flip.
		 * ENFORCED BY: AirportMgr.Actions.RunwayRowsDescribeOncePerFrame (one describe a tick, and the flip visible in the same frame)
		 */
		bool SelectedRunway(const FBuildActionContext& Ctx, FRunwayCardFacts& Out)
		{
			return Ctx.Controller.SelectedRunwayFactsThisFrame(Out);
		}

		bool CanChangeRunway(const FBuildActionContext& Ctx)
		{
			FRunwayCardFacts Unused;
			return SelectedRunway(Ctx, Unused);
		}

		/**
		 * Change the selected runway's direction in use to its other end, through the actor's SetRunwayFacts (so it is one undo step and
		 * logs "Runway 09/27 in use: 27 (was 09)"). Flights already planned finish as planned; the next plan reads the new direction
		 * (ruling 2, spec 2026-09-28-runway-in-use).
		 */
		void FlipRunway(const FBuildActionContext& Ctx)
		{
			FRunwayCardFacts Card;
			if (!SelectedRunway(Ctx, Card))
			{
				UE_LOG(LogRoadBuild, Warning, TEXT("Runway in use: no runway selected."));
				return;
			}
			const FRoadSegmentId Segment = Ctx.Target->GetNetwork()->SegmentIdAt(Ctx.Selection.Id);
			FRunwayFacts Facts = Ctx.Target->GetNetwork()->RunwayFactsFor(Segment);
			Facts.InUse = Card.Other;
			if (!Ctx.Target->SetRunwayFacts(Ctx.Selection.Id, Facts))
			{
				UE_LOG(LogRoadBuild, Warning, TEXT("Runway in use: the change to %02d was refused."), Card.Other);
			}
		}

		/**
		 * Step the selected runway's ERunwayUse on - mixed, arrivals only, departures only, mixed (RunwayUse::Next) - through the actor's
		 * SetRunwayFacts, FlipRunway's path: one undo step, logged "Runway at segment N takes: arrivals only (was mixed)". Planned flights
		 * keep their plan; the next plan reads it. Enabled whenever the flip is (CanChangeRunway).
		 */
		void CycleRunwayUse(const FBuildActionContext& Ctx)
		{
			FRunwayCardFacts Card;
			if (!SelectedRunway(Ctx, Card))
			{
				UE_LOG(LogRoadBuild, Warning, TEXT("Runway use: no runway selected."));
				return;
			}
			const FRoadSegmentId Segment = Ctx.Target->GetNetwork()->SegmentIdAt(Ctx.Selection.Id);
			FRunwayFacts Facts = Ctx.Target->GetNetwork()->RunwayFactsFor(Segment);
			Facts.Use = RunwayUse::Next(Card.Use);
			if (!Ctx.Target->SetRunwayFacts(Ctx.Selection.Id, Facts))
			{
				UE_LOG(LogRoadBuild, Warning, TEXT("Runway use: the change to %s was refused."), RunwayUse::Name(Facts.Use));
			}
		}

		/** selection.unstick's gate: an agent is selected, and the runtime's OWN verdict for Despawn - the one that is always allowed
		 *  (UAgentRescue::Decide). The inspector's menu lines ask the runtime the same question for each action. */
		bool CanUnstick(const FBuildActionContext& Ctx)
		{
			return Ctx.Runtime != nullptr && Ctx.Selection.Kind == ESelectionKind::Aircraft
				&& Ctx.Runtime->CanUnstick(Ctx.Selection.Id, EUnstickAction::Despawn).bAllowed;
		}

		/**
		 * THE DEPOT CARD'S VERBS (facility-upgrades spec §4), bound to UOpsRuntime with the selected depot - the fee lever's shape. The quote
		 * is asked fresh on every call, so an enabled check and the command it guards read the same state. Refused (logged) with no depot
		 * selected or no runtime. The selected depot is ARoadBuildController::DepotForSelection's walk, written once (ruling C4).
		 */
		FFacilityQuote QuoteSelectedFacility(const FBuildActionContext& Ctx)
		{
			const FEntityInstanceId Id = ARoadBuildController::DepotForSelection(Ctx.Target, Ctx.Selection);
			return Ctx.Runtime != nullptr && Id.IsSet() ? Ctx.Runtime->QuoteFacility(Id) : FFacilityQuote();
		}

		/**
		 * The quote row the run's ARGUMENT names (DepotModuleCode) - the card has one Buy per module since the tank became a second
		 * offer (2026-10-02), and the row it was clicked on is the only thing that knows which. NO ARGUMENT KEEPS THE SHED, found BY
		 * KIND: a plain TryRun (a caller from before the tank) meant the shed, and Modules is in TMap order, so "the first row" is not
		 * it. Unlike buy_vehicle, no argument is not refused - there was always exactly one thing it could mean.
		 * ENFORCED BY: AirportMgr.Actions.BuyModuleUsesItsArgument
		 */
		const FModuleOfferQuote* ChosenModule(const FFacilityQuote& Quote, const FBuildActionArg& Arg)
		{
			if (Arg.Code.IsNone())
			{
				return Quote.FindModule(EDepotModule::Shed);
			}
			return Quote.Modules.FindByPredicate([&Arg](const FModuleOfferQuote& Row) { return DepotModuleCode(Row.Module) == Arg.Code; });
		}

		bool CanBuyModule(const FBuildActionContext& Ctx)
		{
			const FFacilityQuote Quote = QuoteSelectedFacility(Ctx);
			const FModuleOfferQuote* Row = ChosenModule(Quote, Ctx.Arg);
			return Row != nullptr && Row->Refusal == EPurchaseRefusal::None;
		}

		void BuyModule(const FBuildActionContext& Ctx)
		{
			const FFacilityQuote Quote = QuoteSelectedFacility(Ctx);
			const FModuleOfferQuote* Row = ChosenModule(Quote, Ctx.Arg);
			if (Ctx.Runtime == nullptr || Row == nullptr)
			{
				UE_LOG(LogRoadBuild, Warning, TEXT("Buy module: no depot selected, no such module offered, or no ops runtime."));
				return;
			}
			// UFacilityPurchases logs the "Purchase: ..." line; this one says the click arrived.
			const FEntityInstanceId Depot = ARoadBuildController::DepotForSelection(Ctx.Target, Ctx.Selection);
			UE_LOG(LogRoadBuild, Log, TEXT("Buy module %s: depot %d"), *UEnum::GetValueAsString(Row->Module), Depot.Index);
			Ctx.Runtime->BuyModule(Depot, Row->Module);
		}

		/**
		 * THE FUEL ROW'S VERBS (2026-10-03), bound to UOpsRuntime's forwarders. Gated on the SUPPLY'S OWN QUOTE - the one the card's row
		 * is worded from (FDepotCard::FuelViewOf) - so a lit button is an order the supply will take. A DEPOT MUST BE SELECTED, though the
		 * fuel is the airport's: these are the depot card's buttons, and a selection verb that ran with nothing selected is a hotkey
		 * waiting to happen. Logged here as the click arriving; the supply logs what it did.
		 */
		bool FuelQuoted(const FBuildActionContext& Ctx, FFuelQuote& Out)
		{
			const UFuelSupply* Supply = Ctx.Runtime != nullptr ? Ctx.Runtime->GetFuelSupply() : nullptr;
			if (Supply == nullptr || !ARoadBuildController::DepotForSelection(Ctx.Target, Ctx.Selection).IsSet())
			{
				return false;
			}
			Out = Supply->Quote(OpsDesignDefaults::SpotOrderLitres);
			return true;
		}

		bool CanOrderFuel(const FBuildActionContext& Ctx)
		{
			FFuelQuote Q;
			return FuelQuoted(Ctx, Q) && Q.Spot == EFuelOrderRefusal::None;
		}

		void OrderFuel(const FBuildActionContext& Ctx)
		{
			if (Ctx.Runtime == nullptr)
			{
				UE_LOG(LogRoadBuild, Warning, TEXT("Fuel order: no ops runtime."));
				return;
			}
			const EFuelOrderRefusal Why = Ctx.Runtime->OrderSpotFuel(OpsDesignDefaults::SpotOrderLitres);
			UE_LOG(LogRoadBuild, Log, TEXT("Fuel order %.0f L: %s"), OpsDesignDefaults::SpotOrderLitres,
				Why == EFuelOrderRefusal::None ? TEXT("taken") : *UFacilityPurchases::FuelOrderRefusalText(Why).ToString());
		}

		bool CanSignFuelContract(const FBuildActionContext& Ctx)
		{
			FFuelQuote Q;
			return FuelQuoted(Ctx, Q) && Q.Sign == EFuelOrderRefusal::None;
		}

		/** Signs the quote's NextTier - the tier the button's caption named. */
		void SignFuelContract(const FBuildActionContext& Ctx)
		{
			FFuelQuote Q;
			if (!FuelQuoted(Ctx, Q))
			{
				UE_LOG(LogRoadBuild, Warning, TEXT("Fuel contract: no depot selected, or no ops runtime."));
				return;
			}
			const EFuelOrderRefusal Why = Ctx.Runtime->SignFuelContract(Q.NextTier);
			UE_LOG(LogRoadBuild, Log, TEXT("Fuel contract tier %d: %s"), Q.NextTier,
				Why == EFuelOrderRefusal::None ? TEXT("signed") : *UFacilityPurchases::FuelOrderRefusalText(Why).ToString());
		}

		bool CanCancelFuelContract(const FBuildActionContext& Ctx)
		{
			FFuelQuote Q;
			return FuelQuoted(Ctx, Q) && Q.Cancel == EFuelOrderRefusal::None;
		}

		void CancelFuelContract(const FBuildActionContext& Ctx)
		{
			if (Ctx.Runtime == nullptr)
			{
				UE_LOG(LogRoadBuild, Warning, TEXT("Fuel contract cancel: no ops runtime."));
				return;
			}
			const EFuelOrderRefusal Why = Ctx.Runtime->CancelFuelContract();
			UE_LOG(LogRoadBuild, Log, TEXT("Fuel contract cancel: %s"),
				Why == EFuelOrderRefusal::None ? TEXT("cancelled") : *UFacilityPurchases::FuelOrderRefusalText(Why).ToString());
		}

		/** The kind the run's argument names is on offer at the selected depot and not refused. NO ARGUMENT, NO BUY: a run that names no kind
		 *  (a hotkey, a test) must not buy whatever was last chosen - there is nothing "last chosen" to find (#448). */
		bool CanBuyVehicle(const FBuildActionContext& Ctx)
		{
			const FFacilityQuote Quote = QuoteSelectedFacility(Ctx);
			const FName Chosen = Ctx.Arg.Code;
			const FVehicleOfferQuote* Offer = Chosen.IsNone() ? nullptr
				: Quote.VehicleOffers.FindByPredicate([Chosen](const FVehicleOfferQuote& O) { return O.TypeCode == Chosen; });
			return Offer != nullptr && Offer->Refusal == EPurchaseRefusal::None;
		}

		void BuyVehicle(const FBuildActionContext& Ctx)
		{
			const FEntityInstanceId Depot = ARoadBuildController::DepotForSelection(Ctx.Target, Ctx.Selection);
			if (Ctx.Runtime == nullptr || !Depot.IsSet() || Ctx.Arg.Code.IsNone())
			{
				UE_LOG(LogRoadBuild, Warning, TEXT("Buy vehicle: no depot selected, no type chosen, or no ops runtime."));
				return;
			}
			UE_LOG(LogRoadBuild, Log, TEXT("Buy vehicle %s: depot %d"), *Ctx.Arg.Code.ToString(), Depot.Index);
			Ctx.Runtime->BuyVehicle(Depot, Ctx.Arg.Code);
		}

		/** The argument's vehicle is in the SELECTED depot's fleet and may be sold. Asked of the selected depot's fleet, not the board at
		 *  large: an id from another card cannot sell a vehicle the player is not looking at. NO ARGUMENT, NO SALE (BuyVehicle's reason). */
		bool CanSellVehicle(const FBuildActionContext& Ctx)
		{
			const FFacilityQuote Quote = QuoteSelectedFacility(Ctx);
			const int32 Wanted = Ctx.Arg.Id;
			const FFleetRowQuote* Row = Wanted == 0 ? nullptr
				: Quote.Fleet.FindByPredicate([Wanted](const FFleetRowQuote& R) { return R.VehicleId == Wanted; });
			return Row != nullptr && Row->Refusal == EPurchaseRefusal::None;
		}

		void SellVehicle(const FBuildActionContext& Ctx)
		{
			if (Ctx.Runtime == nullptr || Ctx.Arg.Id == 0)
			{
				UE_LOG(LogRoadBuild, Warning, TEXT("Sell vehicle: no vehicle named, or no ops runtime."));
				return;
			}
			UE_LOG(LogRoadBuild, Log, TEXT("Sell vehicle %d"), Ctx.Arg.Id);
			Ctx.Runtime->SellVehicle(Ctx.Arg.Id);
		}
	}

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
			[](FBuildActionContext& Ctx) { BuildActionVerbs::ToggleWindow(Ctx, EHudWindow::Land); },
			[](const FBuildActionContext& Ctx) { return BuildActionVerbs::WindowShowing(Ctx, EHudWindow::Land); },
			// AND OPEN: a closed airport admits no arrivals, the debug one included (ruling I1, 2026-09-30) - UAirport::
			// AdmitsArrivals, the one predicate (#431). AND A RUNTIME: landing is the flight board's, and with none there is
			// nothing to land through - the board-less fallback that made "no runtime" look like a working Land went
			// with #431 (it ran only in headless tests).
			// ENFORCED BY: AirportMgr.Actions.LandGreyedWhileClosed
			[](const FBuildActionContext& Ctx)
			{
				return Ctx.Runtime != nullptr && Ctx.Controller.HasRunway() && Ctx.Runtime->GetAirport()->AdmitsArrivals();
			}));
		Out.Add(Make(TEXT("aircraft.guidelines"), EActionSection::Aircraft, LOCTEXT("Guidelines", "Guidelines"), EKeys::G, false,
			[](FBuildActionContext& Ctx) { Ctx.Controller.OnToggleGuidelines(); },
			[](const FBuildActionContext& Ctx) { return Ctx.Controller.IsGuidelineOverlayOn(); }, Always));

		// --- Selection: the inspector's verbs. Rows HERE so the panel's buttons, the bar
		// and the C key are one list (spec §6.2). Depart has no key: a key that departed
		// whatever happened to be selected is a misclick away from an unintended take-off.
		Out.Add(Make(TEXT("selection.depart"), EActionSection::Selection, LOCTEXT("Depart", "Depart"), EKeys::Invalid, false,
			[](FBuildActionContext& Ctx) { BuildActionVerbs::Depart(Ctx); }, Never,
			[](const FBuildActionContext& Ctx) { return BuildActionVerbs::CanDepart(Ctx); }));
		Out.Add(Make(TEXT("selection.follow"), EActionSection::Selection, LOCTEXT("Follow", "Follow"), EKeys::C, false,
			[](FBuildActionContext& Ctx) { Ctx.Controller.ToggleWatchAgent(); },
			[](const FBuildActionContext& Ctx) { return Ctx.Controller.IsWatchingAgent(); },
			[](const FBuildActionContext& Ctx) { return Ctx.Selection.Kind == ESelectionKind::Aircraft || Ctx.Controller.HasAgent() || Ctx.Controller.IsWatchingAgent(); }));
		// THE RUNWAY IN USE, flipped from the selected runway's card (spec 2026-09-28-runway-in-
		// use, ruling 4): a run-time traffic control, so a Selection verb and not a runway-tool
		// modifier. No key, Depart's reason: a key that reversed whatever runway happened to be
		// selected is a misclick away from sending the next arrival the other way. Captioned with
		// the end it would CHANGE TO - "Use 27" - so the button says what pressing it does.
		{
			FBuildAction Flip = Make(TEXT("selection.runway_in_use"), EActionSection::Selection,
				LOCTEXT("RunwayInUse", "Change runway in use"), EKeys::Invalid, false,
				[](FBuildActionContext& Ctx) { BuildActionVerbs::FlipRunway(Ctx); }, Never,
				[](const FBuildActionContext& Ctx) { return BuildActionVerbs::CanChangeRunway(Ctx); });
			Flip.DynamicLabel = [](const FBuildActionContext& Ctx)
			{
				FRunwayCardFacts Card;
				return BuildActionVerbs::SelectedRunway(Ctx, Card)
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
				[](FBuildActionContext& Ctx) { BuildActionVerbs::CycleRunwayUse(Ctx); }, Never,
				[](const FBuildActionContext& Ctx) { return BuildActionVerbs::CanChangeRunway(Ctx); });
			Mode.DynamicLabel = [](const FBuildActionContext& Ctx)
			{
				FRunwayCardFacts Card;
				if (!BuildActionVerbs::SelectedRunway(Ctx, Card))
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
		// (UUiMenuButton) - so ONE row here, whose Execute opens that popup through the HUD layer (#446: it was a count the inspector
		// diffed every tick); the
		// three choices are UAgentRescue's, not three rows, or the bar would grow three buttons for a
		// rescue the player needs rarely. No key, Depart's reason: a despawn is a misclick away.
		// Enabled whenever an agent is selected at all - Despawn always is (UAgentRescue::Decide).
		Out.Add(Make(TEXT("selection.unstick"), EActionSection::Selection, LOCTEXT("Unstick", "Unstick"), EKeys::Invalid, false,
			[](FBuildActionContext& Ctx) { if (Ctx.Hud != nullptr) { Ctx.Hud->OpenUnstickMenu(); } }, Never,
			[](const FBuildActionContext& Ctx) { return BuildActionVerbs::CanUnstick(Ctx); }));

		// FACILITY PURCHASES (spec 2026-09-29-facility-upgrades §4): the depot card's three verbs, INSPECTOR
		// ONLY. Buy-vehicle and sell carry an ARGUMENT - the kind, the vehicle - and so, since the tank's own row (2026-10-03), does
		// buy-module - the module (DepotModuleCode; none means the shed, ChosenModule's reason) - and they carry it: the card runs the
		// row WITH it (FBuildAction::TryRunWith, #448), where it used to choose the type / arm the vehicle on the
		// controller and then run the row, leaving a second caller to run with whatever was last armed. A sale still
		// takes two clicks (a destructive gesture needs a deliberate second one - memory) - the first arms the ROW,
		// the second runs the verb with its id. Keyless: a key that spent money on whatever was selected is a
		// misclick, and with no argument these rows are disabled anyway.
		// ENFORCED BY: AirportMgr.Actions.SellTakesItsVehicleFromTheRow, AirportMgr.Actions.ParameterisedVerbsRefuseNoArgument
		{
			FBuildAction Module = Make(TEXT("selection.buy_module"), EActionSection::Selection,
				LOCTEXT("BuyModule", "Buy module"), EKeys::Invalid, false,
				[](FBuildActionContext& Ctx) { BuildActionVerbs::BuyModule(Ctx); }, Never,
				[](const FBuildActionContext& Ctx) { return BuildActionVerbs::CanBuyModule(Ctx); });
			Module.bInspectorOnly = true;
			Out.Add(MoveTemp(Module));

			FBuildAction Vehicle = Make(TEXT("selection.buy_vehicle"), EActionSection::Selection,
				LOCTEXT("BuyVehicle", "Buy vehicle"), EKeys::Invalid, false,
				[](FBuildActionContext& Ctx) { BuildActionVerbs::BuyVehicle(Ctx); }, Never,
				[](const FBuildActionContext& Ctx) { return BuildActionVerbs::CanBuyVehicle(Ctx); });
			Vehicle.bInspectorOnly = true;
			Out.Add(MoveTemp(Vehicle));

			FBuildAction Sell = Make(TEXT("selection.sell_vehicle"), EActionSection::Selection,
				LOCTEXT("SellVehicle", "Sell vehicle"), EKeys::Invalid, false,
				[](FBuildActionContext& Ctx) { BuildActionVerbs::SellVehicle(Ctx); }, Never,
				[](const FBuildActionContext& Ctx) { return BuildActionVerbs::CanSellVehicle(Ctx); });
			Sell.bInspectorOnly = true;
			Out.Add(MoveTemp(Sell));
		}

		// THE FUEL ROW (2026-10-03, fuel-supply spec §7): order a spot load, sign a contract, cancel it - the depot card's buttons,
		// INSPECTOR ONLY and keyless for the purchases' reason (a key that spent money on whatever was selected is a misclick). No
		// argument: the spot order is one fixed size (OpsDesignDefaults::SpotOrderLitres) and the contract the quote's next tier.
		// ENFORCED BY: AirportMgr.Actions.FacilityVerbsRegistered, AirportMgr.Actions.FuelVerbsReachTheSupply
		{
			FBuildAction Spot = Make(TEXT("selection.fuel_spot"), EActionSection::Selection,
				LOCTEXT("FuelSpot", "Order fuel"), EKeys::Invalid, false,
				[](FBuildActionContext& Ctx) { BuildActionVerbs::OrderFuel(Ctx); }, Never,
				[](const FBuildActionContext& Ctx) { return BuildActionVerbs::CanOrderFuel(Ctx); });
			Spot.bInspectorOnly = true;
			Out.Add(MoveTemp(Spot));

			FBuildAction Sign = Make(TEXT("selection.fuel_contract_up"), EActionSection::Selection,
				LOCTEXT("FuelContractUp", "Sign fuel contract"), EKeys::Invalid, false,
				[](FBuildActionContext& Ctx) { BuildActionVerbs::SignFuelContract(Ctx); }, Never,
				[](const FBuildActionContext& Ctx) { return BuildActionVerbs::CanSignFuelContract(Ctx); });
			Sign.bInspectorOnly = true;
			Out.Add(MoveTemp(Sign));

			FBuildAction Cancel = Make(TEXT("selection.fuel_contract_cancel"), EActionSection::Selection,
				LOCTEXT("FuelContractCancel", "Cancel fuel contract"), EKeys::Invalid, false,
				[](FBuildActionContext& Ctx) { BuildActionVerbs::CancelFuelContract(Ctx); }, Never,
				[](const FBuildActionContext& Ctx) { return BuildActionVerbs::CanCancelFuelContract(Ctx); });
			Cancel.bInspectorOnly = true;
			Out.Add(MoveTemp(Cancel));
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
			[](FBuildActionContext& Ctx) { BuildActionVerbs::ToggleWindow(Ctx, EHudWindow::Ledger); },
			[](const FBuildActionContext& Ctx) { return BuildActionVerbs::WindowShowing(Ctx, EHudWindow::Ledger); }, HasRuntime));

		// THE ALERTS BADGE (ops alerts spec 2026-09-29 §3): lit while anything needs the player, counting
		// how many, and opening the window that lists them. NO KEY: a panel toggle needs none, and the
		// free letters are fewer than the tools still to come.
		{
			FBuildAction Alerts = Make(TEXT("game.alerts"), EActionSection::Game, LOCTEXT("Alerts", "Alerts"),
				EKeys::Invalid, false,
				[](FBuildActionContext& Ctx) { BuildActionVerbs::ToggleWindow(Ctx, EHudWindow::Alerts); },
				[](const FBuildActionContext& Ctx) { return BuildActionVerbs::AlertCount(Ctx) > 0 || BuildActionVerbs::WindowShowing(Ctx, EHudWindow::Alerts); },
				HasRuntime);
			Alerts.DynamicLabel = [](const FBuildActionContext& Ctx)
			{
				const int32 Count = BuildActionVerbs::AlertCount(Ctx);
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
	return TryRunWith(C, FBuildActionArg(), Via);
}

bool FBuildAction::TryRunWith(ARoadBuildController& C, const FBuildActionArg& Arg, const TCHAR* Via) const
{
	// THE ONE PLACE a bare controller reference becomes an FBuildActionContext (issue #191) -
	// every caller above this keeps passing what it always held.
	FBuildActionContext Context(C);
	return TryRunWith(Context, Arg, Via);
}

bool FBuildAction::TryRunWith(FBuildActionContext& Context, const FBuildActionArg& Arg, const TCHAR* Via) const
{
	// THE ARGUMENT IS IN THE CONTEXT BEFORE THE GATE, so IsEnabled and Execute judge the same one - a sale is disabled for a vehicle
	// that is not in the selected depot's fleet, not merely refused later (#448). TryRun is this with no argument, so every run takes
	// this gate. ENFORCED BY: AirportMgr.Actions.TryRunGatesOnEnabled, AirportMgr.Actions.ParameterisedVerbsRefuseNoArgument
	Context.Arg = Arg;
	if (!IsEnabled(Context))
	{
		return false;
	}
	UE_LOG(LogRoadBuild, Log, TEXT("%s: %s%s"), Via, *Id.ToString(), Arg.IsSet() ? *(TEXT(" ") + Arg.Describe()) : TEXT(""));
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

FName DepotModuleCode(EDepotModule Module)
{
	return FName(*StaticEnum<EDepotModule>()->GetNameStringByValue(static_cast<int64>(Module)));
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
