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
 * NAMED BOOLS, NOT AN ARRAY INDEXED BY ESource. SnapGuide::ESource is a plain enum in a
 * Solve/ header with no .generated.h - UHT cannot see it - so a TArray<bool> keyed by it
 * would serialise BY INDEX, and reordering the enum would silently repoint every toggle a
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

	/** The edge the gesture is already extending, and its perpendicular. */
	UPROPERTY(EditAnywhere)
	bool bExtending = true;

	/** Lines through the gesture's own pinned corners. On with Extending: the same geometry. */
	UPROPERTY(EditAnywhere)
	bool bPointAlign = true;

	/** A placed entity's pose direction. */
	UPROPERTY(EditAnywhere)
	bool bAligned = false;

	/** The line an existing segment already lies on. */
	UPROPERTY(EditAnywhere)
	bool bCollinear = false;

	/** The nearest road's direction. */
	UPROPERTY(EditAnywhere)
	bool bParallel = true;

	/** Every runway's heading. */
	UPROPERTY(EditAnywhere)
	bool bRunway = false;

	/** 0/45/90/135 degrees. */
	UPROPERTY(EditAnywhere)
	bool bWorld = true;

	/** The gap a neighbouring parallel road keeps. Nothing proposes this until stage 4. */
	UPROPERTY(EditAnywhere)
	bool bOffset = false;

	/** Whether this source may propose at all. The ONE mapping from the enum to these flags. */
	bool IsEnabled(SnapGuide::ESource Source) const;

	/** Flips one. What the bar button does. */
	void Toggle(SnapGuide::ESource Source);
};
