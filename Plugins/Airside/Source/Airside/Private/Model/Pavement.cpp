#include "Model/Pavement.h"

// Declared in Pavement.h beside the enum they name; used to live in RunwayFacts.cpp (and, before
// that, RunwayAdmission.cpp, #103) instead, which meant the header's declaration and its
// definition were in two different files with no obvious link between them. Moved here when
// ERunwaySurface became EPavement (2026-09-27-shared-pavement) so a doc comment on the
// declaration is the whole story, the way every other header in this module works.

namespace Pavement
{
	const TCHAR* Name(EPavement P)
	{
		switch (P)
		{
		case EPavement::Grass:      return TEXT("grass");
		case EPavement::Tarmac:     return TEXT("tarmac");
		case EPavement::Concrete:   return TEXT("concrete");
		case EPavement::Reinforced: return TEXT("reinforced");
		default:                    break;
		}
		return TEXT("unknown");
	}

	int32 MaterialSlot(EPavement P)
	{
		switch (P)
		{
		case EPavement::Grass:  return 0;
		case EPavement::Tarmac: return 1;
		// Reinforced shares concrete's slot - see the declaration's own comment for why.
		case EPavement::Concrete:
		case EPavement::Reinforced:
		default:                return 2;
		}
	}
}
