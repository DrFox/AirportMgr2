#include "Tool/SnapToggleRegistry.h"

#include "Tool/SnapGuideSettings.h"

#define LOCTEXT_NAMESPACE "SnapToggleRegistry"

namespace
{
	FSnapToggleRegistration Relation(const TCHAR* Id, SnapGuide::ERelation Which, FText Name, FText Tooltip)
	{
		FSnapToggleRegistration Toggle;
		Toggle.Id = FName(Id);
		Toggle.Group = ESnapToggleGroup::AlignBy;
		Toggle.Name = MoveTemp(Name);
		Toggle.Tooltip = MoveTemp(Tooltip);
		Toggle.Key = EKeys::Invalid;
		Toggle.Apply = [Which](FSnapGuideSettings& S) { S.ToggleRelation(Which); };
		Toggle.IsActive = [Which](const FSnapGuideSettings& S) { return S.IsRelationOn(Which); };
		return Toggle;
	}

	// THE ROW FLAG ALONE, not FSnapGuideSettings::IsEnabled: a button is lit when its own axis is
	// on, and a cell that happens to be a hole must not make the column look switched off. The
	// AND belongs in the chain, where a candidate is judged - not in what the bar draws. (Moved
	// with ARoadBuildController::IsGuideReferenceOn, which carried it before this table did.)
	FSnapToggleRegistration Reference(const TCHAR* Id, SnapGuide::EReference Which, FText Name, FText Tooltip)
	{
		FSnapToggleRegistration Toggle;
		Toggle.Id = FName(Id);
		Toggle.Group = ESnapToggleGroup::SnapTo;
		Toggle.Name = MoveTemp(Name);
		Toggle.Tooltip = MoveTemp(Tooltip);
		Toggle.Key = EKeys::Invalid;
		Toggle.Apply = [Which](FSnapGuideSettings& S) { S.ToggleReference(Which); };
		Toggle.IsActive = [Which](const FSnapGuideSettings& S) { return S.IsReferenceOn(Which); };
		return Toggle;
	}

