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
 *
 * EVERY WRITE GOES THROUGH SelectionDoor::Write (#446), which is what makes FBuildSession::OnSelectionChanged fire exactly once per
 * REAL change and never for a re-select of what is already selected. The inspector used to learn of a change by comparing (Kind, Id)
 * with the last one it saw, every tick.
 * ENFORCED BY: Check-Architecture rule 75 (no production write to a selection outside this file, the session and the context's door),
 * Airside.Tool.BuildSession.SelectionChangedFiresOncePerChange
 */
struct FSelection
{
	ESelectionKind Kind = ESelectionKind::None;
	int32 Id = 0;

	/**
	 * The GENERATION of the slot Id indexes, for the kinds whose Id is a slot index (Stand, Runway, Taxiway); 0 for an aircraft, and for a
	 * selection written with no network to read it from (FBuildSession::Select's default). A slot map reuses an index once its item is
	 * removed and bumps the generation when it does, so (Id, Generation) names ONE item where Id alone named whatever later lived there: a
	 * selected stand, deleted, and another placed in its slot, used to be silently selected instead. Real generations start at 1, so 0 is free
	 * to mean "not recorded" - FSelectTool::IsStale skips the check for it rather than call every code-made selection stale.
	 */
	int32 Generation = 0;

	bool IsSet() const { return Kind != ESelectionKind::None; }

	/** The same thing selected - kind, slot and generation. What "a real change" is compared by. */
	bool operator==(const FSelection& Other) const { return Kind == Other.Kind && Id == Other.Id && Generation == Other.Generation; }
	bool operator!=(const FSelection& Other) const { return !(*this == Other); }
	/** Raw clear, for a value a caller holds. A SESSION'S selection is cleared through the door, never with this - see SelectionDoor. */
	void Clear() { Kind = ESelectionKind::None; Id = 0; Generation = 0; }
};

/**
 * The selection changed: what it was, and what it is now. Fired by SelectionDoor::Write after the new value is in place, so a subscriber
 * that reads the session's selection sees the new one. Native, not a dynamic delegate: FBuildSession is not a UObject, and its
 * subscribers (the controller, the inspector) bind by UObject weak pointer, which this supports.
 */
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnSelectionChanged, const FSelection& /*Old*/, const FSelection& /*New*/);

namespace SelectionDoor
{
	/**
	 * THE ONE WRITE: sets *Slot to Wanted and tells Listeners what it was and what it is, and does NOTHING when Wanted is what is already
	 * selected - so a re-click on the selected stand is not a change. A null Slot writes nothing (a context with no session); a null
	 * Listeners writes without announcing (a headless test's bare FSelection, which has nobody to tell). None is normalised: a
	 * selection of nothing has no id and no generation, however the caller spelled it. True when it changed.
	 *
	 * HERE, AS A FREE FUNCTION OVER A POINTER PAIR, because the writers are not one class: FToolContext::SetSelection (the tools) and
	 * FBuildSession (Select, SelectTool, OnNetworkReplaced) both write the session's one FSelection, and two copies of "compare, store,
	 * announce" are two chances to announce twice or not at all.
	 */
	inline bool Write(FSelection* Slot, FOnSelectionChanged* Listeners, FSelection Wanted)
	{
		if (Slot == nullptr)
		{
			return false;
		}
		if (Wanted.Kind == ESelectionKind::None)
		{
			Wanted.Clear();
		}
		if (*Slot == Wanted)
		{
			return false;
		}
		const FSelection Old = *Slot;
		*Slot = Wanted;
		if (Listeners != nullptr)
		{
			Listeners->Broadcast(Old, Wanted);
		}
		return true;
	}
}
