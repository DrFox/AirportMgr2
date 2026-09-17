#pragma once

#include "CoreMinimal.h"

/**
 * ICAO Annex 14 code letters A-F, and the figures each one sets, kept as ONE table.
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

	/**
	 * How wide a stand of this letter is, uu: its span band plus twice the letter's wingtip
	 * clearance. DERIVED, never stored - a stored width would be a third figure obliged to
	 * agree with two others, and this table exists because three such figures once drifted.
	 *
	 * Letter matched case-insensitively, unknown letters resolving to C, as RadiusForLetter
	 * does and for the same reason.
	 */
	AIRSIDE_API double StandWidthForLetter(const FString& Letter);

	/**
	 * How deep a stand of this letter is, uu - nose to the back of its GSE road. AUTHORED,
	 * and the only figure here that is; see the row's comment for why no rule produces it.
	 *
	 * Letter matched as StandWidthForLetter matches it.
	 */
	AIRSIDE_API double StandDepthForLetter(const FString& Letter);

	/**
	 * The letter a stand of this size is, or empty when it is smaller than any stand.
	 * The mirror of LetterForWingspan: that one asks what an AIRCRAFT is, this asks what a
	 * piece of GROUND is, and together they decide which aircraft a stand admits.
	 *
	 * BOTH DIMENSIONS, NEVER ONE. A 67 x 30 m stand is D-wide and nothing bigger than a
	 * King Air fits in 30 m of depth, so it is a Code B. The answer is the largest letter
	 * whose width AND depth both fit, which is not the largest whose width fits.
	 *
	 * Empty is a real answer, not a failure: a stand smaller than Code A is refused at
	 * placement, and returning "A" would admit an aircraft to a space it does not fit.
	 */
	AIRSIDE_API FString LetterForStandSize(double WidthUu, double DepthUu);

	/**
	 * How far AFT of the nose-gear stop mark the longest airframe this letter admits reaches,
	 * uu. Positive - it is a distance, and the geometry that uses it negates it.
	 *
	 * THE ONE FIGURE A STAND'S GROUND GEOMETRY IS KEPT CLEAR OF, and it is per LETTER rather
	 * than per named type on purpose. The stand's service geometry was sized from the A320's
	 * tail at -3250 while DA_Aircraft_B738 parks on the same stand with its tail at -3430 -
	 * 1.2 m of clearance where 3 m was intended. Sizing from a named aeroplane is what caused
	 * that; sizing from the letter is the rule the taxiway widths and the service road fillet
	 * already follow.
	 *
	 * Aft of the STOP MARK, not a length, because that is what a layout measured from the
	 * nose-gear origin actually needs - a nose overhang differs by type and is not this
	 * question. An airframe whose own origin is elsewhere (the Piper declares its main gear;
	 * see FAirframe::SteerAxleX) is measured about that origin instead, and is far inside any
	 * of these figures.
	 *
	 * Letter matched as StandWidthForLetter matches it.
	 */
	AIRSIDE_API double MaxTailAftForLetter(const FString& Letter);

	/**
	 * How far FORWARD of the nose-gear stop mark the longest airframe this letter admits
	 * reaches, uu. Positive.
	 *
	 * THE SIBLING OF MaxTailAftForLetter, and it exists because StandDepthForLetter is
	 * measured NOSE to the back of the GSE road. Without the nose overhang there is no way to
	 * turn that depth into the x the layout may actually use, and a layout would silently
	 * gain the overhang as free room.
	 *
	 * Two figures rather than one length for the reason the aft one gives: the origin is the
	 * nose GEAR, not the nose, and the overhang between them differs by type.
	 */
	AIRSIDE_API double MaxNoseFwdForLetter(const FString& Letter);
}
