#include "BuildActions.h"
#include "Model/Pricing.h"
#include "Present/OpsRuntime.h"
#include "Present/RoadNetworkActor.h"
#include "Present/OpsRuntimeSubsystem.h"
#include "RoadBuildController.h"
#include "Tool/BuildSession.h"

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
			[](FBuildActionContext& Ctx) { Ctx.Controller.LandAircraftNearViewFocus(); }, Never,
			[](const FBuildActionContext& Ctx) { return Ctx.Controller.HasRunway(); }));
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

		// B, and a key is fine here where the fee lever's is not: opening a panel changes
		// nothing about the airport, so a mis-hit costs a keystroke rather than repricing
		// every future offer. IsActive lights the button while the panel is open, the way
		// pause and the overlay toggles already do.
		Out.Add(Make(TEXT("game.ledger"), EActionSection::Game, LOCTEXT("Ledger", "Ledger"),
			EKeys::B, false,
			[](FBuildActionContext& Ctx) { Ctx.Controller.ToggleLedger(); },
			[](const FBuildActionContext& Ctx) { return Ctx.Controller.IsLedgerShowing(); }, HasRuntime));

		// TWO LISTS, ONE PER AXIS, and AirportMgr.Actions.GuideGridIsInTheRegistry walks BOTH
		// enums against them rather than counting: a row or column added without a button is a
		// guide the player cannot switch, and nothing else would say so.
		//
		// NO KEYS. Ten more bindings would crowd a keyboard already spending 0-9 on tools, and a
		// toggle is set once rather than reached for mid-drag. What mid-drag needs is the Alt
		// hold, which is not a registry action - see FToolContext::bSuspendGuides.
		Out.Add(Make(TEXT("snap.extending"), EActionSection::Snap, LOCTEXT("SnapExtending", "Extending"),
			EKeys::Invalid, false,
			[](FBuildActionContext& Ctx) { Ctx.Controller.ToggleGuideRelation(SnapGuide::ERelation::Extending); },
			[](const FBuildActionContext& Ctx) { return Ctx.Controller.IsGuideRelationOn(SnapGuide::ERelation::Extending); },
			Always));
		Out.Add(Make(TEXT("snap.levelwith"), EActionSection::Snap, LOCTEXT("SnapLevelWith", "Level with"),
			EKeys::Invalid, false,
			[](FBuildActionContext& Ctx) { Ctx.Controller.ToggleGuideRelation(SnapGuide::ERelation::LevelWith); },
			[](const FBuildActionContext& Ctx) { return Ctx.Controller.IsGuideRelationOn(SnapGuide::ERelation::LevelWith); },
			Always));
		// "DIRECTION", NOT "PARALLEL", although the relation behind it is ERelation::Parallel -
		// renamed 2026-09-20 after a player switched on Angled from and World, got nothing, and
		// pointed out that the row does three things and the button claimed one of them. It
		// offers a direction AND its perpendicular ("square to the taxiway" is not parallel to
		// anything), and for the World column an absolute compass axis, which is parallel to no
		// thing at all. The design doc's own grid already called the row "Parallel / square";
		// the button had taken the first word and dropped the rest.
		//
		// The enum keeps its name - see SnapGuide::ERelation::Parallel, which records this.
		Out.Add(Make(TEXT("snap.direction"), EActionSection::Snap, LOCTEXT("SnapDirection", "Direction"),
			EKeys::Invalid, false,
			[](FBuildActionContext& Ctx) { Ctx.Controller.ToggleGuideRelation(SnapGuide::ERelation::Parallel); },
			[](const FBuildActionContext& Ctx) { return Ctx.Controller.IsGuideRelationOn(SnapGuide::ERelation::Parallel); },
			Always));
		Out.Add(Make(TEXT("snap.collinear"), EActionSection::Snap, LOCTEXT("SnapCollinear", "Collinear"),
			EKeys::Invalid, false,
			[](FBuildActionContext& Ctx) { Ctx.Controller.ToggleGuideRelation(SnapGuide::ERelation::Collinear); },
			[](const FBuildActionContext& Ctx) { return Ctx.Controller.IsGuideRelationOn(SnapGuide::ERelation::Collinear); },
			Always));
		Out.Add(Make(TEXT("snap.angledfrom"), EActionSection::Snap, LOCTEXT("SnapAngledFrom", "Angled from"),
			EKeys::Invalid, false,
			[](FBuildActionContext& Ctx) { Ctx.Controller.ToggleGuideRelation(SnapGuide::ERelation::AngledFrom); },
			[](const FBuildActionContext& Ctx) { return Ctx.Controller.IsGuideRelationOn(SnapGuide::ERelation::AngledFrom); },
			Always));
		Out.Add(Make(TEXT("snap.matchinggap"), EActionSection::Snap, LOCTEXT("SnapMatchingGap", "Matching gap"),
			EKeys::Invalid, false,
			[](FBuildActionContext& Ctx) { Ctx.Controller.ToggleGuideRelation(SnapGuide::ERelation::MatchingGap); },
			[](const FBuildActionContext& Ctx) { return Ctx.Controller.IsGuideRelationOn(SnapGuide::ERelation::MatchingGap); },
			Always));

		// THE SECOND AXIS. Before 2026-09-20 these sat in the same list as the rows above, which
		// is why "Runway" read as a source you could switch off for every relation and was not -
		// see SnapGuide::EReference.
		// TWO BUTTONS WHERE "Road" WAS ONE, since 2026-09-20. Everywhere else in this codebase
		// these are different tools under different keys, different cross-sections and
		// different traversal classes, and the guide LABEL already said which - "parallel to
		// the service road" appearing under a button marked Road was the whole complaint. See
		// SnapGuide::EReference.
		Out.Add(Make(TEXT("snapto.taxiway"), EActionSection::SnapTo, LOCTEXT("SnapToTaxiway", "Taxiway"),
			EKeys::Invalid, false,
			[](FBuildActionContext& Ctx) { Ctx.Controller.ToggleGuideReference(SnapGuide::EReference::Taxiway); },
			[](const FBuildActionContext& Ctx) { return Ctx.Controller.IsGuideReferenceOn(SnapGuide::EReference::Taxiway); },
			Always));
		Out.Add(Make(TEXT("snapto.serviceroad"), EActionSection::SnapTo, LOCTEXT("SnapToServiceRoad", "Service road"),
			EKeys::Invalid, false,
			[](FBuildActionContext& Ctx) { Ctx.Controller.ToggleGuideReference(SnapGuide::EReference::ServiceRoad); },
			[](const FBuildActionContext& Ctx) { return Ctx.Controller.IsGuideReferenceOn(SnapGuide::EReference::ServiceRoad); },
			Always));
		Out.Add(Make(TEXT("snapto.runway"), EActionSection::SnapTo, LOCTEXT("SnapToRunway", "Runway"),
			EKeys::Invalid, false,
			[](FBuildActionContext& Ctx) { Ctx.Controller.ToggleGuideReference(SnapGuide::EReference::Runway); },
			[](const FBuildActionContext& Ctx) { return Ctx.Controller.IsGuideReferenceOn(SnapGuide::EReference::Runway); },
			Always));
		Out.Add(Make(TEXT("snapto.apron"), EActionSection::SnapTo, LOCTEXT("SnapToApron", "Apron"),
			EKeys::Invalid, false,
			[](FBuildActionContext& Ctx) { Ctx.Controller.ToggleGuideReference(SnapGuide::EReference::Apron); },
			[](const FBuildActionContext& Ctx) { return Ctx.Controller.IsGuideReferenceOn(SnapGuide::EReference::Apron); },
			Always));
		Out.Add(Make(TEXT("snapto.stand"), EActionSection::SnapTo, LOCTEXT("SnapToStand", "Stand"),
			EKeys::Invalid, false,
			[](FBuildActionContext& Ctx) { Ctx.Controller.ToggleGuideReference(SnapGuide::EReference::Stand); },
			[](const FBuildActionContext& Ctx) { return Ctx.Controller.IsGuideReferenceOn(SnapGuide::EReference::Stand); },
			Always));
		Out.Add(Make(TEXT("snapto.world"), EActionSection::SnapTo, LOCTEXT("SnapToWorld", "World"),
			EKeys::Invalid, false,
			[](FBuildActionContext& Ctx) { Ctx.Controller.ToggleGuideReference(SnapGuide::EReference::World); },
			[](const FBuildActionContext& Ctx) { return Ctx.Controller.IsGuideReferenceOn(SnapGuide::EReference::World); },
			Always));
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
