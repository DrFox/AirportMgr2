#pragma once

#include "CoreMinimal.h"
#include "Model/FlightBoard.h"
#include "Model/FuelService.h"
#include "Model/Ledger.h"
#include "Model/OpsSave.h"
#include "Model/Pricing.h"
#include "Model/SimClock.h"

/**
 * The persistent objects a save test uses, as the list OpsSave::Capture/Restore now take.
 *
 * ONE HELPER RATHER THAN THE LIST BUILT AT SIX CALL SITES. These used to be positional
 * parameters, so adding a system meant editing every test that saved anything; the point of
 * the list was to stop that, and rebuilding it by hand in each test would have kept the whole
 * cost while losing the compiler's help.
 *
 * INLINE AND IN A HEADER, with full includes rather than forward declarations: converting a
 * UObject* to an IOpsPersistent* crosses a multiple-inheritance boundary and needs the
 * complete type, so a forward declaration would silently not compile here.
 *
 * A TEMPORARY IS SAFE AT THE CALL SITE: the returned array outlives the full expression the
 * TArrayView is consumed in, which is the whole of the Capture or Restore call.
 */
namespace OpsSaveTest
{
	inline TArray<IOpsPersistent*> Persistents(USimClock& Clock, UFlightBoard& Board,
		UFuelService& Fuel)
	{
		TArray<IOpsPersistent*> Out;
		Out.Add(&Clock);
		Out.Add(&Board);
		Out.Add(&Fuel);
		return Out;
	}

	/** As above, plus the money. For tests that care what a save does to the ledger. */
	inline TArray<IOpsPersistent*> Persistents(USimClock& Clock, UFlightBoard& Board,
		UFuelService& Fuel, ULedger& Ledger, UPricing& Pricing)
	{
		TArray<IOpsPersistent*> Out = Persistents(Clock, Board, Fuel);
		Out.Add(&Ledger);
		Out.Add(&Pricing);
		return Out;
	}
}
