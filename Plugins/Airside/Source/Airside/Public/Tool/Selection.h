#pragma once

#include "CoreMinimal.h"

/**
 * What the player has clicked on. AN ENUM AND AN ID, not two optional ids: an aircraft and
 * a stand can never both be selected, so the state that would need a tie-break rule is not
 * representable (CLAUDE.md, "a phase is an enum, never a set of bools"). Vehicle is added
 * here the day one exists; the panel that reads this does not change shape for it.
 */
enum class ESelectionKind : uint8
{
	None,
	/** Id is a UGroundTraffic agent id. */
	Aircraft,
	/** Id is an entity INDEX into URoadNetwork::GetEntities() - what FindEntityAt returns. */
	Stand
};

/**
 * Lives on FBuildSession, WRITTEN ONLY by FSelectTool (through FToolContext::Selection), read
 * by the inspector panel and the HUD. Plain struct, not a USTRUCT: it is runtime UI state
 * that never reaches disk or Blueprint.
 */
struct FSelection
{
	ESelectionKind Kind = ESelectionKind::None;
	int32 Id = 0;

	bool IsSet() const { return Kind != ESelectionKind::None; }
	void Clear() { Kind = ESelectionKind::None; Id = 0; }
};
