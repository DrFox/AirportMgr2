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
	Stand,
	/**
	 * Id is a runway SEGMENT index into URoadNetwork::GetSegments() - any member of the strip,
	 * since the facts are the chain's (RunwayQuery::RunwaySegmentAt). Added 2026-09-28 so the
	 * runway in use has a card to be flipped from; an index, like Stand's, because the
	 * segment-index seams (SetRunwayFacts) already take one. A split kills the segment, and
	 * FSelectTool::Tick then clears the selection as it does a deleted stand's.
	 */
	Runway,
	/**
	 * Id is a taxiway SEGMENT index into URoadNetwork::GetSegments() (strip stage 6) - its card
	 * says its letter, strip and any restriction. APPENDED: a value's meaning never moves. Per
	 * segment, not per chain: a restriction is per segment (stage 6 plan ruling 4), so the card
	 * the player opens is the piece whose letter they are reading.
	 */
	Taxiway,

	/**
	 * How many kinds there are - NOT a kind; nothing selects it. The inspector's static_assert
	 * counts its cards against this, so a kind appended above without a card fails to COMPILE
	 * (review fix 3). Stays last: append new kinds above it.
	 */
	Count
};

/**
 * Lives on FBuildSession and is read by the inspector panel and the HUD. It has SEVERAL WRITERS, and this comment named one
 * (FSelectTool) until #448, by which time that had been false for weeks: the Select tool picks and clears it
 * through FToolContext::Selection; the session itself clears it when another tool is lit or a network is replaced, and sets it
 * from code (FBuildSession::Select - an alert's Go). A reader must not assume which of them wrote what it sees. Plain struct,
 * not a USTRUCT: it is runtime UI state that never reaches disk or Blueprint.
 */
struct FSelection
{
	ESelectionKind Kind = ESelectionKind::None;
	int32 Id = 0;

	bool IsSet() const { return Kind != ESelectionKind::None; }
	void Clear() { Kind = ESelectionKind::None; Id = 0; }
};