	TArray<FSnapToggleRegistration> MakeToggles()
	{
		TArray<FSnapToggleRegistration> Out;

		// TWO LISTS, ONE PER AXIS, and AirportMgr.Actions.GuideGridIsInTheRegistry walks BOTH
		// enums against them rather than counting: a row or column added without a button is a
		// guide the player cannot switch, and nothing else would say so.
		//
		// NO KEYS. Ten more bindings would crowd a keyboard already spending 0-9 on tools, and a
		// toggle is set once rather than reached for mid-drag. What mid-drag needs is the Alt
		// hold, which is not a registry action - see FToolContext::bSuspendGuides.
		Out.Add(Relation(TEXT("snap.extending"), SnapGuide::ERelation::Extending,
			LOCTEXT("SnapExtending", "Extending"),
			LOCTEXT("SnapExtendingTooltip", "Guide along the edge the gesture is extending, and its perpendicular.")));
		Out.Add(Relation(TEXT("snap.levelwith"), SnapGuide::ERelation::LevelWith,
			LOCTEXT("SnapLevelWith", "Level with"),
			LOCTEXT("SnapLevelWithTooltip", "Guide through points worth being level with.")));
		// "DIRECTION", NOT "PARALLEL", although the relation behind it is ERelation::Parallel -
		// renamed 2026-09-20 after a player switched on Angled from and World, got nothing, and
		// pointed out that the row does three things and the button claimed one of them. It
		// offers a direction AND its perpendicular ("square to the taxiway" is not parallel to
		// anything), and for the World column an absolute compass axis, which is parallel to no
		// thing at all. The design doc's own grid already called the row "Parallel / square";
		// the button had taken the first word and dropped the rest.
		//
		// The enum keeps its name - see SnapGuide::ERelation::Parallel, which records this.
		Out.Add(Relation(TEXT("snap.direction"), SnapGuide::ERelation::Parallel,
			LOCTEXT("SnapDirection", "Direction"),
			LOCTEXT("SnapDirectionTooltip", "Guide along a direction and its square - of a thing, or of the world's axes.")));
		Out.Add(Relation(TEXT("snap.collinear"), SnapGuide::ERelation::Collinear,
			LOCTEXT("SnapCollinear", "Collinear"),
			LOCTEXT("SnapCollinearTooltip", "Guide along the line an existing thing already lies on.")));
		Out.Add(Relation(TEXT("snap.angledfrom"), SnapGuide::ERelation::AngledFrom,
			LOCTEXT("SnapAngledFrom", "Angled from"),
			LOCTEXT("SnapAngledFromTooltip", "Guide out of a reference's end, at 45, 90 or 135 degrees to it.")));
		Out.Add(Relation(TEXT("snap.matchinggap"), SnapGuide::ERelation::MatchingGap,
			LOCTEXT("SnapMatchingGap", "Matching gap"),
			LOCTEXT("SnapMatchingGapTooltip", "Guide to the gap a neighbouring parallel road already keeps.")));

		// THE WORLD GRID: one button that CYCLES, not three - the design's ruling (2026-09-27),
		// and why it needs a caption that follows state. Lit while any step is on. In the Snap
		// section because it is a way of aligning, not a thing to align against. No key, by the
		// rule above.
		{
			FSnapToggleRegistration Grid;
			Grid.Id = FName(TEXT("snap.grid"));
			Grid.Group = ESnapToggleGroup::AlignBy;
			Grid.Name = LOCTEXT("SnapGrid", "Grid");
			Grid.Tooltip = LOCTEXT("SnapGridTooltip", "Cycle the world grid: off, 1 m, 5 m, 10 m.");
			Grid.Key = EKeys::Invalid;
			Grid.Apply = [](FSnapGuideSettings& S) { S.CycleGridStep(); };
			Grid.IsActive = [](const FSnapGuideSettings& S) { return S.GridStepUu() > 0.0; };
			Grid.DynamicLabel = [](const FSnapGuideSettings& S)
			{
				const double Step = S.GridStepUu();
				return Step > 0.0
					? FText::Format(LOCTEXT("SnapGridOn", "Grid: {0} m"), FText::AsNumber(FMath::RoundToInt(Step / 100.0)))
					: LOCTEXT("SnapGridOff", "Grid: off");
			};
			Out.Add(MoveTemp(Grid));
		}

		// WHICH WAY THE GRID LIES: Follow turns it to what is snapped to, World keeps it square
		// to the map (grid-follows-snap design, 2026-09-28). Lit while following.
		//
		// NO KEY SINCE 2026-09-30 (owner ruling), like every other row here. It had one - H, from
		// 2026-09-28, a dated exception to the NO KEYS rule above because this toggle is reached for
		// mid-drag (lay a stand square to the map beside a diagonal taxiway) and a player asked. But
		// H was only unbound in PIE: the level editor binds it to Toggle Selected Hierarchy
		// Visibility, and once this table became both drivers' one list (#440) the Road Build mode's
		// toolkit would have taken H from the editor while the mode was active. The PIE bar button
		// and the editor's Snap palette both reach it; H is free again in both drivers.
		// ENFORCED BY: AirportMgr.Actions.GridOrientButtonHasNoKey, and
		// Airside.Editor.EveryCommandIsReachable (keyless, so it must be drawn in a palette).
		{
			FSnapToggleRegistration Orient;
			Orient.Id = FName(TEXT("snap.gridorient"));
			Orient.Group = ESnapToggleGroup::AlignBy;
			Orient.Name = LOCTEXT("SnapGridOrient", "Grid follows");
			Orient.Tooltip = LOCTEXT("SnapGridOrientTooltip", "Turn the grid to what is snapped to, or keep it square to the map.");
			Orient.Key = EKeys::Invalid;
			Orient.Apply = [](FSnapGuideSettings& S) { S.ToggleGridOrientation(); };
			Orient.IsActive = [](const FSnapGuideSettings& S) { return S.GridOrientation == EGridOrientation::Follow; };
			Orient.DynamicLabel = [](const FSnapGuideSettings& S)
			{
				return S.GridOrientation == EGridOrientation::Follow
					? LOCTEXT("SnapGridFollow", "Grid: follow")
					: LOCTEXT("SnapGridWorld", "Grid: world");
			};
			Out.Add(MoveTemp(Orient));
		}

		// THE SECOND AXIS. Before 2026-09-20 these sat in the same list as the rows above, which
		// is why "Runway" read as a source you could switch off for every relation and was not -
		// see SnapGuide::EReference.
		// TWO BUTTONS WHERE "Road" WAS ONE, since 2026-09-20. Everywhere else in this codebase
		// these are different tools under different keys, different cross-sections and
		// different traversal classes, and the guide LABEL already said which - "parallel to
		// the service road" appearing under a button marked Road was the whole complaint. See
		// SnapGuide::EReference.
		Out.Add(Reference(TEXT("snapto.taxiway"), SnapGuide::EReference::Taxiway,
			LOCTEXT("SnapToTaxiway", "Taxiway"),
			LOCTEXT("SnapToTaxiwayTooltip", "Measure guides against taxiways.")));
		Out.Add(Reference(TEXT("snapto.serviceroad"), SnapGuide::EReference::ServiceRoad,
			LOCTEXT("SnapToServiceRoad", "Service road"),
			LOCTEXT("SnapToServiceRoadTooltip", "Measure guides against service roads.")));
		Out.Add(Reference(TEXT("snapto.runway"), SnapGuide::EReference::Runway,
			LOCTEXT("SnapToRunway", "Runway"),
			LOCTEXT("SnapToRunwayTooltip", "Measure guides against runways - any segment continuous through its junctions.")));
		Out.Add(Reference(TEXT("snapto.apron"), SnapGuide::EReference::Apron,
			LOCTEXT("SnapToApron", "Apron"),
			LOCTEXT("SnapToApronTooltip", "Measure guides against apron edges.")));
		Out.Add(Reference(TEXT("snapto.stand"), SnapGuide::EReference::Stand,
			LOCTEXT("SnapToStand", "Stand"),
			LOCTEXT("SnapToStandTooltip", "Measure guides against stands and fuel depots - their pose and outline.")));
		Out.Add(Reference(TEXT("snapto.world"), SnapGuide::EReference::World,
			LOCTEXT("SnapToWorld", "World"),
			LOCTEXT("SnapToWorldTooltip", "Measure guides against the world's axes: 0, 45, 90 and 135 degrees.")));
		return Out;
	}
}

TConstArrayView<FSnapToggleRegistration> SnapToggleRegistry()
{
	// A function-local static, like ToolRegistry() and BuildVerbRegistry(), and for their reason:
	// built once on first use and never mutated, so a view over it is safe to hand out.
	static const TArray<FSnapToggleRegistration> Toggles = MakeToggles();
	return Toggles;
}

FString DescribeSnapToggle(const FSnapToggleRegistration& Toggle, const FSnapGuideSettings& Settings)
{
	if (Toggle.DynamicLabel)
	{
		return Toggle.DynamicLabel(Settings).ToString();
	}
	return FString::Printf(TEXT("%s: %s"), *Toggle.Name.ToString(),
		Toggle.IsActive && Toggle.IsActive(Settings) ? TEXT("on") : TEXT("off"));
}

#undef LOCTEXT_NAMESPACE
