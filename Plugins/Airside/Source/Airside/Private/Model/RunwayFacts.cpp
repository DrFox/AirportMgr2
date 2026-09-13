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
	}
	return TEXT("unknown");
}

const TCHAR* RunwayApproachName(ERunwayApproach Approach)
{
	switch (Approach)
	{
	case ERunwayApproach::Visual:       return TEXT("visual");
	case ERunwayApproach::NonPrecision: return TEXT("non-precision");
	case ERunwayApproach::Precision:    return TEXT("precision");
	}
	return TEXT("unknown");
}
