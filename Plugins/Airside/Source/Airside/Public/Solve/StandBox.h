#pragma once

#include "CoreMinimal.h"
#include "Solve/IcaoCode.h"
#include "Solve/LetterEnvelope.h"

/**
 * A drawn stand rectangle and the aircraft's stop-mark pose, as ONE geometry read both ways.
 *
 * The player draws a rectangle off a taxiway - an entrance edge and how far it was dragged
 * inward - and that single gesture has to become both the pose an arrival stops at
 * (PoseFor) and, read back from a saved box, the letter and dimensions that pose implies
 * (WidthOf/DepthOf/LetterOf). BoxAt is PoseFor's exact inverse: building a box from a pose
 * and a letter, then reading that box's letter back, must return the letter it was built
 * from - that round trip is what the commit and migration tasks after this one depend on.
 *
 * Dependency-free like the rest of Solve/: CoreMinimal.h and Solve/ only, no engine types
 * beyond it.
 */
namespace StandBox
{
	/** The aircraft's stop-mark pose: Position is the nose-gear stop mark, Facing is the
	 *  unit nose direction (away from the taxiway the stand opens off). */
	struct FStandPose
	{
		FVector2D Position = FVector2D::ZeroVector;
		FVector2D Facing = FVector2D(1, 0);
	};

	/**
	 * How far in from the entrance edge, along Facing, the stop mark sits: the letter's tail
	 * setback off the entrance PLUS its wingtip clearance, so a parked tail clears the
	 * taxiway pavement by the same margin Annex 14 gives its wingtip, not laid flush against
	 * it. Decided 2026-09-26 (far-side-entry spec): service vehicles now enter from the FAR
	 * (nose-side) edge, so the slack that used to sit behind the tail moves ahead of the nose
	 * instead, where the far-side entry needs the room.
	 *
	 * PoseFor and BoxAt both build off this ONE figure, so the stop mark and the box's own
	 * back edge agree by construction.
	 */
	AIRSIDE_API double EntranceSetback(EIcaoCode Letter, const FLetterEnvelope& Envelope);

	/**
	 * How far in from the FAR edge (the one furthest from the taxiway), along Facing, the stop
	 * mark sits: the letter's floor depth less EntranceSetback, i.e. the nose overhang plus
	 * the service ground ahead of it on a floor-sized stand. PoseFor measures from here.
	 */
	AIRSIDE_API double FarSetback(EIcaoCode Letter, const FLetterEnvelope& Envelope);

	/**
	 * The stop-mark pose for a stand entered along EntranceA->EntranceB, dragged Inward, whose
	 * drawn corners are Outline (any winding, any start corner).
	 *
	 * MEASURED FROM THE FAR EDGE since 2026-09-27 (user: "the measurements should be taken
	 * from the other end of the stand ... so the aircraft uses all of the depth of the stand
	 * and the service points and vehicle routes should also be the same"). The stop mark sits
	 * FarSetback in from the outline's furthest reach along Inward, centred on the entrance.
	 * The stand's service template, service points and far-edge road contacts are placed
	 * relative to this pose, so on a stand drawn deeper than its floor they land on the drawn
	 * far edge and the extra depth lies behind the tail. Was: EntranceSetback in from the
	 * entrance (the 2026-09-26 far-side-entry ruling), which left a deep stand's extra depth as
	 * dead ground ahead of the nose and its far-edge contacts short of the far edge. At exactly
	 * the floor depth the two measurements coincide, so a floor-sized stand has not moved.
	 * An Outline of fewer than three points is read as the letter's floor depth.
	 * ENFORCED BY: Airside.Solve.StandBox.MeasuredFromTheFarEdge
	 *
	 * Inward need not be perpendicular to EntranceA-EntranceB or unit length; only its
	 * direction is read (GetSafeNormal), so a freeform drag still yields a clean pose.
	 *
	 * ENVELOPE BY REFERENCE, since #292: MaxNoseFwd used to be IcaoCode::MaxNoseFwdForLetter's
	 * own lookup - now a fleet figure UAirsideSettings::ResolveLetterEnvelope raises past its
	 * authored floor, so the caller resolves it once (per rebuild, or IcaoCode::
	 * FloorEnvelopeForLetter where Content/ is unreachable - see that function's own comment)
	 * and hands it in, rather than this Solve/ function reaching into Content/ itself.
	 */
	AIRSIDE_API FStandPose PoseFor(const FVector2D& EntranceA, const FVector2D& EntranceB,
		const FVector2D& Inward, TArrayView<const FVector2D> Outline, EIcaoCode Letter,
		const FLetterEnvelope& Envelope);

