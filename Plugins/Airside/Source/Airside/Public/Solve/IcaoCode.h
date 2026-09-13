#pragma once

#include "CoreMinimal.h"

/**
 * ICAO Annex 14 code letters A-F, and the four figures each one sets, kept as ONE table.
 *
 * Three call sites used to type this table separately, in three different orderings -
 * RunwayAdmission (width -> max wingspan, D/E collapsed to one row), InspectFacts
 * (wingspan -> letter, D/E split) and AnchorLink (letter -> stand turn radius) - and a
 * fourth repeated it in prose. A width or a letter changed in one would silently disagree
 * with the other two, and nothing would say so.
 *
 * PROVENANCE, stated plainly as UAircraftType does for its door stations: these are
 * standard aerodrome design values by code letter, not figures lifted from a specific
 * Annex 14 edition. They are what to check first if a real layout looks wrong - but the
 * SHAPE of the rule, one row per letter, is how aerodromes are actually dimensioned.
 *
 * Dependency-free like the rest of Solve/: CoreMinimal only, no engine types beyond it.
 */
namespace IcaoCode
{
	/**
	 * The letter for a wingspan, uu: under 15 m is A, under 24 m B, under 36 m C, under
	 * 52 m D, under 65 m E, anything wider F.
	 */
	AIRSIDE_API FString LetterForWingspan(double WingspanUu);

	/**
	 * The widest wingspan, uu, that a runway of TotalWidth, uu, is built for. Nearest code
	 * wins; a width shared by two letters (45 m serves both D and E) resolves to the WIDER
	 * one, because the wider figure is the one the width was actually chosen for.
	 */
	AIRSIDE_API double MaxWingspanForWidth(double TotalWidth);

	/**
	 * Minimum centreline curve radius, uu, for a stand sized to this code letter. Letter is
	 * matched case-insensitively; no letter, or one nobody recognises, resolves to C - the
	 * commonest stand in the world, so erring here does not put a 60 m curve on a
	 * light-aircraft apron.
	 */
	AIRSIDE_API double RadiusForLetter(const FString& Letter);
}
