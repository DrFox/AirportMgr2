#include "BuildActions.h"
#include "RoadBuildController.h"
#include "Tool/BuildSession.h"

#define LOCTEXT_NAMESPACE "AirportMgr"

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
	bool Always(const ARoadBuildController&) { return true; }
	bool Never(const ARoadBuildController&) { return false; }
	bool HasRuntime(const ARoadBuildController& C) { return C.HasOpsRuntime(); }

	FBuildAction Make(const TCHAR* Id, EActionSection Section, FText Label, FKey Key, bool bCtrl,
		TFunction<void(ARoadBuildController&)> Execute,
		TFunction<bool(const ARoadBuildController&)> IsActive,
		TFunction<bool(const ARoadBuildController&)> IsEnabled)
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
			[](ARoadBuildController& C) { C.StepSpeed(-1); }, Never, HasRuntime));
		Out.Add(Make(TEXT("time.pause"), EActionSection::Time, LOCTEXT("Pause", "Pause"), EKeys::P, false,
			[](ARoadBuildController& C) { C.TogglePause(); },
			[](const ARoadBuildController& C) { return C.IsPaused(); }, HasRuntime));
		Out.Add(Make(TEXT("time.faster"), EActionSection::Time, LOCTEXT("Faster", "Faster"), EKeys::Period, false,
			[](ARoadBuildController& C) { C.StepSpeed(+1); }, Never, HasRuntime));

		// --- Tools: GENERATED from Airside's registry, never listed here ---
		const TConstArrayView<FToolRegistration> Registry = ToolRegistry();
		for (int32 Index = 0; Index < Registry.Num(); ++Index)
		{
			const FToolRegistration& Tool = Registry[Index];
			Out.Add(Make(*FString::Printf(TEXT("tool.%s"), *Tool.Name.ToString().ToLower()),
				EActionSection::Tools, Tool.Name, Tool.Key, false,
				[Index](ARoadBuildController& C) { C.SelectTool(Index); },
				[Index](const ARoadBuildController& C) { return C.GetActiveToolIndex() == Index; },
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
			[](ARoadBuildController& C) { C.OnBuild(); }, Never,
			[](const ARoadBuildController& C) { return C.GetToolReadout().bCommittable; }));
		// THE THREE MODES, one row each and one toggle behind all three. They are mutually
		// exclusive because they are one enum on the session, not because these three rows
		// agree to be - see EGestureMode, and the report that made that necessary: Remove and
		// Edit could both be lit, the bar drew both, and Edit silently won.
		//
		// REMOVE AND INSERT HAVE NO KEY because Ctrl and Shift already mean them while HELD.
		// These rows are the sticky form, for work that outlasts a comfortable reach.
		Out.Add(Make(TEXT("edit.remove"), EActionSection::Edit, LOCTEXT("Remove", "Remove"), EKeys::Invalid, false,
			[](ARoadBuildController& C) { C.ToggleGestureMode(EGestureMode::Remove); },
			[](const ARoadBuildController& C) { return C.GetGestureMode() == EGestureMode::Remove; }, Always));
		Out.Add(Make(TEXT("edit.insert"), EActionSection::Edit, LOCTEXT("Insert", "Insert"), EKeys::Invalid, false,
			[](ARoadBuildController& C) { C.ToggleGestureMode(EGestureMode::Insert); },
			[](const ARoadBuildController& C) { return C.GetGestureMode() == EGestureMode::Insert; }, Always));

		// EDIT IS THE ONE WITH A KEY, because it is the one you enter deliberately and stay
		// in. M, not E: Q/E is camera turn, polled every frame in UpdateView. M is also the
		// key a Cities player already has from Move It, and mnemonic for move and merge.
		//
		// GREYED when the lit tool exposes no handles, so the bar answers "why can I not edit
		// this" instead of lighting over a mode that would do nothing at all.
		Out.Add(Make(TEXT("edit.editmode"), EActionSection::Edit, LOCTEXT("EditMode", "Edit"), EKeys::M, false,
			[](ARoadBuildController& C) { C.ToggleGestureMode(EGestureMode::Edit); },
			[](const ARoadBuildController& C) { return C.GetGestureMode() == EGestureMode::Edit; },
			[](const ARoadBuildController& C) { return C.ActiveToolHasEditHandles(); }));
		Out.Add(Make(TEXT("edit.undo"), EActionSection::Edit, LOCTEXT("Undo", "Undo"), EKeys::Z, true,
			[](ARoadBuildController& C) { C.OnUndo(); }, Never,
			[](const ARoadBuildController& C) { return C.CanUndo(); }));
		Out.Add(Make(TEXT("edit.redo"), EActionSection::Edit, LOCTEXT("Redo", "Redo"), EKeys::Y, true,
			[](ARoadBuildController& C) { C.OnRedo(); }, Never,
			[](const ARoadBuildController& C) { return C.CanRedo(); }));
		Out.Add(Make(TEXT("edit.clear"), EActionSection::Edit, LOCTEXT("Clear", "Clear"), EKeys::BackSpace, false,
			[](ARoadBuildController& C) { C.OnClearNetwork(); }, Never,
			[](const ARoadBuildController& C) { return C.HasNetworkContent(); }));

		// --- Aircraft ---
		Out.Add(Make(TEXT("aircraft.land"), EActionSection::Aircraft, LOCTEXT("Land", "Land"), EKeys::Seven, false,
			[](ARoadBuildController& C) { C.LandAircraftNearViewFocus(); }, Never,
			[](const ARoadBuildController& C) { return C.HasRunway(); }));
		Out.Add(Make(TEXT("aircraft.guidelines"), EActionSection::Aircraft, LOCTEXT("Guidelines", "Guidelines"), EKeys::G, false,
			[](ARoadBuildController& C) { C.OnToggleGuidelines(); },
			[](const ARoadBuildController& C) { return C.IsGuidelineOverlayOn(); }, Always));

		// --- Selection: the inspector's verbs. Rows HERE so the panel's buttons, the bar
		// and the C key are one list (spec §6.2). Depart has no key: a key that departed
		// whatever happened to be selected is a misclick away from an unintended take-off.
		Out.Add(Make(TEXT("selection.depart"), EActionSection::Selection, LOCTEXT("Depart", "Depart"), EKeys::Invalid, false,
			[](ARoadBuildController& C) { C.DepartSelected(); }, Never,
			[](const ARoadBuildController& C) { return C.CanDepartSelected(); }));
		Out.Add(Make(TEXT("selection.follow"), EActionSection::Selection, LOCTEXT("Follow", "Follow"), EKeys::C, false,
			[](ARoadBuildController& C) { C.ToggleWatchAgent(); },
			[](const ARoadBuildController& C) { return C.IsWatchingAgent(); },
			[](const ARoadBuildController& C) { return C.HasSelectedAircraft() || C.HasAgent() || C.IsWatchingAgent(); }));

		// --- Game ---
		Out.Add(Make(TEXT("game.save"), EActionSection::Game, LOCTEXT("Save", "Save"), EKeys::K, false,
			[](ARoadBuildController& C) { C.QuickSave(); }, Never, HasRuntime));
		Out.Add(Make(TEXT("game.load"), EActionSection::Game, LOCTEXT("Load", "Load"), EKeys::L, false,
			[](ARoadBuildController& C) { C.QuickLoad(); }, Never, HasRuntime));

		// THE FEE LEVER, and it goes THROUGH THIS TABLE rather than beside it. BuildActions is
		// already the one list the bar, the key bindings and the inspector all read, and a pair
		// of hand-added buttons next to the generated ones is this codebase's named recurring
		// bug - see CLAUDE.md, "Check where a list is CONSUMED".
		//
		// No keys: a mis-hit that silently repriced every future offer is worse than a click.
		Out.Add(Make(TEXT("game.feedown"), EActionSection::Game, LOCTEXT("FeeDown", "Fee -"),
			EKeys::Invalid, false,
			[](ARoadBuildController& C) { C.StepLandingFee(-1); }, Never, HasRuntime));
		Out.Add(Make(TEXT("game.feeup"), EActionSection::Game, LOCTEXT("FeeUp", "Fee +"),
			EKeys::Invalid, false,
			[](ARoadBuildController& C) { C.StepLandingFee(+1); }, Never, HasRuntime));

		// B, and a key is fine here where the fee lever's is not: opening a panel changes
		// nothing about the airport, so a mis-hit costs a keystroke rather than repricing
		// every future offer. IsActive lights the button while the panel is open, the way
		// pause and the overlay toggles already do.
		Out.Add(Make(TEXT("game.ledger"), EActionSection::Game, LOCTEXT("Ledger", "Ledger"),
			EKeys::B, false,
			[](ARoadBuildController& C) { C.ToggleLedger(); },
			[](const ARoadBuildController& C) { return C.IsLedgerShowing(); }, HasRuntime));

		// TWO LISTS, ONE PER AXIS, and AirportMgr.Actions.GuideGridIsInTheRegistry walks BOTH
		// enums against them rather than counting: a row or column added without a button is a
		// guide the player cannot switch, and nothing else would say so.
		//
		// NO KEYS. Ten more bindings would crowd a keyboard already spending 0-9 on tools, and a
		// toggle is set once rather than reached for mid-drag. What mid-drag needs is the Alt
		// hold, which is not a registry action - see FToolContext::bSuspendGuides.
		Out.Add(Make(TEXT("snap.extending"), EActionSection::Snap, LOCTEXT("SnapExtending", "Extending"),
			EKeys::Invalid, false,
			[](ARoadBuildController& C) { C.ToggleGuideRelation(SnapGuide::ERelation::Extending); },
			[](const ARoadBuildController& C) { return C.IsGuideRelationOn(SnapGuide::ERelation::Extending); },
			Always));
		Out.Add(Make(TEXT("snap.levelwith"), EActionSection::Snap, LOCTEXT("SnapLevelWith", "Level with"),
			EKeys::Invalid, false,
			[](ARoadBuildController& C) { C.ToggleGuideRelation(SnapGuide::ERelation::LevelWith); },
			[](const ARoadBuildController& C) { return C.IsGuideRelationOn(SnapGuide::ERelation::LevelWith); },
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
			[](ARoadBuildController& C) { C.ToggleGuideRelation(SnapGuide::ERelation::Parallel); },
			[](const ARoadBuildController& C) { return C.IsGuideRelationOn(SnapGuide::ERelation::Parallel); },
			Always));
		Out.Add(Make(TEXT("snap.collinear"), EActionSection::Snap, LOCTEXT("SnapCollinear", "Collinear"),
			EKeys::Invalid, false,
			[](ARoadBuildController& C) { C.ToggleGuideRelation(SnapGuide::ERelation::Collinear); },
			[](const ARoadBuildController& C) { return C.IsGuideRelationOn(SnapGuide::ERelation::Collinear); },
			Always));
		Out.Add(Make(TEXT("snap.angledfrom"), EActionSection::Snap, LOCTEXT("SnapAngledFrom", "Angled from"),
			EKeys::Invalid, false,
			[](ARoadBuildController& C) { C.ToggleGuideRelation(SnapGuide::ERelation::AngledFrom); },
			[](const ARoadBuildController& C) { return C.IsGuideRelationOn(SnapGuide::ERelation::AngledFrom); },
			Always));
		Out.Add(Make(TEXT("snap.matchinggap"), EActionSection::Snap, LOCTEXT("SnapMatchingGap", "Matching gap"),
			EKeys::Invalid, false,
			[](ARoadBuildController& C) { C.ToggleGuideRelation(SnapGuide::ERelation::MatchingGap); },
			[](const ARoadBuildController& C) { return C.IsGuideRelationOn(SnapGuide::ERelation::MatchingGap); },
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
			[](ARoadBuildController& C) { C.ToggleGuideReference(SnapGuide::EReference::Taxiway); },
			[](const ARoadBuildController& C) { return C.IsGuideReferenceOn(SnapGuide::EReference::Taxiway); },
			Always));
		Out.Add(Make(TEXT("snapto.serviceroad"), EActionSection::SnapTo, LOCTEXT("SnapToServiceRoad", "Service road"),
			EKeys::Invalid, false,
			[](ARoadBuildController& C) { C.ToggleGuideReference(SnapGuide::EReference::ServiceRoad); },
			[](const ARoadBuildController& C) { return C.IsGuideReferenceOn(SnapGuide::EReference::ServiceRoad); },
			Always));
		Out.Add(Make(TEXT("snapto.runway"), EActionSection::SnapTo, LOCTEXT("SnapToRunway", "Runway"),
			EKeys::Invalid, false,
			[](ARoadBuildController& C) { C.ToggleGuideReference(SnapGuide::EReference::Runway); },
			[](const ARoadBuildController& C) { return C.IsGuideReferenceOn(SnapGuide::EReference::Runway); },
			Always));
		Out.Add(Make(TEXT("snapto.apron"), EActionSection::SnapTo, LOCTEXT("SnapToApron", "Apron"),
			EKeys::Invalid, false,
			[](ARoadBuildController& C) { C.ToggleGuideReference(SnapGuide::EReference::Apron); },
			[](const ARoadBuildController& C) { return C.IsGuideReferenceOn(SnapGuide::EReference::Apron); },
			Always));
		Out.Add(Make(TEXT("snapto.stand"), EActionSection::SnapTo, LOCTEXT("SnapToStand", "Stand"),
			EKeys::Invalid, false,
			[](ARoadBuildController& C) { C.ToggleGuideReference(SnapGuide::EReference::Stand); },
			[](const ARoadBuildController& C) { return C.IsGuideReferenceOn(SnapGuide::EReference::Stand); },
			Always));
		Out.Add(Make(TEXT("snapto.world"), EActionSection::SnapTo, LOCTEXT("SnapToWorld", "World"),
			EKeys::Invalid, false,
			[](ARoadBuildController& C) { C.ToggleGuideReference(SnapGuide::EReference::World); },
			[](const ARoadBuildController& C) { return C.IsGuideReferenceOn(SnapGuide::EReference::World); },
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
	if (!IsEnabled(C))
	{
		return false;
	}
	UE_LOG(LogRoadBuild, Log, TEXT("%s: %s"), Via, *Id.ToString());
	Execute(C);
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
