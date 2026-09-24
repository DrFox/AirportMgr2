#pragma once

#include "CoreMinimal.h"
#include "Solve/GuideArbiter.h"
#include "SnapGuideSettings.generated.h"

/**
 * Which guide sources are live, per airport.
 *
 * BESIDE ARoadNetworkActor::Snap and for the same recorded reason as FRoadSnapSettings: the
 * editor mode and PIE must agree about what is switched on, and a per-driver copy is how the
 * two came to disagree about snap radii before issue #93 merged them.
 *
 * NAMED BOOLS, NOT AN ARRAY INDEXED BY THE ENUMS. ERelation and EReference are plain enums in a
 * Solve/ header with no .generated.h - UHT cannot see it - so a TArray<bool> keyed by it
 * would serialise BY INDEX, and reordering either enum would silently repoint every toggle a
 * player had set. These are reflected, appear in the details panel with their own tooltips,
 * and survive a reorder. The cost is one switch in IsEnabled, which the registry test walks
 * the enum against.
 *
 * INDEPENDENT FLAGS, so bools are right here - CLAUDE.md's "a phase is an enum, never a set
 * of bools" is about states that cannot both be true, and any combination of these can.
 */
USTRUCT(BlueprintType)
struct AIRSIDE_API FSnapGuideSettings
{
	GENERATED_BODY()

	// --- What a guide MEANS -----------------------------------------------------------

	/** The edge the gesture is already extending, and its perpendicular. */
	UPROPERTY(EditAnywhere, Category = "Align by")
	bool bExtending = true;

	/** Lines through a point worth being level with. On with Extending: the same geometry. */
	UPROPERTY(EditAnywhere, Category = "Align by")
	bool bLevelWith = true;

	/** A direction to point along, and its perpendicular. */
	UPROPERTY(EditAnywhere, Category = "Align by")
	bool bParallel = true;

	/**
	 * The line an existing thing already lies on.
	 *
	 * ON SINCE 2026-09-20, and it was off for a reason that stopped being true. It meant one
	 * thing when it was written - "one candidate per road in reach", a nicety beside Parallel -
	 * and it now gates the apron's FLUSH guide, the runway's extended centreline, and every
	 * guide a FREE START can offer, since the positional rows are the only ones a gesture with
	 * no direction yet can propose. A player met that in PIE: switching this on was the
	 * undocumented step between "no edge alignment" and "works".
	 */
	UPROPERTY(EditAnywhere, Category = "Align by")
	bool bCollinear = true;

	/** A line out of a reference's end, at 45, 90 or 135 degrees to it. */
	UPROPERTY(EditAnywhere, Category = "Align by")
	bool bAngledFrom = false;

	/** The gap a neighbouring parallel road already keeps. Judged in PIE first. */
	UPROPERTY(EditAnywhere, Category = "Align by")
	bool bMatchingGap = false;

	// --- What it is measured AGAINST --------------------------------------------------

	/**
	 * Taxiways - the lanes aircraft use.
	 *
	 * TWO FLAGS SINCE 2026-09-20, where one said "Road". That single column was itself new
	 * that day - Parallel and Collinear had each carried "a road" as an unnamed, unswitchable
	 * reference, which is half of why Runway could not mean what it was read to mean - and it
	 * was collapsing two things nothing else in the codebase joins: two registry entries under
	 * two keys, two cross-sections, two traversal classes, and two different words already
	 * appearing in the label. See SnapGuide::EReference.
	 *
	 * BOTH START ON, because "Road" did: splitting a switch is not a reason to change what it
	 * was set to. ThisGesture remains the only column with NO flag - see the design §7.
	 */
	UPROPERTY(EditAnywhere, Category = "Snap to")
	bool bTaxiway = true;

	/** Service roads - what the vans and tugs drive on. See bTaxiway on why these are two. */
	UPROPERTY(EditAnywhere, Category = "Snap to")
	bool bServiceRoad = true;

	/** Runways - any segment whose profile is continuous through junctions. */
	UPROPERTY(EditAnywhere, Category = "Snap to")
	bool bRunway = false;

	/**
	 * Apron edges and corners. Four sources answer for it - see FApronGuideSource.
	 *
	 * ON SINCE 2026-09-24, for the reason bCollinear gives above: a player reported apron guides
	 * "lost" in PIE, and the log showed the column simply defaulted off. A guide the player must
	 * find a button to meet is a guide they conclude is broken.
	 */
	UPROPERTY(EditAnywhere, Category = "Snap to")
	bool bApron = true;

	/**
	 * A stand or fuel depot - its pose, and since 2026-09-24 its drawn outline's edges and
	 * corners (the Plots instances of the four outline sources; see EGuideOutlines). Was
	 * `bAligned`, which named a relation.
	 *
	 * ON SINCE 2026-09-24, beside bApron and for the same report: the edges a road is most often
	 * lined up with on an apron are the stands', and a column that shipped off hid all of them.
	 */
	UPROPERTY(EditAnywhere, Category = "Snap to")
	bool bStand = true;

	/** 0/45/90/135 degrees. */
	UPROPERTY(EditAnywhere, Category = "Snap to")
	bool bWorld = true;

	/**
	 * Whether this CELL may propose: both axes on, and the pair legal.
	 *
	 * THE AND IS THE WHOLE FIX. A column switched off removes every row in it, which is what
	 * the 2026-09-20 report asked for. IsLegalCell is consulted as well as the two flags so a
	 * hole cannot be reached by switching both its axes on.
	 */
	bool IsEnabled(SnapGuide::ERelation Relation, SnapGuide::EReference Reference) const;

	/** Whether this relation may propose at all, ignoring references. The bar's ALIGN BY row. */
	bool IsRelationOn(SnapGuide::ERelation Relation) const;

	/** Whether this reference may be used at all. The bar's SNAP TO row. */
	bool IsReferenceOn(SnapGuide::EReference Reference) const;

	/** Flips one row. What an ALIGN BY button does. */
	void ToggleRelation(SnapGuide::ERelation Relation);

	/** Flips one column. What a SNAP TO button does. */
	void ToggleReference(SnapGuide::EReference Reference);
};
