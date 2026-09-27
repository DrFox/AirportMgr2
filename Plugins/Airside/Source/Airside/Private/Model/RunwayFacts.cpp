#include "Model/RunwayFacts.h"

// Declared in RunwayFacts.h beside the enums they name; used to live in RunwayAdmission.cpp
// instead, which meant the header's declaration and its definition were in two different
// files with no obvious link between them (#103). Moved here so a doc comment on the
// declaration is the whole story, the way every other header in this module works.
// RunwaySurfaceName and RunwayMaterialSlot made a further move, to Pavement::Name and
// Pavement::MaterialSlot in Pavement.h/.cpp, when ERunwaySurface became EPavement
// (2026-09-27-shared-pavement) - RunwayApproachName has no such second scale to share with
// roads and stands, so it stays here. RoadSurfacePavement and RoadSurfaceName (#356's) went
// with ERoadSurface when road surfaces joined EPavement: Pavement::Name spells both now.

const TCHAR* RunwayApproachName(ERunwayApproach Approach)
{
	switch (Approach)
	{
	case ERunwayApproach::Visual:       return TEXT("visual");
	case ERunwayApproach::NonPrecision: return TEXT("non-precision");
	case ERunwayApproach::Precision:    return TEXT("precision");
	default:                            break;
	}
	return TEXT("unknown");
}
