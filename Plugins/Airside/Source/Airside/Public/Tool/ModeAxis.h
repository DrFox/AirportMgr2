#pragma once

#include "CoreMinimal.h"
#include "Tool/RoadBuildTool.h"

/**
 * What a tool's click does to the ground: lay something new, or change a piece already laid
 * (strip stage 6). USER 2026-09-29: "an upgrade mode rather than a specific widen and we can
 * then use that for surface as well as width and other tools can use the same terminology" -
 * so the words live HERE, once, and a later tool (a stand re-pave, a runway resurface) appends
 * the same row rather than naming its own. A mode, not a gesture: memory's "destructive
 * gestures need a deliberate mode" - an upgrade changes placed geometry.
 */
enum class EToolMode : uint8
{
	Build,
	Upgrade,
};

namespace ModeAxis
{
	/** The row's Id, "Mode" - what a tool's SelectVariant keys on (the Pavement::AppendAxis
	 *  "Surface" precedent: rows are looked up by Id, never by index). */
	AIRSIDE_API FName AxisId();

	/** An option's Id and label, "Build" / "Upgrade" - the one spelling. */
	AIRSIDE_API FName OptionId(EToolMode Mode);

	/**
	 * The "Mode" variant row, Build then Upgrade, lighting Current. ONE BUILDER so two tools
	 * cannot name or order it differently.
	 * ENFORCED BY: Airside.Tool.UpgradeMode (the taxiway and road tools' first row is this one)
	 */
	AIRSIDE_API void AppendAxis(TArray<FToolVariantAxis>& Out, EToolMode Current);

	/** The mode an option index of that row names; Build for anything out of range. */
	AIRSIDE_API EToolMode ModeAt(int32 Option);
}
