#pragma once

#include "CoreMinimal.h"
#include "Model/RoadEntity.h"
#include "Model/RoadHandles.h"

/**
 * How many modules of one kind a depot's PLOT can hold - the reservation's ceiling (PlotYard::FReservation::CeilingFor),
 * asked per (depot, kind). In production the game's ops runtime answers it from DepotKit::ReservationOf, the one plot
 * solve the presenter draws from, memoised per depot. UNSET means there is no plot solve to ask - a world-free board or
 * shop - and FDepotCapability then takes the owned list as it stands (see FDepotCapability::Of).
 *
 * IN Model/ AS A TYPE ONLY: Model/ may not call the plot solve (Build/), so the answer is handed in, the way
 * UJobBoard::DesignVehicleOf hands in the design vehicle.
 */
using FModuleCeilingFn = TFunction<int32(FEntityInstanceId Id, const FEntityInstance& Depot, EDepotModule Module)>;

/**
 * WHAT A DEPOT'S MODULES GIVE IT - its vehicle bays and its pumps - and the ONE rule for which modules count: THE SEATED
 * ONES (#443, ruled 2026-09-30 under #266: "a player should not be able to purchase upgrades that don't fit on the plot;
 * the modules that count are only the ones that are placed").
 *
 * OWNED VERSUS SEATED. FEntityInstance::Modules is the OWNED list - what the player bought, and the save's only record of
 * it. A plot holds only so many of each kind (its reservation's ceiling), and the presenter draws the first
 * min(owned, ceiling) of each and DROPS the rest (UPlotPresenter::RebuildFrom: Lit = clamp(Owned, 0, RunLength) per
 * run, the leftover a drop - GetDroppedCount). SEATED is that count. It used to be decided in three places with three
 * rules over the OWNED list: pumps and the plotless exemption in UJobBoard, bays in UFacilityPurchases with no exemption,
 * and the census in DepotKit - so a module the presenter could not seat still granted a bay and a pump the player could
 * not see. This is the one view all three read.
 *
 * A PURCHASE CANNOT OVER-BUY: UFacilityPurchases refuses (NoSlotReserved) once owned reaches the ceiling, so a bought
 * module is seated by construction. What CAN be unseated is the start kit a plot was placed with - #266's remaining item,
 * which this does not touch: a starter mix bigger than the plot holds is seated only up to the ceiling here, and the
 * rest grants nothing.
 * ENFORCED BY: Airside.Present.PlotPresenterDrawsExactlyTheSeatedModules (the presenter's lit count is Seat's),
 * AirportOps.Model.Facility.UnseatedModulesGrantNeitherBaysNorPumps, Check-Architecture rule 45 (capability-from-the-view)
 *
 * THE LEGACY EXEMPTION, NAMED: a depot with NO ground drawn (no outline) AND no module list (placed without a plot - every
 * depot in a save from before plots, and every test depot placed through the plain signature) fuels as it always did. That
 * is not a claim it has a pump; it is that the question does not apply, so bLegacyPlotless reads as one pump. It has no
 * bays, which is unchanged - a starter fleet is seeded whatever it holds, and a bay is only ever asked when the player
 * BUYS. BOTH CONDITIONS, deliberately: a PLOTTED depot with an empty kit is not legacy (its plot seats nothing, so it has no
 * pump - what "only placed modules count" means), and an unplotted depot WITH modules is a test's stand-in for a modular
 * depot whose list is the truth (no pump in it is no pump).
 */
struct FDepotCapability
{
	/** One slot per EDepotModule kind (the sentinel sizes it). */
	static constexpr int32 KindCount = static_cast<int32>(EDepotModule::Count);

	/** The modules the depot OWNS, by kind (indexed by EDepotModule) - what a warning says "N owned" from. */
	int32 Owned[KindCount] = {};

	/** The modules the plot SEATS, by kind (indexed by EDepotModule): min(owned, ceiling). */
	int32 Seated[KindCount] = {};

	/** No ground drawn and no module list: the legacy plotless depot. See the struct comment. */
	bool bLegacyPlotless = false;

	/** Owned modules of Module; 0 for anything out of range. */
	int32 OwnedOf(EDepotModule Module) const
	{
		const int32 Kind = static_cast<int32>(Module);
		return Kind >= 0 && Kind < KindCount ? Owned[Kind] : 0;
	}

	/** Whether Depot has a plot at all: a depot with an outline. (IsDepot with IsPlotted: a drawn stand is plotted too.) */
	static bool HasPlot(const FEntityInstance& Depot) { return Depot.IsDepot() && Depot.IsPlotted(); }

	/** Seated modules of Module; 0 for anything out of range. */
	int32 SeatedOf(EDepotModule Module) const
	{
		const int32 Kind = static_cast<int32>(Module);
		return Kind >= 0 && Kind < KindCount ? Seated[Kind] : 0;
	}

	/** True when the depot can fuel at all: a seated pump, or the legacy exemption. */
	bool HasWorkingPump() const { return bLegacyPlotless || SeatedOf(EDepotModule::Pump) > 0; }

	/**
	 * Pumps the refill rate multiplies by: the seated ones, one for the legacy exemption, and never fewer than one - a
	 * divisor floor, not a claim: a depot with no working pump never gets a vehicle as far as a refill.
	 */
	int32 Pumps() const { return bLegacyPlotless ? 1 : FMath::Max(SeatedOf(EDepotModule::Pump), 1); }

	/** Seat Depot's owned modules against CeilingOf - how many of a kind its plot can hold. */
	static FDepotCapability Seat(const FEntityInstance& Depot, TFunctionRef<int32(EDepotModule)> CeilingOf)
	{
		FDepotCapability Out;
		Out.bLegacyPlotless = Depot.Modules.Num() == 0 && !HasPlot(Depot);
		for (const EDepotModule Module : Depot.Modules)
		{
			const int32 Kind = static_cast<int32>(Module);
			if (Kind >= 0 && Kind < KindCount)
			{
				++Out.Owned[Kind];
			}
		}
		for (int32 Kind = 0; Kind < KindCount; ++Kind)
		{
			Out.Seated[Kind] = Out.Owned[Kind] > 0 ? FMath::Min(Out.Owned[Kind], FMath::Max(CeilingOf(static_cast<EDepotModule>(Kind)), 0)) : 0;
		}
		return Out;
	}

	/**
	 * Seat Depot against Ceiling - or, with no plot to seat against, take the owned list as seated. TWO CASES HAVE NONE:
	 *  - no Ceiling to ask (a bare NewObject board or shop, which has no world and no plot solve). UNSEATING NEEDS A SOLVE, and
	 *    an unset hook is the absence of one, not a ceiling of zero: zero would make every world-free depot pumpless and slotless;
	 *  - a depot with no plot (no outline): a module list on ground that was never drawn - a save from before plots, a test's
	 *    stand-in for a modular depot - has nothing to be smaller than. The plot solve says 0 for it, which is right for a
	 *    PURCHASE (nothing can be bought into no plot) and wrong for what it already holds.
	 * A plotted depot whose plot cannot be solved seats nothing, as the solve says.
	 */
	static FDepotCapability Of(FEntityInstanceId Id, const FEntityInstance& Depot, const FModuleCeilingFn& Ceiling)
	{
		if (Ceiling && HasPlot(Depot))
		{
			return Seat(Depot, [&](EDepotModule Module) { return Ceiling(Id, Depot, Module); });
		}
		return Seat(Depot, [](EDepotModule) { return MAX_int32; });
	}
};
