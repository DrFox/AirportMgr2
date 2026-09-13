#include "Model/RunwayFacts.h"

// Declared in RunwayFacts.h beside the enums they name; used to live in RunwayAdmission.cpp
// instead, which meant the header's declaration and its definition were in two different
// files with no obvious link between them (#103). Moved here so a doc comment on the
// declaration is the whole story, the way every other header in this module works.

const TCHAR* RunwaySurfaceName(ERunwaySurface Surface)
{
	switch (Surface)
	{
	case ERunwaySurface::Grass:      return TEXT("grass");
	case ERunwaySurface::Tarmac:     return TEXT("tarmac");
	case ERunwaySurface::Concrete:   return TEXT("concrete");
	case ERunwaySurface::Reinforced: return TEXT("reinforced");
	default:                         break;
	}
	return TEXT("unknown");
}

int32 RunwayMaterialSlot(ERunwaySurface Surface)
{
	switch (Surface)
	{
	case ERunwaySurface::Grass:  return 0;
	case ERunwaySurface::Tarmac: return 1;
	// Reinforced shares concrete's slot - see the declaration's own comment for why.
	case ERunwaySurface::Concrete:
	case ERunwaySurface::Reinforced:
	default:                     return 2;
	}
}

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
