#pragma once

#include "CoreMinimal.h"

/**
 * THE DESIGN FIGURES THAT WERE TYPED AT SEVERAL SITES, each written once (#449). UScenario's defaults read them, and
 * so do the receivers' bare-NewObject defaults and the fuel policy's, which each carried its own copy: 500 L/min in
 * three files, 8 offers in two, the offer's 50-90% fuel draw beside a fallback of 70% nobody derived from it.
 *
 * A HEADER OF CONSTANTS, not a settings object: these are what a scenario asset starts from and what a test's bare
 * NewObject runs on. The asset is still where a designer changes them for a game (UScenario), and UOpsRuntime::
 * ApplyScenarioFigures is still the one door onto the receivers - see its comment for the Transient ruling.
 * ENFORCED BY: Check-Architecture rule 4 ('ops design default typed twice')
 */
namespace OpsDesignDefaults
{
	/** How fast a depot refills a returning vehicle, litres per GAME minute per pump module. */
	inline constexpr double RefillLitresPerMinutePerPump = 500.0;

	/** How many offers the inbox holds before new ones are dropped (spec 2026-09-28 ruling 6). */
	inline constexpr int32 MaxPendingOffers = 8;

	/** An offer's fuel load, as a share of the tank: drawn between these at the offer (spec 2026-09-28-fuel-litres). */
	inline constexpr double FuelLoadDrawMin = 0.5;
	inline constexpr double FuelLoadDrawMax = 0.9;

	/** The load a flight nobody offered asks for (key 7, an agent no flight owns): the MIDDLE of the draw, derived. */
	inline constexpr double FuelLoadDefault = (FuelLoadDrawMin + FuelLoadDrawMax) * 0.5;

	/** Litres one Tank module holds (spec 2026-10-02 §7). A starter depot's one tank is a working morning's fuel at a
	 *  small field: the pacing model sold ~15,000-25,000 L a day at three stands (2026-10-02). */
	inline constexpr double LitresPerTank = 30000.0;
}
