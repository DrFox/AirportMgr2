#include "Solve/GuideArbiter.h"

bool SnapGuide::IsLegalCell(ERelation Relation, EReference Reference)
{
	// SPELT OUT POSITIVELY, one row at a time, rather than as exclusions. "Every reference but
	// ThisGesture" would be shorter and would silently admit the NEXT reference anyone adds -
	// a new column has to be argued for cell by cell, which is the whole point of the grid.
	// Not hypothetical: Taxiway and ServiceRoad arrived on 2026-09-20 as exactly such a column
	// each, and every row that had said "Road" had to be argued through twice rather than once.
	//
	// NO `default:`, matching FSnapGuideSettings::IsEnabled: a relation added without a row
	// here falls past the switch to `false`, so it proposes nothing rather than everything.
	switch (Relation)
	{
	case ERelation::Extending:
		// The edge this gesture is already growing. There is no other edge it could mean.
		return Reference == EReference::ThisGesture;

	case ERelation::LevelWith:
		// A point to be level with: a node, a corner, a pose. Every column that HAS points.
		//
		// RUNWAY JOINED THIS ROW ON 2026-09-20, AND THE SPLIT IS WHAT PUT IT THERE. The hole
		// used to read "a runway's alignable points are its thresholds, which are ordinary
		// nodes already served by Road" - true while one column covered every node on the
		// graph, and false the moment that column became two. A threshold's node is incident
		// to a RUNWAY segment and to nothing else, so it now answers to this column or to
		// none at all; and being level with a threshold is how a parallel taxiway's end gets
		// placed. Note this is not merely a hole filled, it is a capability KEPT: the split
		// would otherwise have quietly taken it away with the column it used to ride on.
		//
		// WORLD IS STILL ABSENT: a world axis is a direction with no position at all, so
		// there is no point in it to be level with.
		return Reference == EReference::ThisGesture
			|| Reference == EReference::Taxiway
			|| Reference == EReference::ServiceRoad
			|| Reference == EReference::Runway
			|| Reference == EReference::Apron
			|| Reference == EReference::Stand;

	case ERelation::Parallel:
		// A direction. ThisGesture is absent because parallel-to-your-own-edge IS Extending,
		// and a second name for one behaviour is what this split exists to remove.
		return Reference == EReference::Taxiway
			|| Reference == EReference::ServiceRoad
			|| Reference == EReference::Runway
			|| Reference == EReference::Apron
			|| Reference == EReference::Stand
			|| Reference == EReference::World;

	case ERelation::Collinear:
		// A line to be ON, so the reference needs a position as well as a direction. That is
		// every column but World, which is a direction and nothing else.
		return Reference == EReference::ThisGesture
			|| Reference == EReference::Taxiway
			|| Reference == EReference::ServiceRoad
			|| Reference == EReference::Runway
			|| Reference == EReference::Apron
			|| Reference == EReference::Stand;

	case ERelation::AngledFrom:
		// A line out of a reference's END, so the reference needs an end to radiate from - a
		// segment's node, an apron edge's corner. A world axis has no position at all.
		//
		// STAND JOINED THIS ROW ON 2026-09-24. The hole read "a stand is a point and a direction
		// with no end", which was true of the only Stand source then - FAlignedGuideSource reads
		// the POSE - and stopped being true when every stand gained an outline (drawn, or the
		// Code C box EnsureStandOutlines gives an old one) and a fuel depot its plot. Their
		// corners are ends exactly as an apron's are, and the Plots instance of
		// FApronAngledGuideSource throws spokes from them.
		//
		// THISGESTURE IS ABSENT, and this is the one hole here that is not about geometry:
		// LevelWith x ThisGesture already proposes lines through every pinned corner along the
		// anchor's reference AND across it, which IS the 0 and 90 degree members of this family
		// off the same direction. The 90 degree spoke would be the identical line under a second
		// name, which is the duplication the two axes were split apart to remove.
		return Reference == EReference::Taxiway
			|| Reference == EReference::ServiceRoad
			|| Reference == EReference::Runway
			|| Reference == EReference::Apron
			|| Reference == EReference::Stand;

	case ERelation::MatchingGap:
		// THE TWO ROAD COLUMNS, AND THEY ARE GENUINELY DIFFERENT STANDARDS. ICAO separates
		// taxiways by the wingspan admitted; what a service road keeps from the next one is a
		// question of what has to drive between them. Offering a taxiway's spacing as a
		// service road's is exactly the confusion the split exists to make unrepresentable, so
		// FOffsetGuideSource requires the pair it measures between to be of ONE kind.
		//
		// Its reference must also be the same nearest road FParallelGuideSource picks, or the
		// two guides stop describing one road between them - see the design section 5. Runway
		// separation is a real standard and a legitimate future cell, but it needs its own
		// search rather than a free ride on this one.
		return Reference == EReference::Taxiway
			|| Reference == EReference::ServiceRoad;
	}
	return false;
}
