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

	/** The line an existing thing already lies on. */
	UPROPERTY(EditAnywhere, Category = "Align by")
	bool bCollinear = false;

	/** The gap a neighbouring parallel road already keeps. Judged in PIE first. */
	UPROPERTY(EditAnywhere, Category = "Align by")
	bool bMatchingGap = false;

	// --- What it is measured AGAINST --------------------------------------------------

	/**
	 * Taxiways and service roads.
	 *
	 * NEW ON 2026-09-20, and it is the column that had no switch: Parallel and Collinear each
	 * carried "a road" as an unnamed reference, which is half of why Runway could not mean what
	 * it was read to mean. ThisGesture is the only column with NO flag - see the design §7.
	 */
	UPROPERTY(EditAnywhere, Category = "Snap to")
	bool bRoad = true;

	/** Runways - any segment whose profile is continuous through junctions. */
	UPROPERTY(EditAnywhere, Category = "Snap to")
	bool bRunway = false;

	/** Apron edges and corners. */
	UPROPERTY(EditAnywhere, Category = "Snap to")
	bool bApron = false;

	/** A placed entity's pose - a stand, a depot. Was `bAligned`, which named a relation. */
	UPROPERTY(EditAnywhere, Category = "Snap to")
	bool bStand = false;

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
