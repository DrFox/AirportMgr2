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
	 * The NARROWEST stand of this letter, uu: its span band, plus twice the letter's wingtip
	 * clearance, plus twice a service lane.
	 *
	 * A MINIMUM, NOT A SIZE, since 2026-09-17. A stand is a polygon the PLAYER draws and the
	 * letter is derived from what they drew (LetterForStandSize), so no stand has "the" width
	 * of its letter - each letter owns a BAND, and this is its floor. MaxStandWidthForLetter
	 * is the other end.
	 *
	 * DERIVED, never stored - a stored width would be a third figure obliged to agree with two
	 * others, and this table exists because three such figures once drifted. The service lane
	 * is in it because a vehicle has to get PAST the aeroplane to reach the far side, and the
	 * wingtip clearance is separation from anything: a lane laid inside it is a lane that is
	 * not clear of the wingtip. Code C is 3600 + 2 x (450 + 400) = 5300 rather than the 4500
	 * the span and clearance alone give, and 4500 was measured on 2026-09-17 as too narrow to
	 * turn a service vehicle in - every arrangement of a lane, a rank and a bay landed within
	 * a few tens of uu of an edge.
	 *
	 * Letter matched case-insensitively, unknown letters resolving to C, as RadiusForLetter
	 * does and for the same reason.
	 */
	AIRSIDE_API double StandWidthForLetter(const FString& Letter);

	/**
	 * The WIDEST stand still of this letter, uu - the next letter's minimum.
	 *
	 * NOT A COLUMN, and that is the whole point: a stored maximum would have to agree with the
	 * next row's minimum, and the day the two disagreed there would be a width belonging to no
	 * letter, or to two. Derived, the bands TILE: every width from Code A's floor upward has
	 * exactly one letter, and LetterForStandSize needs no second test.
	 *
	 * The widest letter has no letter above it, so its maximum is unbounded and this reports
	 * DBL_MAX. A stand wider than any aeroplane needs is not an error - see the ruling that a
	 * small airframe on a large stand is fine.
	 */
	AIRSIDE_API double MaxStandWidthForLetter(const FString& Letter);

	/**
	 * How wide a lane a service vehicle needs, uu - four metres, a service road's own lane.
	 *
	 * HERE RATHER THAN ON THE STAND BUILDER because the stand's minimum WIDTH is derived from
	 * it, and a figure that sizes the table cannot live downstream of the table. It was
	 * FStandLaneBuild::LaneWidth, which is where it reached the graph; that constant now reads
	 * this one, so widening a lane widens every stand that has to hold two of them.
	 */
	AIRSIDE_API double ServiceLaneWidth();

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
	 * THIS IS THE GAME MECHANIC, not a lookup. The player draws a stand polygon; its size
	 * decides which aircraft may use it. Nobody picks a letter.
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

	/**
	 * The WING KEEP-OUT for this letter: the fore-aft extent, uu about the nose-gear stop
	 * mark, of every wing the letter admits, laid over one another. Fwd is the forward-most
	 * leading edge and Aft the aft-most trailing edge, so both are negative and Aft < Fwd.
	 *
	 * THE UNION ACROSS THE FLEET, NOT ONE TYPE'S WING, and that is the whole idea: a stand
	 * admits several airframes and the ground has to be marked for all of them at once. Real
	 * aprons paint exactly this - a no-entry box under a large swept wing - because the
	 * marking cannot be repainted for each arrival.
	 *
	 * NOTHING DRIVES THROUGH IT. Ruled 2026-09-17: a vehicle may not pass under a wing at all,
	 * so a route reaches a service point forward of the leading edge, aft of the trailing edge,
	 * or outboard of the wingtip - never across. WingKeepOutContains and WingKeepOutCrossedBy
	 * are the ONE place that test is written, so a layout and the test that judges it cannot
	 * disagree about where the wing is.
	 *
	 * AUTHORED, like StandDepth and for the same reason - the model carries no wing planform.
	 * FEntityFootprint has WingX, a single spanwise LINE where the wing crosses the centreline,
	 * and no chord or sweep at all; there is nothing to derive a leading edge from. Code C's
	 * figures are a 737-800 and an A320 root chord plus wing-body fairing, and every builder's
	 * WingX is pinned inside its letter's band by a test, so the two cannot drift.
	 */
	AIRSIDE_API double WingFwdForLetter(const FString& Letter);
	AIRSIDE_API double WingAftForLetter(const FString& Letter);

	/**
	 * True when Local, in the stand's own space, is inside the wing keep-out - between the two
	 * edges above and no further out than the letter's span band.
	 *
	 * A RECTANGLE, NOT A PLANFORM, and said plainly because a reader will assume otherwise:
	 * with no chord or sweep authored anywhere there is no planform to test against, so this
	 * is the smallest box that certainly contains every admitted wing. It is conservative in
	 * the right direction - it refuses ground a real wing leaves clear, never the reverse.
	 */
	AIRSIDE_API bool WingKeepOutContains(const FString& Letter, const FVector2D& Local);

	/**
	 * True when the segment A-B enters the wing keep-out anywhere along its length.
	 *
	 * SEGMENTS, NOT SAMPLED POINTS. A path checked point by point can step clean over a corner
	 * of the box between two samples and report itself clear, which is the same class of defect
	 * as a per-edge drivability test that cannot see a join. This clips the segment against the
	 * box, so a crossing of any length is found.
	 */
	AIRSIDE_API bool WingKeepOutCrossedBy(
		const FString& Letter, const FVector2D& A, const FVector2D& B);
}
