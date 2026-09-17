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
		TEXT("Snap"),
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
		Out.Add(Make(TEXT("edit.remove"), EActionSection::Edit, LOCTEXT("Remove", "Remove"), EKeys::Invalid, false,
			[](ARoadBuildController& C) { C.ToggleClickModifier(EClickModifier::Remove); },
			[](const ARoadBuildController& C) { return C.GetClickModifier() == EClickModifier::Remove; }, Always));
		Out.Add(Make(TEXT("edit.insert"), EActionSection::Edit, LOCTEXT("Insert", "Insert"), EKeys::Invalid, false,
			[](ARoadBuildController& C) { C.ToggleClickModifier(EClickModifier::Insert); },
			[](const ARoadBuildController& C) { return C.GetClickModifier() == EClickModifier::Insert; }, Always));
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

		// ONE PER ESource, and AirportMgr.Actions.SnapTogglesAreInTheRegistry walks the enum
		// against this list rather than counting it - a source added without a toggle is a
		// guide the player cannot switch off, and nothing else would say so.
		//
		// NO KEYS. Eight more bindings would crowd a keyboard already spending 0-9 on tools,
		// and a toggle is set once rather than reached for mid-drag. What mid-drag needs is the
		// Alt hold, which is not a registry action - see FToolContext::bSuspendGuides.
		Out.Add(Make(TEXT("snap.extending"), EActionSection::Snap, LOCTEXT("SnapExtending", "Extending"),
			EKeys::Invalid, false,
			[](ARoadBuildController& C) { C.ToggleGuideSource(SnapGuide::ESource::Extending); },
			[](const ARoadBuildController& C) { return C.IsGuideSourceOn(SnapGuide::ESource::Extending); },
			Always));
		Out.Add(Make(TEXT("snap.pointalign"), EActionSection::Snap, LOCTEXT("SnapPointAlign", "Point"),
			EKeys::Invalid, false,
			[](ARoadBuildController& C) { C.ToggleGuideSource(SnapGuide::ESource::PointAlign); },
			[](const ARoadBuildController& C) { return C.IsGuideSourceOn(SnapGuide::ESource::PointAlign); },
			Always));
		Out.Add(Make(TEXT("snap.aligned"), EActionSection::Snap, LOCTEXT("SnapAligned", "Aligned"),
			EKeys::Invalid, false,
			[](ARoadBuildController& C) { C.ToggleGuideSource(SnapGuide::ESource::Aligned); },
			[](const ARoadBuildController& C) { return C.IsGuideSourceOn(SnapGuide::ESource::Aligned); },
			Always));
		Out.Add(Make(TEXT("snap.collinear"), EActionSection::Snap, LOCTEXT("SnapCollinear", "Collinear"),
			EKeys::Invalid, false,
			[](ARoadBuildController& C) { C.ToggleGuideSource(SnapGuide::ESource::Collinear); },
			[](const ARoadBuildController& C) { return C.IsGuideSourceOn(SnapGuide::ESource::Collinear); },
			Always));
		Out.Add(Make(TEXT("snap.parallel"), EActionSection::Snap, LOCTEXT("SnapParallel", "Parallel"),
			EKeys::Invalid, false,
			[](ARoadBuildController& C) { C.ToggleGuideSource(SnapGuide::ESource::Parallel); },
			[](const ARoadBuildController& C) { return C.IsGuideSourceOn(SnapGuide::ESource::Parallel); },
			Always));
		Out.Add(Make(TEXT("snap.runway"), EActionSection::Snap, LOCTEXT("SnapRunway", "Runway"),
			EKeys::Invalid, false,
			[](ARoadBuildController& C) { C.ToggleGuideSource(SnapGuide::ESource::Runway); },
			[](const ARoadBuildController& C) { return C.IsGuideSourceOn(SnapGuide::ESource::Runway); },
			Always));
		Out.Add(Make(TEXT("snap.world"), EActionSection::Snap, LOCTEXT("SnapWorld", "World"),
			EKeys::Invalid, false,
			[](ARoadBuildController& C) { C.ToggleGuideSource(SnapGuide::ESource::World); },
			[](const ARoadBuildController& C) { return C.IsGuideSourceOn(SnapGuide::ESource::World); },
			Always));

		// LIVE SINCE STAGE 5: FOffsetGuideSource proposes for it, so the button is no longer a
		// promise. It was greyed rather than absent precisely so this change is one word.
		Out.Add(Make(TEXT("snap.offset"), EActionSection::Snap, LOCTEXT("SnapOffset", "Offset"),
			EKeys::Invalid, false,
			[](ARoadBuildController& C) { C.ToggleGuideSource(SnapGuide::ESource::Offset); },
			[](const ARoadBuildController& C) { return C.IsGuideSourceOn(SnapGuide::ESource::Offset); },
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
