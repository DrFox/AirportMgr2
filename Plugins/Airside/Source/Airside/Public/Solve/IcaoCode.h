#pragma once

#include "CoreMinimal.h"

/**
 * ICAO Annex 14 code letter, A through F.
 *
 * A PLAIN ENUM, NOT A UENUM: Solve/ is CoreMinimal.h-only and takes no engine types beyond
 * it (see the namespace comment below), and nothing here needs UHT - no UPROPERTY stores
 * it, no Blueprint reads it. Where a letter is AUTHORED - UAircraftType::Code - it stays an
 * FName on the asset, because that is the UPROPERTY a human edits; IcaoCode::Parse is the
 * one place that turns it into this enum, once, at the call site that reads it.
 *
 * REPLACES const FString& Letter ON EVERY TABLE FUNCTION BELOW. The nine functions keyed on
 * a letter each fell back to Code C's row on any string that did not match A-F exactly -
 * empty, lower-case before this existed, or a straight typo - and did it SILENTLY: nothing
 * logged, so a mis-typed UAircraftType::Code became a stand sized for the wrong aeroplane
 * with no line in the log to say so. An EIcaoCode cannot BE an unrecognised letter, so the
 * table functions need no fallback any more; the one place a bad string can still arrive is
 * Parse, and that is where the Warning now lives.
 */