	/**
	 * Which edge of Outline is the stand's ENTRANCE, as the index i of the edge Outline[i] -> Outline[(i + 1) % Num]: the edge whose
	 * midpoint lies furthest BEHIND Position along Facing. INDEX_NONE for an outline of under three points.
	 *
	 * THE ONE SEARCH, HERE FOR THE ONE WRITER THAT STILL NEEDS IT (#450's leftover). UStandDefinitionCache::PoseFromOutline ran this on every
	 * load and FStandMarkingBuilder::FrameFor ran a corner version of it on every rebuild; FEntityInstance::FrontageEdge stores the answer
	 * now and both read it. What still asks is a WRITER of that field for a stand given no edge: URoadNetwork::PlaceEntity (a
	 * point-placed or fixture stand) and URoadNetwork::EnsureStandFrontages (a stand saved before the field).
	 *
	 * MEASURED, NOT READ FROM A CORNER INDEX: the facade reverses a clockwise outline, which puts the drawn FAR edge at 0 -> 1, so no
	 * index is reliably the entrance. The stand's facing survives any change of geometry, so the entrance is whatever sits furthest
	 * behind the stop mark along it - whatever rule placed that mark. Position is the stop mark the midpoints are measured from;
	 * it shifts every edge by the same amount and so cannot change which is rearmost, but it is taken (and subtracted, as the
	 * reader always did) so the answer is bit-for-bit the one the readers gave. An exact tie keeps the FIRST edge, as the reader did.
	 * ENFORCED BY: Airside.Solve.StandBox.EntranceEdgeIsTheRearmostMidpoint
	 */
	AIRSIDE_API int32 EntranceEdgeOf(TArrayView<const FVector2D> Outline, const FVector2D& Position, const FVector2D& Facing);

	/**
	 * PoseFor's inverse: the four corners of the letter's box at this pose, entrance edge
	 * first. OutCorners is entrance-A, entrance-B, then the two corners inward of them, so
	 * the entrance edge is corners 0->1 and PolygonArea is POSITIVE - the winding the pad
	 * triangulator needs.
	 *
	 * Envelope BY REFERENCE for PoseFor's own reason - MUST be the same envelope PoseFor built
	 * this Pose from, or BoxAt's box will not agree with the pose's own derivation.
	 */
	AIRSIDE_API void BoxAt(const FStandPose& Pose, EIcaoCode Letter, const FLetterEnvelope& Envelope,
		TArray<FVector2D>& OutCorners);

	/** |Rect[1] - Rect[0]| - the entrance edge's own length, ROUNDED TO A WHOLE uu so a stand
	 *  drawn exactly at a floor off the axes reads as that floor (see the .cpp). Rect
	 *  convention: entrance edge 0->1, then inward to 2 and 3, as BoxAt produces and a drawn
	 *  stand is saved. */
	AIRSIDE_API double WidthOf(TArrayView<const FVector2D> Rect);

	/** |Rect[2] - Rect[1]| - how far the entrance edge was dragged inward, rounded to a whole
	 *  uu for WidthOf's reason. */
	AIRSIDE_API double DepthOf(TArrayView<const FVector2D> Rect);

	/**
	 * The letter a drawn rectangle reads as - IcaoCode::LetterForStandSize on
	 * WidthOf/DepthOf, parsed back to the enum. Unset for a Rect with fewer than four points
	 * (nothing to measure) or one too small for any letter (LetterForStandSize's own empty
	 * answer - see its header for why that is refused rather than rounded up to Code A).
	 */
	AIRSIDE_API TOptional<EIcaoCode> LetterOf(TArrayView<const FVector2D> Rect);
}
