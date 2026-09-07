#include "BuildActions.h"
#include "RoadBuildController.h"
#include "Tool/BuildSession.h"

#define LOCTEXT_NAMESPACE "AirportMgr"

const TCHAR* ActionSectionName(EActionSection Section)
{
	switch (Section)
	{
	case EActionSection::Time:     return TEXT("Time");
	case EActionSection::Tools:    return TEXT("Tools");
	case EActionSection::Edit:     return TEXT("Edit");
	case EActionSection::Aircraft: return TEXT("Aircraft");
	case EActionSection::Selection: return TEXT("Selection");
	case EActionSection::Game:     return TEXT("Game");
	}
	return TEXT("?");
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
		return Out;
	}
}

TConstArrayView<FBuildAction> BuildActions()
{
	static const TArray<FBuildAction> Actions = MakeActions();
	return Actions;
}

#undef LOCTEXT_NAMESPACE