enum class EIcaoCode : uint8
{
	A,
	B,
	C,
	D,
	E,
	F
};

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
	 * Letter to EIcaoCode, or nullopt when it names none of A-F.
	 *
	 * TRIMMED THEN UPPERCASED, in that order, once, here - so "c", "C" and " C " (leading or
	 * trailing whitespace, as a hand-typed FName can carry) all parse to EIcaoCode::C, and
	 * anything else - empty, two letters, a digit, "Z" - is nullopt rather than a guess.
	 *
	 * LOGS NOTHING ITSELF: Solve/ is CoreMinimal-only and AirsideLog.h is outside it (see the
	 * namespace comment). The nullopt is the signal; the ONE call site that reads authored
	 * content off a data asset (AnchorLink.cpp's RadiusForCode, off UAircraftType::Code) is
	 * what turns a miss into a UE_LOG Warning, because that is the one place a typo can
	 * actually arrive from a human. Every other caller in this codebase hands Parse a letter
	 * this program derived itself (LetterForWingspan, LetterForStandSize, or a compile-time
	 * literal), which cannot be a typo and is asserted rather than logged where it is used.
	 */
	AIRSIDE_API TOptional<EIcaoCode> Parse(const FString& Letter);

	/**
	 * Parse's inverse, for a log line or a UI label - never for a lookup, which is what the
	 * enum itself is for now.
	 */
	AIRSIDE_API const TCHAR* ToLetter(EIcaoCode Code);

	/**
	 * The code for a wingspan, uu: under 15 m is A, under 24 m B, under 36 m C, under
	 * 52 m D, under 65 m E, anything wider F.
	 *
	 * THE PRIMITIVE, since 2026-09-25 (#292) - LetterForWingspan is built on this, not the
	 * other way round. Three call sites (IcaoCode.cpp's own StandAdmits/StandRank,
	 * AnchorLink.cpp's LeadInSizingFor, StandMarkingBuilder.cpp's glyph letter) used to get an
	 * EIcaoCode by calling Parse(LetterForWingspan(Uu)) - an FString built and torn straight
	 * back down, and StandAdmits runs it per candidate stand per arrival. This is TOTAL over
	 * every WingspanUu (see the row loop's own fallback), so unlike Parse there is no
	 * TOptional to thread through the caller: a wingspan always names a code, the way a
	 * string typed by a human does not.
	 */
	AIRSIDE_API EIcaoCode CodeForWingspan(double WingspanUu);

	/**
	 * LetterForWingspan's own name, kept for a log line or a UI label - never for a lookup,
	 * which is CodeForWingspan's job now. Equivalent to ToLetter(CodeForWingspan(WingspanUu)).
	 */
	AIRSIDE_API FString LetterForWingspan(double WingspanUu);

	/**
	 * LetterForWingspan's own inverse for the WIDEST letter: the widest wingspan, uu, that
	 * Code still admits before the NEXT letter up would be asked for - Rows' own MaxWingspan
	 * column, read back out.
	 *
	 * THE ONE REAL "TOO WIDE" TEST THIS TABLE HAS. LetterForWingspan's contract is
	 * deliberately total - "anything wider F" (see its own comment) - so it can never itself
	 * report a span nothing on the table admits; that is exactly right for a caller that only
	 * ever wants "the nearest reasonable letter" and must not refuse. A caller that DOES need
	 * to refuse a genuinely unadmittable span - StandAdmits below is the one that exists for -
	 * asks THIS function about EIcaoCode::F instead. Added for exactly that call, after an
	 * earlier drawn-stands admission draft tried to special-case "wider than F" by composing
	 * LetterForWingspan + Parse alone and found it structurally could not: a genuine Code F
	 * aircraft and one twice as wide as anything ever built both parse to the string "F", and
	 * nothing outside this file could tell them apart without exposing the row's own ceiling.
	 * Pinned against LetterForWingspan's own band edges by
	 * Airside.Solve.MaxWingspanForLetterMatchesTheBandEdges.
	 *
	 * THE VALUE ITSELF IS THE EXCLUSIVE EDGE FOR EVERY LETTER BUT F, AND INCLUSIVE FOR F.
	 * LetterForWingspan's row loop is `WingspanUu < Row.MaxWingspan` - strictly less - so a
	 * span exactly AT Code X's own ceiling has ALREADY rolled over and reads as the NEXT
	 * letter up; only a span a hair under it still reads as X. Code F alone has no row above
	 * it to roll into, so both a hair under its ceiling and exactly AT it still read "F" - see
	 * the test named above for both halves of this, pinned for all six letters. A caller that
	 * wants a span which reads back as ITS OWN letter - a drawn stand's captured
	 * DesignWingspan, for one - wants DesignSpanForLetter below, not this value directly.
	 */
	AIRSIDE_API double MaxWingspanForLetter(EIcaoCode Code);

	/**
	 * The widest wingspan, uu, that still reads back as Code through LetterForWingspan - "the
	 * widest aircraft this letter's stand may honestly claim to be designed for".
	 *
	 * NOT MaxWingspanForLetter(Code) ITSELF, for every letter but F - see that function's own
	 * comment: its value is the EXCLUSIVE edge, already the next letter's, so a caller that
	 * captured it as a "Code X" aircraft's span would have LetterForWingspan (and therefore
	 * IcaoCode::StandAdmits/StandRank, which both read a captured span through it) report the
	 * stand as one letter wider than it was drawn. A hair under the ceiling - one uu, which has
	 * no meaning beyond "not the ceiling itself" - is the whole of the correction. Code F is
	 * the one exception: it has nothing above it to roll into, so its own ceiling is ALREADY
	 * the widest span the table still calls F (LetterForWingspan reads "F" both a hair under
	 * it and exactly at it), and MaxWingspanForLetter(F) is used unchanged.
	 *
	 * Pinned for all six letters by Airside.Solve.DesignSpanForLetterReadsBackAsItsOwnLetter:
	 * LetterForWingspan(DesignSpanForLetter(L)) == ToLetter(L), and
	 * StandAdmits(DesignSpanForLetter(L), DesignSpanForLetter(L)) is true.
	 */
	AIRSIDE_API double DesignSpanForLetter(EIcaoCode Code);

	/**
	 * The widest wingspan, uu, that a runway of TotalWidth, uu, is built for. Nearest code
	 * wins; a width shared by two letters (45 m serves both D and E) resolves to the WIDER
	 * one, because the wider figure is the one the width was actually chosen for.
	 */
	AIRSIDE_API double MaxWingspanForWidth(double TotalWidth);

	/**
	 * Minimum centreline curve radius, uu, for a stand sized to this code letter.
	 *
	 * TAKES THE ENUM, NOT A STRING, since 2026-09-21 - the fallback to Code C for "no letter,
	 * or one nobody recognises" used to live here and hide a typo; Parse is where that
	 * decision is made now, once, with a Warning at the call site that reads a human's typing.
	 */
	AIRSIDE_API double RadiusForLetter(EIcaoCode Code);

	/**
	 * The NARROWEST stand of this letter, uu: its span band, plus twice the letter's wingtip
	 * clearance, plus twice a service lane, plus the room its road contacts need along the
	 * aft edge.
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
	 * not clear of the wingtip. The aft-edge allowance is in it because every bay is entered
	 * from behind by its own way in, and each of those contacts splits the GSE road and wants
	 * its fillet's run along it.
	 *
	 * Code C is 3600 + 2 x (450 + 400) + 600 = 5900 rather than the 4500 the span and
	 * clearance alone give. 4500 was measured on 2026-09-17 as too narrow to turn a service
	 * vehicle in - every arrangement of a lane, a rank and a bay landed within a few tens of
	 * uu of an edge - and 5300 held the lanes but not the six road contacts, which left
	 * fillets clamped to 212 and 15 uu against a lock of 699.
	 *
	 * TAKES THE ENUM, as RadiusForLetter does and for the same reason - there is no unknown
	 * letter left to fall back from once the input can only be A-F.
	 */
	AIRSIDE_API double StandWidthForLetter(EIcaoCode Code);

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
	AIRSIDE_API double MaxStandWidthForLetter(EIcaoCode Code);

	/**
	 * How wide a lane a service vehicle needs, uu - four metres, a service road's own lane.
	 *
	 * HERE RATHER THAN ON THE STAND BUILDER because the stand's minimum WIDTH is derived from
	 * it, and a figure that sizes the table cannot live downstream of the table. It was
	 * FStandLayoutBuild::LaneWidth, which is where it reached the graph; that constant now reads
	 * this one, so widening a lane widens every stand that has to hold two of them.
	 */
	AIRSIDE_API double ServiceLaneWidth();

	/**
	 * How deep a stand of this letter is, uu - nose to the back of its GSE road. AUTHORED,
	 * and the only figure here that is; see the row's comment for why no rule produces it.
	 *
	 * Takes the enum, as StandWidthForLetter does.
	 */
	AIRSIDE_API double StandDepthForLetter(EIcaoCode Code);

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
	 * tail at -3250 while DA_Aircraft_B738 parks on the same stand with its tail at -3538 -
	 * 0.1 m of clearance where 3 m was intended. Sizing from a named aeroplane is what caused
	 * that; sizing from the letter is the rule the taxiway widths and the service road fillet
	 * already follow.
	 *
	 * Aft of the STOP MARK, not a length, because that is what a layout measured from the
	 * nose-gear origin actually needs - a nose overhang differs by type and is not this
	 * question. An airframe whose own origin is elsewhere (the Piper declares its main gear;
	 * see FChassis::SteerAxleX) is measured about that origin instead, and is far inside any
	 * of these figures.
	 *
	 * Takes the enum, as StandWidthForLetter does.
	 */
	AIRSIDE_API double MaxTailAftForLetter(EIcaoCode Code);

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
	AIRSIDE_API double MaxNoseFwdForLetter(EIcaoCode Code);

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
	AIRSIDE_API double WingFwdForLetter(EIcaoCode Code);
	AIRSIDE_API double WingAftForLetter(EIcaoCode Code);

	/**
	 * True when Local, in the stand's own space, is inside the wing keep-out - between the two
	 * edges above and no further out than the letter's span band.
	 *
	 * A RECTANGLE, NOT A PLANFORM, and said plainly because a reader will assume otherwise:
	 * with no chord or sweep authored anywhere there is no planform to test against, so this
	 * is the smallest box that certainly contains every admitted wing. It is conservative in
	 * the right direction - it refuses ground a real wing leaves clear, never the reverse.
	 */
	AIRSIDE_API bool WingKeepOutContains(EIcaoCode Code, const FVector2D& Local);

	/**
	 * True when the segment A-B enters the wing keep-out anywhere along its length.
	 *
	 * SEGMENTS, NOT SAMPLED POINTS. A path checked point by point can step clean over a corner
	 * of the box between two samples and report itself clear, which is the same class of defect
	 * as a per-edge drivability test that cannot see a join. This clips the segment against the
	 * box, so a crossing of any length is found.
	 */
	AIRSIDE_API bool WingKeepOutCrossedBy(EIcaoCode Code, const FVector2D& A, const FVector2D& B);

	/**
	 * THE ONE ADMISSION RULE: may an aircraft of AircraftSpanUu use a stand designed for
	 * StandDesignSpanUu - compared by LETTER, never by the raw spans against each other.
	 *
	 * WHY A LETTER COMPARE AND NOT StandDesignSpanUu >= AircraftSpanUu: a stand's captured
	 * DesignWingspan is often a LEGACY figure (an older measurement of the type it was drawn
	 * for, or a hand-typed one) rather than today's authored span, and two different numbers
	 * can be the SAME letter - LetterForWingspan(3410) and LetterForWingspan(3580) are both
	 * "C". Comparing the raw doubles would refuse a 737-800 (3580) a stand built for an A320
	 * whose captured span happens to read 3410, even though both are ordinary Code C
	 * aeroplanes and the stand fits either. See Airside.Solve.StandAdmitsComparesLetters.
	 *
	 * ONE RULE, TWO CALLERS: ArrivalPlanner::ChooseStand (live dispatch, shortest taxi among
	 * admitted stands) and UStandAllocator::Reserve (holding a stand for an accepted flight
	 * before it lands) used to each compare the raw spans their own way, and had started to
	 * disagree - a legacy-span stand admitted a 737 in one and refused it in the other, same
	 * aircraft, same stand. Moved here so there is exactly one place this can be decided.
	 *
	 * UNKNOWN ADMITS ANYTHING, EITHER SIDE: StandDesignSpanUu <= 0 (a stand nobody measured -
	 * FEntityInstance::DesignWingspan's own "unknown") or AircraftSpanUu <= 0 (an airframe
	 * that never set Wingspan - every hand-built TestAirframes fixture bar the measured ones,
	 * and any content older than this rule) means there is nothing to compare a size against,
	 * so nothing is refused for size - exactly the behaviour every caller saw before this rule
	 * existed.
	 *
	 * WIDER THAN EVERY LETTER ADMITS (AircraftSpanUu > MaxWingspanForLetter(EIcaoCode::F)) IS
	 * NEVER ADMITTED, full stop - see MaxWingspanForLetter's own comment on why this is the
	 * one place that ceiling can be asked about at all.
	 */
	AIRSIDE_API bool StandAdmits(double StandDesignSpanUu, double AircraftSpanUu);

	/**
	 * Ordinal for ranking stands against each other by size: A=0 .. F=5, for "the SMALLEST
	 * admitted stand wins" (GDD: big stands are kept for big aircraft). UNKNOWN (<= 0) RANKS
	 * AS C - the legacy default IcaoCode's own RadiusForLetter fallback used before Parse
	 * existed - so an unmeasured stand competes at a nominal middling size rather than always
	 * winning (as the smallest) or always losing (as the largest) a best-stand comparison
	 * against measured ones. Only meaningful for a stand StandAdmits already agreed to.
	 */
	AIRSIDE_API int32 StandRank(double StandDesignSpanUu);
}
