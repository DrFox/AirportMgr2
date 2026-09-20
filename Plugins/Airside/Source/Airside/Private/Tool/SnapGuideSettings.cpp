#include "Tool/SnapGuideSettings.h"

bool FSnapGuideSettings::IsRelationOn(SnapGuide::ERelation Relation) const
{
	// NO `default:`. This project does not build switches as exhaustive-or-error, so a relation
	// added to ERelation without a case here would fall past the switch - which is why the
	// return below is `false` and AirportMgr.Actions.GuideGridIsInTheRegistry walks the enum:
	// a relation with no toggle is OFF and has no button, and the test says so out loud rather
	// than leaving a guide that quietly never fires.
	switch (Relation)
	{
	case SnapGuide::ERelation::Extending:   return bExtending;
	case SnapGuide::ERelation::LevelWith:   return bLevelWith;
	case SnapGuide::ERelation::Parallel:    return bParallel;
	case SnapGuide::ERelation::Collinear:   return bCollinear;
	case SnapGuide::ERelation::AngledFrom:  return bAngledFrom;
	case SnapGuide::ERelation::MatchingGap: return bMatchingGap;
	}
	return false;
}

bool FSnapGuideSettings::IsReferenceOn(SnapGuide::EReference Reference) const
{
	switch (Reference)
	{
	// THE ONE COLUMN WITH NO FLAG. Extending is the only cell in its row, so a ThisGesture
	// button and an Extending button would switch off exactly the same behaviour - two
	// controls for one thing. The Alt hold covers "not for this drag". See design section 7.
	case SnapGuide::EReference::ThisGesture: return true;
	case SnapGuide::EReference::Taxiway:     return bTaxiway;
	case SnapGuide::EReference::ServiceRoad: return bServiceRoad;
	case SnapGuide::EReference::Runway:      return bRunway;
	case SnapGuide::EReference::Apron:       return bApron;
	case SnapGuide::EReference::Stand:       return bStand;
	case SnapGuide::EReference::World:       return bWorld;
	}
	return false;
}

bool FSnapGuideSettings::IsEnabled(SnapGuide::ERelation Relation,
	SnapGuide::EReference Reference) const
{
	// THE GRID IS ASKED AS WELL AS THE TWO FLAGS, so a hole cannot be reached by switching both
	// its axes on. Without it, "Collinear" plus "World" would name a guide that is not a guide.
	return SnapGuide::IsLegalCell(Relation, Reference)
		&& IsRelationOn(Relation) && IsReferenceOn(Reference);
}

void FSnapGuideSettings::ToggleRelation(SnapGuide::ERelation Relation)
{
	switch (Relation)
	{
	case SnapGuide::ERelation::Extending:   bExtending   = !bExtending;   return;
	case SnapGuide::ERelation::LevelWith:   bLevelWith   = !bLevelWith;   return;
	case SnapGuide::ERelation::Parallel:    bParallel    = !bParallel;    return;
	case SnapGuide::ERelation::Collinear:   bCollinear   = !bCollinear;   return;
	case SnapGuide::ERelation::AngledFrom:  bAngledFrom  = !bAngledFrom;  return;
	case SnapGuide::ERelation::MatchingGap: bMatchingGap = !bMatchingGap; return;
	}
}

void FSnapGuideSettings::ToggleReference(SnapGuide::EReference Reference)
{
	switch (Reference)
	{
	// ThisGesture has no flag and so cannot be toggled - see IsReferenceOn.
	case SnapGuide::EReference::ThisGesture: return;
	case SnapGuide::EReference::Taxiway:     bTaxiway     = !bTaxiway;     return;
	case SnapGuide::EReference::ServiceRoad: bServiceRoad = !bServiceRoad; return;
	case SnapGuide::EReference::Runway:      bRunway      = !bRunway;      return;
	case SnapGuide::EReference::Apron:       bApron       = !bApron;       return;
	case SnapGuide::EReference::Stand:       bStand       = !bStand;       return;
	case SnapGuide::EReference::World:       bWorld       = !bWorld;       return;
	}
}
