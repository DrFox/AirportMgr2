#pragma once

#include "CoreMinimal.h"
#include "Kismet/GameplayStatics.h"
#include "Model/FlightBoard.h"
#include "Model/JobBoard.h"
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
	/**
	 * A SAVE SLOT THAT DIES WITH THE TEST, however the test ends (RAII). A SaveToSlot in a test writes a real file under
	 * Saved/SaveGames, and a test that does not take it back leaves state its NEXT run did not make: 22 slots had piled up
	 * there by 2026-10-01, written by tests that never deleted them (#462's review), and a stale slot is exactly what lets a
	 * "load a missing slot is refused" or a "save then load" test pass or fail on the previous run's file.
	 *
	 * DELETED AT CONSTRUCTION TOO, not only at the end: a crashed or killed run (the dedicated test editor is a hard-kill
	 * target) never reaches a destructor, so the first thing the next run must see is no slot at all.
	 *
	 * CONVERTS TO FString so it drops in where the bare `const FString Slot` was: SaveToSlot, LoadFromSlot, ReadSlot and
	 * WriteSlot all take const FString&.
	 * ENFORCED BY: Check-Architecture rule 102 (ops-test-slots-scoped) - an "AirportOpsTest_..." slot literal that is not
	 * the argument of an FScopedSlot (or a slot read that nothing wrote) fails the lint.
	 */
	struct FScopedSlot
	{
		FString Name;

		explicit FScopedSlot(const TCHAR* InName) : Name(InName) { UGameplayStatics::DeleteGameInSlot(Name, 0); }
		~FScopedSlot() { UGameplayStatics::DeleteGameInSlot(Name, 0); }
		FScopedSlot(const FScopedSlot&) = delete;
		FScopedSlot& operator=(const FScopedSlot&) = delete;

		operator const FString&() const { return Name; }
	};

	inline TArray<IOpsPersistent*> Persistents(USimClock& Clock, UFlightBoard& Board,
		UJobBoard& Fuel)
	{
		TArray<IOpsPersistent*> Out;
		Out.Add(&Clock);
		Out.Add(&Board);
		Out.Add(&Fuel);
		return Out;
	}

	/** As above, plus the money. For tests that care what a save does to the ledger. */
	inline TArray<IOpsPersistent*> Persistents(USimClock& Clock, UFlightBoard& Board,
		UJobBoard& Fuel, ULedger& Ledger, UPricing& Pricing)
	{
		TArray<IOpsPersistent*> Out = Persistents(Clock, Board, Fuel);
		Out.Add(&Ledger);
		Out.Add(&Pricing);
		return Out;
	}
}
