#include "Solve/GuideArbiter.h"

bool SnapGuide::IsLegalCell(ERelation Relation, EReference Reference)
{
	// SPELT OUT POSITIVELY, one row at a time, rather than as exclusions. "Every reference but
	// ThisGesture" would be shorter and would silently admit the NEXT reference anyone adds -
	// a new column has to be argued for cell by cell, which is the whole point of the grid.
	//
	// NO `default:`, matching FSnapGuideSettings::IsEnabled: a relation added without a row
	// here falls past the switch to `false`, so it proposes nothing rather than everything.
	switch (Relation)
	{
	case ERelation::Extending:
		// The edge this gesture is already growing. There is no other edge it could mean.
		return Reference == EReference::ThisGesture;

	case ERelation::LevelWith:
		// A point to be level with. A runway's alignable points are its thresholds, which are
		// ordinary nodes already served by Road; a world axis has no position at all.
		return Reference == EReference::ThisGesture
			|| Reference == EReference::Road
			|| Reference == EReference::Apron
			|| Reference == EReference::Stand;

	case ERelation::Parallel:
		// A direction. ThisGesture is absent because parallel-to-your-own-edge IS Extending,
		// and a second name for one behaviour is what this split exists to remove.
		return Reference == EReference::Road
			|| Reference == EReference::Runway
			|| Reference == EReference::Apron
			|| Reference == EReference::Stand
			|| Reference == EReference::World;

	case ERelation::Collinear:
		// A line to be ON, so the reference needs a position as well as a direction. That is
		// every column but World, which is a direction and nothing else.
		return Reference == EReference::ThisGesture
			|| Reference == EReference::Road
			|| Reference == EReference::Runway
			|| Reference == EReference::Apron
			|| Reference == EReference::Stand;

	case ERelation::AngledFrom:
		// A line out of a reference's END, so the reference needs an end to radiate from - a
		// segment's node, an apron edge's corner. A stand is a point and a direction with no
		// end; a world axis has no position at all.
		//
		// THISGESTURE IS ABSENT, and this is the one hole here that is not about geometry:
		// LevelWith x ThisGesture already proposes lines through every pinned corner along the
		// anchor's reference AND across it, which IS the 0 and 90 degree members of this family
		// off the same direction. The 90 degree spoke would be the identical line under a second
		// name, which is the duplication the two axes were split apart to remove.
		return Reference == EReference::Road
			|| Reference == EReference::Runway
			|| Reference == EReference::Apron;

	case ERelation::MatchingGap:
		// Road alone. Its reference must be the same nearest road FParallelGuideSource picks,
		// or the two guides stop describing one road between them - see the design section 5.
		// Runway-to-taxiway separation is a real standard and a legitimate future cell, but it
		// needs its own search rather than a free ride on this one.
		return Reference == EReference::Road;
	}
	return false;
}
