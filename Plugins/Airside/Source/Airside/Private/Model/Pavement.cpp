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

	TArray<EPavement> Offered(TConstArrayView<EPavement> Allowed)
	{
		if (Allowed.Num() > 0)
		{
			TArray<EPavement> Sorted(Allowed);
			Sorted.Sort();
			return Sorted;
		}
		// ALL FOUR IN SCALE ORDER, from the enum rather than typed, so a fifth step is offered
		// by the runway row without anyone remembering this list.
		TArray<EPavement> All;
		for (uint8 Each = 0; Each < static_cast<uint8>(EPavement::Count); ++Each)
		{
			All.Add(static_cast<EPavement>(Each));
		}
		return All;
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

	FPavementCheck Judge(EPavement Have, EPavement Need)
	{
		FPavementCheck Out;
		Out.Have = Have;
		Out.Need = Need;
		return Out;
	}

	FString Describe(const FPavementCheck& Check)
	{
		return Check.Passes() ? FString()
			: FString::Printf(TEXT("the surface is %s; this aircraft needs %s"), Name(Check.Have), Name(Check.Need));
	}

	double RateFactor(EPavement P)
	{
		switch (P)
		{
		case EPavement::Grass:      return 0.4;
		case EPavement::Tarmac:     return 1.0;
		case EPavement::Concrete:   return 1.4;
		case EPavement::Reinforced: return 1.8;
		default:                    break;
		}
		// An out-of-range byte from a bad save: bill at the authored rate rather than at zero,
		// because a zero factor would build anything for free.
		return 1.0;
	}
}
