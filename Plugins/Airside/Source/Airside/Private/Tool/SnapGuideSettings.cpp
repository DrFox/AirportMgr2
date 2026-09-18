#include "Tool/SnapGuideSettings.h"

bool FSnapGuideSettings::IsEnabled(SnapGuide::ESource Source) const
{
	// NO `default:`. This project does not build switches as exhaustive-or-error, so a source
	// added to ESource without a case here would fall past the switch - which is why the
	// return below is `false` and AirportMgr.Actions.SnapTogglesAreInTheRegistry walks the
	// enum: a source with no toggle is OFF and has no button, and the test says so out loud
	// rather than leaving a guide that quietly never fires.
	switch (Source)
	{
	case SnapGuide::ESource::Extending:  return bExtending;
	case SnapGuide::ESource::PointAlign: return bPointAlign;
	case SnapGuide::ESource::Aligned:    return bAligned;
	case SnapGuide::ESource::Collinear:  return bCollinear;
	case SnapGuide::ESource::Parallel:   return bParallel;
	case SnapGuide::ESource::Runway:     return bRunway;
	case SnapGuide::ESource::World:      return bWorld;
	case SnapGuide::ESource::Offset:     return bOffset;
	}
	return false;
}

void FSnapGuideSettings::Toggle(SnapGuide::ESource Source)
{
	switch (Source)
	{
	case SnapGuide::ESource::Extending:  bExtending  = !bExtending;  return;
	case SnapGuide::ESource::PointAlign: bPointAlign = !bPointAlign; return;
	case SnapGuide::ESource::Aligned:    bAligned    = !bAligned;    return;
	case SnapGuide::ESource::Collinear:  bCollinear  = !bCollinear;  return;
	case SnapGuide::ESource::Parallel:   bParallel   = !bParallel;   return;
	case SnapGuide::ESource::Runway:     bRunway     = !bRunway;     return;
	case SnapGuide::ESource::World:      bWorld      = !bWorld;      return;
	case SnapGuide::ESource::Offset:     bOffset     = !bOffset;     return;
	}
}
